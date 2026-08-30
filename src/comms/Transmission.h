#pragma once

// The communication window: a speaker seen over a bad video link.
//
// WHY THIS INSTEAD OF A CHARACTER
//   A drawn character costs art per speaker, per pose, per expression. A
//   transmission costs none of that, because THE FILTER IS THE STYLE: a
//   phosphor grille, scanlines, bloom and noise are what the player reads,
//   and the picture underneath only has to be a silhouette with eyes in it.
//   Everything expensive about a character -- line quality, shading, hands,
//   a mouth that matches the words -- is destroyed by the link anyway.
//
// WHAT IS ACTUALLY DRAWN
//   1. A SIGNAL, rendered small (a few hundred pixels tall) into an offscreen
//      buffer, in luminance only: head, shoulders, eyes. Nothing else.
//   2. A FILTER, which is where the whole look lives. Luminance is mapped
//      through a ramp built from the player's accent colour, so the link is
//      the colour the player chose -- amber, green, ice, whatever -- and the
//      speaker is that colour too. Then grille, scanline, bloom, roll, noise.
//   3. A FRAME around it: bezel, speaker plate, signal readout.
//
// WHAT MOVES
//   The eyes, and only the eyes. They drift, they fix on something, and they
//   blink on an irregular clock. Everything else that reads as "alive" comes
//   from the signal itself -- flicker, drift, a tear when the line is poor.
//
// The window is deliberately LARGE: big enough that a speaker could raise an
// arm inside it without leaving the frame, so gestures remain possible later
// without moving or resizing anything.

#include <algorithm>
#include <string>
#include <vector>

#include "raylib.h"

namespace comms {

/** The few numbers that make one speaker's silhouette differ from another's. */
struct Profile {
    // Framed like a video call: the head high and large in frame, the
    // shoulders entering from the bottom edge. Fractions of the signal
    // buffer, so the same numbers hold at any window size.
    float headW = 0.46f;      ///< head width
    float headH = 0.40f;      ///< head height
    float headY = 0.38f;      ///< centre of the head, fraction of height
    float shoulderW = 0.96f;
    float shoulderY = 0.72f;  ///< where the shoulders cross the frame
    float neckW = 0.17f;
    bool  hat = false;
    float hatBrim = 0.66f;    ///< brim width when `hat`
    float hatCrown = 0.20f;
    float eyeGap = 0.19f;     ///< distance between eye centres
    float eyeW = 0.105f;
    float eyeH = 0.062f;
    float eyeY = -0.02f;      ///< eye line, relative to the head's centre
    float browDrop = 0.0f;    ///< 0 open, 1 heavy-lidded
    /// False for a speaker whose face is not shown at all -- a figure in the
    /// dark. Nothing then moves but the drift and the line itself, which is
    /// the point of them.
    bool  eyes = true;
    /**
     * Close the eye from BELOW instead of from above.
     *
     * A lid that rides low and comes up reads as a smile reaching the eyes;
     * the same eye with the lid dropping from the top reads as tired. It is
     * one number and it changes the whole face, which is why it is here and
     * not baked into the art.
     */
    bool  lidBottom = false;

    /**
     * How this speaker's link sits against the player's accent.
     *
     * The filter builds every colour from the accent, which is what makes one
     * greyscale drawing serve every palette -- but it also makes every speaker
     * exactly the same colour. These two shift THIS speaker off it: `hueShift`
     * in degrees, `tintGain` as a brightness multiplier. Still derived from
     * the accent, so a player who picks green still gets a green room; one
     * character is simply warmer and brighter in it than another.
     */
    float hueShift = 0.0f;
    float tintGain = 1.0f;

