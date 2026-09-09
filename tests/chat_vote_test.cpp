// Chat plays the country: the counting, without a socket.
//
// Build target: ChatVoteTest. Non-zero exit means a case failed.

#include "stream/ChatVote.h"

#include <cstdio>
#include <string>

namespace {
int g_checks = 0, g_fails = 0;
void ok(bool c, const std::string& what) {
    ++g_checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++g_fails;
}
void section(const char* t) { printf("\n== %s ==\n", t); }

std::vector<chatvote::Option> threeWays() {
    return {{"1", "Declare war on Poland", 11},
            {"2", "Ask them for a pact", 22},
            {"3", "Do nothing", 33}};
}
}  // namespace

int main() {
    printf("Chat vote\n");

    section("what counts as a vote");
    {
        ok(chatvote::tokenOf("war") == "war", "a bare word");
        ok(chatvote::tokenOf("!war") == "war", "with the bang every other bot uses");
        ok(chatvote::tokenOf("  WAR  ") == "war", "in any case, with space around it");
        ok(chatvote::tokenOf("#2") == "2", "a hash, which people also type");
        // Somebody TALKING is not somebody voting. Counting this is how a chat
        // stops believing the tally.
        ok(chatvote::tokenOf("war is stupid").empty(), "a sentence is conversation");
        ok(chatvote::tokenOf("").empty(), "nothing is nothing");
        ok(chatvote::tokenOf("   ").empty(), "and so is whitespace");
        ok(chatvote::tokenOf(std::string(40, 'a')).empty(), "a very long word is not a token");
    }

    section("one vote each");
    {
        chatvote::Poll p;
        p.open(threeWays(), 0.0, 30.0);
        ok(p.cast("anna", "1", 1.0), "a viewer votes");
        // THE RULE THAT MAKES IT A VOTE AND NOT A KEYBOARD TEST.
        ok(!p.cast("anna", "1", 2.0), "voting again for the same thing changes nothing");
        for (int i = 0; i < 50; ++i) p.cast("anna", "1", 3.0);
        ok(p.totalVotes() == 1, "and fifty more do not stack");

        ok(p.cast("anna", "2", 4.0), "but she may change her mind");
        ok(p.totalVotes() == 1, "and still has one vote");
        ok(p.winner().id == 22, "which now counts for the other option");
    }

    section("the winner, and the draw");
    {
        chatvote::Poll p;
        p.open(threeWays(), 0.0, 30.0);
        p.cast("a", "1", 1.0);
        p.cast("b", "1", 1.0);
        p.cast("c", "2", 1.0);
        ok(p.winner().id == 11, "the most votes wins");
        ok(p.winner().votes == 2, "and says how many");

        chatvote::Poll tie;
        tie.open(threeWays(), 0.0, 30.0);
        tie.cast("a", "2", 1.0);
        tie.cast("b", "1", 1.0);
        // Ties resolve to the EARLIEST option, deterministically. A stream
        // where the same tally can produce two outcomes is a stream where chat
        // argues about the game instead of playing it.
        ok(tie.winner().id == 11, "a draw goes to the option offered first");
        ok(tie.winner().id == 11, "and does so again, given the same tally");
    }

    section("a poll nobody voted in has no winner");
    {
        // Not a default, and not the first option: acting because the clock ran
        // out is how a stream ends up at war by accident.
        chatvote::Poll p;
        p.open(threeWays(), 0.0, 30.0);
        ok(p.winner().id == 0, "silence picks nothing");
        ok(p.totalVotes() == 0, "and counts nothing");
        p.cast("a", "nonsense", 1.0);
        ok(p.winner().id == 0, "a line matching no option is not a vote");
    }

    section("the window");
    {
        chatvote::Poll p;
        p.open(threeWays(), 100.0, 30.0);
        ok(p.open_at(100.0), "open when it opens");
        ok(p.open_at(129.9), "and just before it closes");
        ok(!p.open_at(130.0), "closed at the moment it closes");
        ok(!p.cast("late", "1", 130.0), "a vote after the bell does not count");
        ok(p.secondsLeft(115.0) == 15.0, "the clock reads what is left");
        ok(p.secondsLeft(999.0) == 0.0, "and never goes negative");

        chatvote::Poll empty;
        empty.open({}, 0.0, 30.0);
        ok(!empty.open_at(1.0), "a poll with nothing to pick is not open");
        ok(!empty.cast("a", "1", 1.0), "and cannot be voted in");
    }

    section("what the stream shows");
    {
        chatvote::Poll p;
        p.open(threeWays(), 0.0, 30.0);
        p.cast("a", "3", 1.0);
        p.cast("b", "3", 1.0);
        p.cast("c", "1", 1.0);
        const auto s = p.standings();
        ok(s.size() == 3, "every option is listed, including the ones nobody picked");
        ok(s[0].id == 33 && s[0].votes == 2, "the leader is first");
        ok(s[1].id == 11 && s[1].votes == 1, "then the rest, in order");
        ok(s[2].votes == 0, "and an option with no votes still shows");
        ok(s[0].label == "Do nothing", "with the words the streamer wrote");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
