// Devanagari rendering: HarfBuzz decides the glyphs, stb_truetype draws them.
//
// See Devanagari.h for why this cannot go through raylib's font path. The
// short version: the glyph a conjunct needs has no codepoint, and raylib is
// codepoint-keyed everywhere.
//
// THE ATLAS IS RASTERISED AT THE SIZE IT IS DRAWN AT, once per size.
//
// The first version rasterised everything at 48 and scaled down, which is what
// the Unifont atlas does. It looked wrong, and the reason is worth writing
// down: the shirorekha -- the headline every Devanagari letter hangs from -- is
// ONE PIXEL tall at interface sizes. Scale a 48px raster to 14 and that line
// lands between pixels and comes out as a dotted rule, so the word loses the
// bar that visually joins it into a word. Noto is an outline font and can be
// rasterised at any size, so it is, and the hinted line stays solid.
//
// The cost is one entry per (glyph, size) rather than per glyph. The interface
// uses a handful of sizes, so this is tens of kilobytes, not a texture per
// size.

#include "i18n/Devanagari.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifdef OD_HAVE_HARFBUZZ
#include <hb.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace odDeva {
namespace {

// Big enough that a conjunct's parts are still distinct after scaling down to
// interface sizes, small enough that a few hundred glyphs fit one atlas page.
constexpr int  kAtlasDim  = 1024;
constexpr int  kPad       = 1;

struct Packed {
    Rectangle rect;      // where it sits in the atlas
    int   xoff, yoff;    // stb's bearing, in atlas pixels
    float advance;       // unused for drawing; HarfBuzz supplies advances
};

std::vector<unsigned char> g_fontData;
stbtt_fontinfo             g_info{};
bool                       g_ready = false;
int                        g_ascent = 0;

/// One cache entry per glyph AND size; see the note at the top of the file.
inline unsigned long long key(unsigned gid, int px) {
    return ((unsigned long long)gid << 16) | (unsigned)(px & 0xFFFF);
}

Image                      g_atlasImg{};
Texture2D                  g_atlasTex{};
bool                       g_atlasDirty = false;
int                        g_penX = kPad, g_penY = kPad, g_rowH = 0;
std::unordered_map<unsigned long long, Packed> g_packed;

#ifdef OD_HAVE_HARFBUZZ
hb_blob_t* g_blob = nullptr;
hb_face_t* g_face = nullptr;
hb_font_t* g_hbFont = nullptr;
#endif

/// Rasterise one glyph by ID at one size and shelf-pack it. Null if it will not fit.
const Packed* packGlyph(unsigned gid, int px) {
    const unsigned long long k = key(gid, px);
    auto it = g_packed.find(k);
    if (it != g_packed.end()) return &it->second;

    const float scale = stbtt_ScaleForPixelHeight(&g_info, (float)px);
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* bmp = stbtt_GetGlyphBitmap(&g_info, 0, scale, (int)gid,
                                              &w, &h, &xoff, &yoff);

    // A space, or any glyph with no ink, is still a glyph: it takes an entry
    // with an empty rectangle so the advance from HarfBuzz still applies.
    if (!bmp || w <= 0 || h <= 0) {
        if (bmp) stbtt_FreeBitmap(bmp, nullptr);
        Packed p{{0, 0, 0, 0}, 0, 0, 0.0f};
        return &(g_packed[k] = p);
    }

    if (g_penX + w + kPad > kAtlasDim) {      // next shelf
        g_penX = kPad;
        g_penY += g_rowH + kPad;
        g_rowH = 0;
    }
    if (g_penY + h + kPad > kAtlasDim) {      // atlas full
        stbtt_FreeBitmap(bmp, nullptr);
        return nullptr;
    }

    // White with the coverage in alpha, so a tint colours it the way every
    // other piece of text in the game is coloured.
    unsigned char* atlas = (unsigned char*)g_atlasImg.data;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            const size_t dst = ((size_t)(g_penY + yy) * kAtlasDim + (g_penX + xx)) * 4;
            atlas[dst + 0] = 255;
            atlas[dst + 1] = 255;
            atlas[dst + 2] = 255;
            atlas[dst + 3] = bmp[yy * w + xx];
        }
    }
    stbtt_FreeBitmap(bmp, nullptr);

    Packed p{{(float)g_penX, (float)g_penY, (float)w, (float)h}, xoff, yoff, 0.0f};
    g_penX += w + kPad;
    if (h > g_rowH) g_rowH = h;
    g_atlasDirty = true;
    return &(g_packed[k] = p);
}