    /**
     * A mouth the window draws, rather than one baked into the art.
     *
     * Same reasoning as the eyes: it has to move. It is not a set of drawn
     * shapes either -- it is one curve whose bow is the speaker's mood and
     * whose opening is the glyph currently being typed, so every expression
     * between a frown and a grin exists and they blend into each other.
     * Coordinates are fractions of the image; comms_signal.py prints them.
     */
    bool  mouth = false;
    float mouthX = 0.50f, mouthY = 0.56f;
    float mouthW = 0.125f;
    float mouthBase = 0.0f;   ///< the resting bow: + a smile, - a frown
    /// How far the lid sits over an open eye. Low for a wide, alert face;
    /// high for a heavy-lidded one. Drawn art usually wants less than a
    /// silhouette does, because the art already carries its own lids.
    float lidRest = 0.40f;
    /**
     * The lash line: a heavy stroke along the top of the eye with a flick past
     * the outer corner. On a drawn face it is usually the single most
     * characteristic thing about the eyes, and leaving it out is what made
     * these read as two flat ovals with dots in them.
     *
     * 0 for none. `pupilAspect` above 1 makes a tall pupil rather than a dot.
     */
    float lashes = 0.0f;
    float pupilAspect = 1.0f;
    float pupilScale = 0.50f;   ///< of the eye's smaller radius

    /**
     * How far the lash floats ABOVE the eye's rim, in multiples of the
     * outline weight. Her lashes are a separate stroke with clear skin
     * between them and the eye; a lash laid straight onto the rim just
     * reads as a thicker rim.
     */
    float lashLift = 0.0f;

    /**
     * Where the pair of eyes is centred, as a fraction of the image width.
     * 0.5 is the middle of the picture, which is only right when the face
     * happens to be centred in it -- hers sits slightly right of centre, so
     * eyes placed on the image's midline land off her face.
     */
    float imgEyeX = 0.5f;

    /**
     * The flick: the lash carries on past the outer corner and kicks up away
     * from the eye. It is the most recognisable thing about this pair of
     * eyes, and an arc that simply stops at the corner does not read as the
     * same drawing however well the rest is matched.
     *
     * Its length, as a fraction of the eye's half-width. 0 for none.
     */
    float lashFlick = 0.0f;

    /**
     * The lower eyelid: a short heavy stroke under the eye, toward the nose.
     * It is a separate mark from the outline, and leaving it out is a large
     * part of why a rebuilt eye reads as a cartoon oval rather than as the
     * drawn one -- the drawing has three marks per eye (the C, the lash and
     * this), not one closed ring.
     *
     * A multiple of the outline weight; 0 for none.
     */
    float lowerLid = 0.0f;

    /**
     * Keep the artist's eyes and move a pupil inside them.
     *
     * The window can BUILD an eye -- rim, lid, white, lash, pupil -- and that
     * is the only option for a speaker who is a silhouette. But when the art
     * already has eyes, rebuilding them means reproducing somebody's line
     * work from parameters, and it will always be a near miss. This keeps
     * the drawn eye exactly as it is and animates the two things that have to
     * move: the pupil, and the lid coming down over it.
     *
     * `pupilRadius` is a fraction of the image width; imgEyeW/imgEyeH are then
     * the drawn eye's extent, used to cover it during a blink.
     */
    bool  eyeArt = false;
    float pupilRadius = 0.021f;
    uint32_t seed = 1;

    /**
     * A drawn speaker, instead of the silhouette above.
     *
     * A greyscale PNG (tools/comms_signal.py makes one from any character
     * art): background already keyed out, EYES ALREADY PAINTED OUT. It
     * carries no colour, because the filter supplies that from the accent --
     * so one file serves every palette the player can choose.
     *
     * The eye numbers below are fractions of THAT IMAGE, not of the head,
     * and the tool prints them.
     */
    std::string image;
    float imgEyeY = 0.338f;   ///< eye line, from the top of the image
    float imgEyeGap = 0.223f; ///< separation, fraction of image width
    float imgEyeW = 0.132f;   ///< the whole eye, not the part that shows
    float imgEyeH = 0.094f;
    float imgOutline = 0.0114f; ///< outline weight, fraction of image width
    /// How much of the ring is left off on the nose side, in degrees. The
    /// reference draws each eye as a C, open toward the middle of the face.
    float eyeOpenArc = 34.0f;
};

/** How good the line is. Drives tearing, noise and how often it stutters. */
struct Signal {
    float quality = 0.86f;    ///< 0 unusable, 1 clean
    float gain = 1.0f;        ///< overall brightness
};

class Transmission {
public:
    /** Builds the offscreen buffer and compiles the filter. Needs a window. */
    bool open(int signalWidth = 448, int signalHeight = 560);
    void close();
    bool ready() const { return m_target.id != 0; }
    /// True when the filter compiled; false means the plain fallback is drawn.
    bool filtered() const { return m_shader.id != 0; }

