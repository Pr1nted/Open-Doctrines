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
#include <set>
#include <thread>

namespace {

/// One answer, on its way back to the game thread.
struct Answer {
    int from = 0;
    int to = 0;
    std::string body;
    /// How the exchange left `from` disposed toward `to`: -1, 0 or +1.
    /// Carried home with the letter rather than written from the worker,
    /// for the same reason the letter is: the worker touches no game state.
    int disposition = 0;
};

std::mutex g_lock;
std::vector<Answer> g_answers;
int g_inFlight = 0;

/// Whether the release host answered, from a one-shot probe. See probeLlmNetwork.
std::mutex g_netLock;
int  g_netState = 0;          // 0 unknown, 1 online, 2 offline
bool g_netProbing = false;

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
/**
 * "Britain, France and Russia" -- an English list, capped.
 *
 * Capped because a country bordering fifteen others produces a sentence no
 * reader finishes and no small model reads to the end of. The overflow is
 * counted rather than dropped silently, so "and four others" still tells the
 * model there is more map than it was shown.
 */
static std::string joinNames(const std::vector<std::string>& names, size_t cap) {
    if (names.empty()) return "";
    const size_t shown = std::min(names.size(), cap);
    std::string out;
    for (size_t i = 0; i < shown; ++i) {
        if (i > 0) out += (i + 1 == shown && names.size() <= cap) ? " and " : ", ";
        out += names[i];
    }
    if (names.size() > cap)
        out += " and " + std::to_string(names.size() - cap) + " others";
    return out;
}

/// A share of the army as a soldier would say it, never as a count.
static std::string llmForceShare(long long part, long long total) {
    const double f = total > 0 ? (double)part / (double)total : 0.0;
    if (f > 0.50) return "The bulk of your army";
    if (f > 0.25) return "A large part of your army";
    if (f > 0.10) return "A lesser force";
    return "Little more than a garrison";
}

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
/**
 * Keep m_llmAvailable in step with the configuration, every frame, cheaply.
 *
 * rebuildLlmCountries used to be called from ONE place -- the "use a language
 * model" checkbox. Every other way of becoming configured left it stale: a
 * model finishing its pull, a runner being started, an endpoint typed into the
 * field, an install filling the endpoint in. So a player could set the whole
 * thing up correctly, watch the pane say "Model ready", and never see the Mail
 * button, because the only code that could have made it appear had run once,
 * before any of it was true.
 *
 * The rebuild itself is a few hundred set inserts, so this compares a
 * fingerprint of the three fields that decide the answer and rebuilds only when
 * one of them actually moves.
 */
void Game::refreshLlmAvailability() {
    std::string seen = (m_config.llmEnabled ? "1" : "0");
    seen += "\x1f" + m_config.llmEndpoint;
    seen += "\x1f" + m_config.llmModel;
    // The player's own country is excluded from the correspondents, so a change
    // of seat changes the answer too.
    seen += "\x1f" + std::to_string(m_playerCountryId);
    if (seen == m_llmConfigSeen) return;
    m_llmConfigSeen = seen;
    rebuildLlmCountries();

    // ── START THE RUNNER WE INSTALLED, ONCE, WHEN THE GAME COMES BACK ──
    //
    // Otherwise every session begins with a correspondence system that is
    // configured, enabled, shows a Mail button, and answers nothing until the
    // player remembers to open the settings and press Start it. The runner is
    // ours: we installed it into the game's own folder, we stop it on quit, and
    // it binds to loopback -- so starting it is restoring the state the player
    // already chose, not taking a new liberty.
    //
    // Only when they asked for it (llmEnabled), only for a runner in OUR
    // folder, and only for a local endpoint -- somebody pointing at a remote
    // API has nothing here to start. Once per session: if they press Stop it,
    // it stays stopped.
    if (!m_llmAutoStartTried && m_config.llmEnabled &&
        llm::isLocal(m_config.llmEndpoint) && llm::installed(m_dataDir) &&
        !llmServerRunning()) {
        m_llmAutoStartTried = true;
        m_llmServerPid = llm::startServer(m_dataDir);
    }
}

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

