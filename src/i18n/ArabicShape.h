// Shaped Arabic-script rendering (Urdu), the HarfBuzz path.
//
// The table shaper in Arabic.cpp is enough for the Arabic language: every
// letter it needs has a precomposed presentation form in Unicode, so choosing
// the form and substituting it is all the joining that is required. Urdu is
// not like that. Its retroflexes and its two-eyed heh (ٹ ڈ ڑ ں ے ھ) have no
// presentation forms, so the substitution table cannot join them and they fall
// apart into stumps. That is the same wall Devanagari hit, and the answer is
// the same: let HarfBuzz choose the glyphs from the font's own tables and draw
// them by id. See Devanagari.h for the shape of the pipeline; this is its
// sibling, right-to-left, against Noto Naskh Arabic.

#pragma once

#include <string>
#include <vector>

#include "raylib.h"

namespace odArab {

bool load(const std::string& fontPath);
void unload();
bool available();

// The Arabic-script blocks this path is responsible for. Standard Arabic in
// the base block is handled here too when Urdu is the active language, so a
// line mixing the two shapes as one run.
bool isArabic(unsigned cp);

// Draw a single Arabic-script run (already isolated by the caller) at (x, y),
// shaping it right-to-left. Returns the run's advance width.
float draw(const std::vector<unsigned>& cps, float x, float y, int fontSize, Color tint);
float measure(const std::vector<unsigned>& cps, int fontSize);

}  // namespace odArab
