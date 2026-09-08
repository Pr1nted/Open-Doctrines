// Who may write to whom, and when a letter stops being yours.
//
// Two properties carry this feature and both are easy to break by accident:
//
//   1. A pending letter can be changed; a delivered one cannot, ever. That is
//      the entire reason mail is turn-delivered rather than instant, and a
//      regression here silently removes the one thing it offers over a chat box.
//
//   2. A recipient's own setting beats the host's. A host can narrow who may
//      speak on their server; nothing a host permits may force a letter on
//      somebody who has shut their door. Get the precedence backwards and the
//      "nobody may write to me" switch becomes advisory.

#include "Mail.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    ++g_fails; printf("  FAIL  %s\n", what.c_str());
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

using namespace mail;

static Rules server()      { return Rules{Policy::Everyone, true,  true }; }
static Rules soloWithBots(){ return Rules{Policy::BotsOnly, true,  false}; }
// PlayersOnly with the module PRESENT. The distinction matters: "the host
// forbids letters to advisors" and "there are no advisors here" are different
// refusals, and a fixture that sets llmAvailable=false cannot tell them apart.
static Rules playersOnly() { return Rules{Policy::PlayersOnly, true, true }; }
static Rules playersOnlyNoModule() { return Rules{Policy::PlayersOnly, false, true}; }

