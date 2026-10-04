// The rig's parser and its interpolation.
//
// WHY THESE EXIST. Both halves fail quietly. A pose that names a bone the
// skeleton no longer has is a limb that stops moving -- nothing crashes, the
// character just goes subtly stiff, and nobody attributes it to a rename three
// weeks earlier. Interpolation fails the same way: an arm that sweeps the long
// way round through the body is obviously wrong ON SCREEN and completely
// invisible in a diff.
//
// No window, no art, no raylib: parsing and solving are the parts with edge
// cases and they are deliberately kept clear of the renderer.
#include "rig/Rig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;
static void ok(bool c, const char* what) {
    g_checks++;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!c) g_failed++;
}
static void section(const char* s) { printf("\n=== %s ===\n", s); }
static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

// Swing the bone a coat hangs off, fast, and record where the hem and the
// anchor are on every frame of the gesture and of the settle after it. The
// chest goes from straight up to nearly horizontal in a fifth of a second,
// which is a gesture rather than a drift; idle is off, so the only things that
// move the chain are the swing and whatever wind is set.
struct Swing {
    std::vector<rig::Vec2> hem, anchor;
    static const int kGesture = 12;      // frames the pose transition lasts
};

static Swing swingCoat(float drag, float windSpeed = 0.0f) {
    char src[512];
    std::snprintf(src, sizeof src,
        "bone root len 0 angle 0\n"
        "bone chest parent root len 40 angle -90\n"
        "cloth coat parent chest segs 4 len 10 stiff 0.6 damp 0.93 drag %.3f\n"
        "pose hang 0.01s linear\n  chest -90\n"
        "pose swung 0.20s linear\n  chest -20\n", drag);
    rig::Rig r;
    r.setSkeleton(rig::parse(src));
    r.setIdleEnabled(false);
    rig::Wind w;
    w.speed = windSpeed;
    w.gust = 0.0f;                       // a steady push: no noise in the numbers
    r.setWind(w);
    r.setPose("hang", /*immediate=*/true);
    for (int i = 0; i < 90; ++i) r.update(1.0f / 60.0f);   // settle it hanging
    r.setPose("swung");
    Swing out;
    for (int i = 0; i < 180; ++i) {                        // gesture, then three seconds
        r.update(1.0f / 60.0f);
        out.hem.push_back(r.clothPoints()[0].back());
        out.anchor.push_back(r.clothPoints()[0].front());
    }
    return out;
}

// The furthest the hem is left behind the anchor at any point in the gesture.
// The chest tip travels RIGHT, so a hem that trails is left of it.
static float peakLag(const Swing& s) {
    float peak = 0.0f;
    for (int i = 0; i < Swing::kGesture; ++i)
        peak = std::max(peak, s.anchor[i].x - s.hem[i].x);
    return peak;
}

// How far apart two coats are on screen at the worst moment. This is the one
// that matters: drag mostly shows up as WHEN the hem is where, not as a bigger
// peak, so two coats can have near-identical extremes and still never be in
// the same place at the same time.
static float worstGap(const Swing& a, const Swing& b) {
    float worst = 0.0f;
    for (size_t i = 0; i < a.hem.size(); ++i) {
        const float dx = a.hem[i].x - b.hem[i].x, dy = a.hem[i].y - b.hem[i].y;
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
    }
    return worst;
}

// A body with two identical coat panels on the same shoulder, for the wind.
static const char* kWindRig =
    "bone root len 0 angle 0\n"
    "bone chest parent root len 40 angle -90\n"
    "cloth coatA parent chest segs 4 len 10 stiff 0.6 damp 0.93\n"
    "cloth coatB parent chest segs 4 len 10 stiff 0.6 damp 0.93\n"
    "pose hang 0.01s linear\n  chest -90\n";

// Hang a coat in a steady wind and report where the hem ends up, IN SCENE
// SPACE -- which is rig space with x negated when the body is drawn mirrored.
// A scene is entitled to one answer here whichever way the character faces.
static float hemSceneOffset(bool flip) {
    rig::Rig r;
    r.setSkeleton(rig::parse(kWindRig));
    r.setIdleEnabled(false);
    rig::Placement pl;
    pl.flip = flip;
    r.setPlacement(pl);
    rig::Wind w;
    w.dir = {-1.0f, 0.0f};       // blowing toward scene -x
    w.speed = 900.0f;
    w.gust = 0.0f;               // a steady push: no noise in the numbers
    r.setWind(w);
    r.setPose("hang", /*immediate=*/true);
    for (int i = 0; i < 240; ++i) r.update(1.0f / 60.0f);
    const auto& ch = r.clothPoints()[0];
    const float sx = flip ? -1.0f : 1.0f;
    return (ch.back().x - ch.front().x) * sx;
}

