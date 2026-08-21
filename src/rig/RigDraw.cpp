#include "RigDraw.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#include "raylib.h"

namespace rig {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg2Rad = kPi / 180.0f;

// ONE RULE for how big a joint is drawn, everywhere on the skeleton.
//
// Not one radius: a knuckle is an order of magnitude smaller than a shoulder,
// and drawing them the same size turned the hand into a solid blob of
// overlapping circles -- the joints were bigger than the bones between them.
// The rule is the consistency; the pixels follow the bone.
//
// A joint belongs to the segment it terminates, so its size comes from that
// segment's length. Clamped at both ends so a zero-length bone still shows a
// joint and a long one does not grow a dinner plate.
// TAKES PIXELS, RETURNS PIXELS. It used to take rig units and apply the draw
// scale itself -- but the hand hands it lengths that have ALREADY been scaled,
// so every knuckle was scaled twice and came out about seven times too big.
// That is what made the hand a solid blob of overlapping circles, and what made
// two hands of identical size look like different sizes.
//
// One unit in, one unit out; the caller scales once, wherever it is.
float jointR(float segLenPx) {
    const float r = segLenPx * 0.13f;
    return r < 1.5f ? 1.5f : (r > 7.0f ? 7.0f : r);
}


// A distinct colour per bone index, with no palette to maintain.
//
// Golden-ratio hue stepping: consecutive indices land far apart on the wheel,
// so a forearm never comes out the same colour as the upper arm it grows from.
// A hand-written palette would have to be extended every time somebody adds a
// bone, and would be wrong the first time they forgot.
Color boneColor(int i, float alpha) {
    const float h = std::fmod(i * 0.6180339887f, 1.0f);
    const float r = std::fabs(h * 6.0f - 3.0f) - 1.0f;
    const float g = 2.0f - std::fabs(h * 6.0f - 2.0f);
    const float b = 2.0f - std::fabs(h * 6.0f - 4.0f);
    auto ch = [](float v) {
        // Lifted off full saturation so the labels stay readable on it.
        v = 0.35f + 0.65f * (v < 0 ? 0 : (v > 1 ? 1 : v));
        return (unsigned char)(v * 255.0f);
    };
    return ColorAlpha({ch(r), ch(g), ch(b), 255}, alpha);
}


// ─── A hand, without any art ──────────────────────────────────────────────
//
// The hand design says a hand is a CARD: a transform from the wrist, plus a
// pose class and a facing that together choose a drawing. Until those drawings
// exist, this stands in for them -- and it has to be an actual hand, because a
// schematic that reads wrong is worse than a box.
//
// PROPORTIONS, from a real hand and stated so they can be argued with:
//   the palm is a touch longer than it is wide, and TAPERS to the wrist
//   fingers are about as long as the palm; the middle one is longest
//   the knuckle line is RAKED -- the index knuckle sits further out than the
//     little finger's, which is most of why a hand is not a paddle
//   every finger has three visible bones, the thumb two
//
// WHAT THE CLASSES MEAN, which is the vocabulary an artist would draw to:
//   relaxed -- fingers loosely curled, the default at somebody's side
//   open    -- fingers extended and spread; the palm is the subject
//   fist    -- curled right back into the palm
//   point   -- index extended, the rest curled
void drawHandCardImpl(Vector2 c, float worldAngleDeg, float size,
                      const std::string& cls, float facing, float chir,
                      Color col, float alpha, bool labels) {
    const float rad = worldAngleDeg * kDeg2Rad;
    const Vector2 U{std::cos(rad), std::sin(rad)};   // wrist -> knuckles
    // The one screen perpendicular, U turned a QUARTER TURN ANTICLOCKWISE: for
    // a hand pointing up the screen this is screen-left, which is the side a
    // right hand's thumb is on when its palm is toward you. See thumbSide().
    // It was the other way round, which put both thumbs on the inside of a
    // raised pair of hands -- and flipping the axis rather than the individual
    // across values is deliberate, because the palm normal rides on this same
    // axis and the two have to mirror together or the fingers curl toward the
    // back of the hand at half the facings.
    const Vector2 W{U.y, -U.x};

    // ── THE ONLY 3D IN THE HAND, AND IT IS TWO NUMBERS ──────────────────────
    //
    // Hand space has three axes: U along the hand, V across the palm, N out of
    // it. U lies in the screen plane always. V and N have to share the single
    // screen direction perpendicular to U, and `facing` -- how far the hand has
    // rolled about its own length -- says how much of each survives:
    //
    //   across the palm  projects at  facing         (SIGNED: turning the hand
    //                                                 over swaps the thumb side)
    //   out of the palm  projects at  sqrt(1-f^2)    (the complement: nothing
    //                                                 when the palm is square
    //                                                 to you, everything when
    //                                                 it is edge-on)
    //
    // That complement is what the first version was missing, and it is the
    // whole of the orientation bug. Curl was applied as a rotation in the
    // SCREEN plane no matter which way the hand was turned -- so a fist held
    // palm-out swept its fingers sideways across the palm like a spiral, when
    // what a fist palm-out really does is fold them away behind the knuckles.
    // Fingers bend in the U-N plane, and U-N is precisely the plane that
    // vanishes as the palm turns to face the viewer.
    //
    // The sign of the N projection is a CONVENTION, not a measurement: `facing`
    // says how far the hand has rolled, never which way it rolled, and both
    // answers look the same face-on. It has to be a constant either way,
    // because sqrt(1-f^2) is largest exactly at f=0 -- a sign that flipped with
    // the sign of `facing` would flip it at the moment it is doing the most
    // work, and a hand mid-turn would snap its fingers to the other side.
    const float vProj = facing;
    const float nProj = std::sqrt(std::max(0.0f, 1.0f - facing * facing)) * -chir;

    // Hand space -> screen. `out` is toward the palm side; the fingers close
    // that way and the thumb crosses over it.
    auto at = [&](float along, float across, float out) {
        const float k = across * vProj + out * nProj;
        return Vector2{c.x + U.x * along + W.x * k, c.y + U.y * along + W.y * k};
    };

    const float palmLen   = size * 0.52f;
    const float palmHalf  = size * 0.21f;
    const float wristHalf = palmHalf * 0.76f;      // the palm tapers to the wrist
    const float fingerLen = size * 0.48f;
    // Fingers as wide as the gaps between them read as a garden fork; real
    // ones very nearly touch.
    const float thick     = std::max(1.8f, size * 0.075f);

    // A HAND HAS A THIRD DIMENSION AND IT IS NOT ZERO.
    //
    // Turned edge-on the palm's width projects to nothing, and the first
    // version drew exactly that: a wire. But a hand seen edge-on is not a line,
    // it is a hand seen edge-on -- perhaps two centimetres of it. That depth
    // occupies the SAME screen axis as the width and is at its widest exactly
    // where the width has vanished, so the two combine in quadrature and the
    // palm keeps a body at every angle.
    const float palmDeep = size * 0.055f;
    const float nAbs     = std::fabs(nProj);
    auto edge = [&](float half) {
        return std::sqrt(half * vProj * half * vProj + palmDeep * nAbs * palmDeep * nAbs);
    };
    const float hw0 = edge(wristHalf), hw1 = edge(palmHalf);

    // Palm: FILLED and tapered, not an outlined box. Four sticks off a
    // rectangle read as a garden fork; the same sticks off a solid tapered mass
    // read as a hand.
    auto side = [&](float along, float half) {
        return Vector2{c.x + U.x * along + W.x * half, c.y + U.y * along + W.y * half};
    };
    const Vector2 q0 = side(0.0f, -hw0),     q1 = side(0.0f, hw0);
    const Vector2 q2 = side(palmLen, hw1),   q3 = side(palmLen, -hw1);
    // WOUND THE WAY raylib WANTS, or there is no palm at all.
    //
    // DrawTriangle backface-culls, and these two were wound the other way --
    // so the fill was discarded, silently, at every angle and every facing.
    // Not intermittently: cross(W, U) is U.x^2 + U.y^2, which is 1 whichever
    // way the wrist is turned, so the winding is constant and was constantly
    // wrong. The palm has been a hollow wire loop since it was written, and
    // the comment above describing a solid tapered mass has never once
    // described what was on the screen.
    //
    // It reads worst on the THUMB, which is what gets reported: a thumb lying
    // alongside an outline, touching it at one end, is a loose stroke. Give
    // the palm its body back and the same thumb is a thumb on a hand.
    DrawTriangle(q0, q2, q1, ColorAlpha(col, 0.30f * alpha));
    DrawTriangle(q0, q3, q2, ColorAlpha(col, 0.30f * alpha));
    DrawLineEx(q0, q1, thick, col);
    DrawLineEx(q1, q2, thick, col);
    DrawLineEx(q2, q3, thick, col);
    DrawLineEx(q3, q0, thick, col);
    DrawCircleV(c, jointR(palmLen), col);          // the wrist

    // WHICH FACE AM I LOOKING AT. Chirality answers it only when you have both
    // hands to compare, and most of the time you have one. So the palm side
    // gets its crease and the back gets its knuckle line -- one stroke each,
    // fading out together as the hand turns edge-on and neither is visible.
    if (nAbs < 0.98f) {
        const float cue = (1.0f - nAbs) * alpha;
        if (facing > 0.0f) {                       // palm: the crease across it
            DrawLineEx(side(palmLen * 0.42f, -hw1 * 0.62f),
                       side(palmLen * 0.62f,  hw1 * 0.70f),
                       thick * 0.7f, ColorAlpha(col, 0.55f * cue));
        } else {                                   // back: the knuckle line
            DrawLineEx(side(palmLen * 0.90f, -hw1 * 0.86f),
                       side(palmLen * 0.98f,  hw1 * 0.86f),
                       thick * 0.7f, ColorAlpha(col, 0.45f * cue));
        }
    }

    // How curled each finger is, per class. A table rather than four branches,
    // because the classes differ in nothing else.
    //                     index  middle  ring   little  thumb
    // NOTHING IS EVER PERFECTLY STRAIGHT. An "open" hand with zero curl draws
    // four rigid spokes, which is the other half of why this read as a fork --
    // a real open hand still has a little bend in every finger.
    float curl[5] = {0.45f, 0.42f, 0.48f, 0.55f, 0.40f};        // relaxed
    if (cls == "open")       { curl[0]=0.14f; curl[1]=0.11f; curl[2]=0.13f; curl[3]=0.18f; curl[4]=0.16f; }
    else if (cls == "fist")  { curl[0]=curl[1]=curl[2]=curl[3]=1.00f; curl[4]=0.85f; }
    else if (cls == "point") { curl[0]=0.02f; curl[1]=curl[2]=curl[3]=1.00f; curl[4]=0.70f; }

    // A finger walks in hand space and is projected joint by joint, so the
    // bend and the roll compose instead of fighting: `psi` splays it within the
    // palm plane, `phi` closes it out of that plane.
    //
    // `taper` decays the splay along the chain. A finger leaves the knuckle on
    // one bearing and keeps it, so a finger passes 1. A THUMB does not: its
    // metacarpal strikes out across the palm at a steep angle and the two bones
    // past the web turn back up, which is the shape that says thumb. Without
    // it the thumb is a straight spar at a constant angle -- a fifth finger
    // pointing the wrong way, which is what it has read as all along.
    auto finger = [&](float baseAlong, float baseAcross, float len,
                      float amount, float splayDeg, const float* seg, int nseg,
                      float taper = 1.0f, float oppose = 0.0f,
                      float* webAlong = nullptr, Vector2* webAt = nullptr) {
        float psi = splayDeg * kDeg2Rad;
        float along = baseAlong, across = baseAcross, out = 0.0f, phi = 0.0f;
        Vector2 prev = at(along, across, out);
        DrawCircleV(prev, jointR(len * 0.42f), col);                   // knuckle
        const float bend[3] = {52.0f, 44.0f, 36.0f};
        for (int k = 0; k < nseg; ++k) {
            // WHICH WAY A DIGIT BENDS, WHICH IS NOT THE SAME QUESTION FOR A
            // THUMB AS FOR A FINGER.
            //
            // A finger closes straight out of the palm plane and nowhere else,
            // so all of its bend belongs to `phi`. A thumb does not: most of
            // what a thumb does is OPPOSITION -- it swings across the palm
            // toward the little finger, which happens IN the palm plane.
            //
            // Spending all of a thumb's bend on `phi` is invisible at exactly
            // the two facings where the hand is most readable. `out` projects
            // through nProj = sqrt(1 - f^2), which is ZERO at f = +-1: palm
            // square to the viewer, depth edge-on. So a thumb curled palm-on
            // did not curl. It RETRACTED -- 43 pixels of bend went into the one
            // axis that could not be seen, and all the eye got was the thumb
            // getting shorter, from 40 pixels to 11 between open and fist. A
            // digit that shortens instead of folding does not read as a thumb,
            // it reads as a stub, and that is the state it was reported in.
            //
            // `oppose` is how much of the bend goes across the palm instead of
            // out of it. Zero for the fingers, which is what a finger does.
            const float b = amount * bend[k] * kDeg2Rad;
            phi += b * (1.0f - oppose);
            // Across the palm is TOWARD THE OTHER SIDE, so it works against
            // the splay that carried the thumb out there: chir is which side
            // that was.
            psi -= b * oppose * chir;
            const float s = seg[k] * len;
            // A UNIT DIRECTION, which means the bend foreshortens the splay.
            //
            // `across` was taking sin(psi) flat, without the cos(phi) the other
            // two axes carry -- so the step was not a direction at all, it was
            // longer than the bone it stood for, by up to 11% on a thumb curled
            // into a fist. Which is where it showed: the more a digit curled
            // the further out it also swung, because the sideways part of the
            // step never gave anything up to the bend. A curling thumb has to
            // come BACK over the palm; this one struck out across it and kept
            // going, ending a fifth of a palm-length adrift and reading as a
            // spar that had come loose from the hand rather than as a thumb.
            along  += std::cos(phi) * std::cos(psi) * s;
            across += std::cos(phi) * std::sin(psi) * s;
            out    += std::sin(phi) * s;
            psi *= taper;
            const Vector2 next = at(along, across, out);
            // The first joint is the web, which the thenar needs. See below.
            if (k == 0) {
                if (webAlong) *webAlong = along;
                if (webAt) *webAt = next;
            }
            DrawLineEx(prev, next, thick, col);
            DrawCircleV(next, jointR(s) * (k + 1 == nseg ? 0.8f : 1.0f), col);
            prev = next;
        }
    };

    // Fingers FAN, and their knuckles are RAKED. Parallel fingers on a straight
    // knuckle line are a garden fork; a hand's spread outward and start from an
    // edge that slopes back toward the little finger.
    const float splay = (cls == "open") ? 14.0f : 6.0f;
    const float fseg[3] = {0.42f, 0.33f, 0.25f};
    const float rel[4]  = {0.94f, 1.0f, 0.92f, 0.76f};     // index..little
    for (int f = 0; f < 4; ++f) {
        const float t = (f / 3.0f);                        // 0 index .. 1 little
        // Index side is the THUMB side, so it follows chirality.
        const float across = chir * (0.74f - 1.48f * t) * palmHalf;
        const float rake   = palmLen * (1.0f - 0.22f * t); // knuckle line slopes back
        // (0.5 - t), NOT (t - 0.5). THE FAN WAS INSIDE OUT.
        //
        // `across` puts the index on the thumb side and the little finger
        // opposite it, and the splay has to carry each one FURTHER OUT from
        // there. Signed the other way it carried each one toward the other:
        // the index swung across toward the little finger and the little
        // finger back toward the index, so four fingers that started 34 pixels
        // apart at the knuckles ended 13 apart at the tips. They converged to
        // a point, and a hand whose fingers meet at a point is not read as a
        // hand at all -- it is a flame, or a leaf. Which is what it looked
        // like on screen, at every facing, for every class.
        //
        // The comment above has always said "a hand's spread outward". It now
        // also happens.
        finger(rake, across, fingerLen * rel[f], curl[f], chir * (0.5f - t) * 2.0f * splay,
               fseg, 3, 1.0f);
    }

    // The thumb leaves the SIDE of the palm and crosses in FRONT of it -- which
    // is the reason chirality has to exist at all.
    //
    // It is not a fifth finger and drawing it as one is what made it read as a
    // handle welded to the wrist: it starts a THIRD of the way up the palm, not
    // at the corner; it is shorter than any finger; and it is never straight.
    // A thumb at full extension still has a bend in it, so its curl has a floor
    // that the class can raise but not remove.
    {
        // A THUMB HAS THREE BONES AND THE FIRST ONE IS INSIDE THE PALM.
        //
        // Every version of this until now drew the two bones past the web and
        // hung them off the side of the palm, which is why it kept reading as a
        // stubby fin no matter where it was moved to or how long it was made.
        // What is missing is the METACARPAL -- the bone that runs from the
        // wrist out across the palm and puts the web where it belongs. Drawn,
        // the thumb leaves the wrist rather than sprouting from a flank, and
        // the joint the eye looks for is in the picture.
        //
        // The taper is what turns it: out hard on the metacarpal, then up.
        const float tseg[3] = {0.44f, 0.31f, 0.25f};
        // HOW MUCH OF THE BEND GOES ACROSS THE PALM RATHER THAN OUT OF IT --
        // AND IT IS NOT A CONSTANT.
        //
        // The floor below is not opposition. A thumb doing nothing still has a
        // bend in it, which is all the floor is saying, and that bend is the
        // thumb's own curve -- out of the palm, where a finger's bend goes.
        // Opposition is what CLOSING does: the reach across toward the little
        // finger. So it scales with how far past resting the class actually
        // closes, and an `open` hand keeps the thumb spread wide where it
        // belongs instead of tucking it against the palm.
        const float kThumbFloor = 0.30f;    ///< never quite straight
        const float kMaxOppose  = 0.95f;    ///< at a full fist, nearly all of it
        const float amt = std::max(kThumbFloor, curl[4]);
        const float closing =
            std::clamp((amt - kThumbFloor) / (1.0f - kThumbFloor), 0.0f, 1.0f);
        const float kThumbOppose = kMaxOppose * closing;
        const float baseAlong = palmLen * 0.04f;
        const float baseAcross = chir * wristHalf * 0.55f;
        float webAlong = 0.0f;
        Vector2 webAt{};
        // A THUMB IS LONGER THAN A FINGER, BECAUSE IT STARTS AT THE WRIST.
        //
        // 0.46 was a finger's length given to a chain that has one more bone in
        // it, and the shortfall all landed where it shows: splayed 44 degrees
        // and bent, the tip reached only 70% of the way up the palm. A thumb
        // held palm-on reaches the INDEX KNUCKLE -- the top of the palm -- and
        // one that stops well short of it does not read as a thumb at all, it
        // reads as a lump on the side of the hand.
        //
        // 0.66 is what puts the tip on that landmark once the splay and the
        // bend have taken their share. It is longer than fingerLen (0.48) and
        // has to be: a finger is drawn from its knuckle and this is drawn from
        // the wrist, with the whole metacarpal in between.
        finger(baseAlong, baseAcross, size * 0.66f,
               amt, chir * 44.0f, tseg, 3, 0.45f,
               kThumbOppose, &webAlong, &webAt);

        // ── THE THENAR: the mass that makes the thumb part of the hand ──
        //
        // A thumb held palm-on has nowhere to go. `out` -- the axis it crosses
        // in front of the palm on -- projects to exactly zero at facing +-1,
        // because that is what facing +-1 MEANS: the palm is square to the
        // viewer and depth is edge-on. So all that survives of "leaves the side
        // and crosses in front" is "leaves the side", and the two bones past
        // the web come to lie alongside the palm, a stroke's width out from its
        // edge and parallel to it. Nothing then joins them to the hand but the
        // metacarpal at the very bottom, and a line that runs beside a shape
        // touching it only at one end does not read as attached to it -- it
        // reads as a loose spar lying across the drawing. Which is exactly what
        // it was reported as.
        //
        // A real hand does not have that gap, and not because its thumb is
        // anywhere else: because the web is not empty. The thenar -- the mound
        // of muscle at the base of the thumb -- fills the whole triangle
        // between the metacarpal and the palm's edge, and it is the single
        // biggest lump of the hand seen palm-on. Drawn, the thumb emerges from
        // a mass instead of hanging off a line, and the gap it used to read
        // across is the shape of the hand rather than a hole in it.
        //
        // Filled at the palm's own weight and bounded by the palm's own edge,
        // so it is one silhouette with the palm and not a fin stuck onto it.
        const float tside = (chir * vProj) < 0.0f ? -1.0f : 1.0f;
        const float hwWeb = edge(wristHalf +
                                 (palmHalf - wristHalf) *
                                     std::clamp(webAlong / std::max(1e-3f, palmLen),
                                                0.0f, 1.0f));
        // BOTH WINDINGS, ON PURPOSE. The palm's winding is fixed -- cross(W, U)
        // is 1 whichever way the wrist is turned -- so it can be written down
        // once. This one is not: `tside` flips with chirality AND with the sign
        // of the facing, so which order is the front face changes underneath
        // it. Drawing each triangle both ways lets the cull pick, and costs two
        // discarded triangles rather than a thenar that vanishes on left hands.
        const Vector2 t0 = at(baseAlong, baseAcross, 0.0f);
        const Vector2 t1 = side(0.0f, tside * hw0);
        const Vector2 t2 = side(webAlong, tside * hwWeb);
        for (int pass = 0; pass < 2; ++pass) {
            const Vector2 a = pass ? t2 : t1, b = pass ? t1 : t2;
            DrawTriangle(t0, a, b, ColorAlpha(col, 0.30f * alpha));
            DrawTriangle(t0, pass ? webAt : t2, pass ? t2 : webAt,
                         ColorAlpha(col, 0.30f * alpha));
        }
    }

    if (labels && !cls.empty())
        DrawText(TextFormat("%s %+.2f", cls.c_str(), facing),
                 (int)(c.x + palmHalf + 8), (int)(c.y - 6), 10, col);
}

}  // namespace

