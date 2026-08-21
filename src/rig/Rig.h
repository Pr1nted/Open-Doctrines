#pragma once

// A 2D character rig: bones, named poses, and everything that keeps a still
// drawing from looking dead.
//
// ─── WHAT THIS IS FOR ────────────────────────────────────────────────────
//
// A dialogue page names a pose. The character moves into it, holds it, and
// breathes while it waits for the player to read. That is the whole contract:
// no timeline, no keyframe editor, no animator. Poses are named, dialogue picks
// one, and everything between two poses is interpolation.
//
// ─── THE HAND PROBLEM, WHICH IS THE ONLY HARD ONE ────────────────────────
//
// A hand is the part of a character that is seen from the most angles, and in
// 2D an angle is a different DRAWING. You cannot rotate a flat palm and get the
// edge of a hand. The obvious answers are all bad:
//
//   Rotate the sprite      -- a palm seen edge-on is not a rotated palm.
//   Hard-swap drawings     -- pops, and the pop lands exactly where the eye is.
//   Cross-fade drawings    -- two hands dissolving through each other, which
//                             reads as a ghost rather than as a hand turning.
//   Go 3D                  -- excluded, and it would not match the art.
//
// WHAT THIS DOES INSTEAD: separate the TRANSFORM from the APPEARANCE.
//
//   The transform -- where the hand is, how big, how rotated -- comes from the
//   wrist bone and is fully continuous. It interpolates like every other bone.
//
//   The appearance -- which drawing -- is discrete, chosen from an atlas by a
//   POSE CLASS (open, fist, point, relaxed…) and a FACING scalar running -1
//   (back of hand toward the viewer) through 0 (edge-on) to +1 (palm toward
//   the viewer). The atlas holds a handful of drawings at fixed facing keys.
//
//   `facing` interpolates continuously between poses like an angle does. The
//   drawing it selects changes in steps. And the step is invisible, because it
//   happens WHILE THE WRIST IS ROTATING -- the eye reads a swap that occurs
//   during motion as the thing turning. A swap on a stationary hand is a pop; a
//   swap mid-gesture is a hand.
//
// So the art stays hand-drawn and flat, the motion stays continuous, and the
// two never fight. The same trick works for a head that needs a three-quarter
// and a profile view, which is why Bone::Kind has Head in it.
//
// ─── AND EVERYTHING ELSE THAT MAKES IT LOOK ALIVE ────────────────────────
//
// A held pose is a corpse. Every pose is therefore a BASE that four additive
// layers move on top of, none of which the pose author writes:
//
//   breath  -- slow rise and fall through chest and shoulders
//   sway    -- weight shifting between feet, much slower than breath
//   gaze    -- the head and eyes drifting to a new point every few seconds
//   blink   -- a discrete channel, Poisson-timed with a refractory period
//
// plus cloth: verlet chains hanging off named bones, so a coat moves because
// the shoulder it hangs from moved.
//
// All of it is DETERMINISTIC -- driven by a hash of (bone, layer, time) rather
// than rand(). Same reason the dialogue effects are: a recorded timelapse and a
// multiplayer session must both show the same character, and none of this may
// touch the simulation's random stream.
//
// ─── TESTING ─────────────────────────────────────────────────────────────
//
// The rig solves to line segments and joint points, which is a thing you can
// draw without any art at all. RigDraw's debug mode gives every bone its own
// colour, so a skeleton can be built, posed and checked long before anybody
// draws a character to hang on it. See data/characters/test/.
//
// Nothing in this header knows about raylib: parsing and solving are the parts
// with edge cases, and tests/rig_test.cpp exercises them without a window.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rig {

struct Vec2 { float x = 0, y = 0; };

/** How a pose eases in. Named in the file, so it is an authoring decision. */
enum class Ease { Linear, OutQuad, InOutQuad, OutBack };

struct Bone {
    std::string name;
    int   parent = -1;      ///< index, -1 for the root
    float length = 0;       ///< pixels at rig scale
    float restAngle = 0;    ///< degrees, relative to the parent

    /// What is drawn at this bone. See the hand note above for why a Hand is
    /// not just a bone with a sprite on it.
    enum class Kind { Bone, Hand, Head } kind = Kind::Bone;
    std::string atlas;      ///< Hand/Head: which drawing set
    float size = 0;         ///< Hand/Head: draw size in pixels
    /**
     * Hands only: +1 a right hand, -1 a left one.
     *
     * A hand is not symmetric and the difference is the one feature a viewer
     * reads without being told: the thumb is on the inside. Draw both hands
     * from the same template and one of them is always a left hand on a right
     * arm, which is the sort of wrong that nobody can name but everybody sees.
     *
     * Authored with `side left` / `side right`, and otherwise taken from a
     * trailing L or R in the name -- because every rig that has two of a thing
     * already names them that way, and a field you have to remember to set is
     * a field that gets forgotten.
     */
    float chir = 1.0f;