// The hem's downwind offset, sampled once settled, with the scene's clock
// either running or stopped.
static std::vector<float> hemOverTime(bool clockRunning, int chain = 0) {
    rig::Rig r;
    r.setSkeleton(rig::parse(kWindRig));
    r.setIdleEnabled(false);
    r.setPose("hang", /*immediate=*/true);
    std::vector<float> out;
    float t = 0.0f;
    for (int i = 0; i < 1200; ++i) {          // twenty seconds
        rig::Wind w;
        w.dir = {-1.0f, 0.0f};
        // Not so hard that the coat pins itself horizontal: past about 600
        // the chain has run out of length and a gust has nowhere to put it,
        // so the wind can vary all it likes and the hem cannot answer.
        w.speed = 250.0f;
        w.gust = 0.45f;
        w.time = clockRunning ? t : 0.0f;
        r.setWind(w);
        r.update(1.0f / 60.0f);
        t += 1.0f / 60.0f;
        if (i >= 240) {                        // after it has settled
            const auto& ch = r.clothPoints()[chain];
            out.push_back(ch.back().x - ch.front().x);
        }
    }
    return out;
}

static const char* kRig =
    "bone root len 0 angle 0\n"
    "bone chest parent root len 40 angle -90 breath 1.0\n"
    "bone armL parent chest len 30 angle 150\n"
    "hand handL parent armL len 0 angle 0 atlas hands size 20\n"
    "cloth coat parent chest segs 3 len 10 stiff 0.5\n"
    "pose a 0.2s linear\n"
    "  chest -90\n"
    "  armL 150\n"
    "  handL open facing 1.0\n"
    "pose b 0.2s linear\n"
    "  chest -80\n"
    "  armL 100\n"
    "  handL fist facing -1.0\n";

