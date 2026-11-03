#pragma once
#include <cstdint>

// ─── Per-country AI personality ──────────────────────────────────────────────
//
// A few traits in [0,1], 0.5 == neutral (no effect). Derived deterministically
// from a country's politics and id, so a country has a stable character across
// games and the save format does not change -- the profile is recomputed on
// load, never stored.
//
// EXPERIMENTAL AND OFF BY DEFAULT. The AI reads a profile only when
// OD_AI_PERSONALITIES is set; unset, every country is neutral and the trained
// AI plays exactly as it does today (the decision hash is unchanged). The trait
// -> behaviour magnitudes are tuned through the LOOP before this becomes a
// default or a settings toggle; a profile is wired into the AI through the same
// additive qbias channel that stance and coalition pressure use.
struct PersonalityProfile {
    float aggressiveness = 0.5f;  ///< war propensity: higher declares/attacks more
    float tactfulness    = 0.5f;  ///< diplomacy: higher prefers deals over force
    float mindfulness    = 0.5f;  ///< caution: higher avoids risky moves
    float paranoia       = 0.5f;  ///< defence: higher invests more in military/forts
    bool  set            = false; ///< false == neutral (feature off / not assigned)
};

namespace odpers {

inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
/// A deterministic value in [0,1) from an integer id and a per-trait salt.
inline float hash01(int id, uint64_t salt) {
    const uint64_t h = splitmix64((uint64_t)(uint32_t)id ^ (salt * 0x100000001B3ull));
    return (float)((h >> 40) / (double)(1ull << 24));
}
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/**
 * A country's character, from its politics and a per-id jitter.
 *
 * `compassSocial` runs -100 (authoritarian) to +100 (libertarian). The lean:
 * authoritarian states run hotter and more fearful (more aggressive, more
 * paranoid, less tactful, less cautious); libertarian states the reverse. The
 * jitter -- a different hash per trait -- keeps two countries of the same
 * politics from being identical. Nothing here depends on game state, so the
 * same country is the same character every game.
 */
inline PersonalityProfile makePersonality(int id, float /*compassEconomic*/,
                                          float compassSocial) {
    const float auth = clamp01((100.0f - compassSocial) / 200.0f);  // 0 lib .. 1 auth
    PersonalityProfile p;
    p.aggressiveness = clamp01(0.35f + 0.30f * auth + 0.25f * (hash01(id, 0xA1) - 0.5f));
    p.mindfulness    = clamp01(0.65f - 0.25f * auth + 0.25f * (hash01(id, 0xB2) - 0.5f));
    p.paranoia       = clamp01(0.40f + 0.35f * auth + 0.20f * (hash01(id, 0xC3) - 0.5f));
    p.tactfulness    = clamp01(0.60f - 0.25f * auth + 0.25f * (hash01(id, 0xD4) - 0.5f));
    p.set = true;
    return p;
}

}  // namespace odpers
