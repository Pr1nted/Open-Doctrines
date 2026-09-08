// Does an advisor's request actually survive a real HTTP round trip?
//
// Everything else about the module is unit-tested without a network: the
// prompt, the isolation, the parsing. This is the one thing those cannot
// cover -- that the body we build is accepted by a runner, and that what comes
// back becomes a letter. Run against a stand-in, so no model is needed.
#include "Advisor.h"
#include "Mail.h"
#include "net/HttpClient.h"

#include <cstdio>
#include <string>

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}

int main(int argc, char** argv) {
    const std::string base = argc > 1 ? argv[1] : "http://127.0.0.1:8799/v1";
    // The stub ignores the model; a real runner does not, and answers 404
    // for one it has never heard of. Nameable so this can be pointed at
    // either without pretending a missing model is a broken request.
    const std::string model = argc > 2 ? argv[2] : "test-model";
    const bool stub = (model == "test-model");
    printf("LLM round trip against %s (model %s)\n\n",
           base.c_str(), model.c_str());

    // A real correspondence, built the way the game builds it.
    std::vector<mail::Message> thread;
    mail::Message m;
    m.fromCountry = 2; m.toCountry = 1; m.status = mail::Status::Delivered;
    m.body = "Your fleet worries us. What are your intentions?";
    thread.push_back(m);

    const llm::Persona persona{"Britain", "France", "", false};
    const llm::Situation situation{12, "March 1914", false, "comparable to you"};
    const auto turns = llm::buildConversation(thread, 1, persona, situation, "Ukrainian");
    const std::string body = llm::chatRequestBody(turns, model);

    HttpRequest req;
    req.method = "POST";
    req.url = base + "/chat/completions";
    req.body = body;
    req.timeoutMs = 10000;
    req.allowInsecure = true;      // loopback http, which is where a runner lives
    const HttpResponse res = httpRequest(req);

    ok(res.ok(), "the runner accepted the request we build");
    if (!res.ok()) {
        printf("      status %d, error: %s\n", res.status, res.error.c_str());
        printf("\n%d checks, %d failed\n", checks, fails);
        return 1;
    }

    const std::string raw = llm::replyFromResponse(res.body);
    ok(!raw.empty(), "and a reply was parsed out of what it sent back");
    if (stub) {
        // The stand-in echoes back what it was sent, so the request's shape can
        // be asserted. A real model answers in prose and cannot be asked to.
        ok(raw.find("roles=system,user") != std::string::npos,
           "the conversation arrived as a system turn then the other side's letter");
        ok(raw.find("model=test-model") != std::string::npos, "with the model we named");
        ok(raw.find("saw_language=Ukrainian") != std::string::npos,
           "and the instruction really did ask for the player's language");
    } else {
        printf("\n  the model wrote:\n  ---\n%s\n  ---\n\n",
               llm::tidyReply(raw, "Britain").c_str());
        ok(raw.size() > 20, "a real model wrote something of a letter's length");
    }

    const std::string letter = llm::tidyReply(raw, "Britain");
    ok(letter.rfind("Britain:", 0) == std::string::npos,
       "the name a model prefixes is stripped from the letter");
    ok(letter.find("**") == std::string::npos, "and any markdown it added");
    if (stub) {
        ok(letter.find("We accept your terms") != std::string::npos,
           "leaving what it actually said");
    }
    ok(!letter.empty() && letter.size() <= mail::kMaxBody,
       "and what is left is a letter, within what one may hold");

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