    /// Which additive layers touch this bone, and how strongly. Authored,
    /// because "which bones breathe" is a property of the character.
    float breath = 0;
    float sway   = 0;
    float gaze   = 0;

    /**
     * Heads only: the mouth this character wears when nothing is happening.
     *
     * A RESTING EXPRESSION IS A PROPERTY OF THE PERSON, not of the drawing and
     * not of the pose. The first version of the mouth was three line segments
     * hard-coded in RigDraw, which meant every character in the game had the
     * same one for ever and no pose could change it -- a rig that can raise an
     * eyebrow's worth of arm but cannot stop frowning.
     *
     * Authored `mouth -0.35 jaw 0`. `mouth` runs -1 (down at the corners)
     * through 0 (flat) to +1 (up); `jaw` runs 0 (shut) to 1 (wide). Poses move
     * both; this is where they return to, the same way restAngle is where a
     * limb returns to.
     *
     * `jaw` rather than `open` because `open` is already a hand class, and one
     * flat namespace means a key has to be unique across every line in it.
     */
    float mouth = 0.0f;
    float mouthOpen = 0.0f;

    /**
     * How thick this bone is to CLOTH, in rig units. 0 -- the default -- means
     * cloth passes straight through it.
     *
     * A coat is not a curtain hung off a shoulder. It is worn, and most of what
     * makes it read as worn is what it CANNOT do: it cannot pass through the
     * chest, and when a leg swings forward the coat in front of it has to go
     * somewhere. Without this the panels hang in the same place whatever the
     * body underneath is doing, which is a flag on a pole.
     *
     * Authored on the bones a garment actually wraps -- chest, hips, legs --
     * and left at zero on the arms, which hang outside a coat rather than
     * inside it.
     */
    float collide = 0.0f;
};

/** A verlet chain hanging off a bone: a coat hem, a scarf, hair. */
struct Cloth {
    std::string name;
    int   parent = -1;
    int   segments = 4;
    float segLen = 10.0f;
    float stiffness = 0.7f;   ///< 0..1, how hard constraints pull
    float damping = 0.92f;    ///< velocity retained per step
    float gravity = 420.0f;   ///< px/s^2
    /**
     * How much of the parent's motion the chain is LEFT BEHIND by, authored
     * with `drag`.
     *
     * 1 is a chain that keeps its own place in the world while the bone moves
     * out from under it -- pure inertia, and the default. Below 1 some of the
     * bone's motion is carried by the whole chain, so it travels WITH the body
     * instead of trailing, and 0 is a coat painted on. Above 1 the chain is
     * carried backwards: it trails further than merely standing still would
     * leave it, which is what makes a hem crack on a fast gesture rather than
     * swing politely behind one.
     *
     * The carry moves the chain's PREVIOUS positions along with it, so it
     * changes where the cloth is and not how fast it is going -- a body that
     * walks on and stops leaves its coat hanging, not swinging. Which also
     * means it composes with the wind rather than competing with it: the wind
     * is an acceleration on the same points, and neither reads the other.
     */
    float drag = 1.0f;
    /**
     * How much of the wind this garment catches, authored with `wind`.
     *
     * A heavy greatcoat and a scarf hang off the same solver and should not
     * answer the same gust the same way, and the alternative -- tuning the
     * wind until the coat looks right and finding the scarf now behaves like
     * canvas -- makes the wind a property of whoever was drawn last.
     */
    float windCatch = 1.0f;
    /**
     * The other half of the same garment, and how far apart the two are allowed
     * to get. Authored `pair coatR width 46`.
     *
     * TWO CHAINS ARE NOT A COAT. Hung separately off the two shoulders they
     * fall straight down and stay shoulder-width apart for their whole length,
     * because nothing joins them -- which is why the first version had to fake
     * a waist in the renderer by drawing the silhouette inside where the
     * physics actually was. A drawing that disagrees with the simulation is a
     * drawing that will disagree with it differently in every pose.
     *
     * A garment is one piece of cloth: the panels are joined, and below the
     * armhole the join is narrower than the shoulders are. So the two chains
     * hold each other, and the coat takes its own shape -- in from the
     * shoulder, out over the hips -- because it is built that way rather than
     * because it is drawn that way.
     *
     * Empty means an unpaired chain: a scarf, a hem, a rope.
     */
    std::string pair;
    float pairWidth = 0.0f;
};

