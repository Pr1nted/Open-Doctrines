// Making the advisors answer: what runs when, and on which thread.
//
// The rules and the prompt live in src/llm/Advisor.h and are tested without a
// network. This is the part that decides WHEN an advisor writes, gets the
// request off the game thread, and puts the answer back safely.
//
// THREE THREADING RULES, EACH LEARNED THE HARD WAY
//
//   1. THE REQUEST IS NOT ON THE GAME THREAD. A local model can take twenty
//      seconds on a laptop. Blocking a turn on it would turn a slow model into
//      a frozen game, and the player would report a hang.
//   2. NOTHING TOUCHES THE MAIL FROM THE WORKER. Boxes are game state. The
//      worker produces plain strings and the game thread writes the letters.
//   3. NO T() OFF THE GAME THREAD. The translation arena is a shared deque with
//      no lock (see i18n/Locale.cpp) and the renderer hits it hundreds of times
//      a frame. Every string a worker needs is resolved before it starts.

#include "Game.h"
#include "GameInternals.h"
#include "llm/Advisor.h"
#include "llm/Runner.h"
#include "net/HttpClient.h"
#include "i18n/Locale.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <system_error>
#include <mutex>
#include <thread>

namespace {

/// One answer, on its way back to the game thread.
struct Answer {
    int from = 0;
    int to = 0;
    std::string body;
};

std::mutex g_lock;
std::vector<Answer> g_answers;
int g_inFlight = 0;

/// The runner test's outcome, waiting to be collected by the game thread.
std::string g_testResult;
bool g_testOk = false;
bool g_testFresh = false;
bool g_installDone = false;

}  // namespace

/**
 * How the two compare, in one word.
 *
 * A word rather than a number, and a coarse one. An advisor that can quote your
 * army size is reading the save file rather than the mail, and a player spots
 * that instantly -- so the model is told "stronger" and never "412,000 men".
 * Province count is the crudest possible proxy and that is the point.
 */
std::string Game::llmRelativeStrength(int fromCountry, int toCountry) const {
    int mine = 0, theirs = 0;
    for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
        (void)pid;
        if (pr.countryId == fromCountry) ++mine;
        else if (pr.countryId == toCountry) ++theirs;
    }
    if (mine <= 0 || theirs <= 0) return "";
    const double ratio = (double)theirs / (double)mine;
    if (ratio > 1.6) return "considerably stronger than you";
    if (ratio > 1.15) return "somewhat stronger than you";
    if (ratio < 0.62) return "considerably weaker than you";
    if (ratio < 0.87) return "somewhat weaker than you";
    return "comparable to you";
}

bool Game::llmConfigured() const {
    return m_config.llmEnabled && !m_config.llmEndpoint.empty() &&
           !m_config.llmModel.empty();
}

/**
 * Which countries answer their own mail.
 *
 * Every country that is not the player and not a rebel, when the module is on.
 * Rebels are excluded because they are a mechanic rather than a state -- a
 * peasant revolt with a foreign ministry is a joke the game does not need.
 */
void Game::rebuildLlmCountries() {
    m_llmCountries.clear();
    if (!llmConfigured()) { m_llmAvailable = false; return; }
    for (const auto& [cid, c] : m_countries.getAll()) {
        (void)c;
        if (cid <= 0 || cid == m_playerCountryId) continue;
        if (cid >= REBEL_CID_MIN) continue;
        m_llmCountries.insert(cid);
    }
    m_llmAvailable = !m_llmCountries.empty();
}

/**
 * Ask one advisor for a reply, off the game thread.
 *
 * Everything the worker needs is copied in before it starts: the conversation,
 * the endpoint, and the already-translated language name. It returns a string
 * and touches nothing else.
 */
