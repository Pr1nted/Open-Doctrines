// Does a real model actually REACH FOR the tools that steer a country?
//
// The unit tests prove each link of the chain in isolation: the tools array is
// well formed, a tool call is parsed out of a reply, a phrase becomes a lean, a
// lean becomes a bias on a decision. What none of them can prove is the first
// link -- that a model handed this prompt ever calls `intend`, `press` or
// `prefer_doctrine` at all. A chain of six sound links that nothing ever pulls
// is exactly the failure this game keeps finding: the code is there, the
// counter reads zero.
//
// So this is not a test of our code. It is a measurement OF THE MODEL, which is
// why it is not in run_all.sh: it needs a runner, it needs weights, and it
// answers differently on different ones. A model that never calls a tool is a
// fact about that model worth knowing before shipping it as the default.
//
// Usage (the runner must already be up):
//     build/LlmToolsLiveCheck http://127.0.0.1:11434/v1 llama3.1:8b [attempts]
//
// It reports what fraction of attempts produced a steering call, and whether
// what came back survives parseLean -- a call naming something we cannot map is
// no better than no call.
#include "Advisor.h"
#include "Mail.h"
#include "net/HttpClient.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string base  = argc > 1 ? argv[1] : "http://127.0.0.1:11434/v1";
    const std::string model = argc > 2 ? argv[2] : "llama3.1:8b";
    const int attempts      = argc > 3 ? std::atoi(argv[3]) : 4;

    const bool bland = argc > 4 && std::string(argv[4]) == "bland";

    printf("Does %s reach for the steering tools? (%d attempts, %s letter)\n\n",
           model.c_str(), attempts, bland ? "BLAND" : "urgent");

    // A letter that gives the advisor something to want. Deliberately NOT an
    // instruction to call a tool: a model that only steers when told to steer
    // is a model that will never steer during a game.
    std::vector<mail::Message> thread;
    mail::Message m;
    m.fromCountry = 2; m.toCountry = 1; m.status = mail::Status::Delivered;
    // TWO LETTERS, AND THE SECOND IS THE ONE THAT MATTERS. The first deserves
    // steering: it names a threat and refuses help. The second is small talk.
    // A pass that steers on both is not reading the letter -- it is answering
    // the question it was asked, every time, and a country steered by every
    // piece of correspondence plays worse than one nobody wrote to.
    m.body = bland
        ? std::string("Our delegation enjoyed the reception in Vienna last "
                      "month. Please pass on our regards to the Archduke, and "
                      "we hope the spring finds you well.")
        : std::string("Serbia is massing on your border and your army is half "
                      "theirs. We will not help you. What will you do about it?");
    thread.push_back(m);

    llm::Persona persona;
    persona.countryName = "Austria";
    persona.correspondent = "Germany";
    persona.goal = "survive the decade without losing a province";
    const llm::Situation situation{12, "March 1914", false, "stronger than you"};
    const auto base_turns =
        llm::buildConversation(thread, 1, persona, situation, "English");

    int steered = 0, parsed = 0, anyCall = 0, failed = 0, declined = 0, gated = 0;
    for (int i = 0; i < attempts; ++i) {
        // The GAME'S loop, not one request. A model that looks something up
        // first and steers on the strength of the answer would read as "never
        // steers" if only the first round were counted -- which was the first
        // version of this check, and it was wrong in the direction that makes
        // a feature look dead.
        auto turns = base_turns;
        bool answered = false;
        std::string trace;
        std::string disposition;        // what the letter itself recorded
        for (int round = 0; round <= llm::kMaxToolRounds; ++round) {
            const bool offerTools = (round < llm::kMaxToolRounds);
            HttpRequest req;
            req.method = "POST";
            req.url = base + "/chat/completions";
            req.body = offerTools ? llm::chatRequestBodyWithTools(turns, model)
                                  : llm::chatRequestBody(turns, model);
            req.allowInsecure = true;
            req.timeoutMs = 45000;          // what the game allows a letter
            const HttpResponse res = httpRequest(req);
            if (!res.ok()) {
                trace += "  no reply (" + res.error + ")";
                break;
            }
            answered = true;
            const auto calls = offerTools ? llm::toolCallsFromResponse(res.body)
                                          : std::vector<llm::ToolCall>{};
            if (calls.empty()) break;
            ++anyCall;

            llm::Turn asked;
            asked.role = "assistant";
            asked.content = llm::replyFromResponse(res.body);
            turns.push_back(asked);

            for (const auto& c : calls) {
                trace += "  " + c.name + "(" + c.argument + ")";
                if (c.name == "note_disposition") disposition = c.argument;
                const bool steer = c.name == "intend" || c.name == "press" ||
                                   c.name == "prefer_doctrine" || c.name == "set_goal";
                if (steer) {
                    ++steered;
                    if (c.name != "intend") {
                        ++parsed;
                    } else {
                        const llm::Lean lean = llm::parseLean(c.argument);
                        if (lean.ok) ++parsed;
                        trace += lean.ok
                               ? std::string(" -> ") +
                                 (lean.reflex ? lean.reflex : "module/action")
                               : std::string(" -> UNPARSEABLE");
                    }
                }
                // Any answer keeps the conversation coherent; this check is
                // about whether it steers, not about lookup fidelity.
                turns.push_back(llm::toolResultTurn(c, "Noted."));
            }
        }
        // ── THE STEERING PASS ──
        // Asked after the letter, with only the three steering tools on the
        // table. Counted separately from the letter's own calls, because the
        // question this check exists to answer is whether a country ever
        // steers AT ALL -- and "only when asked on its own" is a different
        // answer from "never", with different consequences for the game.
        // ── THE GATE, AND WHY IT IS THIS ONE ──
        //
        // Asked on its own, this model steers whatever the letter said: 10 of
        // 10 on a note about a reception in Vienna, pressing Italy, Russia and
        // once its own correspondent. It is answering the question, not reading
        // the letter, so the model cannot be trusted to decide WHETHER.
        //
        // But it can already tell the difference, and it says so in a field
        // that was measured discriminating: disposition came back "cooler" on
        // every urgent letter and "unchanged" on every bland one. So the game
        // asks the cheap reliable question first and only spends the steering
        // pass when the letter actually moved the country.
        const bool moved = !disposition.empty() &&
                           disposition.find("unchanged") == std::string::npos;
        if (answered && !moved) {
            ++gated;
            trace += disposition.empty() ? "  [pass: skipped, no disposition]"
                                         : "  [pass: skipped, unchanged]";
        }
        if (answered && moved) {
            HttpRequest sreq;
            sreq.method = "POST";
            sreq.url = base + "/chat/completions";
            sreq.body = llm::steeringRequestBody(turns, model);
            sreq.allowInsecure = true;
            sreq.timeoutMs = 45000;
            const HttpResponse sres = httpRequest(sreq);
            if (sres.ok()) {
                const auto scalls = llm::toolCallsFromResponse(sres.body);
                if (scalls.empty()) { ++declined; trace += "  [pass: declined]"; }
                for (const auto& c : scalls) {
                    ++steered;
                    trace += "  [pass: " + c.name + "(" + c.argument + ")";
                    if (c.name == "intend") {
                        const llm::Lean lean = llm::parseLean(c.argument);
                        if (lean.ok) ++parsed;
                        trace += lean.ok
                               ? std::string(" -> ") +
                                 (lean.reflex ? lean.reflex : "module/action")
                               : std::string(" -> UNPARSEABLE");
                    } else {
                        ++parsed;
                    }
                    trace += "]";
                }
            } else {
                trace += "  [pass: no reply]";
            }
        }
        if (!answered) ++failed;
        printf("  attempt %d:%s\n", i + 1,
               trace.empty() ? "  (wrote a letter, no tools)" : trace.c_str());
    }

    printf("\n%d/%d attempts answered, %d tool round(s), %d steering call(s), "
           "%d of those usable; the pass was gated out %d/%d and declined %d\n",
           attempts - failed, attempts, anyCall, steered, parsed,
           gated, attempts - failed, declined);
    if (failed == attempts) {
        printf("\nThe runner never answered. This measures nothing.\n");
        return 1;
    }
    if (steered == 0)
        printf("\nThis model never steered its country. The four hooks in\n"
               "AISystem would read zero for it in a real game.\n");
    return 0;
}