/**
 * Which side of the hand the thumb falls on.
 *
 * Returns a multiplier on the ACROSS axis, where across is the hand's own
 * direction rotated a quarter turn anticlockwise on screen -- for a hand held
 * pointing up the screen, that is screen-LEFT.
 *
 * THE PHYSICAL CHECK, because this is the third time it has been wrong and
 * reasoning about it in the abstract is how it keeps getting that way. Hold up
 * your right hand, palm toward you, fingers up: the thumb is on YOUR left. It
 * is a mirror twice over -- once for which hand, once for which face is toward
 * you -- so it is the product of the two, and getting either sign wrong looks
 * exactly like getting the other one wrong.
 *
 * `chir` is +1 for a right hand, -1 a left. `facing` is +1 palm to the viewer,
 * -1 the back of the hand.
 */
inline float thumbSide(float chir, float facing) { return chir * facing; }

/**
 * Moving air.
 *
 * Set on the rig rather than authored per garment, because it belongs to the
 * SCENE: everything standing in the same weather is in the same wind, and a
 * coat that leans one way while a scarf leans the other reads as a bug rather
 * than as two fabrics.
 */
struct Wind {
    /**
     * Which way it blows, IN SCENE SPACE. Normalised on use.
     *
     * The scene's space and not the rig's, which is the whole point. A
     * character drawn mirrored is the same character standing the other way
     * round, and the weather does not turn round with them -- but cloth is
     * solved in rig-local pixels and mirrored at draw time, so a direction
     * handed straight to the solver came out reversed on exactly the people
     * who were facing the other way. Two of them in one wind had their coats
     * blown apart from each other, which is not weather; it is the drawing
     * leaking into the physics.
     *
     * The solver un-mirrors it. Nothing outside here has to know, and a scene
     * that sets one direction gets one direction on everybody in it.
     */
    Vec2  dir{-1.0f, 0.0f};
    float speed = 0.0f;       ///< px/s^2 on cloth held broadside to it
    float gust  = 0.4f;       ///< 0..1, how far the strength wanders from `speed`
    /**
     * The SCENE's clock, in seconds. Left at zero the air never breathes.
     *
     * Weather that varies has to vary the same way for everybody standing in
     * it, and a rig's own clock cannot do that: only the speaker is updated,
     * so no two of them are anywhere near each other. Driving the gusts off a
     * clock the scene owns is the difference between one wind over the stage
     * and each character having private weather.
     *
     * Advanced by the caller rather than read from a system clock, because
     * everything else in here is deterministic and a recorded timelapse has to
     * replay the same gusts. See the note on hashStep.
     */
    float time = 0.0f;
};

/** A colour, in the file as #rrggbb. Kept raylib-free like the rest of Rig.h. */
struct Rgb { unsigned char r = 200, g = 200, b = 200; };

/**
 * How a character is DRAWN, as opposed to how it moves.
 *
 * Everything else in this file is skeleton: where the bones are and what they
 * do. None of it says what the character looks like, and until there is an art
 * pipeline the only view was RigDraw's debug mode -- one flat colour per bone,
 * which is exactly right for finding a misplaced elbow and no use at all for
 * seeing a person.
 *
 * A `look` block fills that gap without inventing an asset format: a handful of
 * colours and two or three switches, out of which RigDraw::drawSkin builds a
 * coat, a hat and a face on the bones that are already there. It is not art and
 * it is not pretending to be -- it is the difference between a diagram of
 * somebody and a drawing of them, which turns out to be about nine numbers.
 *
 * A rig with no `look` block has `drawn` false and keeps the debug skeleton,
 * so nothing that existed before this changes.
 */
struct Look {
    bool drawn = false;      ///< a `look` line was authored
    Rgb  coat  {0x5a, 0x6b, 0x80};   ///< the main garment
    Rgb  lapel {0x39, 0x43, 0x4f};   ///< its darker facings and collar
    Rgb  shirt {0xc9, 0xd2, 0xda};   ///< what shows at the front
    Rgb  skin  {0x8d, 0x9a, 0xa8};   ///< face and hands
    Rgb  hat   {0x26, 0x26, 0x2c};
    Rgb  band  {0xc9, 0xcc, 0xd2};   ///< the ribbon round the hat
    Rgb  line  {0x10, 0x10, 0x14};   ///< the ink everything is drawn with
    bool hasHat = false;
    float brim = 1.55f;      ///< hat brim width, in head radii
    float ink  = 0.055f;     ///< outline weight, as a fraction of head size
};