int main() {
    printf("Mail rules\n");

    section("the button exists only when someone could answer");
    {
        ok(!available(Rules{Policy::Nobody, true, true}),
           "off means off, however many people are connected");
        ok(!available(Rules{Policy::PlayersOnly, false, false}),
           "players-only in a single-player game offers nobody to write to");
        ok(available(Rules{Policy::PlayersOnly, false, true}),
           "players-only on a server is fine");
        ok(!available(Rules{Policy::BotsOnly, false, false}),
           "advisors-only without the module offers nobody either");
        ok(available(Rules{Policy::BotsOnly, true, false}),
           "advisors-only with the module works in single player");
        ok(available(Rules{Policy::Everyone, false, true}),
           "everyone, on a server with no module, still has people");
        ok(available(Rules{Policy::Everyone, true, false}),
           "everyone, in single player with a module, still has advisors");
    }

    section("the recipient has the last word");
    {
        // The precedence that matters. Everything is permitted by the host here.
        ok(mayWrite(server(), false, Lock::Nobody) == Refusal::RecipientClosed,
           "a closed door stays closed on a permissive server");
        ok(mayWrite(server(), false, Lock::BotsOnly) == Refusal::RecipientClosed,
           "a player taking advisors only does not have to hear from people");
        ok(mayWrite(server(), true, Lock::BotsOnly) == Refusal::None,
           "...but the advisors they asked for still reach them");
        ok(mayWrite(server(), true, Lock::Nobody) == Refusal::RecipientClosed,
           "nobody means nobody, machines included");
        ok(mayWrite(server(), false, Lock::Open) == Refusal::None,
           "an open door on a permissive server is open");
    }

    section("what the host allows");
    {
        ok(mayWrite(Rules{Policy::Nobody, true, true}, false, Lock::Open) == Refusal::MailOff,
           "mail off refuses everyone");
        ok(mayWrite(playersOnly(), true, Lock::Open) == Refusal::NotToMachines,
           "players-only refuses a letter to an advisor, module or no module");
        ok(mayWrite(playersOnlyNoModule(), true, Lock::Open) == Refusal::MailOff,
           "and with no module at all there is no advisor to refuse");
        ok(mayWrite(playersOnly(), false, Lock::Open) == Refusal::None,
           "...and allows one to a player");
        ok(mayWrite(soloWithBots(), false, Lock::Open) == Refusal::MailOff,
           "advisors-only in single player has no players to write to");
        ok(mayWrite(soloWithBots(), true, Lock::Open) == Refusal::None,
           "...and the advisor is reachable");
        ok(mayWrite(Rules{Policy::Everyone, false, true}, true, Lock::Open) == Refusal::MailOff,
           "everyone without the module still cannot reach an advisor");
    }

    section("a letter is yours until the turn resolves");
    {
        Box box;
        const int id = box.write(1, 2, "Shall we discuss the border?", 10);
        ok(id != 0, "a letter is written");
        ok(box.find(id)->status == Status::Pending, "and it sits on the desk");
        ok(box.find(id)->deliverTurn == 11, "addressed to arrive next turn");

        ok(box.edit(id, "On reflection: the border, and the river."),
           "it can be rewritten while it waits");
        ok(box.find(id)->body.find("river") != std::string::npos, "the rewrite took");

        const int torn = box.write(1, 3, "Never mind.", 10);
        ok(box.discard(torn), "and another can be torn up");
        ok(box.find(torn) == nullptr, "leaving nothing behind");

        ok(box.deliver(11) == 1, "the turn resolves and the one letter goes");
        ok(box.find(id)->status == Status::Delivered, "it is delivered");

        // The property the whole design exists for.
        ok(!box.edit(id, "Actually, war."), "a delivered letter cannot be rewritten");
        ok(!box.discard(id), "nor recalled");
        ok(box.find(id)->body.find("river") != std::string::npos,
           "and it still says exactly what was sent");
    }

    section("delivery is stamped with when it happened");
    {
        // Not with when it was promised. A save loaded weeks later, or a turn
        // that never ran, would otherwise carry a delivery date that never was.
        Box box;
        const int id = box.write(1, 2, "Terms enclosed.", 5);
        ok(box.find(id)->deliverTurn == 6, "promised for the next turn");
        box.deliver(40);
        ok(box.find(id)->deliverTurn == 40, "stamped with the turn it actually landed on");
        ok(box.arrivedOn(40).size() == 1, "and it shows up as arriving then");
        ok(box.arrivedOn(6).empty(), "not on the turn it was promised");
    }

    section("threads are separate, which is what keeps advisors honest");
    {
        // A model answering as Britain must never see the Russian thread. The
        // separation starts here, in the shape of the data.
        Box box;
        box.write(1, 2, "A secret for Britain alone.", 3);
        box.write(1, 4, "Something else entirely, for Russia.", 3);
        box.deliver(4);

        ok(box.thread(2) != nullptr && box.thread(2)->messages.size() == 1,
           "each correspondent has their own thread");
        ok(box.thread(4) != nullptr && box.thread(4)->messages.size() == 1, "both of them");
        ok(box.thread(2)->messages[0].body.find("Russia") == std::string::npos,
           "and neither thread contains the other's letters");
        ok(box.thread(99) == nullptr, "a country never written to has no thread");
        ok(box.threads().size() == 2, "the list holds exactly the two");
    }

    section("delivery puts the letter in the other side's box");
    {
        // Both halves of the exchange live in each party's own box, so "what
        // Britain can see" is a fact about Britain's box rather than a filter
        // somebody has to remember to apply.
        Box france, britain;
        const int id = france.write(1, 2, "Shall we discuss the border?", 10);
        france.deliver(11);
        for (const Message* m : france.arrivedOn(11)) britain.receive(*m);

        ok(britain.thread(1) != nullptr, "Britain now has a thread with France");
        ok(britain.thread(1)->messages.size() == 1, "holding the one letter");
        ok(britain.thread(1)->messages[0].id == id,
           "with the same id on both sides, so it can be named later");
        ok(britain.thread(1)->messages[0].status == Status::Delivered,
           "and arriving delivered, never pending");
        ok(!britain.edit(id, "tampered"),
           "the recipient cannot rewrite what they were sent");
        ok(britain.thread(2) == nullptr, "and no thread with itself");
    }

    section("a save round-trip does not duplicate anything");
    {
        // The exact shape of the doctrine bug: a container the load fills and
        // then appends to. Only the sender's copy is written to the save; the
        // recipient's is rebuilt, so the two can never disagree and a reload
        // cannot double a conversation.
        Box senderBefore;
        const int a = senderBefore.write(1, 2, "Delivered already.", 3);
        senderBefore.deliver(4);
        const int b = senderBefore.write(1, 2, "Still on the desk.", 4);

        // What the save writes: the sender's copies, whatever their status.
        std::vector<Message> saved;
        for (const Thread* t : senderBefore.threads())
            for (const Message& m : t->messages)
                if (m.fromCountry == 1) saved.push_back(m);
        ok(saved.size() == 2, "both letters are written to the save");

        // What the load does.
        Box sender, recipient;
        for (const Message& m : saved) {
            sender.adopt(m);
            if (m.status == Status::Delivered) recipient.receive(m);
        }
        ok(sender.thread(2)->messages.size() == 2, "the sender has exactly two, not four");
        ok(recipient.thread(1) != nullptr && recipient.thread(1)->messages.size() == 1,
           "the recipient has the delivered one only");
        ok(sender.find(b)->status == Status::Pending,
           "and the unsent letter comes back unsent, still yours to change");
        ok(sender.edit(b, "Changed my mind after reloading."),
           "which means it can still be rewritten");
        ok(!sender.edit(a, "tampering with history"),
           "while the delivered one is still beyond reach");

        // Ids must not collide with the ones restored.
        const int fresh = sender.write(1, 3, "A new letter after loading.", 5);
        ok(fresh != a && fresh != b, "a new letter gets an id that is not already taken");
    }

    section("the most restrictive answer always wins");
    {
        // The precedence chain, end to end. Three parties can each narrow who
        // may speak -- the host, the recipient, and (through the age prompt)
        // the recipient's own declared age -- and none of them may widen what
        // another has narrowed. Any one saying no is a no.
        const Rules permissive = server();

        ok(mayWrite(permissive, false, Lock::Open) == Refusal::None,
           "nobody objecting means the letter goes");
        ok(mayWrite(Rules{Policy::Nobody, true, true}, false, Lock::Open) != Refusal::None,
           "the host alone can stop it");
        ok(mayWrite(permissive, false, Lock::Nobody) != Refusal::None,
           "the recipient alone can stop it");
        ok(mayWrite(Rules{Policy::Nobody, true, true}, false, Lock::Nobody) != Refusal::None,
           "and both objecting is still a no, not a double negative");

        // The one that would be a real bug: a permissive host must never be
        // able to reopen a door the recipient shut.
        ok(mayWrite(Rules{Policy::Everyone, true, true}, true, Lock::Nobody)
               == Refusal::RecipientClosed,
           "the most permissive host setting cannot reopen a closed door");
    }

    section("what a letter may contain");
    {
        const std::vector<std::string> none;
        const std::vector<std::string> banned = {"Frobnicate"};
        const Rules r = server();

        ok(check("", r, false, Lock::Open, none) == Refusal::Empty, "nothing is not a letter");
        ok(check("   \n\t ", r, false, Lock::Open, none) == Refusal::Empty,
           "and neither is whitespace");
        ok(check(std::string(kMaxBody + 1, 'x'), r, false, Lock::Open, none) == Refusal::TooLong,
           "there is an upper bound");
        ok(check(std::string(kMaxBody, 'x'), r, false, Lock::Open, none) == Refusal::None,
           "and the bound itself is allowed");

        ok(blacklisted("we shall FROBNICATE at dawn", banned),
           "a forbidden word is caught whatever its case");
        ok(!blacklisted("we shall attack at dawn", banned), "and an innocent letter is not");
        ok(!blacklisted("anything at all", none), "an empty list forbids nothing");
        ok(check("Frobnicate", r, false, Lock::Open, banned) == Refusal::Blacklisted,
           "and the check reports it");

        // Order matters for the sentence the writer is shown: being told off
        // about a word in a letter that was never going anywhere is a worse
        // answer than being told the door is shut.
        ok(check("Frobnicate", r, false, Lock::Nobody, banned) == Refusal::RecipientClosed,
           "who comes before what");
    }

    section("odds and ends that would be bugs");
    {
        Box box;
        ok(box.write(3, 3, "Dear me,", 1) == 0, "nobody writes to themselves");
        ok(box.edit(999, "x") == false, "editing a letter that does not exist fails");
        ok(box.discard(999) == false, "so does discarding one");
        ok(box.deliver(2) == 0, "delivering an empty box sends nothing");

        box.write(1, 2, "one", 1);
        box.write(1, 2, "two", 1);
        ok(box.thread(2)->pendingCount() == 2, "two letters waiting");
        box.deliver(2);
        ok(box.thread(2)->pendingCount() == 0, "and none after the turn");
        ok(box.thread(2)->delivered().size() == 2, "both are now history");

        ok(std::string(botTag()) == "bot", "the tag a machine's letter carries");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