        // The ones that name a country are answered for each country this
        // correspondence could plausibly be about. The ones that ask about our
        // own country are answered once, under an empty argument -- keying
        // those per country would compute the same sweep of our own provinces
        // once for every neighbour on the board.
        for (int t = 0; t < toolCount; ++t) {
            if (toolList[t].argName) continue;
            lookups[std::string(toolList[t].name) + "\x1f"] =
                answerAdvisorTool(fromCountry, toolList[t].name, "");
        }
        for (const auto& [cid, c] : m_countries.getAll()) {
            if (cid <= 0 || cid == fromCountry || cid >= REBEL_CID_MIN) continue;
            // Only countries this correspondence could plausibly mention: the
            // one being written to, and whoever either of them is fighting.
            const bool relevant = (cid == toCountry) || modAtWar(cid, fromCountry) ||
                                  modAtWar(cid, toCountry);
            if (!relevant) continue;
            for (int t = 0; t < toolCount; ++t) {
                // `records` tools answer nothing and are handled by the worker.
                // Without this the recording tool would be "answered" here for
                // every country on the board, each time with the fall-through
                // "that is not something you can find out" -- which is both
                // wasted work and a wrong answer sitting in the table.
                if (!toolList[t].argName || toolList[t].records) continue;
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
        int disposition = 0;

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
                // The recording tool is answered here, not from the table:
                // there is nothing to look up, and what it says has to travel
                // back with the letter.
                if (call.name == "note_disposition") {
                    std::string v = call.argument;
                    std::transform(v.begin(), v.end(), v.begin(),
                                   [](unsigned char c) { return (char)std::tolower(c); });
                    // Matched loosely: models write "warmer", "Warmer.", and
                    // "much warmer" for the same intent, and an exact-match
                    // check would silently record every one of them as no
                    // change at all -- a feature that looks wired up and is
                    // not.
                    if (v.find("warm") != std::string::npos)      disposition = +1;
                    else if (v.find("cool") != std::string::npos ||
                             v.find("cold") != std::string::npos) disposition = -1;
                    else                                          disposition = 0;
                    turns.push_back(llm::toolResultTurn(call, "Noted."));
                    continue;
                }
                auto it = lookups.find(call.name + "\x1f" + call.argument);
                // A model asked for our own army will often pass an argument
                // anyway -- its own country's name, or the correspondent's --
                // because every other tool it has takes one. Answering that
                // with "there is no such country" would be a lie about a
                // question we can answer perfectly well, so an argument to an
                // argument-free tool is simply ignored.
                if (it == lookups.end() && !call.argument.empty())
                    it = lookups.find(call.name + "\x1f");
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
        if (!reply.empty())
            g_answers.push_back(Answer{fromCountry, toCountry, reply, disposition});
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

        // ── The only place a letter reaches the rest of the game ──
        //
        // Accumulated and clamped, so one warm letter is a nudge and a
        // correspondence is a position. Three consistent letters saturate it,
        // which is the point: a player who has genuinely talked a country
        // round should get the whole of the (small) effect, and a player who
        // sends thirty should not get more than that.
        if (a.disposition != 0) {
            float& d = m_llmDisposition[((long long)a.from << 20) | (long long)a.to];
            d = llm::foldDisposition(d, a.disposition);
        }
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
    if (g_installDone) {
        g_installDone = false;
        m_llmInstalling = false;
        // Point at what was just installed. The placeholder said port 8080,
        // which is llama.cpp's; Ollama serves 11434 -- so a player who typed
        // the greyed-out hint got a runner that never answered, and nothing on
        // the screen said which of the two numbers was wrong.
        if (m_llmTestOk && m_config.llmEndpoint.empty()) {
            m_config.llmEndpoint = llm::localEndpoint();
            m_config.save(m_configPath);
        }
    }
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

    // A tool that names a country cannot be answered without one. The
    // precompute only ever passes names that resolve, but a model may call a
    // tool with a country that is not in this world, or -- commoner -- omit the
    // argument altogether. Both arrive here as `them == 0`, and without this
    // the answers below would talk about a country called "".
    {
        int toolCount = 0;
        const llm::Tool* list = llm::tools(&toolCount);
        for (int i = 0; i < toolCount; ++i) {
            if (tool != list[i].name) continue;
            if (list[i].argName && them <= 0)
                return "There is no country by that name in this world.";
            break;
        }
    }

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

    // ─────────────────────────────────────── what we know about ourselves ────
    //
    // From here down the answers may name PLACES. A minister knows his own
    // frontier, his own districts and what his own government has enacted, and
    // pretending otherwise made the letters vaguer than the fiction requires.
    // The numbers rule still holds: shares are rendered as "the bulk of" and
    // "little more than a garrison", never as a count of men.

    if (tool == "our_territory") {
        const std::vector<int>& mine = provincesOf(me);
        int held = 0, world = 0, ports = 0;
        for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
            (void)pid;
            if (pr.countryId > 0 && pr.countryId < REBEL_CID_MIN) ++world;
        }
        std::set<std::string> neighbours;
        for (int pid : mine) {
            const Province* pr = m_provinces.getProvinceById(pid);
            if (!pr || pr->countryId != me) continue;   // stale: see provincesOf
            ++held;
            if (m_provincePorts.count(pid)) ++ports;
            auto nb = m_provinceNeighbors.find(pid);
            if (nb == m_provinceNeighbors.end()) continue;
            for (int npid : nb->second) {
                const Province* np = m_provinces.getProvinceById(npid);
                if (!np || np->countryId == me || np->countryId <= 0) continue;
                if (np->countryId >= REBEL_CID_MIN) continue;
                if (const Country* nc = m_countries.getCountry(np->countryId))
                    neighbours.insert(nc->name);
            }
        }
        if (held == 0) return "Your country holds no land at all.";
        const double share = world > 0 ? (double)held / (double)world : 0.0;
        std::string out = "Yours is ";
        if (share > 0.18)      out += "one of the great powers of this world";
        else if (share > 0.08) out += "a large country";
        else if (share > 0.03) out += "a country of middling size";
        else                   out += "a small country";
        out += ". ";
        out += ports > 0 ? "You have ports on the sea. "
                         : "You have no ports on the sea. ";
        if (neighbours.empty()) {
            out += "You border no one.";
        } else {
            out += "You border ";
            out += joinNames(std::vector<std::string>(neighbours.begin(),
                                                      neighbours.end()), 8);
            out += ".";
        }
        return out;
    }

    if (tool == "our_forces") {
        // Every man is attributed to the frontier he stands on -- that is, to
        // each foreign country whose land his province touches. A province deep
        // inside the country touches none and counts as the interior.
        //
        // A province on a corner between two neighbours is counted toward BOTH,
        // which is deliberate: those men do face both, and splitting them would
        // invent a precision about intent that the map does not carry. It means
        // the shares can sum past the whole, so they are compared against the
        // total army rather than against each other.
        long long total = 0;
        std::map<std::string, long long> facing;
        long long interior = 0;
        std::set<std::string> kinds;
        for (int pid : provincesOf(me)) {
            const Province* pr = m_provinces.getProvinceById(pid);
            if (!pr || pr->countryId != me) continue;
            auto ar = m_provinceArmies.find(pid);
            if (ar == m_provinceArmies.end()) continue;
            long long here = 0;
            for (const ArmyUnit& u : ar->second) {
                if (u.countryId != me || u.count <= 0) continue;
                here += u.count;
                kinds.insert(troopCost(u.type).name);
            }
            if (here <= 0) continue;
            total += here;
            bool frontier = false;
            auto nb = m_provinceNeighbors.find(pid);
            if (nb != m_provinceNeighbors.end()) {
                std::set<std::string> touched;
                for (int npid : nb->second) {
                    const Province* np = m_provinces.getProvinceById(npid);
                    if (!np || np->countryId == me || np->countryId <= 0) continue;
                    if (np->countryId >= REBEL_CID_MIN) continue;
                    if (const Country* nc = m_countries.getCountry(np->countryId))
                        touched.insert(nc->name);
                }
                for (const std::string& n : touched) { facing[n] += here; frontier = true; }
            }
            if (!frontier) interior += here;
        }
        if (total <= 0) return "You have no army in the field.";

        std::vector<std::pair<std::string, long long>> ranked(facing.begin(), facing.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        std::string out;
        for (size_t i = 0; i < ranked.size() && i < 4; ++i) {
            out += llmForceShare(ranked[i].second, total) + " faces " + ranked[i].first + ". ";
        }
        if (interior > 0)
            out += llmForceShare(interior, total) + " is held back in the interior. ";
        if (!kinds.empty()) {
            out += "You have ";
            out += joinNames(std::vector<std::string>(kinds.begin(), kinds.end()), 4);
            out += " under arms.";
        }
        return out;
    }

    if (tool == "our_doctrines") {
        std::vector<std::string> active, coming;
        auto idx = m_countryActivePolicyIndices.find(me);
        if (idx != m_countryActivePolicyIndices.end()) {
            for (int i : idx->second) {
                if (i < 0 || i >= (int)m_activePolicies.size()) continue;
                const ActivePolicy& ap = m_activePolicies[i];
                if (ap.turnsRemaining < 0) continue;   // cancelled
                std::string label = ap.policyId;
                for (const Policy& def : m_allPolicies)
                    if (def.id == ap.policyId) { label = def.name; break; }
                (ap.turnsRemaining > 0 ? coming : active).push_back(label);
            }
        }
        if (active.empty() && coming.empty())
            return "Your government runs no doctrines at present.";
        std::string out;
        if (!active.empty()) out += "Your government runs " + joinNames(active, 8) + ". ";
        if (!coming.empty()) out += "You are still bringing in " + joinNames(coming, 6) + ".";
        return out;
    }

    if (tool == "our_districts") {
        auto it = m_districts.find(me);
        if (it == m_districts.end() || it->second.empty())
            return "Your country is governed as one piece, undivided.";
        std::string out;
        for (const District& d : it->second) {
            if (d.provinces.empty()) continue;
            double sum = 0.0;
            int counted = 0;
            for (int pid : d.provinces) {
                const Province* pr = m_provinces.getProvinceById(pid);
                if (!pr || pr->countryId != me) continue;
                sum += getProvinceRebellionChance(pid);
                ++counted;
            }
            if (counted == 0) continue;
            const double risk = sum / (double)counted;
            out += d.name + " is ";
            if (risk > 0.20)      out += "close to open revolt";
            else if (risk > 0.10) out += "restless";
            else if (risk > 0.03) out += "uneasy but holding";
            else                  out += "quiet";
            out += ". ";
        }
        return out.empty() ? "Your country is governed as one piece, undivided." : out;
    }

    if (tool == "our_claims") {
        const Country* self = m_countries.getCountry(me);
        if (!self) return "You claim nothing beyond what you hold.";
        return llmDescribeClaims(self->isoA3, me, /*mine=*/true);
    }

    if (tool == "incoming_requests") {
        const Country* self = m_countries.getCountry(me);
        if (!self) return "Nothing is waiting on your answer.";
        std::vector<std::string> lines;
        for (const PendingDiplomaticAction& da : m_pendingDiplomaticActions) {
            if (da.targetIso != self->isoA3) continue;
            const int src = cidForIso(da.sourceIso);
            const Country* sc = m_countries.getCountry(src);
            if (!sc) continue;
            const std::string who = sc->name;
            if (da.action == "request_alliance")      lines.push_back(who + " has proposed an alliance");
            else if (da.action == "request_nap")      lines.push_back(who + " has proposed a non-aggression pact");
            else if (da.action == "request_guarantee")lines.push_back(who + " has asked you to guarantee them");
            else if (da.action == "call_to_arms") {
                const Country* subj = m_countries.getCountry(cidForIso(da.subjectIso));
                lines.push_back(who + " has called on you to join their war" +
                                (subj ? " against " + subj->name : ""));
            }
            else if (da.action == "propose_trade")    lines.push_back(who + " has offered you a trade");
            else if (da.action == "break_alliance")   lines.push_back(who + " is leaving your alliance");
            else if (da.action == "break_nap")        lines.push_back(who + " is tearing up your pact");
            else if (da.action == "break_guarantee")  lines.push_back(who + " is withdrawing their guarantee");
            else if (da.action == "declare_war")      lines.push_back(who + " is declaring war on you");
        }
        if (lines.empty()) return "Nothing is waiting on your answer.";
        std::string out;
        for (const std::string& l : lines) out += l + ". ";
        return out;
    }

    if (tool == "claims_of") {
        if (them <= 0) return "There is no country by that name in this world.";
        const Country* other = m_countries.getCountry(them);
        if (!other) return "There is no country by that name in this world.";
        return llmDescribeClaims(other->isoA3, me, /*mine=*/false);
    }

    if (tool == "profile_of") {
        if (them <= 0) return "There is no country by that name in this world.";
        std::string out = name + " is ";
        auto comp = m_countryCompass.find(them);
        if (comp != m_countryCompass.end()) {
            const float e = comp->second.economic, so = comp->second.social;
            std::string econ = e < -33 ? "of the left" : (e > 33 ? "of the right" : "of the centre");
            std::string soc  = so < -33 ? "and rules with a hard hand"
                                        : (so > 33 ? "and rules loosely" : "and rules evenly");
            out += "governed " + econ + " " + soc + ". ";
        } else {
            out += "governed in a manner you cannot judge. ";
        }
        const std::string rel = llmRelativeStrength(me, them);
        if (!rel.empty()) out += "Their position is " + rel + ". ";

        llm::Situation s;
        s.atWar = modAtWar(me, them);
        describeSituation(me, them, s);
        out += s.theirWars.empty() ? "They are at war with no one. "
                                   : (s.theirWars + ". ");
        if (!s.pact.empty())        out += "You have " + s.pact + " with them. ";
        else if (s.atWar)           out += "You are at war with them. ";
        else                        out += "You have no treaty with them. ";
        if (!s.proximity.empty())   out += s.proximity + ".";
        return out;
    }

    return "That is not something you can find out.";
}

std::string Game::llmDescribeClaims(const std::string& iso, int me, bool mine) const {
    auto it = m_claims.find(iso);
    if (it == m_claims.end() || it->second.empty())
        return mine ? "You claim nothing beyond what you hold."
                    : "They claim nothing beyond what they hold.";

    // Grouped by WHO HOLDS THE LAND rather than listed province by province.
    // A claim on nine provinces of one neighbour is one grievance, and reading
    // it out as nine place names buries that under a gazetteer.
    std::map<std::string, int> byHolder;
    int onUs = 0, unheld = 0;
    for (int pid : it->second) {
        const Province* pr = m_provinces.getProvinceById(pid);
        if (!pr) continue;
        if (pr->countryId == me && !mine) { ++onUs; continue; }
        if (pr->countryId <= 0 || pr->countryId >= REBEL_CID_MIN) { ++unheld; continue; }
        const Country* holder = m_countries.getCountry(pr->countryId);
        if (!holder) { ++unheld; continue; }
        if (holder->isoA3 == iso) continue;   // already theirs; not a claim outstanding
        byHolder[holder->name] += 1;
    }
    if (byHolder.empty() && onUs == 0 && unheld == 0)
        return mine ? "You claim nothing you do not already hold."
                    : "They claim nothing they do not already hold.";

    const std::string subject = mine ? "You claim " : "They claim ";
    std::string out;
    if (onUs > 0) {
        // Said first and said plainly. A claim on our own land is the single
        // most consequential thing in this answer and must not arrive as the
        // third clause of a list.
        out += "They claim land of yours";
        out += onUs > 3 ? " -- a great deal of it. " : ". ";
    }
    std::vector<std::string> parts;
    for (const auto& [holder, count] : byHolder) {
        std::string much = count > 5 ? "a great deal of " : (count > 2 ? "several provinces of " : "land of ");
        parts.push_back(much + holder);
    }
    if (!parts.empty()) out += subject + joinNames(parts, 5) + ". ";
    if (unheld > 0)
        out += mine ? "You also claim land no country holds." : "They also claim land no country holds.";
    return out;
}

float Game::llmDispositionToward(int me, int them) const {
    // OFF IS EXACTLY ZERO, AND IT IS CHECKED HERE RATHER THAN AT THE CALL SITE.
    // Every caller of this is inside the measured AI, and a caller that forgot
    // the check would make the benched game depend on whether a player happens
    // to have installed a language model -- which would be the single worst
    // thing this module could do.
    if (!llmConfigured()) return 0.0f;
    auto it = m_llmDisposition.find(((long long)me << 20) | (long long)them);
    return it == m_llmDisposition.end() ? 0.0f : it->second;
}

bool Game::llmServerRunning() const {
    return m_llmServerPid != 0 && llm::serverAlive(m_llmServerPid);
}

void Game::startLlmServer() {
    if (llmServerRunning()) return;
    m_llmServerPid = llm::startServer(m_dataDir);
    if (m_llmServerPid == 0) {
        m_llmTestOk = false;
        m_llmTestResult = T("Could not start it.");
        return;
    }
    if (m_config.llmEndpoint.empty()) {
        m_config.llmEndpoint = llm::localEndpoint();
        m_config.save(m_configPath);
    }
    // Not "started" -- STARTING. It takes a moment to bind, and a Test pressed
    // immediately after would say nothing answered, which is the exact wrong
    // thing to tell somebody whose runner is fine.
    m_llmTestOk = true;
    m_llmTestResult = T("Starting it. Give it a moment, then press Test the runner.");
}

void Game::stopLlmServer() {
    if (m_llmServerPid == 0) return;
    llm::stopServer(m_llmServerPid);
    m_llmServerPid = 0;
    m_llmTestOk = false;
    m_llmTestResult = T("Stopped.");
}

void Game::probeLlmNetwork() {
    {
        std::lock_guard<std::mutex> g(g_netLock);
        // Once per session unless it came back offline: a player who plugs the
        // cable in and reopens the pane should get a fresh answer, and one who
        // is online does not need asking again.
        if (g_netProbing || g_netState == 1) return;
        g_netProbing = true;
    }
    std::thread([]() {
        const bool ok = llm::reachable();
        std::lock_guard<std::mutex> g(g_netLock);
        g_netState = ok ? 1 : 2;
        g_netProbing = false;
    }).detach();
}

int Game::llmNetworkState() const {
    std::lock_guard<std::mutex> g(g_netLock);
    return g_netState;
}