void Game::askAdvisor(int fromCountry, int toCountry) {
    const mail::Box* box = mailboxIfAny(fromCountry);
    if (!box) return;
    const mail::Thread* thread = box->thread(toCountry);
    if (!thread) return;

    llm::Persona persona;
    if (const Country* c = m_countries.getCountry(fromCountry)) persona.countryName = c->name;
    if (const Country* c = m_countries.getCountry(toCountry)) persona.correspondent = c->name;
    persona.toAnotherAdvisor = (toCountry != m_playerCountryId) && mailIsBot(toCountry);
    if (persona.countryName.empty() || persona.correspondent.empty()) return;

    llm::Situation situation;
    situation.turn = m_turnNumber;
    situation.date = m_mapDate;
    situation.atWar = modAtWar(fromCountry, toCountry);
    situation.relativeStrength = llmRelativeStrength(fromCountry, toCountry);
    describeSituation(fromCountry, toCountry, situation);

    // Resolved HERE, on the game thread. See rule 3 at the top of this file.
    // The ENGLISH name of the active language, because that is what the
    // instruction is written in -- telling a model to answer in "Українська"
    // inside an English prompt is less reliable than telling it "Ukrainian".
    const std::string languageName = od::i18n::current().english;

    // Not const: the worker appends the model's tool calls and their answers to
    // it as the conversation goes round.
    auto turns = llm::buildConversation(thread->messages, fromCountry, persona,
                                        situation, languageName);

    std::string url = m_config.llmEndpoint;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";

    // THE KEY GOES ONLY TO A REMOTE ENDPOINT THE PLAYER TYPED.
    //
    // A local runner needs no key, and attaching one anyway would send a
    // credential to whatever happens to be listening on that port. Sent only
    // when the endpoint is NOT local, which is exactly the case where the
    // player supplied a key on purpose.
    const std::string key = llm::isLocal(m_config.llmEndpoint) ? std::string() : m_config.llmApiKey;

    const std::string me = persona.countryName;
    const std::string model = m_config.llmModel;

    // What the model may look up, ANSWERED ON THIS THREAD.
    //
    // The worker cannot read the world: it runs while turns are being processed
    // and every accessor it would need is game state. So the answers to every
    // question it could ask are computed here, up front, and handed to it as a
    // small table. That costs a few string builds per letter and removes an
    // entire class of race for good.
    //
    // It does mean answering questions that may not be asked. They are cheap,
    // and the alternative -- a worker reaching back into the game and waiting
    // for the game thread to answer -- is the shape of a deadlock during turn
    // processing, which is the worst possible place for one.
    std::map<std::string, std::string> lookups;
    {
        int toolCount = 0;
        const llm::Tool* toolList = llm::tools(&toolCount);
        for (const auto& [cid, c] : m_countries.getAll()) {
            if (cid <= 0 || cid == fromCountry || cid >= REBEL_CID_MIN) continue;
            // Only countries this correspondence could plausibly mention: the
            // one being written to, and whoever either of them is fighting.
            const bool relevant = (cid == toCountry) || modAtWar(cid, fromCountry) ||
                                  modAtWar(cid, toCountry);
            if (!relevant) continue;
            for (int t = 0; t < toolCount; ++t) {
                lookups[std::string(toolList[t].name) + "\x1f" + c.name] =
                    answerAdvisorTool(fromCountry, toolList[t].name, c.name);
            }
        }
    }

    {
        std::lock_guard<std::mutex> g(g_lock);
        ++g_inFlight;
    }
    std::thread([fromCountry, toCountry, url, key, me, model, turns, lookups,
                 timeout = 45000]() mutable {
        std::string reply;

        // ── Ask, answer, ask again -- but only so many times ──
        //
        // A model that can look things up is a model that can look them up
        // forever, and a letter is not worth a minute of somebody's turn. After
        // the last round the tools are withdrawn, so the final request can only
        // be answered with prose.
        for (int round = 0; round <= llm::kMaxToolRounds; ++round) {
            const bool offerTools = (round < llm::kMaxToolRounds);
            const std::string body = offerTools
                                   ? llm::chatRequestBodyWithTools(turns, model)
                                   : llm::chatRequestBody(turns, model);
            HttpRequest req;
            req.method = "POST";
            req.url = url;
            req.body = body;
            req.bearer = key;
            req.timeoutMs = timeout;
            // A local runner is plain http on loopback, which is the one place
            // a bearer-free request over http is not a mistake.
            req.allowInsecure = true;
            const HttpResponse res = httpRequest(req);
            if (!res.ok()) break;

            const auto calls = offerTools ? llm::toolCallsFromResponse(res.body)
                                          : std::vector<llm::ToolCall>{};
            if (calls.empty()) {
                reply = llm::tidyReply(llm::replyFromResponse(res.body), me);
                break;
            }

            // The model's own turn has to go back with the answers, or the
            // conversation no longer explains why the tool results are there.
            llm::Turn asked;
            asked.role = "assistant";
            asked.content = llm::replyFromResponse(res.body);
            turns.push_back(asked);

            for (const llm::ToolCall& call : calls) {
                auto it = lookups.find(call.name + "\x1f" + call.argument);
                turns.push_back(llm::toolResultTurn(
                    call, it == lookups.end()
                              ? "There is no country by that name in this world."
                              : it->second));
            }
        }

        std::lock_guard<std::mutex> g(g_lock);
        --g_inFlight;
        // An empty reply is dropped rather than written as an empty letter: a
        // model that failed to answer should look like a country that chose not
        // to write, which is a thing countries do.
        if (!reply.empty()) g_answers.push_back(Answer{fromCountry, toCountry, reply});
    }).detach();
}