void flushAtlas() {
    if (!g_atlasDirty) return;
    if (g_atlasTex.id == 0) g_atlasTex = LoadTextureFromImage(g_atlasImg);
    else                    UpdateTexture(g_atlasTex, g_atlasImg.data);
    g_atlasDirty = false;
}

struct Shaped {
    unsigned gid;
    float xAdvance, xOffset, yOffset;   // atlas pixels
};

/// Shape at the size the text will be DRAWN at.
///
/// This used to shape once at 48 and scale the advances down, which is what
/// you do when the raster is also at 48. It is wrong when the raster is at the
/// target size, and it is wrong in a way that is specific to this script: the
/// shirorekha, the horizontal bar every Devanagari letter hangs from, is drawn
/// as part of each glyph and is supposed to ABUT its neighbour's so a word
/// carries one unbroken line. Scale the advance from another size and the
/// abutment misses by a fraction of a pixel -- and at interface sizes the bar
/// is one pixel tall, so a fractional miss is a visible gap and the word comes
/// apart into separate letters.
std::vector<Shaped> shapeRun(const std::vector<unsigned>& cps, int px) {
    std::vector<Shaped> out;
#ifdef OD_HAVE_HARFBUZZ
    if (!g_hbFont) return out;

    // HARFBUZZ AND STB DISAGREE ABOUT WHAT "19 PIXELS" MEANS, and the whole
    // headline problem was that disagreement.
    //
    // hb_font_set_scale(px*64) makes one EM equal px pixels.
    // stbtt_ScaleForPixelHeight(px) makes ASCENT-TO-DESCENT equal px pixels.
    // For Noto Sans Devanagari those differ by about 30%, so every advance
    // came back ~30% wider than the glyph stb had drawn, and each letter was
    // pushed clear of its neighbour -- which in this script means the
    // shirorekha stops being a line and becomes a row of dashes.
    //
    // The raster is the thing on screen, so the raster's scale wins and the
    // shaper is told to use it: units * (upem * sc) / upem == units * sc.
    const float sc = stbtt_ScaleForPixelHeight(&g_info, (float)px);
    const int scale = (int)std::lround(64.0f * (float)hb_face_get_upem(g_face) * sc);
    hb_font_set_scale(g_hbFont, scale, scale);
    hb_buffer_t* buf = hb_buffer_create();
    for (size_t i = 0; i < cps.size(); ++i) hb_buffer_add(buf, cps[i], (unsigned)i);
    hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_DEVANAGARI);
    hb_buffer_set_language(buf, hb_language_from_string("hi", -1));
    hb_shape(g_hbFont, buf, nullptr, 0);

    unsigned n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        out.push_back({info[i].codepoint,
                       pos[i].x_advance / 64.0f,
                       pos[i].x_offset / 64.0f,
                       pos[i].y_offset / 64.0f});
    hb_buffer_destroy(buf);
#else
    (void)cps; (void)px;
#endif
    return out;
}

}  // namespace

bool isDevanagari(unsigned cp) { return cp >= 0x0900 && cp <= 0x097F; }

bool available() {
#ifdef OD_HAVE_HARFBUZZ
    return g_ready;
#else
    return false;
#endif
}

