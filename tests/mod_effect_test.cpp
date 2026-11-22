// Screen effects a mod may ask for.
//
//   ModEffectTest
//
// WHY THIS TEST EXISTS
//
// The dangerous failure here is not an ugly map. It is a SEIZURE. A mod that
// alternates two perfectly legal values every frame is a full-screen strobe at
// the frame rate, and clamping the values does nothing about it -- both ends of
// the alternation are inside the range. So the property that matters most is
// the one about rate, and it is the one measured hardest below: drive the layer
// the way a hostile mod would and count how fast the rendered value can
// actually cross the range.
//
// The others are ordinary capability hygiene with one twist. A mod must not be
// able to tell that the player switched effects off, because a mod that could
// tell could route round it -- so the test asserts that the call still SUCCEEDS
// while the output stays neutral, which is the one place where "it worked and
// did nothing" is the correct behaviour.
//
// Neutral is not zero, either: zero saturation is a greyscale screen, so a
// target that reset to zeroes would drain the colour out of the map on release.

#include "../src/ModEffectLayer.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace odeffect;

namespace {

int checks = 0, fails = 0;
void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
void section(const char* t) { printf("\n== %s ==\n", t); }

/// Run the layer forward at a fixed frame rate.
void run(Layer& l, float seconds, float fps = 60.0f) {
    const float dt = 1.0f / fps;
    for (int i = 0; i < (int)std::lround(seconds * fps); ++i) l.advance(dt);
}

}  // namespace