void drawDebug(const Rig& rig, Vector2 origin, float scale, bool labels,
               float alpha, bool flip) {
    alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const Skeleton& sk = rig.skeleton();
    const auto& solved = rig.solved();
    // Mirroring happens HERE and nowhere else: one negated x at the point of
    // drawing, so no pose, no cloth step and no solver has to know which way
    // the character is facing.
    const float sx = flip ? -scale : scale;
    auto P = [&](Vec2 v) {
        return Vector2{origin.x + v.x * sx, origin.y + v.y * scale};
    };

    // Cloth first, behind the body -- a coat hangs from the shoulders and the
    // arms are in front of it.
    const auto& chains = rig.clothPoints();
    for (size_t ci = 0; ci < chains.size(); ++ci) {
        const Color col = boneColor(1000 + (int)ci, 0.75f * alpha);
        for (size_t k = 1; k < chains[ci].size(); ++k)
            DrawLineEx(P(chains[ci][k - 1]), P(chains[ci][k]), 3.0f * scale, col);
        for (const Vec2& p : chains[ci])
            DrawCircleV(P(p), 2.0f * scale, ColorAlpha(col, 0.9f));
    }

    for (size_t i = 0; i < solved.size(); ++i) {
        const Bone& b = sk.bones[i];
        const Solved& s = solved[i];
        const Color col = boneColor((int)i, alpha);

        if (b.length > 0.0f) {
            DrawLineEx(P(s.a), P(s.b), std::max(2.0f, 5.0f * scale), col);
            // The tip, so the direction a zero-length child grows in is visible.
            DrawCircleV(P(s.b), 3.0f * scale, ColorAlpha(col, 0.65f));
        }

        // THE JOINT IS THE THING BEING TESTED, so it is drawn last and biggest:
        // this whole mode exists to answer "is that elbow where I meant it".
        const float jr = jointR((b.length > 0.0f ? b.length : 20.0f) * scale);
        DrawCircleV(P(s.a), jr, col);
        DrawCircleLinesV(P(s.a), jr, ColorAlpha(WHITE, alpha));

        if (b.kind == Bone::Kind::Hand)
            drawHandCardImpl(P(s.b), flip ? 180.0f - s.worldAngle : s.worldAngle,
                     b.size * scale, s.handClass,
                     flip ? -s.facing : s.facing,
                     // Mirroring the whole rig turns a right hand into a left
                     // one -- the thumb has to move with everything else.
                     flip ? -b.chir : b.chir, col, alpha, labels);

        if (b.kind == Bone::Kind::Head) {
            const Vector2 c = P(s.b);
            const float r = std::max(6.0f, b.size * 0.5f * scale);
            DrawCircleLinesV(c, r, col);
            // Eyes, so blink and gaze are visible without any art either.
            const Vec2 g = rig.gaze();
            const float lid = 1.0f - rig.blink();
            for (int e = -1; e <= 1; e += 2) {
                const Vector2 ec{c.x + e * r * 0.34f + g.x * r * 0.16f,
                                 c.y - r * 0.12f + g.y * r * 0.16f};
                DrawEllipse((int)ec.x, (int)ec.y,
                            r * 0.16f, std::max(0.6f, r * 0.16f * lid), col);
            }
        }

        if (labels)
            DrawText(b.name.c_str(), (int)(P(s.a).x + 7), (int)(P(s.a).y - 5), 10,
                     ColorAlpha(col, 0.95f));
    }
}


