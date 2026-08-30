#pragma once
#include "raylib.h"

#include <string>
#include <vector>

/**
 * Devanagari: shaped with HarfBuzz, drawn from an atlas keyed by GLYPH ID.
 *
 * WHY THIS IS A SEPARATE PIPELINE. Everything else in this game is drawn
 * through raylib, whose font API is codepoint-keyed from end to end --
 * LoadFontData, GetGlyphAtlasRec and DrawTextCodepoint all take a codepoint.
 * HarfBuzz answers in glyph IDs, and the whole point of shaping Devanagari is
 * that the glyph you must draw HAS NO CODEPOINT: the conjunct in हिन्दी is
 * "uni0928094D", one glyph made out of two characters, and no codepoint names
 * it. So the glyphs are rasterised here, by id, with stb_truetype.
 *
 * Absent HarfBuzz (see OD_ENABLE_SHAPING) every entry point below reports
 * unavailable and the caller falls back to drawing the codepoints, which is
 * readable and visibly wrong at every conjunct.
 */
namespace odDeva {

/// Load the font and start the shaper. Safe to call twice; false if either
/// the file is missing or the build has no shaper.
bool load(const std::string& fontPath);
void unload();

/// True when text can actually be shaped and drawn.
bool available();

/// True if this codepoint belongs to the Devanagari block.
bool isDevanagari(unsigned cp);

/// Draw a shaped run. Returns the width used.
float draw(const std::vector<unsigned>& codepoints, float x, float y,
           int fontSize, Color tint);
/// The width `draw` would use.
float measure(const std::vector<unsigned>& codepoints, int fontSize);

}  // namespace odDeva
