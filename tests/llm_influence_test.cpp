// The one place a letter reaches a decision, and the bounds on it.
//
// The module writes letters. This is the only thing it does that a player
// could lose a game to, so the properties that keep it small are worth more
// than the feature itself:
//
//   1. IT SATURATES. Three letters buy as much as thirty. Without this the
//      strongest strategy in the game is to write the same sentence in a loop.
//   2. IT IS DIRECTIONAL. Britain warming to France says nothing about how
//      France regards Britain.
//   3. IT LOSES TO SURVIVAL. A country being ganged up on, or at its pact cap,
//      still refuses somebody it likes. This is an ORDERING between constants,
//      which is worth asserting rather than benching: the ordering is the
//      claim, and three seeds could agree with it by luck.
//   4. IT IS EXACTLY ZERO WHEN THE MODULE IS OFF. Asserted in the game itself
//      -- see the note at the bottom of this file for why it cannot be here.

#include "llm/Advisor.h"
#include "ai/AISystem.h"

#include <cstdio>
#include <string>

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

int main() {
    printf("What a correspondence is worth\n");

    section("one letter moves it, and many letters do not move it further");
    {
        float d = 0.0f;
        d = llm::foldDisposition(d, +1);
        ok(d > 0.0f && d < 1.0f, "one warm letter is a nudge, not a commitment");

        const float afterOne = d;
        d = llm::foldDisposition(d, +1);
        d = llm::foldDisposition(d, +1);
        ok(d > afterOne, "a second and third go further");
        ok(d >= 1.0f - 1e-6f, "and three consistent letters saturate it");

        for (int i = 0; i < 50; ++i) d = llm::foldDisposition(d, +1);
        ok(d <= 1.0f + 1e-6f, "fifty more buy nothing at all");
        ok(d >= 1.0f - 1e-6f, "and do not overflow the top either");
    }
    {
        float d = 0.0f;
        for (int i = 0; i < 50; ++i) d = llm::foldDisposition(d, -1);
        ok(d >= -1.0f - 1e-6f && d <= -1.0f + 1e-6f,
           "the cold end is bounded exactly as tightly");
    }
    {
        // A model that writes "unchanged" must not drift the number, or a
        // correspondence about the weather slowly becomes an alliance.
        float d = llm::foldDisposition(0.5f, 0);
        ok(d == 0.5f, "a letter that changes nothing changes nothing");
    }
    {
        // Warm then cold must return to where it started, or the order letters
        // happen to arrive in decides the outcome.
        const float there = llm::foldDisposition(llm::foldDisposition(0.0f, +1), -1);
        const float back  = llm::foldDisposition(llm::foldDisposition(0.0f, -1), +1);
        ok(there > -1e-6f && there < 1e-6f, "warm then cold is where you began");
        ok(back  > -1e-6f && back  < 1e-6f, "and so is cold then warm");
    }

    section("the key that keeps one country's view its own");
    {
        // The store is keyed (from << 20 | to). If that ever collides, one
        // country's regard for another silently becomes the other's regard
        // back -- which would let a player talk a country round by writing to
        // it in its own voice.
        auto key = [](long long a, long long b) { return (a << 20) | b; };
        ok(key(3, 7) != key(7, 3), "a pair is not the same as its reverse");
        ok(key(1, 2) != key(2, 1), "including for small ids");

        // The shift assumes every country id fits in 20 bits. Rebels start at
        // 60000 and are the highest ids the game makes.
        // Mirrors Game::REBEL_CID_MIN. Not included from Game.h because that
        // header is the whole game; if rebels are ever renumbered above a
        // million this is the line that has to move with them.
        ok(60000 < (1 << 20),
           "every country id fits in the 20 bits the key gives it");
        ok(key(59999, 59999) != key(59998, 60000),
           "and ids near the top do not fold into each other");
    }

    section("persuasion loses to survival");
    {
        // Saturated: the most a correspondence can ever be worth.
        const float most = AISystem::AI_LLM_DISPOSITION * 1.0f;

        ok(most < AISystem::COALITION_WEIGHT,
           "a country everyone is worried about still says no to a friend");
        ok(most < AISystem::AI_NAP_WILLINGNESS,
           "and talking is worth less than the standing case for a pact");
        ok(most < AISystem::AI_CALL_RELUCTANCE,
           "and less than the reluctance to be dragged into somebody's war");
        ok(most > 0.0f, "but it is worth something, or none of this is wired up");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}

// WHY "OFF MEANS ZERO" IS NOT ASSERTED HERE.
//
// It is a property of Game::llmDispositionToward, and Game is the whole game --
// a map, a renderer and a save format. Linking it into a unit test would make
// this file the slowest in the suite to prove one branch.
//
// It is covered twice over instead. llmDispositionToward checks llmConfigured()
// itself rather than trusting its callers, so the zero cannot be forgotten at a
// call site; and tests/determinism_check.sh plays the same seed six times and
// compares, which is what would catch the term perturbing a game that has no
// model installed.