// ─── The character, drawn as a person ──────────────────────────────────────
//
// WHAT MAKES A DRAWING READ, AND WHY THE FIRST ATTEMPT DID NOT
//
// The first version of this filled the space between the two cloth chains and
// stuck capsules on for arms. Everything was in the right PLACE and it looked
// like a sandwich board with pipes attached, because a figure is not read off
// its joint positions -- it is read off its silhouette, and the silhouette is
// made of four things this had none of:
//
//   THE SHOULDER SLOPE. A coat's top edge falls away from the neck. Run it
//   straight across between the shoulder joints and the result is a table with
//   a head balanced on it, which is exactly what happened.
//
//   TAPER. Nothing on a body is a cylinder. A sleeve is wide at the shoulder
//   and narrow at the wrist, and a constant-width limb reads as plumbing no
//   matter how well it is articulated.
//
//   THE LAPEL V. On an overcoat this is the largest shape after the coat
//   itself and it is what says "overcoat" rather than "sack" -- two wide
//   peaked facings meeting low on the chest, with the shirt as a broad wedge
//   between them.
//
//   A HAT THAT IS NOT A BOX. A fedora is a curved brim and a pinched crown.
//   Two rectangles read as a chimney.
//
// So this builds outlined POLYGONS, in the order a person is stacked, and the
// cloth chains drive only the parts of the outline that hang -- the coat's
// sides and hem -- which is what keeps the solver's work in the picture.