/**
 * One piece of DRAWN ART, pinned to a bone.
 *
 * The rest of this file describes a character with no art on it. This is the
 * hook for art that exists: a PNG per body part, positioned and rotated by the
 * bone it hangs on, so a drawing inherits the whole rig -- poses, cloth, blink,
 * gaze -- without the artist knowing any of that is there.
 *
 * THE FOUR THINGS A PART HAS TO AGREE WITH THE RIG ABOUT:
 *
 *   which BONE it belongs to      -- named, so a rename is a warning not a
 *                                    silently misplaced arm
 *   where the JOINT is in the image -- `pivot`, in 0..1 of the image, is the
 *                                    point that lands on the bone's origin and
 *                                    the point it rotates about
 *   which way it was DRAWN        -- `turn`, degrees to rotate the image so it
 *                                    lies along the bone. 0 for a part drawn
 *                                    pointing right, 90 for one drawn upright
 *   how BIG a rig unit is         -- `unit`, image pixels per rig unit, so the
 *                                    artist can work at whatever resolution
 *                                    suits and the rig scales it
 *
 * A bone with no part keeps its procedural shape, so art can arrive ONE PIECE
 * AT A TIME and every stage in between is a character you can look at.
 */
struct Part {
    std::string bone;
    std::string file;        ///< relative to the .odrig's own directory
    float pivotU = 0.5f, pivotV = 0.5f;
    float turn = 0.0f;       ///< degrees; 0 = drawn pointing right, 90 = up
    float unit = 4.0f;       ///< image pixels per rig unit
};

/** What a pose says about one bone. Absent fields inherit the rest pose. */
struct PoseBone {
    bool  hasAngle = false;
    float angle = 0;
    /// Hands only. The class picks the drawing set; facing picks within it.
    bool  hasHand = false;
    std::string handClass;
    float facing = 0;
    /**
     * Heads only, and both optional: `mouth -0.7 jaw 0.3` on the head's line
     * in a pose.
     *
     * Two CONTINUOUS scalars rather than named shapes, so they interpolate.
     * The hand had to be discrete -- a palm is a different drawing from a fist
     * and no blend between them is a hand -- but a mouth is one shape being
     * pulled about, and a mouth that snapped between "frown" and "flat" would
     * read as a glitch in exactly the place a viewer is looking.
     */
    bool  hasMouth = false;
    float mouth = 0;
    bool  hasOpen = false;
    float open = 0;
};

/**
 * One waypoint of a pose after the first.
 *
 * A gesture is not a destination. A shrug is shoulders UP, a beat of holding
 * them there, and then down again -- and with one interpolation target the
 * best that can be authored is the shoulders arriving somewhere and staying,
 * which is a posture and not a shrug. So a pose may name further keyframes and
 * the rig walks them in order.
 *
 * A KEYFRAME INHERITS WHAT IT DOES NOT MENTION, which is the opposite of what
 * a whole pose does. Moving to a pose returns every unnamed bone to rest, so
 * poses cannot silently compose; but a keyframe is a step WITHIN one gesture,
 * and a shrug's second beat that only lowers the shoulders must not also drop
 * the arms it never mentioned. Same file, two rules, because they are answers
 * to two different questions.
 */
struct PoseStep {
    std::unordered_map<std::string, PoseBone> bones;
    float seconds = 0.35f;
    Ease  ease = Ease::InOutQuad;
    float hold = 0.0f;        ///< seconds to sit still on arriving, before the next
};

