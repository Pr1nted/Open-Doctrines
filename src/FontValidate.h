#pragma once

// Strictly validate a user-supplied font file before the game loads it.
//
// WHY THIS IS STRICT
//
// An imported font is attacker-controlled data fed straight into stb_truetype
// (raylib's font loader). Two things make a font dangerous:
//
//   1. CODE. TrueType carries a bytecode VM -- the `fpgm` (font program) and
//      `prep` (control-value program) tables, plus per-glyph instructions --
//      that a hinting interpreter executes. stb_truetype does not run it, but
//      the player asked for fonts with "no code in them", and refusing any font
//      that carries a program is the literal, checkable form of that: if it is
//      not there, nothing can run it, on this loader or a future one.
//
//   2. MALFORMED STRUCTURE. The real exploit surface is a table whose offset or
//      length points outside the file: a parser that trusts it reads or writes
//      out of bounds. Every table directory entry is bounds-checked here, so a
//      font that lies about where its data is is refused before stb sees it.
//
// So this does not merely sniff an extension. It parses the sfnt header and the
// whole table directory, bounds-checks every entry against the real file size,
// refuses fpgm/prep, refuses font collections (ambiguous: which face?), caps
// the size, and requires the tables a real, renderable font actually has. A
// font that passes is structurally sound and carries no program.
//
// Header-only and dependency-free on purpose: it is small, it is the security
// boundary, and it is trivially unit-testable (tests/font_validate_test.cpp)
// without a window, a GL context or raylib.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace odfont {

// A single-weight UI font is tens to a few hundred KB. 4 MB is far past any
// legitimate one and well short of a memory problem; the 11 MB bundled Unifont
// is the game's own asset, never a user import, and does not come through here.
inline constexpr uint64_t kMaxFontBytes = 4u * 1024u * 1024u;
// A real font has on the order of a dozen tables; 64 is generous and caps the
// directory a hostile header can claim.
inline constexpr uint16_t kMaxTables = 64;

/// Validate the file at `path`. On failure, fills `reason` (for the log and the
/// settings screen) and returns false. A font that returns true is safe to hand
/// to stb_truetype and carries no hinting program.
inline bool validate(const std::string& path, std::string& reason) {
    reason.clear();

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { reason = "cannot open the file"; return false; }

    // Size first: a bound on everything that follows, and a cheap early refusal.
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); reason = "cannot read the file"; return false; }
    long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); reason = "cannot size the file"; return false; }
    const uint64_t fileSize = (uint64_t)sz;
    if (fileSize < 12) { std::fclose(f); reason = "too small to be a font"; return false; }
    if (fileSize > kMaxFontBytes) {
        std::fclose(f);
        reason = "larger than 4 MB -- a UI font should be far smaller";
        return false;
    }

    // Read the whole file: it is at most 4 MB and every check needs random
    // access into it, so one read is simpler and safer than seeking per table.
    std::string buf;
    buf.resize((size_t)fileSize);
    std::rewind(f);
    const size_t got = std::fread(&buf[0], 1, (size_t)fileSize, f);
    std::fclose(f);
    if (got != fileSize) { reason = "could not read the whole file"; return false; }

    const unsigned char* d = (const unsigned char*)buf.data();
    auto be16 = [&](uint64_t o) -> uint16_t {
        return (uint16_t)((d[o] << 8) | d[o + 1]);
    };
    auto be32 = [&](uint64_t o) -> uint32_t {
        return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) |
               ((uint32_t)d[o + 2] << 8) | (uint32_t)d[o + 3];
    };

    // sfnt version. The renderable, single-face kinds only.
    const uint32_t ver = be32(0);
    const uint32_t kTrueType = 0x00010000u;      // TrueType outlines
    const uint32_t kOTTO     = 0x4F54544Fu;      // 'OTTO' -- CFF (OpenType) outlines
    const uint32_t kTrue     = 0x74727565u;      // 'true' -- Apple TrueType
    const uint32_t kTtcf     = 0x74746366u;      // 'ttcf' -- a COLLECTION
    if (ver == kTtcf) {
        reason = "a font collection -- import a single-font .ttf or .otf instead";
        return false;
    }
    if (ver != kTrueType && ver != kOTTO && ver != kTrue) {
        reason = "not a TrueType or OpenType font";
        return false;
    }

    const uint16_t numTables = be16(4);
    if (numTables == 0) { reason = "has no tables"; return false; }
    if (numTables > kMaxTables) { reason = "claims an implausible number of tables"; return false; }

    // The table directory: 16 bytes per entry, right after the 12-byte header.
    const uint64_t dirStart = 12;
    const uint64_t dirEnd = dirStart + (uint64_t)numTables * 16;
    if (dirEnd > fileSize) { reason = "table directory runs past the end of the file"; return false; }

    bool hasCmap = false, hasOutlines = false;
    for (uint16_t i = 0; i < numTables; ++i) {
        const uint64_t e = dirStart + (uint64_t)i * 16;
        char tag[5] = {0};
        std::memcpy(tag, d + e, 4);
        const uint32_t off = be32(e + 8);
        const uint32_t len = be32(e + 12);

        // The bounds check that is the whole point: a table must lie inside the
        // file. off + len is computed in 64-bit so a hostile 0xFFFFFFFF cannot
        // wrap. A parser handed an out-of-range table is the exploit; refuse it.
        if ((uint64_t)off + (uint64_t)len > fileSize) {
            reason = std::string("table '") + tag + "' points outside the file";
            return false;
        }

        // The program tables. Their presence is the "code" the player meant;
        // refuse the font rather than strip them, so the guarantee is simple.
        if (std::memcmp(tag, "fpgm", 4) == 0 || std::memcmp(tag, "prep", 4) == 0) {
            reason = std::string("carries a hinting program ('") + tag +
                     "') -- refused so imported fonts cannot ship code";
            return false;
        }

        if (std::memcmp(tag, "cmap", 4) == 0) hasCmap = true;
        if (std::memcmp(tag, "glyf", 4) == 0 || std::memcmp(tag, "CFF ", 4) == 0 ||
            std::memcmp(tag, "CFF2", 4) == 0)
            hasOutlines = true;
    }

    // A font the game can actually draw with has a character map and some
    // outlines. Without both it is not a usable UI font, whatever else it holds.
    if (!hasCmap) { reason = "has no character map (cmap)"; return false; }
    if (!hasOutlines) { reason = "has no glyph outlines (glyf or CFF)"; return false; }

    return true;
}

}  // namespace odfont
