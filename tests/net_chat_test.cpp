// Lobby chat: whether it is on, and how fast anyone may talk.
//
// The transport was written years before any of these rules and had none of
// them, so a joined peer could send as fast as it could write and the host
// would fan every line out to everybody. These are the rules in front of it,
// tested where they can be tested -- with a number and a clock rather than a
// relay and eight sockets.
//
// Build target: NetChatTest. Non-zero exit means a case failed.

#include "net/ChatRules.h"
#include "net/RateLimit.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0, g_checks = 0;

void check(const char* what, bool ok) {
    ++g_checks;
    printf(ok ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!ok) ++g_failures;
}

void section(const char* t) { printf("\n== %s ==\n", t); }

}  // namespace

int main() {
    printf("Lobby chat rules\n");

    section("the ordinary case: people talking");
    {
        netchat::Gate g;
        check("a first line goes through",
              g.admit(1, "hello", 0.0) == netchat::Verdict::Allowed);
        // Three in hand, because a thought split over three messages is how
        // people type. A rule that refuses the second line is not usable.
        check("and so does a second, straight away",
              g.admit(1, "are we starting?", 0.0) == netchat::Verdict::Allowed);
        check("and a third",
              g.admit(1, "I am France", 0.0) == netchat::Verdict::Allowed);
        check("the fourth in the same instant is too fast",
              g.admit(1, "and again", 0.0) == netchat::Verdict::TooFast);
    }

    section("and it recovers, at the rate it says");
    {
        netchat::Gate g;
        for (int i = 0; i < 3; ++i) g.admit(1, "x", 0.0);
        check("still refused a moment later",
              g.admit(1, "x", 0.5) == netchat::Verdict::TooFast);
        // 0.5/s means one line every two seconds.
        check("allowed again after two seconds",
              g.admit(1, "x", 2.1) == netchat::Verdict::Allowed);
    }

    section("a refusal costs nothing");
    {
        // Being told to slow down must not push the budget further out, or a
        // client that retries is punished for retrying and never recovers.
        netchat::Gate g;
        for (int i = 0; i < 3; ++i) g.admit(1, "x", 0.0);
        for (int i = 0; i < 50; ++i) g.admit(1, "x", 0.1);   // hammering
        check("hammering does not delay recovery",
              g.admit(1, "x", 2.1) == netchat::Verdict::Allowed);
    }

    section("one player's flood is one player's problem");
    {
        netchat::Gate g;
        for (int i = 0; i < 10; ++i) g.admit(1, "flood", 0.0);
        check("the flooder is cut off",
              g.admit(1, "flood", 0.0) == netchat::Verdict::TooFast);
        check("everybody else is unaffected",
              g.admit(2, "hello", 0.0) == netchat::Verdict::Allowed);
        check("and so is a third",
              g.admit(3, "hello", 0.0) == netchat::Verdict::Allowed);
    }

    section("the host's switch");
    {
        netchat::Policy off;
        off.enabled = false;
        netchat::Gate g(off);
        check("nothing goes through when the host turned it off",
              g.admit(1, "hello", 0.0) == netchat::Verdict::Disabled);
        check("and the sender is told which it was",
              std::string(netchat::explain(netchat::Verdict::Disabled))
                  .find("off") != std::string::npos);
    }

    section("what is not worth sending");
    {
        netchat::Gate g;
        check("an empty line is refused",
              g.admit(1, "", 0.0) == netchat::Verdict::Empty);
        check("so is one that is only spaces",
              g.admit(1, "   \t\n ", 0.0) == netchat::Verdict::Empty);
        check("over the protocol's cap is refused",
              g.admit(1, std::string(513, 'a'), 0.0) == netchat::Verdict::TooLong);
        check("and exactly at the cap is not",
              g.admit(1, std::string(512, 'a'), 0.0) == netchat::Verdict::Allowed);
        // None of those should have cost a token, so the budget is intact.
        check("a refused line did not spend the budget",
              g.admit(1, "a", 0.0) == netchat::Verdict::Allowed &&
              g.admit(1, "b", 0.0) == netchat::Verdict::Allowed);
    }

    section("a clock that goes backwards mints nothing");
    {
        // Long-running hosts see this, and the naive refill would hand out
        // negative time as tokens -- or, worse, a huge positive number.
        netchat::Gate g;
        for (int i = 0; i < 3; ++i) g.admit(1, "x", 100.0);
        check("still refused when the clock steps back",
              g.admit(1, "x", 50.0) == netchat::Verdict::TooFast);
        // THE MEASUREMENT IS THE TIMESCALE, and the first version of this got
        // it wrong: it asked again at 102.1, fifty seconds after the step, and
        // passed with the guard deleted -- the naive refill goes deeply
        // negative and then climbs back, so given long enough it looks fine.
        // What the guard actually buys is that the peer recovers on the normal
        // two-second timescale rather than being silenced for the LENGTH OF
        // THE STEP. So ask 2.1s after the step, not 2.1s after the old clock.
        check("and recovers two seconds later, not fifty",
              g.admit(1, "x", 52.1) == netchat::Verdict::Allowed);
    }

    section("a peer that left takes its budget with it");
    {
        // Relay handles are reused, so a new player must not inherit the
        // last one's exhausted bucket.
        netchat::Gate g;
        for (int i = 0; i < 3; ++i) g.admit(7, "x", 0.0);
        check("the old peer is out of budget",
              g.admit(7, "x", 0.0) == netchat::Verdict::TooFast);
        g.forget(7);
        check("a new peer on the same id starts fresh",
              g.admit(7, "hello", 0.0) == netchat::Verdict::Allowed);
    }

    section("the general per-peer budget the host puts in front of every frame");
    {
        // The host's real numbers. A game sends a handful of frames a turn, so
        // these are orders of magnitude of headroom -- the point is only that
        // an unbounded flood is bounded.
        netrate::PerPeer frames(25.0, 50.0);
        for (int i = 0; i < 50; ++i)
            if (!frames.allow(1, 1.0, 0.0)) { check("the burst is fifty", false); break; }
        check("fifty frames at once are allowed", true);
        check("the fifty-first in the same instant is not",
              !frames.allow(1, 1.0, 0.0));
        check("and another peer is unaffected", frames.allow(2, 1.0, 0.0));
        check("a second later, twenty-five more are available",
              frames.allow(1, 25.0, 1.0));
        // That spent the lot, so it is empty again -- and hammering it must not
        // push recovery further away, or a client that retries is punished for
        // retrying.
        // Every one of these asks for MORE than can possibly be there, so every
        // one is refused -- which is the only way to measure that a refusal is
        // free. Asking for one token instead would let a dozen of them succeed
        // and spend real budget, which is what the first version of this check
        // did and why it failed.
        for (int i = 0; i < 500; ++i) frames.allow(1, 40.0, 1.5);
        check("hammering with refused frames does not delay recovery",
              frames.allow(1, 25.0, 2.0));
    }
    {
        // Bytes, because one frame may be megabytes and a hundred of those is a
        // different attack from a hundred small ones.
        netrate::PerPeer bytes(4.0 * 1024 * 1024, 16.0 * 1024 * 1024);
        check("a 12 MB burst is allowed", bytes.allow(1, 12.0 * 1024 * 1024, 0.0));
        check("another 12 MB in the same instant is not",
              !bytes.allow(1, 12.0 * 1024 * 1024, 0.0));
        check("but a small frame still gets through",
              bytes.allow(1, 1024.0, 0.0));
    }
    {
        netrate::PerPeer p(1.0, 1.0);
        p.allow(7, 1.0, 0.0);
        check("a peer that spent its budget is refused", !p.allow(7, 1.0, 0.0));
        p.forget(7);
        check("and a new peer reusing the relay handle starts fresh",
              p.allow(7, 1.0, 0.0));
    }
    {
        netrate::PerPeer p(1.0, 3.0);
        for (int i = 0; i < 3; ++i) p.allow(1, 1.0, 100.0);
        check("a clock that steps backwards does not silence a peer for the step",
              !p.allow(1, 1.0, 50.0) && p.allow(1, 1.0, 51.1));
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