namespace {

/// raylib culls a triangle wound the wrong way -- see the palm's note. The
/// winding here varies with the pose, so both orders go in and the cull picks.
void fillTri(Vector2 a, Vector2 b, Vector2 c, Color col) {
    DrawTriangle(a, b, c, col);
    DrawTriangle(a, c, b, col);
}

/// Two triangles. Small and local, so it holds for any shape the cloth takes.
void fillQuad(Vector2 a, Vector2 b, Vector2 c, Vector2 d, Color col) {
    fillTri(a, b, c, col);
    fillTri(a, c, d, col);
}

/// A filled polygon, fanned from its own centroid -- valid only for a shape you
/// can see every edge of from the middle. Fine for a head or a lapel; NOT fine
/// for a coat mid-swing, which is why the coat is filled as a strip instead.
void fillPoly(const std::vector<Vector2>& p, Color col) {
    if (p.size() < 3) return;
    Vector2 c{0.0f, 0.0f};
    for (const Vector2& v : p) { c.x += v.x; c.y += v.y; }
    c.x /= (float)p.size();
    c.y /= (float)p.size();
    for (size_t i = 0; i < p.size(); ++i)
        fillTri(c, p[i], p[(i + 1) % p.size()], col);
}

/// The ink round a shape. Circles at the corners so the joins are round rather
/// than mitred into spikes.
void inkPoly(const std::vector<Vector2>& p, float w, Color col, bool closed = true) {
    if (p.size() < 2) return;
    const size_t n = closed ? p.size() : p.size() - 1;
    for (size_t i = 0; i < n; ++i) DrawLineEx(p[i], p[(i + 1) % p.size()], w, col);
    for (const Vector2& v : p) DrawCircleV(v, w * 0.5f, col);
}

/// A tapered limb: wide at `a`, narrow at `b`, with round ends.
void limbShape(Vector2 a, Vector2 b, float wa, float wb, Color col) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) { DrawCircleV(a, wa * 0.5f, col); return; }
    const Vector2 n{-dy / len, dx / len};
    fillPoly({{a.x + n.x * wa * 0.5f, a.y + n.y * wa * 0.5f},
              {b.x + n.x * wb * 0.5f, b.y + n.y * wb * 0.5f},
              {b.x - n.x * wb * 0.5f, b.y - n.y * wb * 0.5f},
              {a.x - n.x * wa * 0.5f, a.y - n.y * wa * 0.5f}}, col);
    DrawCircleV(a, wa * 0.5f, col);
    DrawCircleV(b, wb * 0.5f, col);
}