/**
 * Decide who writes back this turn, and post whatever came back.
 *
 * Called once per turn, after the post has been delivered. An advisor answers
 * when the last delivered letter in a correspondence was not its own -- which
 * is the same rule a person follows, and means advisors do not talk over each
 * other or reply to themselves forever.
 */
void Game::runAdvisors() {
    if (!llmConfigured()) return;

    // First: post anything the workers finished since last turn. Written as
    // ordinary pending letters, so they leave on the NEXT turn like everybody
    // else's -- an advisor gets no speed advantage over a person.
    std::vector<Answer> ready;
    {
        std::lock_guard<std::mutex> g(g_lock);
        ready.swap(g_answers);
    }
    for (const Answer& a : ready) {
        std::string name;
        if (const Country* c = m_countries.getCountry(a.from)) name = c->name;
        mailbox(a.from).write(a.from, a.to, a.body, m_turnNumber,
                              mail::Author::Bot, name);
    }

    // Then: ask for the next round. Bounded, because a hundred countries each
    // holding a correspondence would be a hundred simultaneous requests to one
    // local model, which is how a laptop stops responding.
    int asked = 0;
    constexpr int kMaxPerTurn = 6;
    for (int cid : m_llmCountries) {
        if (asked >= kMaxPerTurn) break;
        const mail::Box* box = mailboxIfAny(cid);
        if (!box) continue;
        for (const mail::Thread* t : box->threads()) {
            if (asked >= kMaxPerTurn) break;
            if (t->messages.empty()) continue;

            // The last thing DELIVERED. A pending letter of its own does not
            // count -- it has already written and is waiting for the post.
            const mail::Message* last = nullptr;
            for (const mail::Message& m : t->messages) {
                if (m.status == mail::Status::Delivered) last = &m;
                else if (m.fromCountry == cid) { last = nullptr; break; }
            }
            if (!last || last->fromCountry == cid) continue;

            askAdvisor(cid, t->otherCountry);
            ++asked;
        }
    }
}

/**
 * Ask the configured runner whether it is actually there.
 *
 * Sends the smallest real request the module ever makes -- one short exchange
 * with the configured model -- rather than merely opening a socket. The three
 * ways this fails are all invisible until a letter goes unanswered on turn
 * three, and each wants a different fix: nothing listening (start the runner),
 * a connection refused on the wrong port (fix the address), and a runner that
 * answers but does not know the model (fix the name). A ping would only catch
 * the first.
 */