bool load(const std::string& fontPath) {
#ifndef OD_HAVE_HARFBUZZ
    (void)fontPath;
    return false;
#else
    if (g_ready) return true;

    std::ifstream f(fontPath, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "  Devanagari: cannot open %s\n", fontPath.c_str());
        return false;
    }
    g_fontData.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (g_fontData.empty()) return false;

    if (!stbtt_InitFont(&g_info, g_fontData.data(),
                        stbtt_GetFontOffsetForIndex(g_fontData.data(), 0))) {
        std::fprintf(stderr, "  Devanagari: stb_truetype refused %s\n", fontPath.c_str());
        return false;
    }
    int descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&g_info, &g_ascent, &descent, &lineGap);

    g_atlasImg = GenImageColor(kAtlasDim, kAtlasDim, BLANK);
    ImageFormat(&g_atlasImg, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    g_blob = hb_blob_create((const char*)g_fontData.data(), (unsigned)g_fontData.size(),
                            HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    g_face = hb_face_create(g_blob, 0);
    g_hbFont = hb_font_create(g_face);
    g_ready = true;
    std::printf("  Devanagari: shaped rendering ready (%s)\n", fontPath.c_str());
    return true;
#endif
}

void unload() {
#ifdef OD_HAVE_HARFBUZZ
    if (g_hbFont) { hb_font_destroy(g_hbFont); g_hbFont = nullptr; }
    if (g_face)   { hb_face_destroy(g_face);   g_face = nullptr; }
    if (g_blob)   { hb_blob_destroy(g_blob);   g_blob = nullptr; }
#endif
    if (g_atlasTex.id > 0) { UnloadTexture(g_atlasTex); g_atlasTex = {}; }
    if (g_atlasImg.data)   { UnloadImage(g_atlasImg);   g_atlasImg = {}; }
    g_packed.clear();
    g_penX = g_penY = kPad;
    g_rowH = 0;
    g_ready = false;
}

float measure(const std::vector<unsigned>& cps, int fontSize) {
    if (!available()) return 0.0f;
    float w = 0.0f;
    for (const Shaped& s : shapeRun(cps, fontSize)) w += s.xAdvance;
    return w;
}

float draw(const std::vector<unsigned>& cps, float x, float y, int fontSize, Color tint) {
    if (!available()) return 0.0f;
    const std::vector<Shaped> glyphs = shapeRun(cps, fontSize);

    // Pack first, upload once: a texture update per glyph would be a stall per
    // letter on the first frame a language is shown.
    for (const Shaped& s : glyphs) packGlyph(s.gid, fontSize);
    flushAtlas();

    // stb's bearings are measured from the baseline; the caller gives the top
    // of the line, as every other DrawText in this game does.
    const float rasterScale = stbtt_ScaleForPixelHeight(&g_info, (float)fontSize);
    const float baseline = y + g_ascent * rasterScale;

    // THE PEN IS FRACTIONAL, THE BLIT IS NOT.
    //
    // The accumulated position keeps its fraction so the run does not drift,
    // but each glyph is placed on a whole pixel. A one-pixel bar landing on a
    // half pixel is drawn as two half-lit rows, which reads as a grey smear
    // rather than a rule -- and the neighbouring letter's bar smears the other
    // way, so the join looks broken even when the arithmetic is right.
    float penX = x;
    for (const Shaped& s : glyphs) {
        auto it = g_packed.find(key(s.gid, fontSize));
        if (it != g_packed.end() && it->second.rect.width > 0.0f) {
            const Packed& p = it->second;
            const Rectangle dst{std::floor(penX + s.xOffset + p.xoff + 0.5f),
                                std::floor(baseline - s.yOffset + p.yoff + 0.5f),
                                p.rect.width, p.rect.height};
            DrawTexturePro(g_atlasTex, p.rect, dst, {0, 0}, 0.0f, tint);
        }
        penX += s.xAdvance;
    }
    return penX - x;
}

}  // namespace odDeva