    void setProfile(const Profile& p);
    /**
     * Change who is on the link, through a dropout.
     *
     * The picture collapses into static, the speaker is swapped while nothing
     * can be seen, and the new one locks on -- which is how a link behaves and
     * is also the only honest way to replace one portrait with another. A
     * straight cut reads as a bug.
     */
    void changeSpeaker(const Profile& p, const std::string& name,
                       const std::string& role = "", const std::string& tag = "");
    /// Lock on / drop out. The window draws nothing once it has dropped.
    void tuneIn();
    void tuneOut();
    /// True while anything at all should be drawn -- static counts.
    bool onAir() const { return m_visible; }
    /// 0 fully locked on, 1 pure static.
    float staticLevel() const { return m_static; }
    /**
     * How far the tube is open: 0 dark, 1 the picture at full size.
     *
     * Tuning in and out is not a fade. The window is a screen, and a screen
     * coming on spreads a point of light sideways into a line and then opens
     * the line into a picture; going off, it does the same in reverse and
     * leaves the line burning for a moment after the picture has gone. That
     * shape is what makes somebody joining the call read as equipment being
     * switched on rather than as a panel sliding in.
     */
    float power() const { return m_power; }
    /// True while the tube is closing: this speaker is going, not gone.
    bool leaving() const { return m_visible && m_powerTo <= 0.0f; }
    const Profile& profile() const { return m_profile; }
    /// Sets the signal AND the level it drifts around. See drift().
    void setSignal(const Signal& s) { m_signal = s; m_signalBase = s.quality; }
    /**
     * Who is on the link, as the player should read it.
     *
     * `name` is what they are called, `role` what they are -- "Mia" and
     * "lazy advisor". The script keys on a formal name ("Signals Officer")
     * because that is a stable id, but a plate reading SIGNALS OFFICER tells
     * a new player nothing about who is talking to them. `tag` is the short
     * form, shown beside the name for anyone who is skimming.
     */
    void setSpeaker(const std::string& name, const std::string& role = "",
                    const std::string& tag = "") {
        m_speaker = name; m_speakerRole = role; m_speakerTag = tag;
    }

    /// Look towards a point on screen, or let the eyes wander again.
    void lookAt(Vector2 screenPoint);
    void lookWander();
    /// Bring the next blink forward -- a reaction, not a tic.
    void blinkSoon();
    /// How the speaker feels: -1 grim, 0 level, +1 pleased. Eased, not cut.
    void setEmotion(float e) { m_emotionTo = std::clamp(e, -1.0f, 1.0f); }
    float emotion() const { return m_emotion; }
    /// A glyph just appeared in the textbox; the mouth follows it.
    void speak(int codepoint);
    void hush() { m_openTo = 0.0f; }
    /// 1 while this speaker is talking, 0 while they listen.
    void setAttention(float a) { m_attentionTarget = a; }

    /**
     * Advance the eyes and the line, and rebuild the picture.
     *
     * Call this OUTSIDE BeginDrawing/BeginTextureMode: it renders into its
     * own target, and raylib cannot nest render targets.
     */
    void update(float dt);
    /** Draw the window into `bounds`, tinted by `accent`. */
    void draw(Rectangle bounds, Color accent);