struct Pose {
    std::string name;
    std::unordered_map<std::string, PoseBone> bones;
    float seconds = 0.35f;    ///< how long moving INTO this pose takes
    /**
     * InOutQuad by default, because a limb has mass.
     *
     * It was OutQuad, and "out" easing means START AT FULL SPEED and decelerate
     * -- which put 10-18% of the whole travel into the FIRST FRAME. On a 120
     * degree arm swing that is eighteen degrees in one frame, and it reads as
     * the pose snapping and then settling rather than as an arm moving. An
     * arm accelerates out of rest; only the arrival should be eased.
     *
     * OutBack is worse again on the way in and is still available, because a
     * deliberately snappy gesture is a legitimate thing for a pose to ask for.
     */
    Ease  ease = Ease::InOutQuad;
    float hold = 0.0f;        ///< seconds held before the first keyframe runs
    /// Further keyframes, played in order. Empty for a pose that is a posture.
    std::vector<PoseStep> more;
    /**
     * What to fall back to when the gesture has finished, authored `then idle`.
     *
     * A gesture ENDS; a posture is what you are doing the rest of the time.
     * Without this a script has to put the character back itself, which means
     * every page after a shrug has to remember to say so, and the one that
     * forgets leaves somebody standing with their arms out for the rest of the
     * scene. Empty -- the default -- means the pose is a posture and holds.
     */
    std::string next;
};

struct Skeleton {
    std::vector<Bone>  bones;
    std::vector<Cloth> cloth;
    std::unordered_map<std::string, Pose> poses;
    /// How to draw it. Default-constructed means "no look block": debug bones.
    Look look;
    /// Drawn art, if any. Empty means everything is procedural.
    std::vector<Part> parts;
    /// The directory the .odrig came from, so part files resolve beside it.
    std::string dir;
    /// Anything the parser did not understand, with a line number. Never
    /// silently dropped -- a mistyped bone name in a pose is a limb that
    /// quietly stops moving, which is very hard to notice and very easy to ship.
    std::vector<std::string> warnings;

    int indexOf(const std::string& name) const;
};

/**
 * Parse a .odrig file: the skeleton and its poses, in one place.
 *
 *   bone  chest  parent hips  len 42  angle -90  breath 1.0  sway 0.4
 *   hand  handL  parent foreL len 0   angle 0    atlas hands  size 26
 *   head  head   parent chest len 30  angle -90  atlas head   size 34  gaze 1
 *   cloth coatL  parent chest segs 5  len 13  stiff 0.6  damp 0.92  drag 1.2
 *
 *   pose shrug  0.35s outquad
 *     chest      -84
 *     armL.upper -125
 *     handL      open  facing 0.8
 *
 * Everything is optional except the bone's name and parent. A pose that names
 * a bone the skeleton does not have is a warning, not a silent no-op.
 */
Skeleton parse(const std::string& source);

/** Ease a normalised 0..1 through the named curve. Exposed for tests. */
float applyEase(Ease e, float t);

/** Shortest signed angular difference a→b, in degrees. Exposed for tests. */
float shortestArc(float a, float b);

// ─── Runtime ──────────────────────────────────────────────────────────────

/**
 * Where the character stands on screen, and how big.
 *
 * NORMALISED, not pixels: `x` and `y` run 0..1 across whatever the caller
 * hands the renderer, so a script that puts somebody at the left third puts
 * them there on every resolution. `scale` multiplies the renderer's own, and
 * `flip` mirrors the whole body -- which is how one set of poses serves a
 * character facing either way, and the reason a pose never says "left of
 * screen", only "left arm".
 *
 * Interpolated like a pose, so a character can walk on rather than appear.
 */
struct Placement {
    Vec2  at{0.5f, 0.5f};
    float scale = 1.0f;
    bool  flip = false;
};

/** One solved bone, in rig-local pixels: a segment plus its world angle. */
struct Solved {
    Vec2  a, b;             ///< joint at `a`, tip at `b`
    float worldAngle = 0;   ///< degrees
    /// Hands only: what to draw and which way it is facing right now.
    std::string handClass;
    float facing = 0;
    /// Heads only: the mouth as it stands this frame. -1..1 and 0..1.
    float mouth = 0;
    float mouthOpen = 0;
};

class Rig {
public:
    bool load(const std::string& path);
    void setSkeleton(Skeleton s);   ///< for tests and generated rigs

    /**
     * Move to a named pose.
     *
     * Interpolation always starts from where the rig CURRENTLY is, not from the
     * pose it was last told to hold -- so interrupting a transition half way
     * bends smoothly out of it rather than snapping back to start again.
     */
    bool setPose(const std::string& name, bool immediate = false);
    /**
     * The pose that was last ASKED for, which is not always the one being
     * played: a gesture with `then` falls back on its own when it finishes.
     *
     * The caller needs the request back rather than the truth, because the
     * question it asks every frame is "have I already told it to do this" --
     * and answering that with the fallback makes a gesture retrigger itself
     * the instant it ends, forever.
     */
    const std::string& pose() const { return m_poseName; }
    /// What is actually being played right now. For debug read-outs.
    const std::string& activePose() const { return m_activePose; }
    /// True only when the WHOLE gesture is done: the last keyframe has arrived
    /// and any hold after it has run out. A caller waiting for a pose to finish
    /// must not be told yes half way through a shrug.
    bool  settled() const {
        return m_blend >= 1.0f && m_stepIx >= m_more.size() && m_holdLeft <= 0.0f;
    }