/// ...inked by drawing it once oversized in the ink colour and once on top in
/// its own. Cheaper and cleaner than tracing the outline of a tapered shape.
void limb(Vector2 a, Vector2 b, float wa, float wb, Color fill, Color ink, float w) {
    limbShape(a, b, wa + w * 2.0f, wb + w * 2.0f, ink);
    limbShape(a, b, wa, wb, fill);
}

/// Textures, loaded once and kept. Keyed by full path, so two characters that
/// share a part share the texture.
std::unordered_map<std::string, Texture2D>& partCache() {
    static std::unordered_map<std::string, Texture2D> c;
    return c;
}

/// Missing files are remembered too, or a character with a typo'd part name
/// hammers the filesystem once per frame for the rest of the session.
const Texture2D* partTexture(const std::string& path) {
    auto& c = partCache();
    auto it = c.find(path);
    if (it == c.end()) {
        Texture2D t{};
        if (FileExists(path.c_str())) {
            t = LoadTexture(path.c_str());
            if (t.id != 0) SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
        } else {
            TraceLog(LOG_WARNING, "RIG: part image not found: %s", path.c_str());
        }
        it = c.emplace(path, t).first;
    }
    return it->second.id != 0 ? &it->second : nullptr;
}

/// One drawn part, placed on its bone.
///
/// `joint` is the bone's origin and the point the image turns about; `pivot` is
/// where in the image that point is. Everything else is the same transform the
/// procedural shapes get, so art and no-art parts sit in the same places.
void drawPart(const Texture2D& t, const Part& pt, Vector2 joint, float angleDeg,
              float scale, bool flip, float alpha) {
    const float k = scale / pt.unit;
    const float w = t.width * k, h = t.height * k;
    // Mirrored art needs a mirrored pivot too, or a part with its joint off
    // centre jumps sideways the moment the character turns round.
    const float pu = flip ? (1.0f - pt.pivotU) : pt.pivotU;
    const Rectangle src{0.0f, 0.0f, flip ? -(float)t.width : (float)t.width,
                        (float)t.height};
    const Rectangle dst{joint.x, joint.y, w, h};
    const Vector2 org{pu * w, pt.pivotV * h};
    DrawTexturePro(t, src, dst, org, angleDeg + pt.turn,
                   ColorAlpha(WHITE, alpha));
}

}  // namespace

