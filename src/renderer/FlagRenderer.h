#pragma once
#include "raylib.h"
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

// ── Flag background patterns ──
enum class FlagType {
    SOLID,           // single color
    HSTRIPES_2,      // 2 horizontal stripes
    HSTRIPES_3,      // 3 horizontal stripes (most common pattern)
    HSTRIPES_N,      // N horizontal stripes (variable)
    VSTRIPES_2,      // 2 vertical stripes
    VSTRIPES_3,      // 3 vertical stripes (France, Italy)
    VSTRIPES_N,      // N vertical stripes (variable)
    DIAGONAL_L,      // diagonal from top-left to bottom-right
    DIAGONAL_R,      // diagonal from top-right to bottom-left
    TRIANGLE,        // triangle on hoist (Bahamas, Cuba, Czechia)
    TRIANGLE_DOUBLE, // double triangle (like a chevron)
    QUARTERED,       // 4 quarters
    SALTIR,          // X cross with 4 colored triangles (Scotland, Alabama)
    CANTON,          // corner box (USA, Australia, Liberia, Malaysia)
    PALE,            // single vertical center stripe
    FESS,            // single horizontal center stripe
    CROSS_NORDIC,    // Nordic cross offset to hoist (Denmark, Sweden)
    CROSS_GREEK,     // Greek cross centered (Switzerland)
    STRIPED_EDGE,    // border/stripe around edge
    SUNBURST         // rays radiating from center
};

// ── Symbols that can be overlaid ──
enum class SymbolType {
    NONE,
    STAR_5,          // 5-pointed star (single)
    STAR_6,          // 6-pointed star (Star of David)
    STAR_7,          // 7-pointed star (Australia, Iraq)
    STARS_CIRCLE,    // stars in circular arrangement (EU)
    STARS_GRID,      // stars in grid arrangement (USA)
    CRESCENT,        // crescent moon (Turkey, Pakistan, Algeria)
    CRESCENT_STAR,   // crescent + star (Islam)
    SUN,             // sun with alternating straight/wavy rays
    SUN_RAYS,        // plain sun with straight rays (Taiwan, Kiribati)
    CROSS_LATIN,     // latin cross (+)
    CROSS_SALTIR,    // X cross (St. Andrew's cross — Scotland)
    CROSS_MALTESE,   // maltese cross (Malta, fire departments)
    CIRCLE,          // filled circle (Japan, Bangladesh)
    DISC,            // outlined circle
    TRIANGLE,        // filled triangle
    DIAMOND,         // filled diamond
    GEAR,            // gear/cogwheel (Angola, industrial flags)
    HAMMER_SICKLE,   // communist symbol
    SWASTIKA,        // hate symbol — only for extreme radical
    SWORD,           // single sword
    CROSSED_SWORDS,  // crossed swords
    ARROW,           // single arrow
    MOUNTAIN,        // mountain silhouette
    TREE,            // tree or leaf silhouette
    TEXT_BLOCK,      // text block placeholder
    COAT_ARMS,       // coat of arms — loaded from SVG/PNG
    SVG_FILE,        // SVG loaded from path in sym.text
    CENSOR_BAR       // black circle with diagonal lines
};

struct FlagSymbol {
    SymbolType type = SymbolType::NONE;
    std::vector<Color> colors;
    int count = 0;        // for STARS_CIRCLE/STARS_GRID/CROSSES
    std::string text;     // for TEXT_BLOCK / COAT_ARMS path
    float x = 0.5f, y = 0.5f;
    float size = 0.3f;    // relative to min(width, height)
    float rotation = 0.0f; // degrees
};

struct FlagPattern {
    FlagType type = FlagType::SOLID;
    std::vector<Color> colors;
    std::vector<FlagSymbol> symbols;
    int starCount = 1;
    std::string imagePath; // non-empty = load actual flag image from file
    bool censored = false; // pixelate to obscure hate symbols
};

static Color hexToColor(const std::string& hex) {
    if (hex.empty() || hex[0] != '#') return BLANK;
    unsigned int r, g, b;
    sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
    return { (unsigned char)r, (unsigned char)g, (unsigned char)b, 255 };
}

class FlagRenderer {
public:
    static Texture2D render(const FlagPattern& pattern, int width, int height, const std::string& baseDir = "",
                             const std::unordered_map<std::string, std::string>* odmData = nullptr);

private:
    static void drawFlagBackground(Image* img, const FlagPattern& pattern, int width, int height);
    static void drawSymbol(Image* img, const FlagSymbol& sym, int width, int height, const std::string& baseDir = "",
                            const std::unordered_map<std::string, std::string>* odmData = nullptr);

    // Background helpers
    static void drawSolid(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawStripesH(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawStripesV(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawDiagonal(Image* img, int w, int h, bool rightDown, const std::vector<Color>& colors);
    static void drawTriangle(Image* img, int w, int h, bool doubleTri, const std::vector<Color>& colors);
    static void drawQuartered(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawSaltir(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawCanton(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawPale(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawFess(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawNordicCross(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawGreekCross(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawBorder(Image* img, int w, int h, const std::vector<Color>& colors);
    static void drawSunburst(Image* img, int w, int h, const std::vector<Color>& colors);

    // Symbol drawing helpers (all now use SVG_FILE from Wikipedia)
    // Only procedural fallbacks remain for symbols without SVG equivalents

    // Core drawing primitives
    static void fillImage(Image* img, Color c);
    static void fillCircle(Image* img, int cx, int cy, int r, Color c);
    static void fillRect(Image* img, int x, int y, int w, int h, Color c);
    static void fillTriangle(Image* img, int x1, int y1, int x2, int y2, int x3, int y3, Color c);
    static void drawLine(Image* img, int x1, int y1, int x2, int y2, int thick, Color c);
    static void imagePixelate(Image* img, int blockSize);
    static void imageBlur(Image* img, int radius);
    static void drawTextBlock(Image* img, int cx, int cy, int w, int h, Color c);
    static void drawCensorBar(Image* img, int cx, int cy, int rad);

    // SVG rasterization with cache
    static Image rasterizeSVG(const std::string& filePath, int width, int height, const std::string& baseDir,
                               const std::unordered_map<std::string, std::string>* odmData = nullptr);
    static Image recolorImage(const Image& src, Color newColor);
};