    /**
     * Move the body on screen. `seconds` of 0 puts it there at once.
     *
     * Separate from the pose because they are separate decisions: a character
     * can cross the stage without changing what their arms are doing, and a
     * gesture should not have to be re-authored because somebody moved them.
     */
    void setPlacement(const Placement& p, float seconds = 0.0f);
    const Placement& placement() const { return m_place; }

    void update(float dt);

    const std::vector<Solved>& solved() const { return m_solved; }
    const std::vector<std::vector<Vec2>>& clothPoints() const { return m_cloth; }
    const Skeleton& skeleton() const { return m_skel; }

    /// 0 = eyes open, 1 = fully closed. Drawn by whoever owns the eye art.
    float blink() const { return m_blink; }
    /// Where the character is looking, -1..1 left to right, -1..1 up to down.
    Vec2 gaze() const { return m_gaze; }

    /**
     * The weather. Zero speed is still air, which is the default.
     *
     * Wind is a property of the scene and not of the character, so it is set
     * from outside per frame rather than authored: the same rig standing in a
     * menu behind a scrolling map and standing in a room should not need two
     * .odrig files to hang its coat differently.
     */
    void setWind(const Wind& w) { m_wind = w; }
    const Wind& wind() const { return m_wind; }

    /// Turn the living layers off — for a screenshot that must not move, and
    /// for tests that compare exact numbers.
    void setIdleEnabled(bool on) { m_idle = on; }

private:
    /**
     * Aim at one set of bone angles.
     *
     * `unnamedToRest` is the difference between a pose and a keyframe: a pose
     * sends every bone it does not mention back to rest, a keyframe leaves
     * them where the previous keyframe put them. See PoseStep.
     */
    /// Start a pose without recording it as the request. See pose().
    bool  playPose(const std::string& name, bool immediate);
    void  beginTarget(const std::unordered_map<std::string, PoseBone>& bones,
                      float seconds, Ease ease, bool unnamedToRest);
    void  commitTarget();
    void solve();
    void stepCloth(float dt);

    Skeleton m_skel;
    std::string m_poseName;

    /**
     * The pose, and the pose plus the living layers, kept APART.
     *
     * They were one array, and setPose captured it as the start of the next
     * transition -- so every pose change baked whatever the breath happened to
     * be doing at that instant into the interpolation source, permanently. It
     * drifts, and it means the same two poses do not transition the same way
     * twice.
     *
     * m_base is what the poses say. m_angle is what gets drawn. Only m_base is
     * ever interpolated from.
     */
    std::vector<float> m_base;
    std::vector<float> m_angle;      ///< m_base plus breath/sway/gaze
    std::vector<float> m_from, m_to; ///< the transition being played
    std::vector<std::string> m_handClass, m_fromHandClass, m_toHandClass;
    std::vector<float> m_facing, m_fromFacing, m_toFacing;
    std::vector<float> m_mouth, m_fromMouth, m_toMouth;
    std::vector<float> m_open, m_fromOpen, m_toOpen;

    float m_blend = 1.0f, m_blendDur = 0.35f;
    Ease  m_ease = Ease::OutQuad;
    /// The keyframes of the gesture being played, and where in them we are.
    std::vector<PoseStep> m_more;
    size_t m_stepIx = 0;
    float  m_holdLeft = 0.0f;
    std::string m_activePose;   ///< what is playing
    std::string m_next;         ///< what to fall back to when it ends

    float m_time = 0.0f;
    bool  m_idle = true;
    Wind  m_wind{};

    Placement m_place{}, m_placeFrom{}, m_placeTo{};
    float m_placeBlend = 1.0f, m_placeDur = 0.0f;

    // Gaze and blink are stateful rather than a pure function of time: both
    // need a dwell and a refractory period, which a sine cannot express.
    Vec2  m_gaze{}, m_gazeTarget{};
    float m_gazeHold = 0.0f;
    float m_blink = 0.0f, m_blinkNext = 2.0f, m_blinkPhase = 0.0f;

    std::vector<Solved> m_solved;
    std::vector<std::vector<Vec2>> m_cloth, m_clothPrev;
};

}  // namespace rig
