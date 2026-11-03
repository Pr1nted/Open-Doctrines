#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Screen effects a mod may ask for: a target, a host effect, and a number.
 *
 * ── A MOD NEVER OWNS THE FRAME ──
 *
 * It names a target and picks from the host's list of effects, and the host
 * composites. There is no framebuffer, no shader, and no draw callback in this
 * API, so there is no sequence of calls that produces an arbitrary image.
 *
 * That is structural rather than policed, and it is the whole reason this is
 * allowed to exist at all. The interface -- panels, dialogs, the turn bar, and
 * the Mod Menu that is the only way to switch a mod off -- draws AFTER every
 * target. A mod that could paint the frame could paint over its own off switch;
 * a mod that can only say "hue rotate 0.4" cannot, whatever it passes.
 *
 * ── PER MILLE, BECAUSE A CLAMPED INTEGER CANNOT ARRIVE AS NaN ──
 *
 * Every parameter is 0..1000 and is clamped HERE, on the way in. A mod cannot
 * learn the clamp (the call succeeds either way) so it cannot search for an
 * edge, and no float from a mod reaches a uniform.
 *
 * ── ONE MOD PER TARGET ──
 *
 * Two mods tinting the same layer produce a picture neither of them wrote and
 * neither can debug, and summing their parameters is worse: 500 means
 * "unchanged" for saturation, so two mods asking for nothing would ask for
 * double. So a target has one owner, first to claim it, and a second mod's
 * set_effect on it is REFUSED -- a visible false rather than an invisible
 * fight. Within its target a mod may set every effect; they compose into one
 * pass.
 *
 * ── THE SLEW LIMITER IS THE PART THAT PROTECTS THE PLAYER ──
 *
 * Clamping the value does nothing about the rate. A mod that alternates two
 * perfectly legal values every frame is a full-screen strobe at the frame rate,
 * which is the thing that triggers photosensitive seizures, and an author
 * writing a psychedelic effect is not thinking about that and should not have
 * to. So the rendered value is not the requested one: it MOVES toward it, by at
 * most kSlewPerSecond of range per second. Asking for a strobe gets a slow
 * fade, and the host does not have to decide whether a given mod meant well.
 *
 * WCAG 2.3.1's general threshold is three flashes per second. A full-range
 * traversal at this rate takes 1/kSlewPerSecond seconds each way, so the
 * fastest cycle a mod can produce is kSlewPerSecond/2 Hz -- 0.75 Hz at the
 * value below, comfortably under three.
 *
 * ── AND AN OFF SWITCH A MOD CANNOT SEE ──
 *
 * setEnabled(false) holds every target at neutral. Calls still succeed, and
 * nothing in the ABI reports the setting: a mod that could read it could route
 * round it, and the guarantee would be advice.
 */
namespace odeffect {

/// What an effect is attached to. A layer, not a mode -- these are different jobs.
///
/// THERE IS NO TARGET FOR THE INTERFACE, and there is not going to be one. See
/// the note above: it is what the rest of this rests on.
enum class Target : uint32_t {
    Map    = 0,  ///< the province raster, under everything drawn on it
    Panel  = 1,  ///< the mod's own panels, already scissor-clipped to their rectangle
    /**
     * The whole game screen: map, counters, panels, the sidebar, dialogs.
     *
     * NOT the always-on-top layer. Toasts, the feedback form and the Mod Menu
     * draw after the pass, and the Mod Menu additionally suppresses every
     * effect for as long as it is open. Whatever this does to the screen, the
     * way to switch it off is drawn clean.
     *
     * ── A DISTORTED INTERFACE LIES ABOUT ITSELF ──
     *
     * Wobble on this target moves PIXELS. It does not move hit boxes, and it
     * cannot: input is resolved against the layout, not against the picture.
     * A button drawn fifteen pixels left of itself still answers where it
     * really is. So the displacement here is clamped far below a control's
     * height (kMaxWobble) -- enough to read as movement, not enough to make
     * the screen unusable. The clamp is the host's and a mod cannot reach it.
     */
    Screen = 2,
    Count_
};

/// The host's effects. The list is the host's and grows by release: each one is
/// a thing somebody wrote a shader for once, with a cost somebody measured once.
enum class Effect : uint32_t {
    HueRotate  = 0,  ///< 0..1000 -> 0..360 degrees, held
    Saturation = 1,  ///< 0..1000 -> 0..2x, 500 is unchanged
    Vignette   = 2,  ///< 0..1000 -> none..full
    Grain      = 3,  ///< 0..1000 -> none..full
    /**
     * A hue that rolls by itself. 0..1000 -> 0..kMaxCycleTurnsPerSecond.
     *
     * ── WHY THE HOST DRIVES THIS AND NOT THE MOD ──
     *
     * `mod_draw_panel` is the only per-frame export the ABI has, so a mod that
     * wants anything to move has to register a panel and use it as a clock.
     * That is a strange thing to require of a mod whose whole purpose is to
     * recolour the map, and it puts a window on screen that exists only to tick.
     *
     * It is also the worse of the two for the player. A mod driving its own
     * animation picks its own rate, and the rate is the dangerous parameter; a
     * host that integrates the phase itself owns the ceiling, and no mod can
     * ask for a faster one than kMaxCycleTurnsPerSecond.
     *
     * Integrated HERE rather than from the shader's clock, so the phase is a
     * number this layer can be tested on, wraps exactly at 1.0, and cannot drift
     * or jump when a float seconds-counter wraps.
     */
    HueCycle   = 4,
    /**
     * Push the image around. 0..1000 -> none..kMaxWobble of the target.
     *
     * A displacement of the texture coordinate inside the same single pass, so
     * it costs no extra geometry and no second target -- and so it can only
     * move what that pass was already drawing. The frequency is the host's;
     * the mod sets the amplitude.
     */
    Wobble     = 5,
    Count_
};

constexpr uint32_t kMaxPermille = 1000;

/// Of full range per second. See the WCAG note above for where it comes from.
constexpr float kSlewPerSecond = 1.5f;

/**
 * The fastest a rolling hue may turn, in full turns per second.
 *
 * Hue rotation in YIQ holds luminance, so this is not a flash the way an
 * alternating brightness is -- but a saturated colour sweeping the wheel
 * several times a second is still the kind of thing the guidance is about, and
 * nothing is lost by making the ceiling slow. Two seconds a turn reads as
 * "the map is breathing" rather than as a warning light.
 */
constexpr float kMaxCycleTurnsPerSecond = 0.5f;

/**
 * The furthest a wobble may push a pixel, as a fraction of the target.
 *
 * Two numbers, because the consequence differs. On the map a displaced pixel
 * means a click lands on the neighbouring province, which is a nuisance. On
 * the whole screen it means a button is not where it is drawn, which is a
 * usability failure -- so the screen's ceiling is small enough that every
 * control still overlaps itself.
 */
constexpr float kMaxWobbleMap    = 0.030f;
constexpr float kMaxWobbleScreen = 0.012f;
float maxWobbleFor(Target t);

/// What a target looks like when nobody has asked for anything.
uint32_t neutralPermille(Effect e);

class Layer {
public:
    Layer() = default;
    /**
     * Ask for an effect on a target. The value is clamped; out-of-range targets
     * and effects are refused.
     *
     * Returns false when another mod holds the target, or the target or effect
     * is not one of the host's. A mod that gets false has asked for something
     * the host does not do, which is worth knowing; it never means "the player
     * switched effects off", which a mod must not be able to tell.
     */
    bool setEffect(const std::string& modId, Target t, Effect e, uint32_t permille);