void Game::testLlmRunner() {
    m_llmTestOk = false;
    m_llmTestResult = T("Asking...");

    std::vector<llm::Turn> turns = {
        {"system", "Answer with the single word: ready"},
        {"user", "ready?"},
    };
    const std::string payload = llm::chatRequestBody(turns, m_config.llmModel, 16);

    std::string url = m_config.llmEndpoint;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";
    const std::string key = llm::isLocal(m_config.llmEndpoint)
                          ? std::string() : m_config.llmApiKey;

    // Translated on this thread; see the note at the top of this file.
    const std::string okMsg      = T("Answered. The advisors will work.");
    const std::string noModel    = T("It answered, but not with a letter. Check the model name.");
    const std::string unreach    = T("Nothing answered there. Is the runner running?");
    const std::string refusedMsg = T("It refused the request. Check the model name and the key.");

    std::thread([this, url, payload, key, okMsg, noModel, unreach, refusedMsg]() {
        HttpRequest req;
        req.method = "POST";
        req.url = url;
        req.body = payload;
        req.bearer = key;
        req.timeoutMs = 15000;
        req.allowInsecure = true;
        const HttpResponse res = httpRequest(req);

        std::string said;
        bool good = false;
        if (!res.ok()) {
            said = (res.status >= 400) ? refusedMsg : unreach;
        } else if (llm::replyFromResponse(res.body).empty()) {
            said = noModel;
        } else {
            said = okMsg;
            good = true;
        }
        // NOT written to the Game members from here.
        //
        // A lock only protects a value if the reader takes it too, and the draw
        // does not -- so assigning m_llmTestResult on this thread would be a
        // data race on a std::string against a renderer reading it every frame,
        // which is a crash rather than a stale word. Left in a guarded box and
        // collected by the game thread in pumpLlmTest().
        std::lock_guard<std::mutex> g(g_lock);
        g_testResult = said;
        g_testOk = good;
        g_testFresh = true;
    }).detach();
}


/**
 * Collect the runner test's outcome, on the game thread.
 *
 * Called once a frame from update(). The worker leaves its answer in a guarded
 * box rather than touching the members the renderer reads.
 */
void Game::pumpLlmTest() {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_testFresh) return;
    g_testFresh = false;
    m_llmTestResult = g_testResult;
    m_llmTestOk = g_testOk;
    if (g_installDone) { g_installDone = false; m_llmInstalling = false; }
}

/**
 * Fetch the runner, off the game thread.
 *
 * A 160 MB download must not block a frame, and every step of it reports back
 * through the same guarded box the connection test uses -- the worker never
 * touches a string the renderer is reading.
 */
void Game::installLlmRunner() {
    if (m_llmInstalling) return;
    m_llmInstalling = true;
    m_llmTestOk = false;
    m_llmTestResult = T("Starting...");

    const std::string dataDir = m_dataDir;
    const std::string doneMsg = T("Installed. Start it, then press Test the runner.");
    std::thread([dataDir, doneMsg]() {
        const llm::Install result = llm::fetch(dataDir, [](const char* what) {
            // Each phase, so a long download is not a frozen screen. Left in
            // the box for the game thread; see pumpLlmTest().
            std::lock_guard<std::mutex> g(g_lock);
            g_testResult = what;
            g_testOk = false;
            g_testFresh = true;
        });

        std::lock_guard<std::mutex> g(g_lock);
        g_testResult = result.ok() ? doneMsg : result.error;
        g_testOk = result.ok();
        g_testFresh = true;
        g_installDone = true;
    }).detach();
}

// ─────────────────────────────────────────────── pulling the weights ────

namespace {
std::mutex g_pullLock;
bool g_pullRunning = false;
bool g_pullFinished = false;
bool g_pullOk = false;
std::string g_pullError;
std::string g_pullFile;
}  // namespace

/**
 * Ask the running Ollama to fetch a model.
 *
 * Much less dangerous than fetching the runner was, and the code shows it:
 * there is no checksum machinery here because Ollama verifies every layer
 * itself, and a model is data that nothing in this game ever executes.
 *
 * The pull happens on a worker; progress is read on the game thread from the
 * file curl is streaming into. See pumpLlmPull().
 */
void Game::pullLlmModel(const std::string& model) {
    if (m_llmPulling || model.empty()) return;
    m_llmPulling = true;
    m_llmPullModel = model;
    m_llmPullFraction = 0.0f;
    m_llmPullStatus = T("Starting...");

    const std::string apiBase = llm::apiRootOf(m_config.llmEndpoint);
    const std::string streamFile = m_dataDir + "/llm-pull.ndjson";
    {
        std::lock_guard<std::mutex> g(g_pullLock);
        g_pullFile = streamFile;
        g_pullRunning = true;
        g_pullFinished = false;
        g_pullError.clear();
    }
    std::thread([apiBase, model, streamFile]() {
        const bool ok = llm::pullModel(apiBase, model, streamFile);
        std::lock_guard<std::mutex> g(g_pullLock);
        g_pullRunning = false;
        g_pullFinished = true;
        g_pullOk = ok;
        if (!ok) g_pullError = "Ollama did not accept that. Is it running, and is "
                               "the model name right?";
    }).detach();
}