    // ── for tests and tools ──
    bool blinking() const { return m_blinkT < m_blinkDur; }
    float lidOpen() const;             ///< 1 fully open, 0 shut
    Vector2 gaze() const { return m_gaze; }
    void seed(uint32_t s);
    /// Hold the blink at a phase, 0 open .. 1 fully shut. For tools only:
    /// a blink lasts 160 ms and catching one by chance is not a test.
    void debugBlink(float phase);

    /// The picture before the filter, for tuning the eyes against the art.
    const RenderTexture2D& signalBuffer() const { return m_target; }

    /// The rectangle a window of this shape wants inside `screen`: large, to
    /// one side, tall enough for a raised arm.
    static Rectangle suggestedBounds(Rectangle screen, bool rightSide = false);

private:
    void renderSignal();               ///< the picture, before the filter
    void drawFrame(Rectangle b, Color accent) const;
    float noise();

    RenderTexture2D m_target{};
    Texture2D m_signalTex{};           ///< the drawn speaker, when there is one
    Shader m_shader{};
    int m_locResolution = -1, m_locTime = -1, m_locAccent = -1;
    int m_locQuality = -1, m_locGain = -1;

    Profile m_profile;
    /// The speaker's own skin tone, read off their art when the profile is
    /// set. The eye's white and the lid are derived from it: a fixed pair of
    /// constants is only ever right for the character they were tuned on.
    float m_skin = 150.0f;
    Signal  m_signal;
    /**
     * The reception WANDERS.
     *
     * A radio link that holds one exact noise level for an hour is a picture
     * with a filter on it; a real one breathes, and every so often it drops
     * for a few seconds and comes back. The window already had a static level
     * -- it simply never changed unless the game changed it, which nothing
     * did after the first frame.
     *
     * `m_signalBase` is where it belongs, set by setSignal; the drift is a
     * slow walk around that, plus occasional dips. Deterministic, from the
     * same xorshift as the blinks: two machines showing the same conversation
     * show the same weather on the line.
     */
    float m_signalBase = 0.86f;
    float m_driftTo = 0.0f;      ///< current offset target
    float m_drift = 0.0f;        ///< and where it actually is
    float m_driftNext = 2.0f;    ///< clock time of the next change of mind
    std::string m_speaker, m_speakerRole, m_speakerTag;
public:
    /// What this window believes it is showing. For diagnostics.
    const std::string& speakerName() const { return m_speaker; }
private:

    uint32_t m_rng = 1;
    float m_clock = 0;
    float m_blinkT = 9, m_blinkDur = 0.16f, m_blinkNext = 2.0f;
    bool  m_blinkHold = false;   ///< debugBlink() froze the lid where it is
    bool  m_doubleBlink = false;
    Vector2 m_gaze{0, 0}, m_gazeTarget{0, 0};
    float m_gazeHold = 1.0f;
    bool  m_gazeHeld = false;
    float m_attention = 1, m_attentionTarget = 1;
    float m_glitch = 0;                ///< decays; spikes when the line drops
    float m_glitchNext = 3.0f;

    // Tuning: how much of the picture is static rather than signal, where it
    // is heading, and who is waiting to appear once it cannot be seen.
    float m_emotion = 0.0f, m_emotionTo = 0.0f;
    float m_open = 0.0f, m_openTo = 0.0f, m_openHold = 0.0f;
    float m_static = 1.0f;
    float m_staticTo = 1.0f;
    float m_power = 0.0f;        ///< see power(): 0 dark, 1 open
    float m_powerTo = 0.0f;
    /// The window's rectangle at the current power, about its own centre.
    Rectangle powerRect(Rectangle b) const;
    /// The line the picture collapses to, drawn on the way in and the way out.
    void drawScanLine(Rectangle b, Color accent) const;
    bool  m_visible = false;
    bool  m_hasSwap = false;
    Profile m_swap;
    std::string m_swapName, m_swapRole, m_swapTag;
    int m_locStatic = -1;
};

}  // namespace comms