    /** Release a target, if this mod holds it. */
    void clearTarget(const std::string& modId, Target t);
    /** Release everything this mod holds. Called when it is unloaded. */
    void clearMod(const std::string& modId);
    /**
     * Drop everything, from every mod.
     *
     * The Mod Menu calls this before it opens: the kill switch has to work on a
     * frame where the mod is still running and still asking.
     */
    void clear();

    /** Move rendered values toward requested ones. Once per frame. */
    void advance(float dtSeconds);

    /**
     * Hold every target at neutral without forgetting what was asked for.
     *
     * THIS IS THE KILL SWITCH, and it is deliberately not clear(). It used to
     * be: the Mod Menu called clear() on every frame it was open, which worked
     * for as long as every effect mod re-asserted from mod_draw_panel. The
     * moment HUE_CYCLE let a mod set an effect ONCE and never run again, that
     * became fatal -- mod_load fires while the Mod Menu is up, so the request
     * was wiped on the frame it was made and the mod never spoke again. The
     * mod looked broken and the host had eaten its only call.
     *
     * So suppression holds the OUTPUT at neutral and leaves the request alone.
     * It snaps rather than fading, because the menu has to come up looking
     * like itself on the first frame; releasing it slews back up through the
     * limiter like anything else.
     */
    void setSuppressed(bool on) { m_suppressed = on; }

    /**
     * The global accessibility setting. Not readable through the ABI.
     *
     * Switching it off does not clear what mods asked for -- it holds the
     * rendered values at neutral, so switching it back on resumes rather than
     * requiring every mod to ask again.
     */
    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }

    /** What to render, after clamping and slewing. Neutral when nothing is set. */
    uint32_t rendered(Target t, Effect e) const;
    /**
     * The hue a target should actually be drawn with, 0..999: the held rotation
     * plus however far the rolling one has turned.
     *
     * The draw path wants one number, and computing it from two in the renderer
     * would be a second place that has to know how HueCycle works.
     */
    uint32_t huePermille(Target t) const;
    /** Whether a target needs a pass at all, so the draw path can skip out. */
    bool active(Target t) const;
    /** Who holds a target, for the Mod Menu to show. Empty when nobody does. */
    std::string owner(Target t) const;
    /** Whether any target is doing anything. */
    bool empty() const;

private:
    struct TargetState {
        std::string owner;
        uint32_t want[(size_t)Effect::Count_];
        float    now[(size_t)Effect::Count_];
        float    phase = 0.0f;   ///< turns of HueCycle so far, wrapped to [0,1)
        TargetState() { reset(); }
        /// Back to neutral, which is what "no effect" is -- not zero. Zero
        /// saturation is a greyscale screen, so a target that reset to zeroes
        /// would drain the colour out of the map on release.
        void reset();
    };
    TargetState m_targets[(size_t)Target::Count_];
    bool m_enabled = true;
    bool m_suppressed = false;
};

}  // namespace odeffect
