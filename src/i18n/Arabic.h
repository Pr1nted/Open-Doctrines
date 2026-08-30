#pragma once
#include <vector>

/**
 * Arabic joining forms and reading direction. See Arabic.cpp for why drawing
 * the codepoints as typed is not enough.
 */
namespace odText {

bool isArabic(unsigned cp);
bool hasArabic(const std::vector<unsigned>& text);

/// Replace each letter with the initial/medial/final/isolated form its
/// neighbours call for, and fuse LAM+ALEF into the one letter it is.
std::vector<unsigned> shapeArabic(const std::vector<unsigned>& in);

/// Put the codepoints in the order they are DRAWN rather than typed: Arabic
/// runs back to front, everything else left alone. One level only.
std::vector<unsigned> reorderForDisplay(const std::vector<unsigned>& in);

}  // namespace odText
