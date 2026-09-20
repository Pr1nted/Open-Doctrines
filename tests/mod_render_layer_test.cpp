// What a mod may draw on the map.
//
//   ModRenderLayerTest
//
// WHY THIS TEST EXISTS
//
// A mod adds entries between frames and the game walks them inside one, so the
// failure that matters is not a wrong colour -- it is a list that grows without
// bound. A fading effect that paints transparent every frame is the obvious way
// to write one, and with a separate clear call it would add an entry per frame
// for ever. That is why an alpha of zero REMOVES, and it is the property most
// worth pinning.
//
// The other is ownership: a mark left behind by a mod that is no longer running
// is indistinguishable, to a player, from the game being wrong.

#include "../src/ModRenderLayer.h"

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

constexpr uint32_t RED   = 0xFF0000FFu;   // opaque
constexpr uint32_t CLEAR = 0xFF000000u;   // same colour, zero alpha

}  // namespace

int main() {
    printf("Mod render layer\n");

    section("a tint is set, replaced and removed");
    {
        odrender::Layer l;
        ok(l.empty(), "nothing is drawn to begin with");
        ok(l.setTint("a.mod", 7, RED), "a tint is set");
        ok(l.tintCount("a.mod") == 1, "and counted once");
        ok(!l.empty(), "the layer has something in it");

        ok(l.setTint("a.mod", 7, 0x00FF00FFu), "the same province is retinted");
        ok(l.tintCount("a.mod") == 1, "and is still ONE entry, not two");

        ok(l.setTint("a.mod", 7, CLEAR), "zero alpha removes it");
        ok(l.tintCount("a.mod") == 0, "and the entry is gone");
        ok(l.empty(), "leaving nothing drawn");
    }

    section("A FADING EFFECT CANNOT GROW THE LIST FOR EVER");
    {
        // THE ONE THE DESIGN EXISTS FOR. Painting transparent is how anybody
        // would write a fade; with a separate clear call it would add an entry
        // per frame and the draw pass would walk more of them every second.
        odrender::Layer l;
        for (int frame = 0; frame < 10000; ++frame) {
            l.setTint("a.mod", 7, RED);
            l.setTint("a.mod", 7, CLEAR);
            l.setLabel("a.mod", 7, "fading", RED);
            l.setLabel("a.mod", 7, "", RED);
        }
        ok(l.tintCount("a.mod") == 0, "ten thousand frames leave no tints");
        ok(l.labelCount("a.mod") == 0, "and no labels");
        ok(l.empty(), "and nothing at all to draw");
    }

    section("the ceiling refuses rather than slowing the game");
    {
        odrender::Layer l;
        size_t accepted = 0;
        for (int pid = 0; pid < (int)odrender::kMaxTintsPerMod + 50; ++pid)
            if (l.setTint("a.mod", pid, RED)) ++accepted;
        ok(accepted == odrender::kMaxTintsPerMod, "it stops at the cap");
        ok(l.tintCount("a.mod") == odrender::kMaxTintsPerMod, "and holds exactly that");
        ok(!l.setTint("a.mod", 999999, RED), "and says no rather than growing");
        // A province already tinted can still be RETINTED at the cap: that
        // replaces, it does not grow, and refusing it would freeze the colours
        // of a mod that filled the map.
        ok(l.setTint("a.mod", 0, 0x00FF00FFu), "but recolouring an existing one still works");

        // The cap is PER MOD, so one greedy mod cannot crowd out the others.
        ok(l.setTint("b.mod", 1, RED), "and another mod is unaffected");
    }

    section("one mod cannot touch another's");
    {
        odrender::Layer l;
        l.setTint("a.mod", 1, RED);
        l.setTint("b.mod", 2, RED);
        l.clearMod("a.mod");
        ok(l.tintCount("a.mod") == 0, "clearing a.mod drops its marks");
        ok(l.tintCount("b.mod") == 1, "and leaves b.mod's standing");
        ok(!l.empty(), "so the layer still has something to draw");
    }

    section("a label is bounded and cleaned");
    {
        odrender::Layer l;
        const std::string huge(200, 'x');
        ok(l.setLabel("a.mod", 1, huge, RED), "an over-long label is accepted");
        const auto labels = l.labels();
        ok(labels.size() == 1 && labels[0].text.size() == odrender::kMaxLabelChars,
           "and truncated rather than refused");

        // A control character would draw as a box or eat the rest of the line.
        ok(l.setLabel("a.mod", 2, std::string("a\nb\tc"), RED), "control characters accepted");
        const auto after = l.labels();
        bool clean = true;
        for (const auto& lb : after)
            for (unsigned char c : lb.text)
                if (c < 0x20 || c == 0x7F) clean = false;
        ok(clean, "and stripped out");

        // A label that is nothing but control characters is not a label.
        l.setLabel("a.mod", 3, std::string("\n\t"), RED);
        bool has3 = false;
        for (const auto& lb : l.labels()) if (lb.provinceId == 3) has3 = true;
        ok(!has3, "a label with nothing left in it is not stored");
    }

    section("the draw order does not shuffle");
    {
        // Two frames of an unchanged world must draw identically, or
        // overlapping labels flicker for no reason a player can act on.
        odrender::Layer l;
        l.setTint("z.mod", 3, RED);
        l.setTint("a.mod", 1, RED);
        l.setTint("m.mod", 2, RED);
        const auto first = l.tints();
        const auto second = l.tints();
        ok(first.size() == 3, "three tints");
        bool same = true;
        for (size_t i = 0; i < first.size(); ++i)
            if (first[i].provinceId != second[i].provinceId) same = false;
        ok(same, "and the same order twice");
        ok(first[0].provinceId == 1, "sorted by mod, not by when it was set");
    }

    section("a mod with no id draws nothing");
    {
        odrender::Layer l;
        ok(!l.setTint("", 1, RED), "an empty mod id is refused");
        ok(l.empty(), "and nothing is stored under it");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