int main() {
    printf("Mod screen effects\n");

    section("nothing asked for is neutral, and neutral is not zero");
    {
        Layer l;
        ok(l.empty(), "a fresh layer is empty");
        ok(!l.active(Target::Map), "the map needs no pass");
        ok(l.rendered(Target::Map, Effect::Saturation) == 500,
           "saturation sits at 1x, not at 0 (which would be greyscale)");
        ok(l.rendered(Target::Map, Effect::HueRotate) == 0, "no hue rotation");
        ok(l.owner(Target::Map).empty(), "nobody owns the map");
    }

    section("a value out of range is clamped, not refused");
    {
        Layer l;
        ok(l.setEffect("a", Target::Map, Effect::Grain, 4000),
           "an absurd per-mille is accepted");
        run(l, 5.0f);
        ok(l.rendered(Target::Map, Effect::Grain) == kMaxPermille,
           "and arrives at the top of the range");
    }

    section("a target or effect the host does not have is refused");
    {
        Layer l;
        ok(!l.setEffect("a", (Target)99, Effect::Grain, 500), "unknown target");
        ok(!l.setEffect("a", Target::Map, (Effect)99, 500), "unknown effect");
        ok(!l.setEffect("", Target::Map, Effect::Grain, 500), "no mod id");
        ok(l.empty(), "and none of it was stored");
    }

    section("one mod per target");
    {
        Layer l;
        ok(l.setEffect("a", Target::Map, Effect::Grain, 800), "the first mod claims the map");
        ok(!l.setEffect("b", Target::Map, Effect::Grain, 0),
           "the second is refused rather than fighting over it");
        ok(l.setEffect("b", Target::Panel, Effect::Grain, 800),
           "but may have a different target");
        ok(l.owner(Target::Map) == "a" && l.owner(Target::Panel) == "b",
           "each target names its owner");
        l.clearMod("a");
        ok(l.owner(Target::Map).empty(), "unloading a mod releases its target");
        ok(l.setEffect("b", Target::Map, Effect::Grain, 300),
           "which another mod may then claim");
    }

    section("a mod cannot clear a target it does not hold");
    {
        Layer l;
        l.setEffect("a", Target::Map, Effect::Grain, kMaxPermille);
        run(l, 5.0f);
        l.clearTarget("b", Target::Map);
        run(l, 5.0f);
        ok(l.rendered(Target::Map, Effect::Grain) == kMaxPermille,
           "the effect is still there");
        ok(l.owner(Target::Map) == "a", "and still owned by the mod that set it");
    }

    // ── THE ONE THAT MATTERS ──
    section("a mod cannot strobe");
    {
        // What a hostile -- or merely enthusiastic -- mod does: ask for full
        // brightness on one frame and none on the next, for a second, at a
        // high frame rate. Count how often the rendered value crosses the
        // middle of the range. WCAG 2.3.1's general threshold is three flashes
        // per second; a crossing each way is half a cycle.
        Layer l;
        const float fps = 240.0f, dt = 1.0f / fps;
        int crossings = 0;
        bool above = l.rendered(Target::Map, Effect::Grain) > kMaxPermille / 2;
        for (int i = 0; i < (int)fps; ++i) {
            l.setEffect("a", Target::Map, Effect::Grain, (i % 2) ? kMaxPermille : 0);
            l.advance(dt);
            const bool now = l.rendered(Target::Map, Effect::Grain) > kMaxPermille / 2;
            if (now != above) { ++crossings; above = now; }
        }
        printf("        %d half-cycles in one second at %.0f fps\n", crossings, fps);
        ok(crossings <= 6, "at most three flashes per second (six half-cycles)");

        // And the slew is not merely slow on average: no SINGLE frame may jump
        // far, which is the other way to build a flash out of legal values.
        Layer m;
        m.setEffect("a", Target::Map, Effect::Grain, kMaxPermille);
        uint32_t prev = m.rendered(Target::Map, Effect::Grain);
        uint32_t worst = 0;
        for (int i = 0; i < 600; ++i) {
            m.advance(dt);
            const uint32_t now = m.rendered(Target::Map, Effect::Grain);
            worst = std::max(worst, now > prev ? now - prev : prev - now);
            prev = now;
        }
        const uint32_t allowed = (uint32_t)std::lround(kSlewPerSecond * kMaxPermille * dt) + 1;
        printf("        worst single frame moved %u of %u permille (allowed %u)\n",
               worst, kMaxPermille, allowed);
        ok(worst <= allowed, "no frame moves more than the slew rate allows");

        // A STALLED FRAME IS NOT A LICENCE TO JUMP. A long dt -- a resolved
        // turn, a map repaint -- would otherwise deliver in one frame exactly
        // the jump the per-frame limit exists to prevent.
        Layer n;
        n.setEffect("a", Target::Map, Effect::Grain, kMaxPermille);
        n.advance(30.0f);
        printf("        after a 30-second frame: %u\n", n.rendered(Target::Map, Effect::Grain));
        ok(n.rendered(Target::Map, Effect::Grain) < kMaxPermille / 2,
           "a 30-second frame still does not arrive");
    }

    section("asking for a legal effect does arrive, though");
    {
        Layer l;
        l.setEffect("a", Target::Map, Effect::HueRotate, 750);
        ok(l.rendered(Target::Map, Effect::HueRotate) != 750,
           "not on the frame it was asked for");
        run(l, 3.0f);
        ok(l.rendered(Target::Map, Effect::HueRotate) == 750, "but within a few seconds");
        ok(l.active(Target::Map), "and the map now needs its pass");
    }

    section("releasing a target fades out and then stops costing a pass");
    {
        Layer l;
        l.setEffect("a", Target::Map, Effect::Vignette, kMaxPermille);
        run(l, 3.0f);
        l.clearTarget("a", Target::Map);
        ok(l.active(Target::Map), "still drawn on the frame it was released");
        run(l, 3.0f);
        ok(!l.active(Target::Map), "neutral once the fade finishes");
        ok(l.empty(), "and the layer is empty again");
    }

    section("a hue that rolls by itself, at the host's rate");
    {
        // The reason this effect exists: mod_draw_panel is the only per-frame
        // export, so without it a mod has to register a panel purely as a clock
        // in order to animate anything. The host integrating the phase also
        // means the RATE is the host's, which is the parameter that matters.
        Layer l;
        l.setEffect("a", Target::Map, Effect::HueCycle, kMaxPermille);
        ok(!l.active(Target::Map),
           "nothing is drawn on the frame it was asked for -- the speed slews in too");
        run(l, 2.0f);
        ok(l.active(Target::Map), "a target with only a rolling hue needs its pass");
        const uint32_t a = l.huePermille(Target::Map);
        run(l, 1.0f);
        const uint32_t b = l.huePermille(Target::Map);
        ok(a != b, "the hue moves without the mod asking again");

        // At full speed it must take at least 1/kMaxCycleTurnsPerSecond seconds
        // to come round. Counted as wraps rather than by reading the value, so
        // a hue that raced round twice between samples cannot pass unnoticed.
        uint32_t wraps = 0, prev = l.huePermille(Target::Map);
        const float dt = 1.0f / 240.0f;
        for (int i = 0; i < 240 * 4; ++i) {       // four seconds
            l.advance(dt);
            const uint32_t now = l.huePermille(Target::Map);
            if (now < prev) ++wraps;
            prev = now;
        }
        printf("        %u full turns in four seconds at maximum speed\n", wraps);
        ok(wraps <= 2, "no faster than one turn every two seconds");
        ok(wraps >= 1, "and it does go round");

        // The player's switch stops it dead rather than freezing the colour at
        // whatever it had reached: the speed slews to zero, so the phase stops
        // advancing on its own.
        l.setEnabled(false);
        run(l, 5.0f);
        const uint32_t frozen = l.huePermille(Target::Map);
        run(l, 2.0f);
        ok(l.huePermille(Target::Map) == frozen, "switched off, it stops turning");
        ok(!l.active(Target::Map), "and costs no pass");
    }

    section("the kill switch is immediate, and does not eat the request");
    {
        // The Mod Menu suppresses effects on every frame it is open, including
        // frames where a mod is still running and still asking. It has to come
        // up looking like itself NOW -- a menu that fades into legibility over
        // a second and a half is a menu you cannot trust.
        Layer l;
        l.setEffect("a", Target::Map, Effect::Grain, kMaxPermille);
        l.setEffect("a", Target::Panel, Effect::Saturation, 0);
        run(l, 3.0f);
        l.setSuppressed(true);
        l.advance(1.0f / 60.0f);
        ok(!l.active(Target::Map) && !l.active(Target::Panel),
           "every target is neutral on the same frame");

        // ── THE REGRESSION ──
        //
        // The kill switch was clear(), which also threw away what the mod had
        // asked for. That was invisible while every effect mod re-asserted
        // from mod_draw_panel each frame -- and fatal the moment HUE_CYCLE let
        // a mod set its effect ONCE, in mod_load, which runs while the Mod
        // Menu is open. The request was destroyed on the frame it was made and
        // the mod never ran again. It looked like the mod was broken.
        l.setSuppressed(false);
        run(l, 3.0f);
        ok(l.rendered(Target::Map, Effect::Grain) == kMaxPermille,
           "and the effect comes back without the mod asking again");
        ok(l.owner(Target::Map) == "a", "the target is still owned");
    }

    section("a mod that speaks once, while the Mod Menu is open");
    {
        // End to end, in the order the game does it: the menu is up, the mod
        // loads and makes its only call, the player closes the menu.
        Layer l;
        l.setSuppressed(true);
        l.advance(1.0f / 60.0f);
        ok(l.setEffect("once", Target::Map, Effect::HueCycle, 750),
           "the call made from mod_load succeeds");
        run(l, 1.0f);
        ok(!l.active(Target::Map), "nothing is drawn while the menu is up");
        l.setSuppressed(false);
        run(l, 2.0f);
        ok(l.active(Target::Map), "and it starts once the menu closes");
    }

    section("the player's off switch, which a mod must not be able to detect");
    {
        Layer l;
        l.setEnabled(false);
        ok(l.setEffect("a", Target::Map, Effect::Grain, kMaxPermille),
           "the call still succeeds");
        run(l, 5.0f);
        ok(l.rendered(Target::Map, Effect::Grain) == 0, "and nothing is rendered");
        ok(!l.active(Target::Map), "no pass is run");
        // Resumes rather than requiring every mod to ask again: what the mod
        // asked for was kept, only held at neutral.
        l.setEnabled(true);
        run(l, 5.0f);
        ok(l.rendered(Target::Map, Effect::Grain) == kMaxPermille,
           "switching it back on resumes what was asked for");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