/**
 * Read how far the pull has got. Game thread only.
 *
 * The progress lives in the file rather than in a shared string because that is
 * what curl already writes and what the cloudflared download already does --
 * there is no pipe plumbing to get wrong on either platform.
 */
void Game::pumpLlmPull() {
    if (!m_llmPulling) return;

    std::string file;
    bool finished = false, ok = false;
    std::string error;
    {
        std::lock_guard<std::mutex> g(g_pullLock);
        file = g_pullFile;
        finished = g_pullFinished;
        ok = g_pullOk;
        error = g_pullError;
    }

    const llm::PullProgress p = llm::pullProgress(file);
    if (p.total > 0) {
        m_llmPullFraction = std::clamp((float)((double)p.completed / (double)p.total),
                                       0.0f, 1.0f);
    }
    if (!p.status.empty()) m_llmPullStatus = p.status;

    if (finished) {
        m_llmPulling = false;
        // Ollama's own error, when it gave one, beats a generic sentence: it
        // says things like "model not found" that name the actual mistake.
        const std::string why = !p.error.empty() ? p.error : error;
        m_llmTestOk = ok && p.error.empty();
        m_llmTestResult = m_llmTestOk
                        ? std::string(T("Model ready. Press Test the runner."))
                        : why;
        if (m_llmTestOk) {
            // The model that was just pulled is almost certainly the one they
            // want used, so it is selected rather than left to be retyped.
            m_config.llmModel = m_llmPullModel;
            m_config.save(m_configPath);
        }
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
}

/**
 * Fill in what a foreign ministry would plausibly know.
 *
 * All of it in words, none of it in numbers -- see llm::Situation. Without
 * these the letters were fluent and generic: the model had a name, a date and
 * one adjective, so it wrote about "the current climate" because that was all
 * it had. Told who is fighting whom and whether the war is going badly, it
 * writes about the war.
 *
 * Every field is optional. A missing one is left empty and simply does not
 * appear in the instruction, rather than becoming "unknown", which a model will
 * cheerfully write a paragraph about.
 */
void Game::describeSituation(int me, int them, llm::Situation& out) const {
    std::string theirName;
    if (const Country* c = m_countries.getCountry(them)) theirName = c->name;
    if (theirName.empty()) return;

    // ── Do we touch? ──
    //
    // A neighbour's letter reads differently from a distant power's, and this
    // is the single fact that changes the register most.
    {
        bool adjacent = false;
        for (const auto& [pid, nbrs] : m_provinceNeighbors) {
            const Province* mine = m_provinces.getProvinceById(pid);
            if (!mine || mine->countryId != me) continue;
            for (int n : nbrs) {
                const Province* np = m_provinces.getProvinceById(n);
                if (np && np->countryId == them) { adjacent = true; break; }
            }
            if (adjacent) break;
        }
        out.proximity = adjacent ? "You share a border with " + theirName
                                 : "You do not share a border with " + theirName;
    }

    // ── What is already signed ──
    if (modAllied(me, them))            out.pact = "an alliance";
    else if (modGuaranteed(me, them))   out.pact = "a guarantee";
    else if (modNonAggression(me, them)) out.pact = "a non-aggression pact";

    // ── Who else each of us is fighting ──
    //
    // Named, because "they are at war with Russia" is the kind of thing that
    // makes a letter land, and it is public knowledge in any case.
    auto warsOf = [&](int who, const char* subject) {
        std::vector<std::string> names;
        for (const auto& [cid, c] : m_countries.getAll()) {
            if (cid == who || cid <= 0 || cid >= REBEL_CID_MIN) continue;
            if (!modAtWar(who, cid)) continue;
            names.push_back(c.name);
            if (names.size() >= 4) break;
        }
        if (names.empty()) return std::string();
        std::string list = names[0];
        for (size_t i = 1; i < names.size(); ++i) {
            list += (i + 1 == names.size()) ? " and " : ", ";
            list += names[i];
        }
        return std::string(subject) + " at war with " + list;
    };
    out.theirWars = warsOf(them, (theirName + " is").c_str());
    out.ourWars   = warsOf(me, "You are");

    // ── Has it been going well? ──
    //
    // Province count against what it was, which is the crudest possible measure
    // of fortune and deliberately so: a minister knows the war is going badly,
    // not by how many hexes.
    {
        auto it = m_llmLastHoldings.find(me);
        int now = 0;
        for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
            (void)pid;
            if (pr.countryId == me) ++now;
        }
        if (it != m_llmLastHoldings.end() && it->second > 0) {
            const double change = (double)(now - it->second) / (double)it->second;
            if (change < -0.06)      out.ourFortunes = "You have been losing ground lately";
            else if (change > 0.06)  out.ourFortunes = "You have been gaining ground lately";
        }
    }

    // ── How this correspondence itself has gone ──
    if (const mail::Box* box = mailboxIfAny(me)) {
        if (const mail::Thread* t = box->thread(them)) {
            const size_t n = t->delivered().size();
            if (n == 0)      out.historyWithThem = "This is your first letter to them";
            else if (n < 4)  out.historyWithThem = "You have exchanged only a few letters";
            else if (n < 12) out.historyWithThem = "You have corresponded regularly";
            else             out.historyWithThem = "You have a long correspondence with them";
        } else {
            out.historyWithThem = "This is your first letter to them";
        }
    }
}

