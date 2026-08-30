// See ArabicShape.h. A near-twin of Devanagari.cpp: glyph-id atlas rastered per
// size, HarfBuzz for the shaping, blit on whole pixels. The two differences
// that matter are the script and the direction -- Arabic, right to left -- and
// that HarfBuzz returns an RTL run already in visual (left-to-right) order, so
// the same forward blit loop the Devanagari path uses is correct here. That was
// checked against the shipped font before this was written, not assumed.

#include "i18n/ArabicShape.h"

#include "stb_truetype.h"   // implementation lives in Devanagari.cpp

#ifdef OD_HAVE_HARFBUZZ
#include <hb.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace odArab {
namespace {

constexpr int kAtlasDim = 1024;
constexpr int kPad      = 1;

struct Packed { Rectangle rect; int xoff, yoff; };

std::vector<unsigned char> g_fontData;
stbtt_fontinfo             g_info{};
bool                       g_ready = false;
int                        g_ascent = 0;

Image     g_atlasImg{};
Texture2D g_atlasTex{};
bool      g_atlasDirty = false;
int       g_penX = kPad, g_penY = kPad, g_rowH = 0;

inline unsigned long long key(unsigned gid, int px) {
    return ((unsigned long long)gid << 16) | (unsigned)(px & 0xFFFF);
}
std::unordered_map<unsigned long long, Packed> g_packed;

#ifdef OD_HAVE_HARFBUZZ
hb_blob_t* g_blob = nullptr;
hb_face_t* g_face = nullptr;
hb_font_t* g_hbFont = nullptr;
#endif

const Packed* packGlyph(unsigned gid, int px) {
    const unsigned long long k = key(gid, px);
    auto it = g_packed.find(k);
    if (it != g_packed.end()) return &it->second;

    const float scale = stbtt_ScaleForPixelHeight(&g_info, (float)px);
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* bmp = stbtt_GetGlyphBitmap(&g_info, 0, scale, (int)gid, &w, &h, &xoff, &yoff);
    if (!bmp || w <= 0 || h <= 0) {
        if (bmp) stbtt_FreeBitmap(bmp, nullptr);
        Packed p{{0, 0, 0, 0}, 0, 0};
        return &(g_packed[k] = p);
    }
    if (g_penX + w + kPad > kAtlasDim) { g_penX = kPad; g_penY += g_rowH + kPad; g_rowH = 0; }
    if (g_penY + h + kPad > kAtlasDim) { stbtt_FreeBitmap(bmp, nullptr); return nullptr; }

    unsigned char* atlas = (unsigned char*)g_atlasImg.data;
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const size_t dst = ((size_t)(g_penY + yy) * kAtlasDim + (g_penX + xx)) * 4;
            atlas[dst + 0] = 255; atlas[dst + 1] = 255; atlas[dst + 2] = 255;
            atlas[dst + 3] = bmp[yy * w + xx];
        }
    stbtt_FreeBitmap(bmp, nullptr);

    Packed p{{(float)g_penX, (float)g_penY, (float)w, (float)h}, xoff, yoff};
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

struct Shaped { unsigned gid; float xAdvance, xOffset, yOffset; };

std::vector<Shaped> shapeRun(const std::vector<unsigned>& cps, int px) {
    std::vector<Shaped> out;
#ifdef OD_HAVE_HARFBUZZ
    if (!g_hbFont) return out;
    const float sc = stbtt_ScaleForPixelHeight(&g_info, (float)px);
    const int scale = (int)std::lround(64.0f * (float)hb_face_get_upem(g_face) * sc);
    hb_font_set_scale(g_hbFont, scale, scale);

    hb_buffer_t* buf = hb_buffer_create();
    for (size_t i = 0; i < cps.size(); ++i) hb_buffer_add(buf, cps[i], (unsigned)i);
    hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_set_direction(buf, HB_DIRECTION_RTL);
    hb_buffer_set_script(buf, HB_SCRIPT_ARABIC);
    hb_buffer_set_language(buf, hb_language_from_string("ur", -1));
    hb_shape(g_hbFont, buf, nullptr, 0);

    unsigned n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        out.push_back({info[i].codepoint, pos[i].x_advance / 64.0f,
                       pos[i].x_offset / 64.0f, pos[i].y_offset / 64.0f});
    hb_buffer_destroy(buf);
#else
    (void)cps; (void)px;
#endif
    return out;
}

}  // namespace

bool isArabic(unsigned cp) {
    return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
           (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFF);
}

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
    if (!f) { std::fprintf(stderr, "  Arabic: cannot open %s\n", fontPath.c_str()); return false; }
    g_fontData.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (g_fontData.empty()) return false;
    if (!stbtt_InitFont(&g_info, g_fontData.data(),
                        stbtt_GetFontOffsetForIndex(g_fontData.data(), 0))) {
        std::fprintf(stderr, "  Arabic: stb_truetype refused %s\n", fontPath.c_str());
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
    std::printf("  Arabic: shaped rendering ready (%s)\n", fontPath.c_str());
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
    g_penX = g_penY = kPad; g_rowH = 0;
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
    for (const Shaped& s : glyphs) packGlyph(s.gid, fontSize);
    flushAtlas();

    const float rasterScale = stbtt_ScaleForPixelHeight(&g_info, (float)fontSize);
    const float baseline = y + g_ascent * rasterScale;

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

}  // namespace odArab