void drawSkin(const Rig& rig, Vector2 origin, float scale, float alpha, bool flip) {
    const Skeleton& sk = rig.skeleton();
    const Look& lk = sk.look;
    if (!lk.drawn) { drawDebug(rig, origin, scale, false, alpha, flip); return; }

    const auto& solved = rig.solved();
    if (solved.empty()) return;
    alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);

    // Mirroring happens HERE and nowhere else, as in drawDebug: one negated x
    // at the point of drawing, so nothing upstream knows which way it faces.
    const float sx = flip ? -scale : scale;
    auto P = [&](Vec2 v) { return Vector2{origin.x + v.x * sx, origin.y + v.y * scale}; };
    auto C = [&](Rgb c, float a = 1.0f) {
        return ColorAlpha(Color{c.r, c.g, c.b, 255}, a * alpha);
    };
    auto lerp = [](Vector2 a, Vector2 b, float t) {
        return Vector2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    };
    auto norm = [](Vector2 v) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y);
        return l < 1e-4f ? Vector2{0.0f, 0.0f} : Vector2{v.x / l, v.y / l};
    };

    // Everything is measured in HEAD SIZES, so one `look` serves builds as
    // different as the ones in data/characters without a scale per rig.
    int headIdx = -1;
    const int chestIdx = sk.indexOf("chest");
    for (size_t i = 0; i < sk.bones.size(); ++i)
        if (sk.bones[i].kind == Bone::Kind::Head) headIdx = (int)i;
    const float H = ((headIdx >= 0 && sk.bones[headIdx].size > 0.0f)
                   ? sk.bones[headIdx].size : 40.0f) * scale;
    const float ink = std::max(1.2f, lk.ink * H);
    const Color inkC = C(lk.line);

    // The two bones above each hand are the arm -- walked from the hand rather
    // than matched by name, because a name is a convention and a parent chain
    // is a fact.
    std::vector<int> arm;
    for (size_t i = 0; i < sk.bones.size(); ++i) {
        if (sk.bones[i].kind != Bone::Kind::Hand) continue;
        int p = sk.bones[i].parent;
        for (int up = 0; up < 2 && p >= 0; ++up) { arm.push_back(p); p = sk.bones[p].parent; }
    }
    auto isArm = [&](int i) {
        for (int a : arm) if (a == i) return true;
        return false;
    };

    // ── Legs, behind everything, tapered into trousers ──
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < sk.bones.size(); ++i) {
            const Bone& b = sk.bones[i];
            if (b.length <= 0.0f || b.kind != Bone::Kind::Bone || isArm((int)i)) continue;
            if ((int)i == chestIdx) continue;
            if (headIdx >= 0 && sk.bones[headIdx].parent == (int)i) continue;  // neck: later
            if (pass == 0)
                limbShape(P(solved[i].a), P(solved[i].b), H * 0.26f + ink * 2.0f,
                          H * 0.22f + ink * 2.0f, inkC);
            else
                limbShape(P(solved[i].a), P(solved[i].b), H * 0.26f, H * 0.22f, C(lk.coat));
        }
    }

    const auto& chains = rig.clothPoints();
    const bool coated = chains.size() >= 2 && chains[0].size() > 1 &&
                        chains[0].size() == chains[1].size();

    if (coated && chestIdx >= 0) {
        const auto& L = chains[0];
        const auto& R = chains[1];
        const Vector2 neck = P(solved[chestIdx].b);
        const Vector2 shL = P(L[0]), shR = P(R[0]);

        // THE SHOULDER LINE FALLS AWAY FROM THE NECK, and the coat is cut a
        // little wider than the joint it hangs on -- padding, and the reason a
        // coat has a shape of its own rather than the shape of the body.
        const Vector2 outL{shL.x + norm({shL.x - neck.x, shL.y - neck.y}).x * H * 0.16f,
                           shL.y + norm({shL.x - neck.x, shL.y - neck.y}).y * H * 0.16f};
        const Vector2 outR{shR.x + norm({shR.x - neck.x, shR.y - neck.y}).x * H * 0.16f,
                           shR.y + norm({shR.x - neck.x, shR.y - neck.y}).y * H * 0.16f};
        const Vector2 nkL = lerp(neck, shL, 0.30f);
        const Vector2 nkR = lerp(neck, shR, 0.30f);

        // The silhouette: down the neck, over the shoulder, along whatever the
        // cloth solver is doing with that side, across the hem, and back.
        // THE SILHOUETTE IS WHERE THE CLOTH IS. No inset, no fudge.
        //
        // This used to draw the sides a little inside the chains to fake a
        // waist, because two chains hung off two shoulders fall straight down
        // and a coat does not. That is now done where it belongs -- the panels
        // are joined below the armhole and narrower than the shoulders, so the
        // simulation makes the shape and the drawing just follows it. A picture
        // that disagrees with the physics disagrees differently in every pose.
        std::vector<Vector2> body;
        body.push_back(nkL);
        body.push_back(outL);
        for (size_t k = 1; k < L.size(); ++k) body.push_back(P(L[k]));
        for (size_t k = R.size(); k-- > 1;) body.push_back(P(R[k]));
        body.push_back(outR);
        body.push_back(nkR);
        // FILLED AS A STRIP, INKED AS AN OUTLINE.
        //
        // fillPoly fans from the centroid, which is only valid for a shape you
        // can see every edge of from the middle. A coat swinging on a gesture
        // stops being one -- the hem folds past the waist -- and the fan then
        // lays triangles across the gap and cuts notches out of the silhouette.
        //
        // Between two chains there is no such problem: pair the points up and
        // every quad is small and local, whatever shape the whole thing has
        // taken. The outline is a line loop and never cared either way.
        for (size_t k = 0; k + 1 < L.size(); ++k)
            fillQuad(P(L[k]), P(R[k]), P(R[k + 1]), P(L[k + 1]), C(lk.coat));
        // ...and the collar notch above the shoulders, which is not between the
        // chains because it is where the coat stops and the neck begins.
        fillQuad(nkL, outL, P(L[0]), P(R[0]), C(lk.coat));
        fillQuad(nkL, P(R[0]), outR, nkR, C(lk.coat));
        inkPoly(body, ink * 1.7f, inkC);

        // ── The lapel V, and the shirt showing between ──
        //
        // The coat closes low, which is what leaves room for the V. Both are
        // built off the same two points so they cannot drift apart.
        const size_t deep = std::max<size_t>(1, (size_t)(L.size() * 0.55f));
        const Vector2 closeL = lerp(P(L[deep]), P(R[deep]), 0.40f);
        const Vector2 closeR = lerp(P(L[deep]), P(R[deep]), 0.60f);
        const Vector2 close = lerp(closeL, closeR, 0.5f);

        std::vector<Vector2> shirt{nkL, nkR, closeR, close, closeL};
        fillPoly(shirt, C(lk.shirt));
        inkPoly(shirt, ink, inkC);

        // Peaked facings: shoulder, out to the peak, down to the closing point.
        const Vector2 peakL = lerp(outL, closeL, 0.42f);
        const Vector2 peakR = lerp(outR, closeR, 0.42f);
        std::vector<Vector2> lapL{nkL, lerp(nkL, outL, 0.85f),
                                  lerp(peakL, outL, 0.30f), closeL};
        std::vector<Vector2> lapR{nkR, lerp(nkR, outR, 0.85f),
                                  lerp(peakR, outR, 0.30f), closeR};
        fillPoly(lapL, C(lk.lapel));
        fillPoly(lapR, C(lk.lapel));
        inkPoly(lapL, ink * 1.2f, inkC);
        inkPoly(lapR, ink * 1.2f, inkC);
    }

    // ── Sleeves: ALL THE INK, THEN ALL THE FILL ──
    //
    // An arm is two bones and one sleeve. Inking and filling each bone in turn
    // draws the forearm's outline ON TOP OF the upper arm's fill, which puts a
    // black bar across the inside of every elbow -- the joint reads as a cut
    // rather than as a bend, and it gets worse the more the elbow closes.
    //
    // Two passes over the whole set fixes it for nothing: the ink of any
    // segment can only ever be covered by fill, so the only outline left
    // standing is the one round the outside.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < sk.bones.size(); ++i) {
            if (!isArm((int)i) || sk.bones[i].length <= 0.0f) continue;
            const bool upper = (sk.bones[i].parent >= 0 && !isArm(sk.bones[i].parent));
            const float wa = H * (upper ? 0.36f : 0.28f);
            const float wb = H * (upper ? 0.28f : 0.21f);
            if (pass == 0)
                limbShape(P(solved[i].a), P(solved[i].b), wa + ink * 2.0f,
                          wb + ink * 2.0f, inkC);
            else
                limbShape(P(solved[i].a), P(solved[i].b), wa, wb, C(lk.coat));
        }
    }
    // A cuff of shirt at each wrist, which is where the drawing puts its only
    // white below the collar.
    for (size_t i = 0; i < sk.bones.size(); ++i) {
        if (sk.bones[i].kind != Bone::Kind::Hand) continue;
        const int fore = sk.bones[i].parent;
        if (fore < 0) continue;
        const Vector2 w = P(solved[fore].b), e = P(solved[fore].a);
        limb(lerp(w, e, 0.20f), w, H * 0.23f, H * 0.23f, C(lk.shirt), inkC, ink);
    }

    // ── The neck, over the coat, so the head has something to sit on ──
    if (headIdx >= 0 && sk.bones[headIdx].parent >= 0) {
        const int nk = sk.bones[headIdx].parent;
        if (sk.bones[nk].length > 0.0f)
            limb(P(solved[nk].a), P(solved[nk].b), H * 0.30f, H * 0.26f,
                 C(lk.skin), inkC, ink);
    }

    // ── The head, in the head bone's own frame so it tilts with it ──
    if (headIdx >= 0) {
        const Vector2 c = P(solved[headIdx].b);
        const float r = H * 0.5f;
        const float ha = (flip ? 180.0f - solved[headIdx].worldAngle
                               : solved[headIdx].worldAngle) * kDeg2Rad;
        const Vector2 up{std::cos(ha), std::sin(ha)};      // neck -> crown
        const Vector2 rt{-up.y, up.x};
        // `up` runs neck -> crown, so a POSITIVE second argument is toward the
        // top of the head. Written the other way round first, which put a whole
        // face upside down inside a head that was the right way up.
        auto at = [&](float across, float along) {
            return Vector2{c.x + rt.x * across * r + up.x * along * r,
                           c.y + rt.y * across * r + up.y * along * r};
        };

        // A skull is taller than it is wide. A circle reads as a balloon.
        // A FACE IS DRAWN WITH A FINER PEN THAN A SILHOUETTE. At the body's
        // ink weight an eyelid line covers a third of the eye it belongs to,
        // and the whole face closes up into a smudge.
        const float fine = ink * 0.62f;
        std::vector<Vector2> skull;
        for (int i = 0; i < 26; ++i) {
            const float t = (float)i / 26.0f * 2.0f * kPi;
            skull.push_back(at(std::cos(t) * 1.00f, std::sin(t) * 1.06f));
        }
        fillPoly(skull, C(lk.skin));
        inkPoly(skull, ink * 1.5f, inkC);

        // Eyes, big and low-lidded. The lid is the whole expression: this one
        // has been awake too long, so it sits low even wide open and blink()
        // only closes it the rest of the way.
        const Vec2 g = rig.gaze();
        const float shut = 0.30f + 0.70f * rig.blink();
        for (int e = -1; e <= 1; e += 2) {
            const float ex = e * 0.44f, ey = 0.02f;
            std::vector<Vector2> eye;
            for (int i = 0; i < 20; ++i) {
                const float t = (float)i / 20.0f * 2.0f * kPi;
                eye.push_back(at(ex + std::cos(t) * 0.25f, ey + std::sin(t) * 0.21f));
            }
            fillPoly(eye, Color{246, 246, 250, (unsigned char)(255 * alpha)});
            const Vector2 pc = at(ex + g.x * 0.09f, ey + g.y * 0.05f);
            DrawCircleV(pc, r * 0.125f, C(lk.line));
            inkPoly(eye, fine, inkC);
            // The lid: a slab of skin over the top of the eye, inked along its
            // lower edge so it reads as an eyelid and not as a shadow.
            const float top = ey + 0.26f, low = top - 0.34f * shut;
            std::vector<Vector2> lid{at(ex - 0.29f, top), at(ex + 0.29f, top),
                                     at(ex + 0.29f, low), at(ex - 0.29f, low)};
            fillPoly(lid, C(lk.skin));
            DrawLineEx(at(ex - 0.26f, low), at(ex + 0.26f, low), fine * 1.3f, inkC);
            // A brow, angled down toward the nose. Thin: at the body's weight
            // it becomes a scowl rather than a look of long-suffering.
            DrawLineEx(at(ex + e * 0.26f, 0.40f), at(ex - e * 0.20f, 0.33f), fine * 1.2f, inkC);
        }
        // A mouth that is not enjoying this.
        // ── The mouth, as the RIG has it this frame ──
        //
        // Not a fixed expression drawn here. `mouth` runs -1 (corners down)
        // through 0 (flat) to +1 (up) and `jaw` 0..1 open, both authored per
        // character and per pose and both interpolated by the solver -- so
        // this only has to turn two numbers into a shape.
        //
        // The corners are FIXED and the middle moves. Curving both would swing
        // the whole mouth up and down the face like a hoop; a real one is
        // pinned at the corners and everything happens between them.
        const float mo = solved[headIdx].mouth;
        const float jaw = solved[headIdx].mouthOpen;
        const float cy = -0.40f;                    // the corner line
        // MINUS. A frown drops the corners and lifts the MIDDLE, so a negative
        // `mouth` has to raise `my` above the corner line, not lower it. Plus
        // here draws a smile for every sad number in the file, which is what
        // it did until somebody looked at seven faces in a row.
        const float my = cy - mo * 0.13f;           // and where the middle goes
        if (jaw > 0.02f) {
            // Open: the same curve as the upper lip, with a lower one dropped
            // away from it, and the gap between them filled dark.
            const float dy = jaw * 0.26f;
            std::vector<Vector2> gap{
                at(-0.28f, cy), at(-0.10f, my), at(0.12f, my), at(0.30f, cy),
                at(0.12f, my - dy), at(-0.10f, my - dy)};
            fillPoly(gap, C(lk.line));
            inkPoly(gap, fine, inkC);
        } else {
            DrawLineEx(at(-0.28f, cy), at(-0.10f, my), fine * 1.3f, inkC);
            DrawLineEx(at(-0.10f, my), at(0.12f, my), fine * 1.3f, inkC);
            DrawLineEx(at(0.12f, my), at(0.30f, cy), fine * 1.3f, inkC);
        }

        if (lk.hasHat) {
            const float bw = lk.brim;
            // Crown: tapered, with the pinch along the top that stops a fedora
            // reading as a chimney.
            std::vector<Vector2> crown{
                at(-0.74f, 0.66f), at(-0.70f, 1.36f), at(-0.46f, 1.60f),
                at(-0.15f, 1.50f), at(0.15f, 1.60f), at(0.46f, 1.60f),
                at(0.70f, 1.36f), at(0.74f, 0.66f)};
            fillPoly(crown, C(lk.hat));
            inkPoly(crown, ink * 1.4f, inkC);
            std::vector<Vector2> band{at(-0.745f, 0.70f), at(0.745f, 0.70f),
                                      at(0.725f, 0.94f), at(-0.725f, 0.94f)};
            fillPoly(band, C(lk.band));
            inkPoly(band, ink, inkC);
            // Brim: a lens, wider than the head and turned down at the ends.
            // The one shape that has to be a curve; as a rectangle it is a
            // plank through the character's forehead.
            std::vector<Vector2> brim{
                at(-bw, 0.62f), at(-bw * 0.66f, 0.44f), at(0.0f, 0.38f),
                at(bw * 0.66f, 0.44f), at(bw, 0.62f), at(bw * 0.70f, 0.78f),
                at(0.0f, 0.84f), at(-bw * 0.70f, 0.78f)};
            fillPoly(brim, C(lk.hat));
            inkPoly(brim, ink * 1.2f, inkC);
        }
    }

    // ── DRAWN ART, over the procedural shapes it replaces ──
    //
    // Over rather than instead-of, deliberately: a part list that is half
    // finished still draws a whole character, with the pieces that exist as
    // art and the rest as shapes. That is what makes it possible to draw one
    // arm, look at it on the actual body in the actual pose, and decide
    // whether the next one is worth drawing.
    //
    // In file order, so the .odrig controls what overlaps what -- later parts
    // are in front, which is the only stacking rule there is.
    for (const Part& pt : sk.parts) {
        const int b = sk.indexOf(pt.bone);
        if (b < 0 || b >= (int)solved.size()) continue;
        const Texture2D* tex = partTexture(sk.dir + pt.file);
        if (!tex) continue;
        drawPart(*tex, pt, P(solved[b].a),
                 flip ? 180.0f - solved[b].worldAngle : solved[b].worldAngle,
                 scale, flip, alpha);
    }

    // ── Hands last, so one raised to the face is in front of it ──
    for (size_t i = 0; i < sk.bones.size(); ++i) {
        const Bone& b = sk.bones[i];
        if (b.kind != Bone::Kind::Hand) continue;
        drawHandCardImpl(P(solved[i].b),
                         flip ? 180.0f - solved[i].worldAngle : solved[i].worldAngle,
                         b.size * scale, solved[i].handClass,
                         flip ? -solved[i].facing : solved[i].facing,
                         flip ? -b.chir : b.chir, C(lk.skin), alpha, false);
    }
}


void drawHandCard(Vector2 wrist, float worldAngleDeg, float size,
                  const std::string& cls, float facing, float chir,
                  Color col, float alpha, bool labels) {
    drawHandCardImpl(wrist, worldAngleDeg, size, cls, facing, chir, col, alpha, labels);
}

}  // namespace rig