int main() {
    section("parsing");
    {
        rig::Skeleton sk = rig::parse(kRig);
        ok(sk.bones.size() == 4, "four bones");
        ok(sk.indexOf("chest") == 1, "bones are indexed by name");
        ok(sk.bones[1].parent == 0, "parents resolve");
        ok(near(sk.bones[1].restAngle, -90.0f), "rest angle");
        ok(sk.bones[3].kind == rig::Bone::Kind::Hand, "a hand is not an ordinary bone");
        ok(sk.cloth.size() == 1 && sk.cloth[0].parent == 1, "cloth hangs off a bone");
        ok(sk.poses.size() == 2, "two poses");
        ok(sk.warnings.empty(), "clean input parses without warnings");
    }

    // A left hand drawn on a right arm is the sort of wrong nobody can name and
    // everybody sees, and it is invisible in a diff -- so the default is pinned
    // here rather than left to whoever next edits an .odrig.
    section("hands know which side they are");
    {
        rig::Skeleton sk = rig::parse(
            "hand handL len 0 angle 0 size 20\n"
            "hand handR len 0 angle 0 size 20\n"
            "hand mitt  len 0 angle 0 size 20\n"
            "hand paw   len 0 angle 0 size 20 side left\n"
            "hand claw  len 0 angle 0 size 20 side right\n");
        ok(near(sk.bones[sk.indexOf("handL")].chir, -1.0f), "a trailing L is a left hand");
        ok(near(sk.bones[sk.indexOf("handR")].chir, 1.0f), "a trailing R is a right hand");
        ok(near(sk.bones[sk.indexOf("mitt")].chir, 1.0f), "an unnamed side defaults to right");
        ok(near(sk.bones[sk.indexOf("paw")].chir, -1.0f), "side left wins over the name");
        ok(near(sk.bones[sk.indexOf("claw")].chir, 1.0f), "side right is explicit too");
    }

    // A gesture that arrives somewhere and stays is a posture. This is the
    // check that the shoulders are actually seen UP before they come down --
    // which no screenshot of the settled pose can show.
    section("a pose with keyframes is a movement, not a destination");
    {
        rig::Skeleton sk = rig::parse(
            "bone root len 0 angle 0\n"
            "bone arm parent root len 10 angle 0\n"
            "pose wave 0.2s linear hold 0.3s\n"
            "  arm 90\n"
            "key 0.2s linear\n"
            "  arm 30\n");
        ok(sk.warnings.empty(), "keyframes parse without warnings");
        ok(sk.poses["wave"].more.size() == 1, "the extra keyframe is attached to the pose");
        ok(near(sk.poses["wave"].hold, 0.3f), "hold is read off the pose line");

        rig::Rig r;
        r.setSkeleton(sk);
        r.setIdleEnabled(false);
        r.setPose("wave");
        const int arm = sk.indexOf("arm");

        for (int i = 0; i < 12; ++i) r.update(1.0f / 60.0f);   // 0.20s: first beat done
        ok(near(r.solved()[arm].worldAngle, 90.0f, 2.0f), "it reaches the first keyframe");
        ok(!r.settled(), "and is NOT settled -- the gesture is still running");

        for (int i = 0; i < 12; ++i) r.update(1.0f / 60.0f);   // 0.20s into a 0.3s hold
        ok(near(r.solved()[arm].worldAngle, 90.0f, 2.0f), "it holds there rather than easing on");
        ok(!r.settled(), "still not settled during the hold");

        for (int i = 0; i < 40; ++i) r.update(1.0f / 60.0f);   // hold expires, second beat runs
        ok(near(r.solved()[arm].worldAngle, 30.0f, 2.0f), "then it moves on to the last keyframe");
        ok(r.settled(), "and only now is it settled");
    }

    // Setting a keyframed pose immediately has to land on the END of it: a
    // speaker who appears mid-shrug stays mid-shrug forever.
    section("an immediate keyframed pose lands on its last beat");
    {
        rig::Skeleton sk = rig::parse(
            "bone root len 0 angle 0\n"
            "bone arm parent root len 10 angle 0\n"
            "pose wave 0.2s linear\n  arm 90\n"
            "key 0.2s linear\n  arm 30\n");
        rig::Rig r;
        r.setSkeleton(sk);
        r.setIdleEnabled(false);
        r.setPose("wave", true);
        ok(near(r.solved()[sk.indexOf("arm")].worldAngle, 30.0f, 0.5f),
           "immediate goes straight to the final keyframe");
        ok(r.settled(), "and reports settled at once");
    }

    // A gesture ends and hands the body back. Without this the script has to
    // remember to put the character down after every one, and the page that
    // forgets leaves somebody standing with their arms out for the whole scene.
    section("a gesture falls back to a posture when it is over");
    {
        rig::Skeleton sk = rig::parse(
            "bone root len 0 angle 0\n"
            "bone arm parent root len 10 angle 0\n"
            "pose rest 0.1s linear\n  arm 0\n"
            "pose wave 0.1s linear hold 0.2s then rest\n  arm 90\n");
        ok(sk.poses["wave"].next == "rest", "`then` is read off the pose line");
        ok(sk.poses["wave"].seconds > 0.05f && sk.poses["wave"].seconds < 0.15f,
           "and does not swallow the duration next to it");

        rig::Rig r;
        r.setSkeleton(sk);
        r.setIdleEnabled(false);
        r.setPose("wave");
        const int arm = sk.indexOf("arm");

        for (int i = 0; i < 6; ++i) r.update(1.0f / 60.0f);       // arrived
        ok(near(r.solved()[arm].worldAngle, 90.0f, 3.0f), "the gesture plays");
        ok(r.activePose() == "wave", "and is what is playing");

        for (int i = 0; i < 20; ++i) r.update(1.0f / 60.0f);      // hold expires
        ok(r.activePose() == "rest", "when it ends the fallback takes over");
        // THE REQUEST IS UNCHANGED, or a caller that re-asserts the page's pose
        // every frame retriggers the gesture the instant it finishes, forever.
        ok(r.pose() == "wave", "but the pose that was ASKED for is still reported");

        for (int i = 0; i < 20; ++i) r.update(1.0f / 60.0f);
        ok(near(r.solved()[arm].worldAngle, 0.0f, 3.0f), "and the body reaches it");
        ok(r.settled(), "and settles there rather than bouncing between the two");
    }

    // Both hands raised with the palms out put the thumbs on the OUTSIDE.
    // They were on the inside, which is what a mirrored sign looks like, and it
    // is invisible in a diff -- so the convention is pinned here.
    section("the thumb is on the correct side of each hand");
    {
        // Across is the hand turned a quarter turn anticlockwise: for a hand
        // pointing up the screen, negative is screen-right.
        ok(rig::thumbSide(+1.0f, +1.0f) > 0.0f, "right hand, palm out: thumb screen-LEFT");
        ok(rig::thumbSide(-1.0f, +1.0f) < 0.0f, "left hand, palm out: thumb screen-RIGHT");
        ok(rig::thumbSide(+1.0f, -1.0f) < 0.0f, "turning a hand over swaps the thumb");
        ok(rig::thumbSide(-1.0f, -1.0f) > 0.0f, "and swaps it for the other hand too");
        // A raised pair, palms out: the character's right hand is drawn on the
        // viewer's left, so its thumb must run further left, not toward the
        // other hand.
        ok(rig::thumbSide(+1.0f, 1.0f) * rig::thumbSide(-1.0f, 1.0f) < 0.0f,
           "a raised pair points its thumbs apart, not at each other");
    }

    section("a mistyped bone in a pose is reported");
    {
        rig::Skeleton sk = rig::parse("bone root len 0 angle 0\npose p\n  nosuchbone 10\n");
        ok(!sk.warnings.empty(), "unknown bone in a pose warns");
        rig::Skeleton f = rig::parse("bone a parent ghost len 1 angle 0\n");
        ok(!f.warnings.empty(), "unknown parent warns");
    }

    section("interpolation takes the short way round");
    {
        ok(near(rig::shortestArc(170.0f, -170.0f), 20.0f), "170 -> -170 is +20, not -340");
        ok(near(rig::shortestArc(-170.0f, 170.0f), -20.0f), "and back again");
        ok(near(rig::shortestArc(0.0f, 90.0f), 90.0f), "an ordinary arc is unchanged");
    }

    section("easing");
    {
        for (auto e : {rig::Ease::Linear, rig::Ease::OutQuad,
                       rig::Ease::InOutQuad, rig::Ease::OutBack}) {
            ok(near(rig::applyEase(e, 0.0f), 0.0f), "starts at 0");
            ok(near(rig::applyEase(e, 1.0f), 1.0f), "ends at 1");
        }
        ok(rig::applyEase(rig::Ease::OutBack, 0.8f) > 1.0f, "outback overshoots on the way");
    }

    section("posing");
    {
        rig::Rig r;
        r.setSkeleton(rig::parse(kRig));
        r.setIdleEnabled(false);          // exact numbers, no breathing
        ok(r.setPose("a"), "a named pose is found");
        ok(!r.setPose("nope"), "an unnamed one is refused rather than assumed");
        r.setPose("a");
        for (int i = 0; i < 30; ++i) r.update(0.02f);
        ok(r.settled(), "it settles");
        const auto& s = r.solved();
        ok(s.size() == 4, "every bone solves");
        // chest at -90 from the root: 40 long, straight up. +y is down.
        ok(near(s[1].b.x, 0.0f, 0.5f) && near(s[1].b.y, -40.0f, 0.5f),
           "a bone at -90 points up its own length");
        ok(near(s[2].a.x, s[1].b.x) && near(s[2].a.y, s[1].b.y),
           "a child starts where its parent ends");
    }

    section("the hand's drawing switches mid-move, not at the ends");
    {
        // The whole hand design rests on this: see the note in Rig.h.
        rig::Rig r;
        r.setSkeleton(rig::parse(kRig));
        r.setIdleEnabled(false);
        r.setPose("a");
        for (int i = 0; i < 30; ++i) r.update(0.02f);
        ok(r.solved()[3].handClass == "open", "pose a holds the open hand");
        ok(near(r.solved()[3].facing, 1.0f, 0.02f), "facing arrives at +1");

        r.setPose("b");
        r.update(0.02f);                                   // 10% in
        ok(r.solved()[3].handClass == "open", "early in the move it is still the old drawing");
        const float early = r.solved()[3].facing;
        ok(early < 1.0f && early > 0.0f, "but facing has already begun to turn");
        for (int i = 0; i < 8; ++i) r.update(0.02f);        // past halfway
        ok(r.solved()[3].handClass == "fist", "past the middle it is the new one");
        for (int i = 0; i < 30; ++i) r.update(0.02f);
        ok(near(r.solved()[3].facing, -1.0f, 0.02f), "and facing lands at -1");
    }

    section("the mouth is a rig channel, not a face drawn in the renderer");
    {
        // It began as three hard-coded line segments in RigDraw, which meant
        // every character in the game wore one expression for ever and no pose
        // could touch it. It is now two scalars per head: `mouth` -1..1 for the
        // curve and `jaw` 0..1 for how far open.
        rig::Skeleton sk = rig::parse(
            "bone root len 0 angle 0\n"
            "head head parent root len 20 angle -90 size 40 mouth -0.45\n"
            "pose glum\n  head mouth -0.8\n"
            "pose talk\n  head 4 mouth -0.1 jaw 0.6\n"
            "pose quiet\n  head 4\n");
        ok(sk.warnings.empty(), "it parses without warnings");
        ok(near(sk.bones[sk.indexOf("head")].mouth, -0.45f),
           "a head carries a RESTING expression, like restAngle");

        rig::Rig r;
        r.setSkeleton(sk);
        r.setIdleEnabled(false);
        const int h = sk.indexOf("head");
        ok(near(r.solved()[h].mouth, -0.45f),
           "and wears it on the first frame, before any pose");

        r.setPose("talk");
        for (int i = 0; i < 60; ++i) r.update(0.02f);
        ok(near(r.solved()[h].mouth, -0.1f, 0.02f), "a pose moves the curve");
        ok(near(r.solved()[h].mouthOpen, 0.6f, 0.02f), "and opens the jaw");

        // BLENDED, NEVER SWITCHED -- the opposite of the hand, which swaps
        // drawing at the midpoint because no blend between a fist and a palm
        // is a hand. A mouth is one shape being pulled about, and a mouth that
        // jumped would do it in the one place a viewer is looking.
        r.setPose("glum");
        r.update(0.02f);
        const float partway = r.solved()[h].mouth;
        ok(partway < -0.1f && partway > -0.8f,
           "and moving to another pose passes THROUGH the values between");

        for (int i = 0; i < 60; ++i) r.update(0.02f);
        ok(near(r.solved()[h].mouth, -0.8f, 0.02f), "then arrives");
        ok(near(r.solved()[h].mouthOpen, 0.0f, 0.02f),
           "and a pose silent about the jaw shuts it, rather than composing");

        r.setPose("quiet");
        for (int i = 0; i < 60; ++i) r.update(0.02f);
        ok(near(r.solved()[h].mouth, -0.45f, 0.02f),
           "a pose that says nothing about the mouth returns it to rest");
    }

    section("`jaw` is not `open`, because `open` is already a hand");
    {
        // The aperture key was called `open` for about ten minutes. `open` is
        // also a hand class, so "handL open facing 1.0" read its own class name
        // as a mouth key, took "facing" as the number, got zero, and quietly
        // stopped being an open hand -- four hand checks went red at once.
        //
        // The rig format has ONE flat namespace for keys, so a new key has to
        // be unique across every line in it, not just the lines it was added
        // for. This is that lesson, pinned.
        rig::Skeleton sk = rig::parse(
            "bone root len 0 angle 0\n"
            "hand handL parent root len 0 angle 0 atlas hands size 20\n"
            "head head parent root len 20 angle -90 size 40\n"
            "pose p\n  handL open facing 1.0\n  head mouth -0.5 jaw 0.4\n");
        ok(sk.warnings.empty(), "both lines parse");
        rig::Rig r;
        r.setSkeleton(sk);
        r.setIdleEnabled(false);
        r.setPose("p", /*immediate=*/true);
        const int hd = sk.indexOf("handL"), he = sk.indexOf("head");
        ok(r.solved()[hd].handClass == "open", "the hand is still an OPEN hand");
        ok(near(r.solved()[hd].facing, 1.0f, 0.01f), "with its facing intact");
        ok(near(r.solved()[he].mouth, -0.5f, 0.01f), "and the mouth is the mouth's");
        ok(near(r.solved()[he].mouthOpen, 0.4f, 0.01f), "jaw and all");
    }

    section("an unnamed bone returns to rest rather than composing");
    {
        rig::Skeleton sk = rig::parse(
            "bone root len 0 angle 0\n"
            "bone a parent root len 10 angle 30\n"
            "bone b parent root len 10 angle 40\n"
            "pose one\n  a 90\n  b 90\n"
            "pose two\n  a 10\n");     // says nothing about b
        rig::Rig r;
        r.setSkeleton(sk);
        r.setIdleEnabled(false);
        r.setPose("one");
        for (int i = 0; i < 60; ++i) r.update(0.02f);
        r.setPose("two");
        for (int i = 0; i < 60; ++i) r.update(0.02f);
        ok(near(r.solved()[2].worldAngle, 40.0f, 0.5f),
           "b went back to its rest angle, not left at 90");
    }

    section("a pose change leaves from the CURRENT stance, not from rest");
    {
        // What was reported as "we always transition from the default rig".
        // The engine was innocent; the caller reloaded the skeleton on every
        // speaker change, and loading resets every bone to rest. Locked down
        // here anyway, because it is the property the whole design rests on.
        rig::Rig r;
        r.setSkeleton(rig::parse(kRig));
        r.setIdleEnabled(false);
        r.setPose("b");
        for (int i = 0; i < 60; ++i) r.update(0.02f);
        const float standing = r.solved()[2].worldAngle;

        r.setPose("a");
        r.update(0.02f);                       // one frame in
        const float justAfter = r.solved()[2].worldAngle;
        const float rest = -90.0f + 150.0f;    // chest rest + armL rest

        ok(std::fabs(justAfter - standing) < std::fabs(justAfter - rest),
           "one frame in, it is still near where it was standing");
        ok(std::fabs(justAfter - standing) < 8.0f, "and it has barely moved yet");
    }

    section("a limb accelerates out of rest, it does not leave at full speed");
    {
        // Reported as "the position just snaps between the positions". The
        // default ease was OutQuad, and every "out" ease STARTS at maximum
        // velocity: 10-18% of the whole travel landed in the first frame, which
        // on a 120 degree arm swing is eighteen degrees and reads as a snap
        // followed by a settle rather than as an arm moving.
        rig::Skeleton sk = rig::parse(
            "bone root len 0 angle 0\n"
            "bone arm parent root len 30 angle 0\n"
            "pose lo 0.5s\n  arm 0\n"      // no ease named: the default
            "pose hi 0.5s\n  arm 120\n");
        rig::Rig r;
        r.setSkeleton(sk);
        r.setIdleEnabled(false);
        r.setPose("lo");
        for (int i = 0; i < 60; ++i) r.update(1.0f / 60.0f);
        const float before = r.solved()[1].worldAngle;
        r.setPose("hi");
        r.update(1.0f / 60.0f);
        const float firstFrame = std::fabs(r.solved()[1].worldAngle - before);
        for (int i = 0; i < 90; ++i) r.update(1.0f / 60.0f);
        const float travel = std::fabs(r.solved()[1].worldAngle - before);
        const float frac = firstFrame / travel;
        printf("        first frame covers %.1f%% of the travel\n", frac * 100.0f);
        ok(frac < 0.02f, "the first frame is a fraction of a percent, not a sixth");
        ok(travel > 100.0f, "and it does get there");
    }

    section("a pose can be taken immediately");
    {
        // A character walking on is already standing the way they stand.
        rig::Rig r;
        r.setSkeleton(rig::parse(kRig));
        r.setIdleEnabled(false);
        r.setPose("a", /*immediate=*/true);
        ok(r.settled(), "an immediate pose is settled at once");
        const float at = r.solved()[2].worldAngle;
        r.update(0.02f);
        ok(near(r.solved()[2].worldAngle, at, 0.001f), "and does not then drift into place");
    }

    section("breathing does not leak into the next transition");
    {
        // m_base and m_angle are separate for this reason: setPose captured the
        // drawn angle, so every pose change baked that instant's breath into
        // the interpolation source and the same transition differed each time.
        rig::Rig a, b;
        a.setSkeleton(rig::parse(kRig));
        b.setSkeleton(rig::parse(kRig));
        a.setPose("a"); b.setPose("a");
        // Settle both, but let them sit for different lengths of time, so they
        // are at different points in the breathing cycle when the pose changes.
        for (int i = 0; i < 60; ++i)  a.update(0.02f);
        for (int i = 0; i < 97; ++i)  b.update(0.02f);
        a.setPose("b"); b.setPose("b");
        a.setIdleEnabled(false); b.setIdleEnabled(false);
        for (int i = 0; i < 60; ++i) { a.update(0.02f); b.update(0.02f); }
        ok(near(a.solved()[2].worldAngle, b.solved()[2].worldAngle, 0.01f),
           "two rigs that breathed differently land in the same place");
    }

    section("idle layers move things, and can be switched off");
    {
        rig::Rig r;
        r.setSkeleton(rig::parse(kRig));
        r.setPose("a");
        for (int i = 0; i < 60; ++i) r.update(0.02f);
        const float withIdle = r.solved()[1].worldAngle;
        bool moved = false;
        for (int i = 0; i < 60 && !moved; ++i) {
            r.update(0.02f);
            if (!near(r.solved()[1].worldAngle, withIdle, 0.05f)) moved = true;
        }
        ok(moved, "a settled pose still breathes");

        rig::Rig q;
        q.setSkeleton(rig::parse(kRig));
        q.setIdleEnabled(false);
        q.setPose("a");
        for (int i = 0; i < 60; ++i) q.update(0.02f);
        const float still = q.solved()[1].worldAngle;
        for (int i = 0; i < 60; ++i) q.update(0.02f);
        ok(near(q.solved()[1].worldAngle, still, 0.001f),
           "and holds perfectly still when idle is off");
    }

    section("the idle layers are SMOOTH, not noise");
    {
        // The first version of the noise used fract(sin(t)*43758), which is a
        // shader HASH: nearby inputs give unrelated outputs by design. Fed
        // continuous time it produced white noise at frame rate and the
        // character shook, always -- up to 2.5 degrees of sway between
        // consecutive frames. A held pose has to drift, not vibrate.
        rig::Rig r;
        r.setSkeleton(rig::parse(kRig));
        r.setPose("a");
        for (int i = 0; i < 90; ++i) r.update(1.0f / 60.0f);   // settle
        float worst = 0.0f, prev = r.solved()[1].worldAngle;
        for (int i = 0; i < 600; ++i) {                         // ten seconds
            r.update(1.0f / 60.0f);
            const float now = r.solved()[1].worldAngle;
            worst = std::max(worst, std::fabs(now - prev));
            prev = now;
        }
        printf("        worst single-frame change: %.4f deg\n", worst);
        // Breath is the fastest legitimate layer: ~1.6 deg amplitude at
        // 0.25 Hz is about 0.04 deg per frame. A tenth of a degree is
        // comfortably above that and far below the 2.5 the bug produced.
        ok(worst < 0.10f, "a settled pose drifts rather than vibrating");

        bool moves = false;
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 600; ++i) {
            r.update(1.0f / 60.0f);
            lo = std::min(lo, r.solved()[1].worldAngle);
            hi = std::max(hi, r.solved()[1].worldAngle);
        }
        moves = (hi - lo) > 0.5f;
        ok(moves, "and it is still alive -- smoothing did not flatten it");
    }

    section("cloth hangs off its bone and stays finite");
    {
        rig::Rig r;
        r.setSkeleton(rig::parse(kRig));
        r.setPose("a");
        for (int i = 0; i < 120; ++i) r.update(0.016f);
        const auto& ch = r.clothPoints();
        ok(ch.size() == 1 && ch[0].size() == 4, "one chain, four points");
        bool finite = true, pinned = true;
        for (const auto& p : ch[0])
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) finite = false;
        const auto& s = r.solved();
        if (!near(ch[0][0].x, s[1].b.x, 0.01f) || !near(ch[0][0].y, s[1].b.y, 0.01f))
            pinned = false;
        ok(finite, "no NaNs after two seconds of simulation");
        ok(pinned, "the first point is pinned to the bone it hangs from");
        // A dropped frame must not fling the coat off the screen.
        r.update(2.0f);
        bool sane = true;
        for (const auto& p : r.clothPoints()[0])
            if (!std::isfinite(p.x) || std::fabs(p.y) > 10000.0f) sane = false;
        ok(sane, "a two-second frame does not explode it");
    }

    section("drag decides how far the cloth is left behind by the bone carrying it");
    {
        const Swing rigid = swingCoat(0.0f);
        const Swing half = swingCoat(0.5f);
        const Swing inertial = swingCoat(1.0f);
        const Swing whippy = swingCoat(1.6f);
        printf("        hem trails by  %.1f (0)  %.1f (0.5)  %.1f (1)  %.1f (1.6)\n",
               peakLag(rigid), peakLag(half), peakLag(inertial), peakLag(whippy));
        ok(peakLag(rigid) < 0.5f, "at 0 the chain rides the bone instead of swinging");
        ok(peakLag(half) > peakLag(rigid) + 4.0f, "carrying it less leaves it further behind");
        ok(peakLag(inertial) > peakLag(half), "and none of the way is further behind again");
        ok(peakLag(whippy) > peakLag(inertial), "past 1 it is carried backwards, further still");

        // A chain cannot trail further than its own length, so the top of the
        // range is compressed and the ORDER above is most of what it proves.
        // What the eye actually reads there is the follow-through: the hem
        // arrives late, overruns the anchor, and swings back.
        float overrunRigid = 0.0f, overrunWhippy = 0.0f;
        for (size_t i = Swing::kGesture; i < whippy.hem.size(); ++i) {
            overrunRigid = std::max(overrunRigid, rigid.hem[i].x - rigid.anchor[i].x);
            overrunWhippy = std::max(overrunWhippy, whippy.hem[i].x - whippy.anchor[i].x);
        }
        printf("        hem overruns the anchor by  %.1f (0)  %.1f (1.6)\n",
               overrunRigid, overrunWhippy);
        ok(overrunRigid < 0.5f, "a coat painted on the body has nothing to overrun with");
        ok(overrunWhippy > 20.0f, "a whippy one swings clean past and comes back");
    }

    section("the drag both shipped rigs author is not the default in disguise");
    {
        // THE ACTUAL BUG. `drag` was documented, parsed, and authored as 1.2 by
        // data/characters/{test,advisor}/rig.odrig -- and read by nothing, so
        // every coat in the game behaved as though it said 1.0. This check is
        // the difference between those two numbers, and it was zero.
        const float gap = worstGap(swingCoat(1.0f), swingCoat(1.2f));
        printf("        1.0 and 1.2 put the hem %.1f px apart on a 40 px chain\n", gap);
        ok(gap > 5.0f, "the authored 1.2 moves the coat somewhere 1.0 does not");
    }

    section("drag composes with the wind rather than replacing it");
    {
        // Two accelerations and a frame shift on the same points. The failure
        // to watch for is one term quietly eating the other -- a coat that
        // stops answering the weather once it is given some drag, or a drag
        // that stops mattering once the wind is up.
        ok(worstGap(swingCoat(1.0f, 0.0f), swingCoat(1.0f, 900.0f)) > 1.0f,
           "the same coat hangs differently in wind");

        const float still = peakLag(swingCoat(1.0f, 0.0f));
        const float windy = peakLag(swingCoat(1.0f, 900.0f));
        ok(!near(still, windy, 0.05f), "and the wind reaches it mid-gesture, not only at rest");

        const float rigidWind = peakLag(swingCoat(0.0f, 900.0f));
        const float inertWind = peakLag(swingCoat(1.0f, 900.0f));
        const float whipWind  = peakLag(swingCoat(1.6f, 900.0f));
        printf("        in wind, hem trails by  %.1f (0)  %.1f (1)  %.1f (1.6)\n",
               rigidWind, inertWind, whipWind);
        ok(rigidWind < inertWind && inertWind < whipWind,
           "drag still orders them the same way with the wind blowing");
    }

    section("the wind is the scene's, not the rig's");
    {
        // THE BUG. Cloth is solved in rig-local pixels and mirrored at draw
        // time, so a scene wind direction handed straight to the solver blew
        // one way on a character facing right and the other way on the same
        // character facing left. Two of them talking in one wind had their
        // coats streaming apart from each other.
        const float facingOneWay = hemSceneOffset(false);
        const float mirrored = hemSceneOffset(true);
        printf("        hem sits at %+.2f facing one way, %+.2f mirrored (scene space)\n",
               facingOneWay, mirrored);
        ok(facingOneWay < -1.0f, "a wind toward scene -x blows the hem that way");
        ok(mirrored < -1.0f, "and it still does when the body is drawn mirrored");
        ok(near(facingOneWay, mirrored, 0.01f),
           "both bodies hang exactly the same way in the same weather");
    }

    section("every garment in one wind agrees about the weather");
    {
        // The gust used to be salted with the chain index, so the two panels
        // of one coat drew independent gusts -- two fabrics disagreeing about
        // the air they are both standing in, which is the thing Wind exists to
        // prevent. They are the same chain on the same bone: they must move as
        // one, and only a per-chain noise could separate them.
        const std::vector<float> a = hemOverTime(true, 0);
        const std::vector<float> b = hemOverTime(true, 1);
        float worst = 0.0f;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            worst = std::max(worst, std::fabs(a[i] - b[i]));
        printf("        two panels never differ by more than %.4f px\n", worst);
        ok(worst < 0.001f, "two identical panels feel one wind, not two");
    }

    section("the wind varies in strength, and only while the scene's clock runs");
    {
        // A constant speed reads as a fan: the coat reaches an angle and stays
        // there. Real air freshens and drops, so the hem has to keep moving
        // long after everything else has settled.
        const std::vector<float> moving = hemOverTime(true);
        const std::vector<float> stopped = hemOverTime(false);
        auto spread = [](const std::vector<float>& v) {
            float lo = 1e9f, hi = -1e9f;
            for (float x : v) { lo = std::min(lo, x); hi = std::max(hi, x); }
            return hi - lo;
        };
        printf("        hem wanders over %.2f px with the clock running, %.2f with it stopped\n",
               spread(moving), spread(stopped));
        ok(spread(moving) > 3.0f, "the hem keeps moving: the air is not a fan");
        // Steady air is NOT still cloth -- a hem held broadside lifts, spills
        // the air as it turns into it, and falls again, which is the flapping
        // the wind model makes on its own and wants to keep. What a stopped
        // clock has to mean is that all the movement left is that flapping,
        // and none of it is the weather changing.
        ok(spread(moving) > spread(stopped) * 5.0f,
           "and with the clock stopped only the flapping is left, a fraction of it");

        // A lull is still air, never air blowing the other way. If the
        // strength could go negative the coat would swing upwind, which reads
        // as the wind reversing rather than as it dropping.
        bool upwind = false;
        for (float x : moving) if (x > 0.0f) upwind = true;
        ok(!upwind, "it never blows backwards, however deep the lull");

        // Gusts run further above the average than lulls run below it, which
        // is what makes it read as air rather than as a sine.
        float mean = 0.0f;
        for (float x : moving) mean += x;
        mean /= (float)moving.size();
        float up = 0.0f, down = 0.0f;
        for (float x : moving) {
            up = std::max(up, mean - x);      // further downwind: a gust
            down = std::max(down, x - mean);  // back toward rest: a lull
        }
        printf("        strongest gust %.2f px past the mean, deepest lull %.2f back\n", up, down);
        ok(up > down, "it surges harder than it sags, the way moving air does");
    }

    section("an absurd drag is clamped rather than believed");
    {
        rig::Skeleton sk = rig::parse(
            "bone root len 0 angle 0\n"
            "cloth a parent root drag -4\n"
            "cloth b parent root drag 99\n"
            "cloth c parent root\n");
        ok(near(sk.cloth[0].drag, 0.0f), "a chain cannot lead the bone that carries it");
        ok(near(sk.cloth[1].drag, 2.0f), "nor be thrown back further than it can be hauled");
        ok(near(sk.cloth[2].drag, 1.0f), "and an unstated drag is plain inertia");
    }

    section("a bone named like a keyword still poses");
    {
        // "head" is both a declaration keyword and the obvious name for the
        // bone a head hangs on. Inside a pose it was read as a new declaration,
        // which ended the pose block and silently dropped every line after it.
        rig::Skeleton sk = rig::parse(
            "bone root len 0 angle 0\n"
            "head head parent root len 20 angle -90 size 30\n"
            "bone armL parent root len 30 angle 150\n"
            "pose p\n"
            "  head 8\n"
            "  armL 100\n");
        ok(sk.warnings.empty(), "no warning: the pose block did not end early");
        auto it = sk.poses.find("p");
        ok(it != sk.poses.end(), "the pose exists");
        ok(it->second.bones.count("head") == 1, "the head is posed");
        ok(it->second.bones.count("armL") == 1,
           "and so is everything written after it");
        ok(sk.bones.size() == 3, "and no phantom bone was declared");
    }

    section("degenerate input");
    {
        ok(rig::parse("").bones.empty(), "an empty file is an empty skeleton");
        rig::Rig r;
        r.setSkeleton(rig::parse(""));
        r.update(0.016f);
        ok(true, "updating an empty rig does not crash");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
