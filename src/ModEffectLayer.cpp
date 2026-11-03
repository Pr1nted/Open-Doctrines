#include "ModEffectLayer.h"

#include <algorithm>
#include <cmath>

namespace odeffect {

uint32_t neutralPermille(Effect e) {
    switch (e) {
        // 500 is 1x. Saturation is the one effect whose "do nothing" is not
        // zero, and getting it wrong drains the map rather than leaving it be.
        case Effect::Saturation: return 500;
        case Effect::HueRotate:
        case Effect::Vignette:
        case Effect::Grain:
        case Effect::HueCycle:
        case Effect::Wobble:
        case Effect::Count_:     break;
    }
    return 0;
}

float maxWobbleFor(Target t) {
    return t == Target::Screen ? kMaxWobbleScreen : kMaxWobbleMap;
}

void Layer::TargetState::reset() {
    owner.clear();
    phase = 0.0f;
    for (size_t i = 0; i < (size_t)Effect::Count_; ++i) {
        want[i] = neutralPermille((Effect)i);
        now[i]  = (float)want[i];
    }
}

bool Layer::setEffect(const std::string& modId, Target t, Effect e, uint32_t permille) {
    if (modId.empty()) return false;
    if ((uint32_t)t >= (uint32_t)Target::Count_) return false;
    if ((uint32_t)e >= (uint32_t)Effect::Count_) return false;
    TargetState& ts = m_targets[(size_t)t];
    if (!ts.owner.empty() && ts.owner != modId) return false;
    ts.owner = modId;
    // Clamped rather than refused, and deliberately: a value one past the end
    // is an arithmetic slip in the mod, and failing the call would have the
    // author debugging a silent nothing instead of seeing a saturated effect.
    ts.want[(size_t)e] = std::min(permille, kMaxPermille);
    return true;
}

void Layer::clearTarget(const std::string& modId, Target t) {
    if ((uint32_t)t >= (uint32_t)Target::Count_) return;
    TargetState& ts = m_targets[(size_t)t];
    // A mod cannot clear another mod's target, for the same reason it cannot
    // clear another mod's tints.
    if (ts.owner != modId) return;
    // The REQUEST goes back to neutral; the rendered value still slews there,
    // so releasing a target fades out rather than snapping. The owner is
    // dropped at once, so another mod can claim it immediately and inherits
    // the fade in progress.
    for (size_t i = 0; i < (size_t)Effect::Count_; ++i)
        ts.want[i] = neutralPermille((Effect)i);
    ts.owner.clear();
}

void Layer::clearMod(const std::string& modId) {
    for (size_t i = 0; i < (size_t)Target::Count_; ++i)
        clearTarget(modId, (Target)i);
}

void Layer::clear() {
    // Hard, not a fade: this is the kill switch. The Mod Menu has to come up
    // looking like itself on the first frame, not a second and a half later.
    for (auto& ts : m_targets) ts.reset();
}

void Layer::advance(float dtSeconds) {
    // A frame that took a long time -- loading, a resolved turn -- must not
    // become a licence to jump. Clamped to a frame's worth, so a stall cannot
    // deliver the strobe the per-frame limit exists to prevent.
    const float dt = std::min(std::max(dtSeconds, 0.0f), 0.05f);
    const float step = kSlewPerSecond * (float)kMaxPermille * dt;
    if (m_suppressed) {
        // Neutral NOW, and the requests left exactly as they are. See
        // setSuppressed for why this is not clear().
        for (auto& ts : m_targets)
            for (size_t i = 0; i < (size_t)Effect::Count_; ++i)
                ts.now[i] = (float)neutralPermille((Effect)i);
        return;
    }
    for (auto& ts : m_targets) {
        for (size_t i = 0; i < (size_t)Effect::Count_; ++i) {
            const float target = m_enabled ? (float)ts.want[i]
                                           : (float)neutralPermille((Effect)i);
            const float d = target - ts.now[i];
            if (std::fabs(d) <= step) ts.now[i] = target;
            else                      ts.now[i] += (d > 0.0f ? step : -step);
        }
        // The rolling hue, integrated from the SLEWED speed rather than the
        // requested one: a mod that slams the speed between 0 and full every
        // frame gets an average, not a lurch, for free and for the same reason
        // as everything else here.
        const float turns = (ts.now[(size_t)Effect::HueCycle] / (float)kMaxPermille)
                            * kMaxCycleTurnsPerSecond;
        ts.phase += turns * dt;
        // Wrapped rather than left to grow: a session left running all day
        // would otherwise lose the fractional part in a float and the colour
        // would visibly quantise.
        ts.phase -= std::floor(ts.phase);
    }
}

uint32_t Layer::rendered(Target t, Effect e) const {
    if ((uint32_t)t >= (uint32_t)Target::Count_) return 0;
    if ((uint32_t)e >= (uint32_t)Effect::Count_) return neutralPermille(e);
    const float v = m_targets[(size_t)t].now[(size_t)e];
    return (uint32_t)std::lround(std::min(std::max(v, 0.0f), (float)kMaxPermille));
}

uint32_t Layer::huePermille(Target t) const {
    if ((uint32_t)t >= (uint32_t)Target::Count_) return 0;
    const uint32_t held = rendered(t, Effect::HueRotate);
    const uint32_t roll = (uint32_t)std::lround(m_targets[(size_t)t].phase
                                                * (float)kMaxPermille);
    return (held + roll) % kMaxPermille;
}

bool Layer::active(Target t) const {
    if ((uint32_t)t >= (uint32_t)Target::Count_) return false;
    // What is RENDERED, not what was asked for. A target whose owner has just
    // released it is still mid-fade and still needs its pass; one nobody has
    // touched needs none, which is every session without an effect mod.
    for (size_t i = 0; i < (size_t)Effect::Count_; ++i)
        if (rendered(t, (Effect)i) != neutralPermille((Effect)i)) return true;
    return false;
}

std::string Layer::owner(Target t) const {
    if ((uint32_t)t >= (uint32_t)Target::Count_) return std::string();
    return m_targets[(size_t)t].owner;
}

bool Layer::empty() const {
    for (size_t i = 0; i < (size_t)Target::Count_; ++i)
        if (active((Target)i) || !m_targets[i].owner.empty()) return false;
    return true;
}

}  // namespace odeffect
