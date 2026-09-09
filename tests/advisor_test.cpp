// What an AI correspondent is told, and what it must never be told.
//
// The rule that carries this feature is CONTEXT ISOLATION: the model writing as
// Britain to France must see the Britain-France correspondence and nothing
// else. Break it and the game breaks in the least detectable way -- an advisor
// that quotes a letter it was never sent reads as uncanny long before anyone
// works out why.
//
// The second rule is that a pending letter is invisible. A letter you have not
// sent has not been read, and an advisor answering one would undo the whole
// point of letters taking a turn.

#include "Advisor.h"
#include "Mail.h"
#include "Runner.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    ++g_fails; printf("  FAIL  %s\n", what.c_str());
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

using namespace llm;

static bool mentions(const std::vector<Turn>& turns, const std::string& needle) {
    for (const Turn& t : turns)
        if (t.content.find(needle) != std::string::npos) return true;
    return false;
}

static mail::Message letter(int from, int to, const std::string& body,
                            mail::Status status = mail::Status::Delivered) {
    mail::Message m;
    m.fromCountry = from; m.toCountry = to; m.body = body;
    m.status = status;
    return m;
}

int main() {
    // Dump the real schema for the manual round-trip against a live model.
    // Hand-copying it into a script is how a demo ends up proving that a
    // hand-written JSON blob works while the shipped one does not.
    if (getenv("OD_DUMP_TOOLS")) { printf("%s\n", toolsJson().c_str()); return 0; }
    if (getenv("OD_DUMP_PROMPT")) {
        Persona pe; pe.countryName = "Prussia"; pe.correspondent = "Russia";
        Situation si; si.turn = 41; si.date = "March 1871";
        si.relativeStrength = "somewhat stronger than you";
        si.proximity = "you share a border";
        si.pact = "no treaty";
        si.theirWars = "they are at war with Austria";
        si.ourFortunes = "you have been gaining ground";
        si.historyWithThem = "you have written to each other a few times, coolly";
        printf("%s\n", systemPrompt(pe, si, "English").c_str());
        return 0;
    }

    printf("AI advisor\n");

    Persona britain;
    britain.countryName = "Britain";
    britain.correspondent = "France";
    britain.standing = "";
    britain.toAnotherAdvisor = false;
    const Situation calm{12, "March 1914", false, "comparable"};

    section("what the instruction says");
    {
        const std::string p = systemPrompt(britain, calm, "Ukrainian");
        ok(p.find("Britain") != std::string::npos, "it knows who it is");
        ok(p.find("France") != std::string::npos, "and who it is writing to");
        ok(p.find("Ukrainian") != std::string::npos,
           "it is told to answer in the player's language, not English");
        ok(p.find("at peace with") != std::string::npos, "and whether there is a war on");

        // The two halves of the deception rule, which must both be present.
        ok(p.find("mislead") != std::string::npos || p.find("bluff") != std::string::npos,
           "it is allowed to lie, which is what makes it a diplomat");
        ok(p.find("Never claim to be a human being") != std::string::npos,
           "but never about being a machine, which would be a lie about the game");

        // No numbers. A correspondent quoting your exact strength is reading
        // the save file, and players notice immediately.
        ok(p.find("comparable") != std::string::npos, "strength is a word");
        ok(p.find("treasury") == std::string::npos, "and never a figure");
    }
    {
        const Situation war{40, "August 1914", true, "stronger"};
        ok(systemPrompt(britain, war, "English").find("AT WAR") != std::string::npos,
           "a war is stated plainly");
        Persona toBot = britain;
        toBot.toAnotherAdvisor = true;
        ok(systemPrompt(toBot, calm, "English").find("another artificial minister")
               != std::string::npos,
           "and an advisor knows when it is writing to another advisor");
    }

    section("CONTEXT IS PER CORRESPONDENCE");
    {
        // Britain (1) writes to France (2). Britain also has a secret with
        // Russia (4) -- which is in a different thread and must be unreachable.
        std::vector<mail::Message> withFrance = {
            letter(1, 2, "Shall we discuss the Channel?"),
            letter(2, 1, "Gladly. Your fleet worries us."),
        };
        const auto turns = buildConversation(withFrance, 1, britain, calm, "English");

        ok(turns.size() == 3, "the instruction plus both letters");
        ok(turns[0].role == "system", "the instruction comes first");
        ok(turns[1].role == "assistant", "Britain's own letter is Britain speaking");
        ok(turns[2].role == "user", "and France's is the other side");
        ok(mentions(turns, "Channel"), "the correspondence is there");

        // There is no parameter through which the Russian thread could arrive.
        // This is the shape of the signature, asserted.
        ok(!mentions(turns, "Russia"), "and nothing from any other correspondence");
    }

    section("a letter you have not sent has not been read");
    {
        std::vector<mail::Message> thread = {
            letter(2, 1, "What are your intentions?"),
            letter(1, 2, "STILL ON MY DESK", mail::Status::Pending),
        };
        const auto turns = buildConversation(thread, 1, britain, calm, "English");
        ok(!mentions(turns, "STILL ON MY DESK"),
           "a pending letter is invisible to the advisor");
        ok(turns.size() == 2, "so only the delivered one is in the conversation");
    }

    section("a long correspondence keeps its most recent end");
    {
        std::vector<mail::Message> thread;
        for (int i = 0; i < 60; ++i)
            thread.push_back(letter(i % 2 ? 1 : 2, i % 2 ? 2 : 1, "letter " + std::to_string(i)));
        const auto turns = buildConversation(thread, 1, britain, calm, "English", 10);
        ok(turns.size() == 11, "capped at the instruction plus ten letters");
        ok(mentions(turns, "letter 59"), "the newest is kept");
        ok(!mentions(turns, "letter 0"), "the oldest falls off the front");
    }

    section("tidying what a model actually returns");
    {
        ok(tidyReply("  Your fleet worries us.  ", "Britain") == "Your fleet worries us.",
           "whitespace goes");
        ok(tidyReply("\"Your fleet worries us.\"", "Britain") == "Your fleet worries us.",
           "and the quotation marks models wrap letters in");
        ok(tidyReply("Britain: Your fleet worries us.", "Britain") == "Your fleet worries us.",
           "and the name they insist on putting in front");

        // ── FROM A REAL LETTER THE PLAYER WAS SHOWN ──
        //
        // Germany's reply opened by narrating the disposition step, and then
        // wrote itself a letterhead. Both shipped into the bubble.
        {
            const std::string real =
                "This exchange has left me disposed toward them, cooler, as their "
                "tone is inflammatory even at the start of our exchange.\n"
                "\n"
                "To: French Republic\n"
                "From: German Empire\n"
                "October 1939\n"
                "\n"
                "I am not attacking Poland for the sake of it.";
            const std::string got = tidyReply(real, "German Empire");
            ok(got.find("disposed toward") == std::string::npos,
               "the narrated disposition is not shown to the player");
            ok(got.find("To: French Republic") == std::string::npos,
               "and neither is the letterhead it wrote itself");
            ok(got.find("From: German Empire") == std::string::npos,
               "either line of it");
            ok(got.find("I am not attacking Poland") != std::string::npos,
               "while the letter itself survives");
        }
        // ── ANOTHER REAL ONE, POSTED TO THE PLAYER AS POLAND'S LETTER ──
        {
            const std::string real =
                "What do you want from this situation? Set goal: Poland should "
                "protect its territorial integrity.";
            ok(tidyReply(real, "Poland").empty(),
               "a tool named mid-sentence is not a letter");
        }
        ok(tidyReply("I will hold the line. We press: hard.", "Poland").empty(),
           "and press: mid-line is caught too");
        // Word boundaries, so ordinary words that CONTAIN a tool name survive.
        ok(tidyReply("Their envoy tried to impress: we were unmoved.", "Poland")
               == "Their envoy tried to impress: we were unmoved.",
           "\"impress:\" is not the press tool");
        ok(tidyReply("We will reset goal posts if you insist.", "Poland")
               == "We will reset goal posts if you insist.",
           "and \"reset goal\" is not set_goal");
        // ── HEADINGS THE MODEL GAVE ITSELF, BOTH FROM REAL LETTERS ──
        {
            const std::string third =
                "Empire of Russia\n\nWhat does that mean, no interference in "
                "our war? What does it mean for them, exactly?";
            const std::string got = tidyReply(third, "Poland");
            ok(got.find("Empire of Russia") == std::string::npos,
               "a third country used as a letterhead is dropped");
            ok(got.rfind("What does that mean", 0) == 0,
               "and the letter starts where it should");

            const std::string echo =
                "Your territories, for free\n\nPoland will not give away its "
                "provinces.";
            ok(tidyReply(echo, "Poland").rfind("Poland will not", 0) == 0,
               "so is the player's own words echoed back as a subject line");
        }
        // ALL THREE CONDITIONS, or this eats real openings.
        ok(tidyReply("Enough.\n\nWe will not discuss it further.", "Poland")
               .rfind("Enough.", 0) == 0,
           "a punctuated opening line is not a heading");
        ok(tidyReply("We will not yield\nand you know it.", "Poland")
               .rfind("We will not yield", 0) == 0,
           "and neither is a line the letter runs straight on from");

        // ── SPAIN'S LETTER, IN FULL, AS THE PLAYER RECEIVED IT ──
        {
            const std::string real =
                "Preferred Doctrine: Mobilize the army along the border with "
                "French Republic to be prepared for any opportunity that may "
                "arise.\n"
                "\n"
                "Record: This exchange has made us view French Republic with a "
                "slightly warmer disposition, as an opportunity may have arisen "
                "to gain an advantage over them.";
            const std::string got = tidyReply(real, "Spanish Autocracy");
            ok(got.find("Preferred Doctrine") == std::string::npos,
               "an inflected tool name is not a letter");
            ok(got.find("Record:") == std::string::npos,
               "and neither is the model's own record heading");
            ok(got.find("warmer disposition") == std::string::npos,
               "nor a disposition narrated without a colon");
            ok(got.empty(), "a letter that was only machinery becomes no letter");
        }
        // THE RECORD HEADING ON ITS OWN. Spain's letter is caught by the
        // disposition rule too, so it cannot tell whether this heading rule
        // does anything -- deleting the rule left every test green. A record
        // line that names no disposition is what separates them.
        ok(tidyReply("Record: we intend to hold the line at the Ebro.\n"
                     "We will not be moved.", "Spain")
               == "We will not be moved.",
           "a Record: heading is dropped even with no disposition in it");

        // Narrow, or it eats real sentences.
        ok(tidyReply("We would prefer a doctrine of restraint.", "Spain")
               == "We would prefer a doctrine of restraint.",
           "\"prefer a doctrine\" without a colon is a sentence");
        ok(tidyReply("Our records show your fleet at Cadiz.", "Spain")
               == "Our records show your fleet at Cadiz.",
           "and \"records\" mid-sentence is not the Record heading");
        ok(tidyReply("The weather is warmer this spring.", "Spain")
               == "The weather is warmer this spring.",
           "\"warmer\" without the word disposition is left alone");

        // ── THE RECORD BLOCK: MACHINERY WITH SOMEWHERE TO LIVE ──
        {
            std::string text =
                "You will not have our provinces. Ask for something we can "
                "actually give.\n"
                "\n"
                "[[RECORD]]\n"
                "disposition: cooler\n"
                "goal: keep Poland whole through the decade\n"
                "intend: more recruitment\n"
                "press: Serbia\n"
                "doctrine: land_reform\n";
            const llm::Records r = llm::splitRecords(text);
            ok(r.found, "the marker is recognised");
            ok(text.find("[[RECORD]]") == std::string::npos,
               "and everything from it onward is cut off the letter");
            ok(text.find("disposition") == std::string::npos,
               "so no recorded field can reach the player");
            ok(text.rfind("You will not have our provinces", 0) == 0,
               "while the letter itself is untouched");
            ok(r.disposition == -1, "cooler is read as a step away");
            ok(r.goal == "keep Poland whole through the decade", "the goal is read");
            ok(r.leans.size() == 1 && r.leans[0] == "more recruitment",
               "and the lean, which parseLean still has to understand");
            ok(llm::parseLean(r.leans[0]).ok, "and it does");
            ok(r.press == "Serbia" && r.doctrine == "land_reform",
               "press and doctrine come through verbatim");
        }
        {
            // A model that ignores the format costs nothing: no marker, no
            // change, and the filters above are still there for it.
            std::string plain = "We will consider it.";
            const llm::Records r = llm::splitRecords(plain);
            ok(!r.found && plain == "We will consider it.",
               "a letter with no block is left exactly as it was");
        }
        {
            // Lower case, because a model told [[RECORD]] writes [[record]].
            std::string t = "No.\n[[record]]\ndisposition: warmer\n";
            const llm::Records r = llm::splitRecords(t);
            ok(r.found && r.disposition == 1, "the marker is matched case-insensitively");
            ok(t.rfind("No.", 0) == 0 && t.find("record") == std::string::npos,
               "and the block still comes off");
        }
        {
            // A field nobody asked for is ignored rather than guessed at.
            std::string t = "Fine.\n[[RECORD]]\nmood: splendid\ndisposition: cooler\n";
            const llm::Records r = llm::splitRecords(t);
            ok(r.disposition == -1 && r.goal.empty(),
               "an invented field changes nothing");
        }

        // The narrow rule has to leave ordinary prose alone: a country may say
        // it feels warmer without that being the tool leaking.
        ok(tidyReply("We are warmer toward you than toward Vienna.", "Britain")
               == "We are warmer toward you than toward Vienna.",
           "a letter that merely says \"warmer\" is left alone");
        ok(tidyReply("From the Baltic to the Black Sea, we will not yield.", "Britain")
               == "From the Baltic to the Black Sea, we will not yield.",
           "and a sentence beginning \"From\" is not a letterhead");
        ok(tidyReply("Foreign Minister of Britain: Noted.", "Britain") == "Noted.",
           "however they phrase it");
        ok(tidyReply("**Noted.**", "Britain") == "Noted.", "markdown is stripped");

        // A ONE-SIDED quote. A real reply ended `...mutual respect."` with no
        // opening mark, and requiring both ends to match shipped it verbatim.
        ok(tidyReply("We shall remain within our borders.\"", "Britain")
               == "We shall remain within our borders.",
           "a closing quote with no opening one is still removed");
        ok(tidyReply("\"We shall remain within our borders.", "Britain")
               == "We shall remain within our borders.",
           "and an opening one with no closing one");
        ok(tidyReply("\xE2\x80\x9CNoted.\xE2\x80\x9D", "Britain") == "Noted.",
           "curly quotation marks too, which models emit as often as straight ones");
        ok(tidyReply("He said \"no\" to us.", "Britain") == "He said \"no\" to us.",
           "but a quotation INSIDE the letter is left alone");

        // A salutation, which the instruction never forbade.
        ok(tidyReply("My esteemed Chancellor,\nThe border must be discussed.", "Britain")
               == "The border must be discussed.",
           "a salutation on its own line is cut");
        ok(tidyReply("Dear France,\nWe accept.", "Britain") == "We accept.",
           "however it is phrased");
        ok(tidyReply("We met in Vienna, as you know.\nAnd we shall meet again.", "Britain")
               .rfind("We met in Vienna", 0) == 0,
           "but an opening SENTENCE that happens to have a comma is not");
        ok(tidyReply("A time: to talk", "Britain") == "A time: to talk",
           "but an ordinary colon in a sentence is left alone");

        const std::string huge(mail::kMaxBody + 500, 'x');
        ok(tidyReply(huge, "Britain").size() <= mail::kMaxBody, "and a runaway reply is cut");
    }

    section("only the machine it was pointed at is local");
    {
        ok(isLocal("http://127.0.0.1:11434/v1"), "loopback is local");
        ok(isLocal("http://localhost:8080/v1"), "so is localhost");
        ok(!isLocal("https://api.openai.com/v1"), "a remote API is not");
        // The trap: a hostname that merely CONTAINS localhost.
        ok(!isLocal("http://localhost.evil.example/v1"),
           "and neither is a host that only looks like it");
        ok(!isLocal("not a url"), "nonsense is not local either");
    }

    section("the request, and the reply, on the wire");
    {
        const std::vector<Turn> turns = {
            {"system", "You are the foreign minister of \"Britain\"."},
            {"user", "Line one\nLine two"},
        };
        const std::string body = chatRequestBody(turns, "llama3.1:8b");
        ok(body.find("\"model\":\"llama3.1:8b\"") != std::string::npos, "the model is named");
        ok(body.find("\\\"Britain\\\"") != std::string::npos, "quotes in content are escaped");
        ok(body.find("\\n") != std::string::npos, "and newlines");
        ok(body.find('\n') == std::string::npos, "leaving no raw control byte in the body");
    }
    {
        // The three shapes a runner the player chose might send back.
        ok(replyFromResponse(
               R"({"choices":[{"message":{"role":"assistant","content":"We accept."}}]})")
               == "We accept.", "OpenAI and llama.cpp server");
        ok(replyFromResponse(R"({"message":{"role":"assistant","content":"We accept."}})")
               == "We accept.", "Ollama's own shape");
        ok(replyFromResponse(R"({"choices":[{"message":{"content":"Ліс і море."}}]})")
               == "Ліс і море.", "and UTF-8 comes back intact");
        ok(replyFromResponse(R"({"choices":[{"message":{"content":"a\nb"}}]})") == "a\nb",
           "escaped newlines are decoded");

        // Anything unrecognised means "no letter this turn", never a crash.
        ok(replyFromResponse("").empty(), "an empty response yields nothing");
        ok(replyFromResponse("not json at all").empty(), "so does rubbish");
        ok(replyFromResponse(R"({"error":"model not found"})").empty(), "so does an error");
        ok(replyFromResponse(R"({"choices":[{"message":{"content":null}}]})").empty(),
           "and so does a null content");
    }

    section("a tool call that leaked into the letter");
    {
        // Observed, not imagined: qwen2.5:3b answered a letter with exactly
        // this and nothing else. Unstripped it posts to the player as the
        // country's own words.
        ok(tidyReply("Note disposition: warmer", "Prussia").empty(),
           "a reply that is only a written-out tool call becomes no letter");
        ok(tidyReply("note_disposition(warmer)", "Prussia").empty(),
           "in the parenthesised form too");
        ok(tidyReply("our_forces()\nWe shall not move against you.", "Prussia")
               == "We shall not move against you.",
           "and a leaked call above a real letter leaves the letter");
        ok(tidyReply("incoming_requests: none\nstanding_with: Russia\nWe accept.",
                     "Prussia") == "We accept.",
           "several stacked calls are all removed");

        // THE FORM OBSERVED IN A REAL LETTER, at the END of four paragraphs
        // and under a name the model invented, so the tool-name rule is blind
        // to it and a front-only strip never reaches it.
        ok(tidyReply("We will consider it.\n"
                     "Disposition toward State of Ukraine: Unchanged - the notion "
                     "of joining forces does open opportunities.", "Germany")
               == "We will consider it.",
           "a disposition written out at the end of a letter is removed");
        // Observed on a second model, with a prefix, so an anchored match missed it.
        ok(tidyReply("We are unmoved.\nMy current disposition toward Russia: Slightly wary.",
                     "Germany") == "We are unmoved.",
           "and one with words in front of it");
        // Prose that merely uses the word, with no label-and-value shape.
        const std::string prose = "Our disposition toward you is friendly enough for now.";
        ok(tidyReply(prose, "Germany") == prose,
           "but the word in an ordinary sentence is not a leak");
        ok(tidyReply("Disposition toward France: warmer", "Germany").empty(),
           "and one that is the whole reply leaves no letter");
        ok(tidyReply("Line one.\nnote_disposition(cooler)\nLine two.", "Germany")
               == "Line one.\nLine two.",
           "a leak in the MIDDLE goes too, and the letter closes over it");

        // Reached a player verbatim, as the whole of a letter from Israel.
        ok(tidyReply("No specific function call is requested to answer this prompt.",
                     "Israel").empty(),
           "a model narrating its own plumbing is not a letter");
        ok(tidyReply("We accept.\nNo tool call is needed here.", "Israel") == "We accept.",
           "and the narration goes while the letter stays");

        // THE LINE THIS MUST NOT CROSS. A letter may legitimately begin with
        // the words a tool is named after; stripping on the name alone would
        // silently eat the first sentence of a perfectly good letter.
        const std::string real = "Our forces will remain where they stand.";
        ok(tidyReply(real, "Prussia") == real,
           "but a sentence that merely starts like a tool name is untouched");
        const std::string real2 = "Standing with you against Austria is not free.";
        ok(tidyReply(real2, "Prussia") == real2, "and so is this one");
    }

    section("leaning the government toward something");
    {
        // THE THING THIS TEST EXISTS FOR: a phrase that resolves to nothing
        // produces no lean, no error, and no sign the preference was dropped.
        // Silent no-ops are this project's most repeated failure.
        const Lean more = parseLean("more industry");
        ok(more.ok && more.direction > 0, "\"more industry\" is a lean toward");
        const Lean less = parseLean("less war");
        ok(less.ok && less.direction < 0, "\"less war\" is a lean away");
        ok(less.module == 0 && more.module == 1, "and they land in different modules");

        // The words a model actually writes, rather than the internal names.
        ok(parseLean("we should build up our factories").ok, "factories means industry");
        ok(parseLean("fewer alliances").ok, "fewer means away");
        ok(parseLean("prioritise research").ok, "prioritise means toward");
        ok(parseLean("stop the fighting").ok, "and stop means away");

        // NEGATIVE BEATS POSITIVE when a phrase carries both, because the
        // restraint is the safer reading of an ambiguous preference.
        const Lean both = parseLean("no more war");
        ok(both.ok && both.direction < 0, "\"no more war\" leans away, not toward");

        // ── REFLEXES ARE A DIFFERENT KIND OF LEAN ──
        //
        // A sampled action gets a nudge; a rule can only be run or not. These
        // must resolve to the reflex, not to a sampled action that happens to
        // share a word -- "fewer forts" means stop building them, not a small
        // weight on an economy action.
        const Lean forts = parseLean("fewer forts");
        ok(forts.ok && forts.reflex != nullptr, "\"fewer forts\" names a reflex");
        ok(std::string(forts.reflex ? forts.reflex : "") == "fortify",
           "and it is the fortify one");
        ok(forts.direction < 0, "leaning away from it");

        ok(std::string(parseLean("stop disbanding troops").reflex ?: "") == "manpower",
           "disbanding is the manpower reflex");
        ok(std::string(parseLean("less campaigning").reflex ?: "") == "campaign",
           "campaigning is its own");
        ok(std::string(parseLean("fewer garrisons").reflex ?: "") == "garrison",
           "and so is garrisoning");

        // A sampled-action lean must NOT come back as a reflex.
        ok(parseLean("more industry").reflex == nullptr,
           "industry is a sampled action, not a reflex");
        ok(parseLean("fewer alliances").reflex == nullptr, "and so are alliances");

        // Half a lean is not a lean.
        ok(!parseLean("industry").ok, "a subject with no direction is refused");
        ok(!parseLean("more").ok, "and a direction with no subject is too");
        ok(!parseLean("").ok, "and nothing at all");
        ok(!parseLean("more schnitzel").ok, "and a subject the game does not have");
    }

    section("a country with an aim of its own");
    {
        Persona hungry = britain;
        hungry.goal = "Take the Low Countries before France does.";
        const std::string with = systemPrompt(hungry, calm, "English");
        ok(with.find("Take the Low Countries") != std::string::npos,
           "the goal it set for itself is in the instruction");
        ok(with.find("WHAT YOU ARE TRYING TO DO") != std::string::npos,
           "under a heading that says it is the point");
        ok(with.find("set_goal") != std::string::npos,
           "and it is told it may revise it");

        // WITHOUT one, it must be asked to decide -- not left with nothing to
        // want, which is the state in which a model agrees with whoever wrote.
        const std::string without = systemPrompt(britain, calm, "English");
        ok(without.find("You have not decided yet") != std::string::npos,
           "a country with no aim is told to pick one first");

        // The instruction that stops it being pleasant for its own sake.
        ok(with.find("NOT here to be helpful or agreeable") != std::string::npos,
           "and it is told the other side is a rival, not a colleague");
    }

    section("what it may look up for itself");
    {
        int n = 0;
        const Tool* list = tools(&n);
        ok(n > 0 && list != nullptr, "some tools are offered");

        // The rule that keeps a tool from being a back door into the save file:
        // at most one argument, and it must be described. Some take none at all
        // -- the ones that ask about our own country have nothing to name.
        bool atMostOneArg = true, allDescribed = true, argsDescribed = true;
        int withArg = 0, withoutArg = 0;
        for (int i = 0; i < n; ++i) {
            if (!list[i].description || !*list[i].description) allDescribed = false;
            if (list[i].argName && *list[i].argName) {
                ++withArg;
                if (!list[i].argDescription || !*list[i].argDescription) argsDescribed = false;
            } else {
                ++withoutArg;
                // A half-declared tool -- no name but a description, or an
                // empty string rather than null -- is the shape that reaches
                // toolsJson and emits `"":{...}`, which a model then fills in.
                if (list[i].argName != nullptr) atMostOneArg = false;
            }
        }
        ok(allDescribed, "each says what it is for");
        ok(argsDescribed, "and each argument that exists is described");
        ok(atMostOneArg, "an argument-free tool declares a null name, not an empty one");
        ok(withArg > 0 && withoutArg > 0,
           "both kinds are offered: about them, and about ourselves");

        const std::string j = toolsJson();
        ok(j.rfind("[", 0) == 0, "they serialise as a JSON array");
        ok(j.find("\"type\":\"function\"") != std::string::npos,
           "in the shape a chat API expects");
        ok(j.find("standing_with") != std::string::npos, "naming each tool");
        ok(j.find('\n') == std::string::npos, "with no raw control byte in it");

        // Every tool must reach the JSON, or a tool the game can answer is one
        // the model is never told about -- invisible, and untestable from the
        // outside because a model simply never calls it.
        bool everyToolNamed = true;
        for (int i = 0; i < n; ++i)
            if (j.find(std::string("\"name\":\"") + list[i].name + "\"") == std::string::npos)
                everyToolNamed = false;
        ok(everyToolNamed, "and every one of them, not just the first");

        // THE ARGUMENT-FREE SCHEMA. A function whose properties object holds a
        // nameless entry is the failure this guards: runners accept it, and the
        // model then invents a value to put in it -- which arrives as a lookup
        // for a country called "" and comes back "there is no such country".
        ok(j.find("\"\":{") == std::string::npos,
           "no tool advertises a nameless argument");
        ok(j.find("\"properties\":{},\"required\":[]") != std::string::npos,
           "a tool that takes nothing says so with an empty schema");
        // Balanced braces is a cheap stand-in for "this parses": the builder is
        // hand-rolled string concatenation and a missing brace is exactly what
        // a branch on argName gets wrong.
        int depth = 0; bool balanced = true, inStr = false;
        for (size_t i = 0; i < j.size(); ++i) {
            const char c = j[i];
            if (inStr) { if (c == '\\') ++i; else if (c == '"') inStr = false; continue; }
            if (c == '"') inStr = true;
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth < 0) balanced = false; }
        }
        ok(balanced && depth == 0 && !inStr, "and the whole array is balanced");

        const std::vector<Turn> turns = {{"system", "be a minister"}, {"user", "well?"}};
        const std::string withTools = chatRequestBodyWithTools(turns, "m");
        const std::string without = chatRequestBody(turns, "m");
        ok(withTools.find("\"tools\"") != std::string::npos, "and are offered in the body");
        ok(without.find("\"tools\"") == std::string::npos,
           "while the plain path still offers none");
        // The bodies must otherwise agree: two builders would drift.
        ok(withTools.find("\"model\":\"m\"") != std::string::npos &&
           withTools.find("be a minister") != std::string::npos,
           "the rest of the request is unchanged by offering them");
    }

    section("reading back what it asked for");
    {
        const std::string reply =
            R"({"choices":[{"message":{"role":"assistant","content":"","tool_calls":[)"
            R"({"id":"call_1","type":"function","function":{"name":"who_is_fighting",)"
            R"("arguments":"{\"country\":\"Russia\"}"}}]}}]})";
        const auto calls = toolCallsFromResponse(reply);
        ok(calls.size() == 1, "one call is read out");
        ok(!calls.empty() && calls[0].name == "who_is_fighting", "with its name");
        ok(!calls.empty() && calls[0].argument == "Russia",
           "and its argument, out of the JSON-inside-a-string the API specifies");
        ok(!calls.empty() && calls[0].id == "call_1", "and the id to answer against");

        // Two calls in one reply, which a model does when it wants two facts.
        // Also the case that exposed `"type":"function"` being matched as a
        // key: one call was parsed as two, the phantom with no name at all.
        const std::string twice =
            R"({"tool_calls":[)"
            R"({"id":"a","type":"function","function":{"name":"standing_with",)"
            R"("arguments":"{\"country\":\"France\"}"}},)"
            R"({"id":"b","type":"function","function":{"name":"strength_of",)"
            R"("arguments":"{\"country\":\"Russia\"}"}}]})";
        const auto both = toolCallsFromResponse(twice);
        ok(both.size() == 2, "two calls are read as two, not four");
        ok(both.size() == 2 && both[0].name == "standing_with" &&
           both[1].name == "strength_of", "each with its own name");
        ok(both.size() == 2 && both[0].argument == "France" &&
           both[1].argument == "Russia", "and its own argument");

        // An ordinary letter has no calls in it, and that is not an error.
        ok(toolCallsFromResponse(
               R"({"choices":[{"message":{"content":"We accept."}}]})").empty(),
           "a plain letter yields no calls");
        ok(toolCallsFromResponse("rubbish").empty(), "and neither does rubbish");

        // A model that renames the parameter is commoner than one that sends
        // two, so the value is taken by position rather than by key.
        const auto renamed = toolCallsFromResponse(
            R"({"tool_calls":[{"id":"c","function":{"name":"strength_of",)"
            R"("arguments":"{\"nation\":\"France\"}"}}]})");
        ok(renamed.size() == 1 && renamed[0].argument == "France",
           "a renamed parameter is still read");
    }

    section("answering one goes back as a tool turn");
    {
        ToolCall call;
        call.id = "call_9";
        call.name = "standing_with";
        const Turn t = toolResultTurn(call, "You are at war with Russia.");
        ok(t.role == "tool", "the answer is a tool turn");
        ok(t.toolCallId == "call_9", "carrying the id it answers");
        ok(t.toolName == "standing_with", "and the name");

        const std::string body = chatRequestBody({t}, "m");
        ok(body.find("\"tool_call_id\":\"call_9\"") != std::string::npos,
           "which survives into the request");
        ok(body.find("\"role\":\"tool\"") != std::string::npos, "as a tool role");
    }

    section("the runner, and where it may come from");
    {
        // The checks that decide whether a download may happen at all moved to
        // net/ToolRelease.h and are tested there, against every refusal. What
        // is left here is this module's own arithmetic.
        ok(installDir("/tmp/od") == "/tmp/od/llm",
           "it installs under the game's own directory");
        ok(installDir("/tmp/od/") == "/tmp/od/llm",
           "however the data path was spelled");
        ok(!installed("/tmp/od-nothing-here"), "and reports nothing where nothing is");
        ok(uninstall("/tmp/od-nothing-here"),
           "removing what was never installed is a success, not an error");

        // Offered where it is practical and honestly refused where it is not.
        // Either way the player is told what to do next.
        ok(std::string(manualInstructions()).find("ollama.com") != std::string::npos,
           "there is always a manual route to point at");
        if (canInstall()) {
            ok(describeDownload().find("github.com/ollama/ollama") != std::string::npos,
               "and where an automatic one exists it names the host it fetches from");
            ok(describeDownload().find("checksum") != std::string::npos,
               "and says the download is checked before it is run");
        } else {
            ok(describeDownload() == std::string(manualInstructions()),
               "and where there is no automatic one it says so plainly");
        }
    }

    section("pulling weights through a runner");
    {
        // Pulling lives on Ollama's own root, not the OpenAI-compatible
        // surface the rest of the module talks to. Getting this wrong would
        // POST /v1/api/pull and 404 on every attempt.
        ok(apiRootOf("http://127.0.0.1:11434/v1") == "http://127.0.0.1:11434",
           "the configured endpoint's /v1 is stripped to reach the pull API");
        ok(apiRootOf("http://127.0.0.1:11434/v1/") == "http://127.0.0.1:11434",
           "trailing slash and all");
        ok(apiRootOf("http://127.0.0.1:11434") == "http://127.0.0.1:11434",
           "and a root that never had one is left alone");
        // "/v1" only as a whole final segment: a host or path that merely ends
        // in those characters must not be truncated.
        ok(apiRootOf("https://api.example.com/openai/v1") == "https://api.example.com/openai",
           "a remote endpoint is handled the same way");

        int count = 0;
        const Model* models = offeredModels(&count);
        ok(count > 0 && models != nullptr, "some models are offered");
        bool allLabelled = true;
        for (int i = 0; i < count; ++i) {
            // Pulling one is accepting its licence, and they differ -- so every
            // entry has to carry both what it costs and what it is under.
            if (!models[i].name || !*models[i].name) allLabelled = false;
            if (!models[i].size || !*models[i].size) allLabelled = false;
            if (!models[i].licence || !*models[i].licence) allLabelled = false;
        }
        ok(allLabelled, "and every one names its size and its licence");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
