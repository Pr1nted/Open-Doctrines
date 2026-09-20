#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * What a mod may draw on the map: a tint and a label.
 *
 * ── DELIBERATELY NOT "RENDERING" ──
 *
 * A mod cannot take over how the map is drawn. It can colour a province and
 * put a word next to one, and that is the whole surface.
 *
 * The line is drawn there because the two sides fail so differently. A mod
 * that tints the wrong province makes the map ugly and the player switches the
 * mod off. A mod that owns the render loop can make the game unstartable,
 * unreadable, or invisible -- and the screen it would have to be switched off
 * from is the one it is drawing. "Ugly" is recoverable without leaving the
 * game; "blank" is not.
 *
 * ── BOUNDED, BECAUSE THE FRAME IS NOT THE MOD'S TO SPEND ──
 *
 * A mod adds entries between frames; the game draws them inside one. So the
 * counts are capped and the text is capped, and a mod that asks for more gets
 * a refusal rather than a slower game. Without a ceiling, the cost of a mod's
 * mistake is paid in frame time by a player who cannot see why.
 *
 * ── EVERY ENTRY IS OWNED ──
 *
 * Keyed by mod id, so unloading a mod takes its marks with it and no mod can
 * clear another's. A tint left behind by a mod that is no longer running is
 * indistinguishable, to a player, from the game being wrong.
 */
namespace odrender {

/// Per mod, not in total: one misbehaving mod cannot crowd out the others.
constexpr size_t kMaxTintsPerMod = 4096;     ///< every province on the largest map, twice
constexpr size_t kMaxLabelsPerMod = 512;     ///< far more than a screen can show
constexpr size_t kMaxLabelChars = 48;

struct Tint {
    int provinceId = 0;
    uint32_t rgba = 0;
};

struct Label {
    int provinceId = 0;
    std::string text;
    uint32_t rgba = 0;
};

class Layer {
public:
    /**
     * Tint a province. An rgba with zero alpha REMOVES the tint rather than
     * drawing nothing, so a mod has one call for both and cannot leak entries
     * by repeatedly "clearing" with transparent paint.
     */
    bool setTint(const std::string& modId, int provinceId, uint32_t rgba);

    /** Put a short label at a province. Empty text removes it, as above. */
    bool setLabel(const std::string& modId, int provinceId, const std::string& text,
                  uint32_t rgba);

    /** Drop everything this mod has drawn. */
    void clearMod(const std::string& modId);
    void clear();

    /** How many marks a mod currently holds, for its own bookkeeping. */
    size_t tintCount(const std::string& modId) const;
    size_t labelCount(const std::string& modId) const;

    /**
     * Everything to draw, in mod order then province order.
     *
     * Stable so two frames of an unchanged world draw identically -- a list
     * that reshuffled would make overlapping labels flicker between frames
     * for no reason the player could act on.
     */
    std::vector<Tint> tints() const;
    std::vector<Label> labels() const;

    /** Whether anything at all is drawn. Lets the draw pass skip out early. */
    bool empty() const;

private:
    struct ModMarks {
        std::vector<Tint> tints;
        std::vector<Label> labels;
    };
    std::vector<std::pair<std::string, ModMarks>> m_byMod;  ///< sorted by mod id
    ModMarks* forMod(const std::string& modId, bool create);
    const ModMarks* forMod(const std::string& modId) const;
};

}  // namespace odrender