/**
 * Answer one thing the model asked to look up.
 *
 * THE SAME DISCIPLINE AS THE INSTRUCTION: words, never numbers, and only what a
 * foreign ministry would plausibly know. A tool is a wider door than a prompt --
 * the model chooses when to open it -- so the narrowness has to be here, in what
 * can come back through, rather than in how often it is opened.
 *
 * A country it cannot name gets a plain "no country by that name", which is
 * both true and the thing that stops a model inventing a correspondent and then
 * writing about it.
 */
std::string Game::answerAdvisorTool(int me, const std::string& tool,
                                    const std::string& argument) const {
    // Resolved by name, loosely, because the model is quoting what it read in a
    // letter rather than an id it was given.
    int them = 0;
    {
        std::string want = argument;
        std::transform(want.begin(), want.end(), want.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (const auto& [cid, c] : m_countries.getAll()) {
            if (cid <= 0 || cid >= REBEL_CID_MIN) continue;
            std::string have = c.name;
            std::transform(have.begin(), have.end(), have.begin(),
                           [](unsigned char c2) { return (char)std::tolower(c2); });
            if (have == want) { them = cid; break; }
        }
    }
    if (them == 0 || them == me) {
        return "There is no country by that name in this world.";
    }
    std::string name;
    if (const Country* c = m_countries.getCountry(them)) name = c->name;

    if (tool == "standing_with") {
        llm::Situation s;
        s.atWar = modAtWar(me, them);
        describeSituation(me, them, s);
        std::string out = s.atWar ? ("You are at war with " + name + ".")
                                  : ("You are at peace with " + name + ".");
        if (!s.pact.empty()) out += " You have " + s.pact + " with them.";
        else out += " You have no treaty with them.";
        if (!s.proximity.empty()) out += " " + s.proximity + ".";
        return out;
    }
    if (tool == "who_is_fighting") {
        llm::Situation s;
        describeSituation(me, them, s);
        return s.theirWars.empty() ? (name + " is not at war with anyone.")
                                   : (s.theirWars + ".");
    }
    if (tool == "strength_of") {
        const std::string rel = llmRelativeStrength(me, them);
        return rel.empty() ? ("You cannot judge " + name + "'s position.")
                           : (name + "'s position is " + rel + ".");
    }
    if (tool == "our_correspondence") {
        llm::Situation s;
        describeSituation(me, them, s);
        return s.historyWithThem.empty() ? "You have not written to them."
                                         : (s.historyWithThem + ".");
    }
    return "That is not something you can find out.";
}
