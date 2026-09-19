// Core.Protected: what a mod may learn about the machine, and what the player
// can see about that.
//
//   ModProtectedTest
//
// WHY THIS TEST EXISTS
//
// This is the only capability that reports the HOST rather than the game.
// Everything else is deliberately opaque about the machine -- the mod clock is
// the turn number, the entropy is seeded from the mod's own id -- so the one
// place that opens a window has to behave exactly as advertised.
//
// The property that matters most is not what the numbers say. It is that A MOD
// CANNOT TELL WHETHER ANYONE IS LOOKING. If recording a call depended on the
// debug console being open, a mod could detect that it was being watched and
// behave differently while it was, and the console would report the behaviour
// of a mod that knows it is on camera. So recording is unconditional, and this
// pins that: the counters move identically whether or not anything reads them.

#include "../src/mods/ModProtected.h"

#include <cstdio>
#include <string>

namespace {

int checks = 0, fails = 0;
void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
void section(const char* t) { printf("\n== %s ==\n", t); }

const odprotected::Usage* find(const std::vector<odprotected::Usage>& v,
                               const std::string& id) {
    for (const auto& u : v) if (u.modId == id) return &u;
    return nullptr;
}

}  // namespace

int main() {
    printf("Core.Protected\n");

    section("nothing is recorded until something asks");
    {
        odprotected::reset();
        ok(odprotected::usage().empty(), "a fresh session has no rows");
    }

    section("a call is counted against the mod that made it");
    {
        odprotected::reset();
        odprotected::record("a.mod", odprotected::Call::ProcessBytes);
        odprotected::record("a.mod", odprotected::Call::ProcessBytes);
        odprotected::record("b.mod", odprotected::Call::ModCount);

        auto rows = odprotected::usage();
        ok(rows.size() == 2, "two mods, two rows");
        const auto* a = find(rows, "a.mod");
        const auto* b = find(rows, "b.mod");
        ok(a && a->processBytes == 2, "both of a.mod's calls are counted");
        ok(a && a->total() == 2, "and nothing else is");
        ok(b && b->modCount == 1, "b.mod's one call is its own");
        ok(b && b->processBytes == 0, "and is not credited to a.mod's column");
    }

    section("THE COUNTERS DO NOT CARE WHETHER ANYONE IS READING THEM");
    {
        // THE PROPERTY THE WHOLE DESIGN RESTS ON. There is no "console is
        // open" input to record() at all -- that is what makes observation
        // undetectable -- so what this can assert is that reading the log has
        // no effect on it. A mod polling its own visible behaviour would see
        // nothing change when a player opens the panel.
        odprotected::reset();
        for (int i = 0; i < 10; ++i)
            odprotected::record("watcher.mod", odprotected::Call::ImageBytes);

        const auto before = odprotected::usage();          // "console opened"
        const auto again = odprotected::usage();           // and read again
        const auto third = odprotected::usage();

        const auto* b = find(before, "watcher.mod");
        const auto* c = find(again, "watcher.mod");
        const auto* d = find(third, "watcher.mod");
        ok(b && c && d, "the row survives being read");
        ok(b->imageBytes == 10 && c->imageBytes == 10 && d->imageBytes == 10,
           "reading the log neither clears nor advances it");

        // And recording keeps working identically afterwards: no latch, no
        // "already reported" state a mod could probe for.
        odprotected::record("watcher.mod", odprotected::Call::ImageBytes);
        // Held in a named vector: find() returns a pointer INTO it, and
        // passing the temporary straight in leaves that pointer dangling the
        // moment the expression ends. The first version of this line did
        // exactly that and read whatever was left on the stack.
        const auto fourth = odprotected::usage();
        const auto* e = find(fourth, "watcher.mod");
        ok(e && e->imageBytes == 11, "and the next call counts exactly as before");
    }

    section("the report is stable");
    {
        // A console that lists mods in a different order each time it opens
        // reads as a different report each time.
        odprotected::reset();
        for (const char* id : {"z.mod", "a.mod", "m.mod"})
            odprotected::record(id, odprotected::Call::ModName);
        auto rows = odprotected::usage();
        ok(rows.size() == 3, "three rows");
        ok(rows[0].modId == "a.mod" && rows[1].modId == "m.mod" &&
           rows[2].modId == "z.mod", "sorted by id, not by arrival");
    }

    section("reset forgets, and only on purpose");
    {
        odprotected::reset();
        odprotected::record("x.mod", odprotected::Call::ModId);
        ok(!odprotected::usage().empty(), "there is something to forget");
        odprotected::reset();
        ok(odprotected::usage().empty(), "and reset forgets it");
    }

    section("what the machine reports");
    {
        // These are platform facts, so the test asserts the CONTRACT rather
        // than a value: 0 means unknown, never "no memory", and a figure that
        // is reported must be plausible rather than a wrapped negative.
        const uint64_t rss = odprotected::processBytes();
        const uint64_t img = odprotected::imageBytes();
        printf("      process %llu bytes, image %llu bytes\n",
               (unsigned long long)rss, (unsigned long long)img);
        ok(rss == 0 || rss > (1ull << 20),
           "resident memory is 0 (unknown) or more than a megabyte");
        ok(img == 0 || img > (1ull << 16),
           "the executable is 0 (unknown) or larger than 64 KiB");
        // Reading them must not itself be recorded: the counters belong to
        // MODS, and a host-side read is not a mod asking.
        odprotected::reset();
        (void)odprotected::processBytes();
        (void)odprotected::imageBytes();
        ok(odprotected::usage().empty(),
           "the game reading its own figures is not a mod call");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
