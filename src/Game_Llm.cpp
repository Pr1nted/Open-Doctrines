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
#include "util/Async.h"

#include <chrono>
#include <thread>

namespace {

/// One answer, on its way back to the game thread.
struct Answer {
    int from = 0;
    int to = 0;
    /// The room it belongs in, or 0 for an ordinary reply.
    int groupId = 0;
    std::string body;
    /// A goal the advisor set for itself this turn, or empty.
    std::string goal;
    /// Leans it asked for: the raw phrase, resolved on the game thread.
    std::vector<std::string> leans;
    /// A country it wants pressed, and a doctrine it wants next. Raw text,
    /// resolved to ids on the game thread -- the worker knows no country names.
    std::string press;
    std::string doctrine;
    /// How the exchange left `from` disposed toward `to`: -1, 0 or +1.
    /// Carried home with the letter rather than written from the worker,
    /// for the same reason the letter is: the worker touches no game state.
    int disposition = 0;
};

std::mutex g_lock;
std::vector<Answer> g_answers;
int g_inFlight = 0;

/// The local runner's state, refreshed on a timer by a worker. See pumpLlmServer.
std::mutex g_statusLock;
llm::Status g_status;
bool g_statusProbing = false;
bool g_statusFresh = false;

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
void Game::askAdvisor(int fromCountry, int toCountry, int groupId) {
    const mail::Box* box = mailboxIfAny(fromCountry);
    if (!box) return;
    const mail::Group* room = groupId ? mailGroup(groupId) : nullptr;
    if (groupId && (!room || !room->has(fromCountry))) return;  // removed since
    const mail::Thread* thread = room ? box->groupThreadIfAny(groupId)
                                      : box->thread(toCountry);
    if (!thread) return;

    llm::Persona persona;
    if (const Country* c = m_countries.getCountry(fromCountry)) persona.countryName = c->name;
    if (room) {
        // ── ANSWERING A ROOM ──
        //
        // The correspondent is the room, and it is described by WHO IS IN IT.
        // A model told only "you are writing to The Entente" writes to a name;
        // told who is listening, it writes to an audience -- which is the
        // difference between a group chat and a noticeboard.
        persona.correspondent = room->name;
        std::string others;
        for (int mem : room->members) {
            if (mem == fromCountry) continue;
            if (const Country* c = m_countries.getCountry(mem))
                others += (others.empty() ? "" : ", ") + c->name;
        }
        if (!others.empty())
            persona.standing = "Everyone in this room reads what you write: " + others;
        // Everybody hears it, so it is never a private word to one machine.
        persona.toAnotherAdvisor = false;
    } else {
        if (const Country* c = m_countries.getCountry(toCountry)) persona.correspondent = c->name;
        persona.toAnotherAdvisor = (toCountry != m_playerCountryId) && mailIsBot(toCountry);
    }
    {
        auto g = m_llmGoal.find(fromCountry);
        if (g != m_llmGoal.end()) persona.goal = g->second;
    }
    if (persona.countryName.empty() || persona.correspondent.empty()) return;

    // In a room there is no single "them" to be at war with, so the situation
    // is described against whoever spoke last -- the country actually being
    // answered. Nothing here is private to that pair; it is the same public
    // standing every member could work out for themselves.
    int about = toCountry;
    if (room) {
        about = 0;
        for (const mail::Message& m : thread->messages) {
            if (m.status == mail::Status::Delivered && m.fromCountry != fromCountry)
                about = m.fromCountry;
        }
        if (about == 0) return;                       // nothing to answer
    }
    llm::Situation situation;
    situation.turn = m_turnNumber;
    situation.date = m_mapDate;
    situation.atWar = modAtWar(fromCountry, about);
    situation.relativeStrength = llmRelativeStrength(fromCountry, about);
    describeSituation(fromCountry, about, situation);

    // Resolved HERE, on the game thread. See rule 3 at the top of this file.
    // The ENGLISH name of the active language, because that is what the
    // instruction is written in -- telling a model to answer in "Українська"
    // inside an English prompt is less reliable than telling it "Ukrainian".
    const std::string languageName = od::i18n::current().english;

    // Not const: the worker appends the model's tool calls and their answers to
    // it as the conversation goes round.
    // Names resolved on the game thread and captured, because the worker must
    // not touch m_countries -- rule 1 at the top of this file.
    std::map<int, std::string> names;
    for (const mail::Message& m : thread->messages) {
        if (names.count(m.fromCountry)) continue;
        if (const Country* c = m_countries.getCountry(m.fromCountry))
            names[m.fromCountry] = c->name;
    }
    auto nameOf = [names](int cid) -> std::string {
        auto it = names.find(cid);
        return it == names.end() ? std::string() : it->second;
    };
    auto turns = llm::buildConversation(thread->messages, fromCountry, persona,
                                        situation, languageName, 24, nameOf);

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
    // ── KEYS ARE NORMALISED, BECAUSE A MODEL DOES NOT TYPE A DATABASE KEY ──
    //
    // The table was keyed by the country's exact name, and anything else missed
    // and came back "There is no country by that name in this world" -- which
    // the model then NARRATED. Posted to a player as Poland's letter: "It seems
    // like there is no country called ~me~ in the game."
    //
    // So the key is lowercased and trimmed, and the pronouns a letter-writer
    // actually reaches for are registered as aliases below.
    auto norm = [](std::string v) {
        for (auto& c : v) c = (char)std::tolower((unsigned char)c);
        const size_t a = v.find_first_not_of(" \t\r\n.\"'");
        if (a == std::string::npos) return std::string();
        const size_t b = v.find_last_not_of(" \t\r\n.\"'");
        return v.substr(a, b - a + 1);
    };
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
        // ── AND WHEN IT ASKS ABOUT ITSELF BY PRONOUN ──
        //
        // "standing_with(me)" is not a question with an answer -- your standing
        // with yourself -- but the honest reply is to say so, not to claim the
        // country does not exist. The country-taking tools get a sentence that
        // points at the our_ tools instead.
        {
            std::string ownName;
            if (const Country* self = m_countries.getCountry(fromCountry))
                ownName = self->name;
            for (int t = 0; t < toolCount; ++t) {
                if (!toolList[t].argName || toolList[t].records) continue;
                const std::string said =
                    "That is your own country. Ask about yourself with the tools "
                    "whose names begin with our_.";
                for (const char* alias : {"me", "us", "myself", "ourselves",
                                          "our country", "my country", "i"})
                    lookups[std::string(toolList[t].name) + "\x1f" + alias] = said;
                if (!ownName.empty())
                    lookups[std::string(toolList[t].name) + "\x1f" + norm(ownName)] = said;
            }
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
                const std::string answer =
                    answerAdvisorTool(fromCountry, toolList[t].name, c.name);
                lookups[std::string(toolList[t].name) + "\x1f" + norm(c.name)] = answer;
                // The country being written to answers to the second person as
                // well as to its name: a letter says "you", not "the Republic
                // of Poland".
                if (cid == toCountry) {
                    for (const char* alias : {"you", "your country", "them", "they",
                                              "their country", "the recipient"})
                        lookups[std::string(toolList[t].name) + "\x1f" + alias] = answer;
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> g(g_lock);
        ++g_inFlight;
    }
    // The same normalisation the table was built with. Copied into the worker
    // rather than captured by reference: this thread outlives this function.
    auto normArg = [](std::string v) {
        for (auto& c : v) c = (char)std::tolower((unsigned char)c);
        const size_t a = v.find_first_not_of(" \t\r\n.\"'");
        if (a == std::string::npos) return std::string();
        const size_t b = v.find_last_not_of(" \t\r\n.\"'");
        return v.substr(a, b - a + 1);
    };
    odasync::run([fromCountry, toCountry, groupId, url, key, me, model, turns, lookups, normArg,
                 timeout = 45000]() mutable {
        std::string reply;
        int disposition = 0;
        std::string goal;
        std::vector<std::string> leans;
        std::string press, doctrine;

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
                std::string raw = llm::replyFromResponse(res.body);
                // ── THE BLOCK COMES OFF BEFORE ANYTHING ELSE LOOKS AT IT ──
                //
                // Split first, so the records never reach tidyReply and can
                // never be mistaken for prose. What the model put in the block
                // is what it MEANT to record; a tool call it also made is
                // merged below, and the block wins where both name the same
                // thing, because it is the channel we asked for.
                const llm::Records rec = llm::splitRecords(raw);
                if (rec.found) {
                    if (!rec.goal.empty()) goal = rec.goal;
                    if (!rec.press.empty()) press = rec.press;
                    if (!rec.doctrine.empty()) doctrine = rec.doctrine;
                    for (const std::string& l : rec.leans)
                        if (leans.size() < 4) leans.push_back(l);
                    if (rec.disposition != 0) disposition = rec.disposition;
                }
                reply = llm::tidyReply(raw, me);
                // NOTHING LEFT AFTER TIDYING means the model answered with
                // machinery rather than a letter -- "No specific function call
                // is requested to answer this prompt" was one, posted to a
                // player as Israel's reply. It happens when tools are on the
                // table and the model narrates its decision not to use one.
                //
                // So: go round again rather than give up. The last round
                // withdraws the tools entirely, which removes the thing it was
                // narrating about, and a country that still says nothing after
                // that is a country that chose not to write.
                if (!reply.empty() || !offerTools) break;
                continue;
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
                // Recorded on the worker and carried home, exactly like the
                // disposition: the worker cannot touch game state, and these
                // are the country's own words rather than a lookup.
                if (call.name == "set_goal") {
                    if (!call.argument.empty()) {
                        goal = call.argument;
                        if (goal.size() > 240) goal.resize(240);
                    }
                    turns.push_back(llm::toolResultTurn(call, "Noted."));
                    continue;
                }
                if (call.name == "intend") {
                    // A cap, because a model asked for a preference can supply
                    // twenty, and twenty leans is not a preference.
                    if (!call.argument.empty() && leans.size() < 4)
                        leans.push_back(call.argument);
                    turns.push_back(llm::toolResultTurn(call, "Noted."));
                    continue;
                }
                if (call.name == "press") {
                    press = call.argument;
                    turns.push_back(llm::toolResultTurn(call, "Noted."));
                    continue;
                }
                if (call.name == "prefer_doctrine") {
                    doctrine = call.argument;
                    turns.push_back(llm::toolResultTurn(call, "Noted."));
                    continue;
                }
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
                auto it = lookups.find(call.name + "\x1f" + normArg(call.argument));
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

        // ── THE LETTER GOES HOME FIRST ──
        //
        // The steering pass below is a second request, and it used to run
        // before this: the reply sat finished on this thread while another
        // round trip completed, and the country simply took longer to answer
        // for no benefit anybody could see. Posting first costs nothing --
        // steering is read by the AI on a later turn, not by the letter.
        {
            std::lock_guard<std::mutex> g(g_lock);
            // An empty reply is dropped rather than written as an empty letter:
            // a model that failed to answer should look like a country that
            // chose not to write, which is a thing countries do.
            if (!reply.empty()) {
                Answer a;
                a.from = fromCountry;
                a.to = toCountry;
                a.groupId = groupId;
                a.body = reply;
                a.goal = goal;
                a.leans = leans;
                a.press = press;
                a.doctrine = doctrine;
                a.disposition = disposition;
                g_answers.push_back(std::move(a));
            }
        }

        // ── THE STEERING PASS, AND WHY IT IS GATED ON THE DISPOSITION ──
        //
        // intend, press and prefer_doctrine are the only tools that reach what
        // the country actually DOES, and measured against llama3.1:8b they were
        // never called: the model makes one tool call per letter and the
        // disposition step in the prompt claims it, 9 times in 10. Zero of ten
        // letters steered, so the four hooks in AISystem that read this state
        // sat at zero in every real game.
        //
        // Asked on its own, with only these three tools, it steers happily --
        // and steers ANYTHING. On a note about a reception in Vienna it steered
        // 10 of 10, pressing Italy, Russia, and once the very country it was
        // writing to. It answers the question rather than reading the letter,
        // so it cannot be trusted to decide whether to steer at all.
        //
        // It can already tell the difference, though, and it says so in a field
        // that measured clean: disposition was "cooler" on every urgent letter
        // and "unchanged" on every bland one. So the cheap reliable question
        // gates the expensive unreliable one. With it: 6 usable steering calls
        // in 10 urgent letters, 0 in 10 bland ones.
        //
        // The cost is one short request, and only on letters that moved the
        // country -- not on every piece of correspondence in the game.
        if (!reply.empty() && disposition != 0 && leans.empty() &&
            press.empty() && doctrine.empty()) {
            HttpRequest req;
            req.method = "POST";
            req.url = url;
            req.body = llm::steeringRequestBody(turns, model);
            req.bearer = key;
            req.allowInsecure = true;
            req.timeoutMs = timeout;
            const HttpResponse res = httpRequest(req);
            if (res.ok()) {
                std::vector<std::string> more;
                std::string morePress, moreDoctrine;
                for (const llm::ToolCall& call : llm::toolCallsFromResponse(res.body)) {
                    // Only the three. A model handed a narrow question still
                    // sometimes answers a wider one, and nothing else it names
                    // here has been answered or should be acted on.
                    if (call.argument.empty()) continue;
                    if (call.name == "intend") {
                        if (more.size() < 4) more.push_back(call.argument);
                    } else if (call.name == "press") {
                        morePress = call.argument;
                    } else if (call.name == "prefer_doctrine") {
                        moreDoctrine = call.argument;
                    }
                }
                // A second answer carrying no letter. runAdvisors applies the
                // steering and writes nothing, so this cannot become a phantom
                // second message in the thread.
                if (!more.empty() || !morePress.empty() || !moreDoctrine.empty()) {
                    Answer a;
                    a.from = fromCountry;
                    a.to = toCountry;
                    a.groupId = groupId;
                    a.leans = more;
                    a.press = morePress;
                    a.doctrine = moreDoctrine;
                    std::lock_guard<std::mutex> g2(g_lock);
                    g_answers.push_back(std::move(a));
                }
            }
        }

        std::lock_guard<std::mutex> g(g_lock);
        --g_inFlight;
    });
}

/**
 * Decide who writes back this turn, and post whatever came back.
 *
 * Called once per turn, after the post has been delivered. An advisor answers
 * when the last delivered letter in a correspondence was not its own -- which
 * is the same rule a person follows, and means advisors do not talk over each
 * other or reply to themselves forever.
 */
/// How many advisor requests are in the air. For the --llm-letter diagnostic.
int Game::llmInFlight() const {
    // Queued work counts as in flight. On desktop odasync::outstanding() and
    // g_inFlight track the same threads; on web the queue holds requests that
    // have not started yet, and a wait that ignored them would give up before
    // the first one ran.
    std::lock_guard<std::mutex> g(g_lock);
    return g_inFlight > 0 ? g_inFlight : odasync::outstanding();
}

bool Game::llmSettling() const {
    return llmConfigured() && llmInFlight() > 0;
}

/**
 * Let the advisors finish before the turn resolves.
 *
 * WHY THIS HAD TO EXIST. An advisor's answer is collected by the NEXT
 * runAdvisors(), so a request still in the air when the turn resolves has its
 * letter held over to the turn after. Play at any speed and that compounds: a
 * letter written on turn 1 came back on turn 5, and from the player's side the
 * country simply was not answering. Nothing was broken -- the reply was always
 * in flight -- but "it will arrive in four turns" and "it is ignoring you" look
 * identical from the mail screen.
 *
 * BOUNDED, AND THE BOUND IS THE POINT. A local model that has stalled, a
 * runner that was killed, an endpoint typed wrong -- none of those may stop a
 * player from taking their turn. It waits for what is outstanding and then goes
 * on regardless, because a game that hangs on a language model is worse than
 * one whose letters are late.
 *
 * `onWait` is called while it waits so the caller can keep drawing; a frozen
 * window for ten seconds is its own bug.
 */
void Game::waitForAdvisors(double seconds, const std::function<void(float)>& onWait) {
    if (!llmConfigured()) return;
    const double until = GetTime() + seconds;
    while (llmInFlight() > 0 && GetTime() < until) {
        if (onWait) {
            const double left = until - GetTime();
            onWait((float)(1.0 - left / seconds));
        }
        // ── SLEEP, AND THIS IS NOT A POLITENESS ──
        //
        // The first version spun with no pause, and llmInFlight() takes the
        // same mutex the worker needs in order to record that it has finished.
        // One thread hammering a lock thousands of times a second can keep the
        // other from ever acquiring it, so the count never fell, the wait never
        // ended, and the turn hung -- observed as a run stuck on "advisor
        // asked" for eighteen minutes against a ten-second bound.
        //
        // 20ms is far below anything a person notices and leaves the lock free
        // essentially all the time. The answers themselves are collected by the
        // caller; this only waits for the workers to put them down.
        // ON WEB THIS IS WHAT RUNS THE WORK. There are no threads: the request
        // sits on a queue until something pumps it, and the main loop is not
        // running while a turn resolves. A wait that did not pump here would be
        // a wait for something that cannot happen, every time.
        if (odasync::pump()) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

/**
 * Post whatever the workers have finished. Safe to call more than once a turn.
 *
 * Split out of runAdvisors so the turn can collect a SECOND time, after waiting
 * for the advisors it just asked. See waitForAdvisors and the note in
 * processTurn about replying at the same speed as a person.
 */
void Game::collectAdvisorAnswers() {
    if (!llmConfigured()) return;

    // Written as ordinary pending letters, so they leave on the next turn like
    // everybody else's -- an advisor gets no speed advantage over a person.
    std::vector<Answer> ready;
    {
        std::lock_guard<std::mutex> g(g_lock);
        ready.swap(g_answers);
    }
    for (const Answer& a : ready) {
        std::string name;
        if (const Country* c = m_countries.getCountry(a.from)) name = c->name;
        // A steering-only answer carries no letter. See the steering pass in
        // askAdvisor: it comes home separately so it cannot delay the reply,
        // and writing an empty body here would post a blank message.
        if (a.body.empty()) {
            if (!a.goal.empty()) m_llmGoal[a.from] = a.goal;
            for (const std::string& lean : a.leans) applyLlmLean(a.from, lean);
            if (!a.press.empty())    applyLlmPress(a.from, a.press);
            if (!a.doctrine.empty()) applyLlmDoctrine(a.from, a.doctrine);
            continue;
        }
        if (a.groupId != 0) {
            const mail::Group* g = mailGroup(a.groupId);
            if (!g || !g->has(a.from)) continue;   // removed while it was thinking
            mailbox(a.from).writeToGroup(a.from, a.groupId, a.body, m_turnNumber,
                                         mail::Author::Bot, name);
        } else {
            mailbox(a.from).write(a.from, a.to, a.body, m_turnNumber,
                                  mail::Author::Bot, name);
        }

        // ── The only place a letter reaches the rest of the game ──
        //
        // Accumulated and clamped, so one warm letter is a nudge and a
        // correspondence is a position. Three consistent letters saturate it,
        // which is the point: a player who has genuinely talked a country
        // round should get the whole of the (small) effect, and a player who
        // sends thirty should not get more than that.
        // A goal it set for itself. Kept until IT changes it -- the game never
        // writes one and never clears one.
        if (!a.goal.empty()) m_llmGoal[a.from] = a.goal;
        for (const std::string& lean : a.leans) applyLlmLean(a.from, lean);
        if (!a.press.empty())    applyLlmPress(a.from, a.press);
        if (!a.doctrine.empty()) applyLlmDoctrine(a.from, a.doctrine);

        // Only for a two-party correspondence. A room has several counterparts
        // and one number cannot say which of them the advisor warmed to --
        // filing it against a room id would mean nothing, and filing it against
        // whoever spoke last would let a third party earn somebody else's
        // goodwill by speaking into the same room.
        if (a.groupId == 0 && a.disposition != 0) {
            float& d = m_llmDisposition[((long long)a.from << 20) | (long long)a.to];
            d = llm::foldDisposition(d, a.disposition);
        }
    }

}

void Game::runAdvisors() {
    if (!llmConfigured()) return;
    collectAdvisorAnswers();

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

            // ── ADVISORS DO NOT TALK AMONG THEMSELVES FOR EVER ──
            //
            // Every advisor answers whatever it did not write itself, so in a
            // room with three of them each turn produces three letters, each
            // of which is something the other two must answer next turn. Left
            // alone that is a conversation that never ends and that no person
            // is in -- and the player who opened the room to ask one question
            // comes back to forty letters.
            //
            // So machines may answer each other TWICE after a person speaks,
            // which is enough for a real exchange -- a proposal, a counter,
            // a reply -- and then the room waits for somebody to say something.
            // A person writing again resets it, because the count is of what
            // has happened since the last human letter.
            int botsSinceHuman = 0;
            for (auto it = t->messages.rbegin(); it != t->messages.rend(); ++it) {
                if (it->status != mail::Status::Delivered) continue;
                if (it->author != mail::Author::Bot) break;
                ++botsSinceHuman;
            }
            if (botsSinceHuman >= 2) continue;

            if (t->groupId != 0) askAdvisor(cid, 0, t->groupId);
            else                 askAdvisor(cid, t->otherCountry);
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

    odasync::run([this, url, payload, key, okMsg, noModel, unreach, refusedMsg]() {
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
    });
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
#ifdef __EMSCRIPTEN__
    // A tab cannot download a binary, unpack it and run it, and should not
    // pretend to be about to. Said plainly, and once: the settings screen also
    // hides the button, and this is the guard for every other way in.
    m_llmTestOk = false;
    m_llmTestResult = T("The browser version cannot install a runner. Run one on "
                        "your computer and give its address here.");
    return;
#endif
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
#ifdef __EMSCRIPTEN__
    // Pulling is Ollama's own job and the game asks for it through curl, which
    // is a program. The runner it is talking to can be asked directly instead.
    {
        std::lock_guard<std::mutex> g(g_pullLock);
        g_pullRunning = false;
        g_pullFinished = true;
        g_pullError = T("Pull the model on the computer running it, then reload.");
    }
    return;
#endif
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
                // `me`, not the player. This runs for whichever country asked
                // the advisor, and the one-argument overload answers for
                // m_playerCountryId -- so every seat but one was told about
                // its own districts measured against somebody else's compass.
                sum += getProvinceRebellionChance(pid, me);
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

void Game::startLlmServer() {
    // Something already answering is a runner, whoever started it. Spawning
    // another only produces a process that cannot bind the port and exits --
    // which is what made this button look broken.
    if (llmServerRunning()) {
        m_llmTestOk = true;
        m_llmTestResult = T("It is already running.");
        return;
    }
    m_llmNextStartAt = GetTime() + 15.0;   // do not let the timer race this
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
    if (m_llmServerPid == 0) {
        // "Running" now means the endpoint answers, whoever started it -- so
        // this button appears for a runner an earlier session left up, or one
        // the player runs themselves. We only know the pid of one we spawned.
        //
        // SAYING SO RATHER THAN KILLING IT. A process this game did not start
        // is not this game's to end: it may be serving something else entirely,
        // and there is no way from here to tell a stale game runner from the
        // player's own. Doing nothing silently was the alternative, and that is
        // the same fault as the Start button that appeared to do nothing.
        if (m_llmAlive) {
            m_llmTestOk = false;
            m_llmTestResult = T("This game did not start that runner, so it will not "
                                "stop it. Close it where it was started.");
        }
        return;
    }
    llm::stopServer(m_llmServerPid);
    m_llmServerPid = 0;
    m_llmAlive = false;
    m_llmProbeAt = 0.0;                 // re-probe at once rather than in 3s
    m_llmNextStartAt = GetTime() + 15.0;  // and do not immediately restart it
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
    odasync::run([]() {
        const bool ok = llm::reachable();
        std::lock_guard<std::mutex> g(g_netLock);
        g_netState = ok ? 1 : 2;
        g_netProbing = false;
    });
}

int Game::llmNetworkState() const {
    std::lock_guard<std::mutex> g(g_netLock);
    return g_netState;
}

// ─────────────────────────────────────────── keeping the runner running ────

bool Game::llmServerRunning() const { return m_llmAlive; }

bool Game::llmModelPresent() const {
    if (m_config.llmModel.empty()) return false;
    for (const std::string& m : m_llmModels) {
        if (m == m_config.llmModel) return true;
        // Ollama reports "llama3.1:8b"; a player may have typed "llama3.1",
        // which ollama itself resolves to the :latest tag. Treat the bare name
        // as matching so the screen does not say "not pulled" about a model
        // that will answer perfectly well.
        const size_t colon = m.find(':');
        if (colon != std::string::npos && m.compare(0, colon, m_config.llmModel) == 0)
            return true;
    }
    return false;
}

/**
 * Ask the runner what it is doing, and start it if it is not doing anything.
 *
 * WHY THIS IS A TIMER AND NOT A ONE-SHOT. The first version started the runner
 * once per session and decided "running" from the pid it had spawned. Both
 * halves were wrong: a runner started by an earlier session answers perfectly
 * well and read as stopped, and a runner that died mid-game stayed dead until
 * the player noticed. The runner should be up whenever the game is up, so this
 * checks, and restarts when it is not.
 *
 * The backoff matters. Without it, a runner that cannot start -- a port held by
 * something else, a broken install -- would be respawned every time this ran,
 * which is a process every few seconds for as long as the game is open.
 */
void Game::pumpLlmServer() {
    {
        std::lock_guard<std::mutex> g(g_statusLock);
        if (g_statusFresh) {
            g_statusFresh = false;
            m_llmAlive = g_status.alive;
            m_llmModels = g_status.models;
        }
    }

    const double now = GetTime();
    if (!m_config.llmEnabled || m_config.llmEndpoint.empty()) { m_llmAlive = false; return; }

    if (now >= m_llmProbeAt) {
        m_llmProbeAt = now + 3.0;
        bool go = false;
        {
            std::lock_guard<std::mutex> g(g_statusLock);
            if (!g_statusProbing) { g_statusProbing = true; go = true; }
        }
        if (go) {
            const std::string endpoint = m_config.llmEndpoint;
            odasync::run([endpoint]() {
                const llm::Status st = llm::probeStatus(endpoint);
                std::lock_guard<std::mutex> g(g_statusLock);
                g_status = st;
                g_statusFresh = true;
                g_statusProbing = false;
            });
        }
    }

    // Only a runner in OUR folder, on a LOCAL endpoint, and only when the
    // player asked for advisors. A remote API has nothing here to start, and a
    // runner somebody else is managing is not ours to respawn.
#ifdef __EMSCRIPTEN__
    // And never in a browser, which has no processes to start. The probe above
    // still runs, so the page can tell whether something is answering.
    return;
#endif
    if (m_llmAlive || !llm::isLocal(m_config.llmEndpoint)) return;
    if (!llm::installed(m_dataDir) || now < m_llmNextStartAt) return;
    m_llmNextStartAt = now + 15.0;
    m_llmServerPid = llm::startServer(m_dataDir);
}

// ────────────────────────────────────────────────── what a lean resolves to ────

/**
 * The action vocabulary an advisor may lean on, in the words it would use.
 *
 * These are the actions the POLICY SAMPLES, which is a smaller set than "what
 * the AI does": garrisoning, fortifying, disbanding and campaigning run as
 * reflexes that never consult the net, so no lean reaches them. Said plainly
 * here because the gap is invisible from the outside -- an advisor can ask for
 * fewer forts all game and nothing will happen.
 *
 * Several words map to one action on purpose. A model asked for a preference
 * writes "war", "fighting", "attacks" and "aggression" for the same thing, and
 * a table that only accepts the internal name accepts almost nothing.
 */
void Game::applyLlmLean(int cid, const std::string& phrase) {
    const llm::Lean lean = llm::parseLean(phrase);
    if (!lean.ok) return;
    if (lean.reflex) {
        float& r = m_llmReflexLean[cid][lean.reflex];
        r = std::clamp(r + 0.34f * lean.direction, -1.0f, 1.0f);
        return;
    }
    const long long key = ((long long)cid << 20) |
                          ((long long)lean.module << 8) | (long long)lean.action;
    float& v = m_llmIntent[key];
    // Accumulated and clamped, exactly like the disposition: asking twice is
    // emphasis, asking twenty times is not twenty times the emphasis.
    v = std::clamp(v + 0.34f * lean.direction, -1.0f, 1.0f);
}

float Game::llmIntentFor(int cid, int module, int action) const {
    if (!llmConfigured()) return 0.0f;
    const long long key = ((long long)cid << 20) |
                          ((long long)module << 8) | (long long)action;
    auto it = m_llmIntent.find(key);
    return it == m_llmIntent.end() ? 0.0f : it->second;
}

bool Game::llmSuppressesReflex(int cid, const char* reflex) const {
    if (!llmConfigured() || !reflex) return false;
    auto byCountry = m_llmReflexLean.find(cid);
    if (byCountry == m_llmReflexLean.end()) return false;

    auto it = byCountry->second.find(reflex);
    if (it == byCountry->second.end() || it->second > kLlmSuppressAt) return false;

    // ── THE CAP, COUNTED OVER THE WHOLE COUNTRY ──
    //
    // Without it an advisor could be talked out of garrisoning, fortifying,
    // campaigning and disbanding one at a time, each individually reasonable,
    // and the country would stop defending itself by increments with no single
    // decision anybody could point at. Ablation says those four are worth
    // roughly 13.5, 6.4, 12.0 and a negative respectively -- so the whole set
    // is most of a country's play.
    //
    // ── WHICH TWO, WHEN MORE THAN TWO ARE ASKED FOR ──
    //
    // The first version sorted by NAME, which is stable and fails badly:
    // alphabetically the first two are austerity and campaign, so an advisor
    // that asks for everything would deterministically get the campaign
    // suppression -- 12 points of world by ablation, the second most expensive
    // thing on the list. Stability was the only property I had reasoned about.
    //
    // Ordered by measured cost instead, cheapest first. Just as stable, and it
    // fails safe: when the cap binds it binds on what costs least, and the
    // expensive reflexes are the ones that survive being argued with.
    //
    // The figures are single-rule ablations against a hold-out control and are
    // PROVISIONAL -- combinations in this game have measured superadditive
    // (two research rules worth +10.1 and +13.9 alone came to +36.0 together),
    // so a pair of these may cost more than the sum. The order is what is being
    // measured now; the cap is what makes being wrong about it survivable.
    static const std::pair<const char*, float> kReflexCost[] = {
        {"austerity",    2.5f}, {"manpower", 2.5f}, {"redeploy",  5.6f},
        {"pacification", 6.0f}, {"withdraw", 6.0f}, {"peace",     6.0f},
        {"siege",        6.0f}, {"fortify",  6.4f}, {"campaign", 12.0f},
        {"garrison",    13.5f},
    };
    auto costOf = [](const std::string& n) {
        for (const auto& [name, c] : kReflexCost) if (n == name) return c;
        return 99.0f;                       // unknown: treat as expensive, keep it
    };

    std::vector<const std::string*> asked;
    for (const auto& [name, value] : byCountry->second)
        if (value <= kLlmSuppressAt) asked.push_back(&name);
    std::sort(asked.begin(), asked.end(),
              [&](const std::string* a, const std::string* b) {
                  const float ca = costOf(*a), cb = costOf(*b);
                  if (ca != cb) return ca < cb;
                  return *a < *b;           // ties by name, so the set is stable
              });

    for (int i = 0; i < (int)asked.size() && i < kLlmMaxSuppressed; ++i)
        if (*asked[i] == reflex) return true;
    return false;
}

void Game::applyLlmPress(int cid, const std::string& name) {
    std::string want;
    for (char c : name) want += (char)std::tolower((unsigned char)c);
    // An explicit stand-down, because "press nobody" has to be sayable or the
    // only way to stop is to name somebody else.
    for (const char* off : {"nobody", "no one", "none", "stop"})
        if (want.find(off) != std::string::npos) { m_llmPress.erase(cid); return; }

    for (const auto& [other, c] : m_countries.getAll()) {
        if (other <= 0 || other == cid || other >= REBEL_CID_MIN) continue;
        std::string have;
        for (char ch : c.name) have += (char)std::tolower((unsigned char)ch);
        if (have != want) continue;
        m_llmPress[cid] = other;
        return;
    }
    // A name that resolves to nothing changes nothing. Deliberately silent:
    // the advisor is not owed an error, and the alternative -- pressing
    // somebody it did not name -- is worse than pressing nobody.
}

void Game::applyLlmDoctrine(int cid, const std::string& name) {
    std::string want;
    for (char c : name) want += (char)std::tolower((unsigned char)c);
    for (const Policy& p : m_allPolicies) {
        std::string have;
        for (char ch : p.name) have += (char)std::tolower((unsigned char)ch);
        if (have.find(want) == std::string::npos && want.find(have) == std::string::npos)
            continue;
        m_llmDoctrine[cid] = p.id;
        return;
    }
}

int Game::llmPressTarget(int cid) const {
    if (!llmConfigured()) return 0;
    auto it = m_llmPress.find(cid);
    return it == m_llmPress.end() ? 0 : it->second;
}

std::string Game::llmPreferredDoctrine(int cid) const {
    if (!llmConfigured()) return std::string();
    auto it = m_llmDoctrine.find(cid);
    return it == m_llmDoctrine.end() ? std::string() : it->second;
}
