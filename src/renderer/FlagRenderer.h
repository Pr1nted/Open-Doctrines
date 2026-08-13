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
    // Assets that shipped in data/symbols/ with no SymbolType able to name
    // them, and so could not appear on any flag, authored or generated.
    ANCHOR,          // anchor (maritime, and a common labour emblem)
    TORCH,           // torch
    ROSE,            // rose (socialist / labour)
    FASCES,          // fasces -- Roman, and used by half of Europe's republics
    CROSS_PATTEE,    // cross pattée
    STAR_4,          // 4-pointed star
    STAR_OF_DAVID,   // star of David
    // Drawn for the identity vocabularies by tools/generate_symbols.py, so that
    // two hundred countries are not all charged with the same star.
    WREATH,          // laurel wreath
    HAMMER,          // hammer (labour)
    LIGHTNING,       // lightning bolt
    SUN_SPLENDOUR,   // sun with straight rays
    TEXT_BLOCK,      // text block placeholder
    COAT_ARMS,       // coat of arms — loaded from SVG/PNG
    SVG_FILE,        // SVG loaded from path in sym.text
    CENSOR_BAR       // black circle with diagonal lines
};

// ── How a generated emblem is laid out ──
//
// One device says "a government"; several say "a union of somebodies". The
// collective identities get arrangements and the authoritarian ones get a
// single charge, which is the distinction real flags draw too.
enum class SymbolLayout {
    SINGLE,          // one emblem
    ROW,             // `count` in a horizontal row
    ARC,             // `count` on a shallow arc
    CIRCLE,          // `count` evenly around a ring
    GRID,            // `count` in rows and columns
    ESCORTED,        // one emblem with `count` small stars arcing beside it
};

// ── A field drawn UNDER a generated emblem ──
//
// Some flags have nowhere to put a device. The United States is stripes and a
// star-filled union edge to edge; the Union Jack is crossed all the way to its
// corners. An emblem dropped on either sits ON the flag rather than in it, and
// no amount of outlining fixes that -- what those flags need is a plain field
// of their own, which is exactly what a revolution putting its mark on an old
// flag has always done.
enum class EmblemField {
    NONE,            // straight onto the flag
    AUTO_CANTON,     // a canton, but only if the flag has nowhere quiet enough
    AUTO_HOIST,      // a hoist band, on the same condition
    CANTON,          // always: a rectangle in the upper hoist
    HOIST,           // always: a full-height band at the hoist
};

/**
 * An ideological recolouring of a whole flag, raster included.
 *
 * Expressed in HSL, because mixing toward an RGB target turns a blue-and-white
 * flag purple: the hue is REPLACED rather than interpolated. Whites, blacks and
 * near-greys are left exactly alone -- they carry the flag's structure and none
 * of its politics.
 *
 * WHERE EACH COLOUR LANDS, AND WHY IT IS NOT ITS OWN LIGHTNESS
 *
 * The first version kept each colour's lightness, on the theory that a light
 * stripe should stay light. It produced mud. The Union Jack's navy sits at
 * lightness 0.19, and gold at lightness 0.19 is olive-brown; the whole flag
 * came out the colour of a wet field. Gold is only gold when it is LIGHT.
 *
 * So a flag's own chromatic colours are ranked and mapped onto a band: its
 * darkest becomes a near-neutral shade, its lightest becomes the ideological
 * colour at full strength, and anything between is interpolated. That keeps the
 * flag's internal contrast -- which is what makes it recognisable -- while
 * letting each ideology's colour appear at the lightness it actually works at.
 * A navy-and-red jack becomes a charcoal-and-gold one rather than two olives.
 *
 * It lives on the pattern rather than in the colours because most countries fly
 * a raster image, where there is no palette to rewrite -- the renderer is the
 * only thing holding the pixels.
 */
struct FlagRecolor {
    bool  active   = false;
    bool  shiftHue = true;   // false for a target that is not a hue: darken instead
    float hue      = 0.0f;   // degrees
    // Where the flag's LIGHTEST chromatic colour goes.
    float sat        = 0.0f;
    float light      = 0.0f;
    // ...and where its DARKEST does. Not simply a dark version of the above.
    float shadeSat   = 0.0f;
    float shadeLight = 0.0f;
    float weight     = 0.0f;   // how far
};

struct FlagSymbol {
    SymbolType type = SymbolType::NONE;
    std::vector<Color> colors;
    int count = 0;        // for STARS_CIRCLE/STARS_GRID/CROSSES
    std::string text;     // for TEXT_BLOCK / COAT_ARMS path
    float x = 0.5f, y = 0.5f;
    float size = 0.3f;    // relative to min(width, height)
    float rotation = 0.0f; // degrees

    // ── For emblems that are GENERATED rather than authored ──
    //
    // A flag in the data files says where its symbol goes and what colour it
    // is, and it is right, because a person placed it against that particular
    // flag. PoliticalIdentity::applyFlag has no such person: it drops one
    // emblem onto two hundred flags it has never seen, and a fixed white star
    // at a fixed point lands invisibly on Poland's white half and across the
    // arm of Britain's saltire. These two ask the renderer -- which is the only
    // thing that can see the finished pixels -- to finish the job.

    // Keep the emblem legible: `colors[0]` becomes a preference, and where it
    // has too little contrast against what it lands on, the emblem is
    // fimbriated (outlined) rather than recoloured, which is how flags have
    // always solved this.
    bool autoContrast = false;

    // Treat (x, y) as the FIRST CANDIDATE rather than the only position, and
    // move the emblem to the quietest part of the flag if that spot is busy.
    bool autoPlace = false;

    // One emblem or an arrangement of them, and how many.
    SymbolLayout layout = SymbolLayout::SINGLE;

    // What to do when the flag has nowhere an emblem can go. An AUTO_ field is
    // drawn only when the best position the search can find is still bad.
    EmblemField field = EmblemField::NONE;
    Color fieldColor = { 170, 30, 30, 255 };
};

struct FlagPattern {
    FlagType type = FlagType::SOLID;
    std::vector<Color> colors;
    std::vector<FlagSymbol> symbols;
    int starCount = 1;
    std::string imagePath; // non-empty = load actual flag image from file
    bool censored = false; // pixelate to obscure hate symbols
    FlagRecolor recolor;   // ideological restyle, applied to pattern and image alike
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

    // SVG rasterization with cache (public: the map editor previews dropped-in
    // custom flag SVGs through it)
    static Image rasterizeSVG(const std::string& filePath, int width, int height, const std::string& baseDir,
                               const std::unordered_map<std::string, std::string>* odmData = nullptr);

private:
    // The shared tail of render(): recolour, symbols, censor, upload.
    static Texture2D compose(Image* img, const FlagPattern& pattern, int width, int height,
                              const std::string& baseDir,
                              const std::unordered_map<std::string, std::string>* odmData);
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
    static void fillRing(Image* img, int cx, int cy, int rOuter, int rInner, Color c);

    // Support for FlagSymbol::autoPlace / autoContrast. All read the
    // destination image, so none of them can live where the pattern is built.
    // `coverage` is the fraction of the square that is actually flag. Flags
    // that are not 2:1 are stored letterboxed into a 256x128 slot, so part of
    // a candidate square can be transparent padding -- which is not somewhere
    // an emblem can go, and reads as black if it is measured as though it were.
    // `purity` is the fraction of the square that is a single flat colour; see
    // measureField for why it, and not variance, decides whether an emblem can
    // sit somewhere.
    struct FieldStats {
        float mean = 0.0f; float variance = 0.0f; float coverage = 0.0f; float purity = 0.0f;
    };
    static FieldStats measureField(const Image& img, int cx, int cy, int r);
    static FieldStats measureRegion(const Image& img, int x, int y, int w, int h);

    // The part of the canvas that is actually flag.
    //
    // A flag is drawn into a 256x128 slot whatever shape it is, so the canvas
    // and the flag are the same thing only for the 2:1 ones. Switzerland is
    // square and fills the left half; Nepal is a double pennon and fills a
    // quarter of its box in a shape with a diagonal edge. Everything that
    // reasons about WHERE on a flag something goes has to reason in these
    // bounds, or it lands in the empty part of the canvas.
    struct FlagBounds { int x = 0, y = 0, w = 0, h = 0; };
    static FlagBounds flagBounds(const Image& img);
    static void fillRectMasked(Image* img, int x, int y, int w, int h, Color c);

    // Returns the score of the position it chose. Low is good; above
    // EMBLEM_FIELD_THRESHOLD the flag has nowhere an emblem can simply sit.
    static float placeSymbol(const Image& img, const FlagSymbol& sym, const FlagBounds& b,
                              int r, Color symbolColor, int* cx, int* cy);

    // One instance of an arrangement: what to draw, where, and how big.
    struct SymbolInstance { SymbolType type; int cx, cy, r; };
    static int  groupExtent(const FlagSymbol& sym, int r);
    static void layoutInstances(const FlagSymbol& sym, int cx, int cy, int r,
                                 std::vector<SymbolInstance>& out);
    static void drawOneSymbol(Image* img, const FlagSymbol& sym, SymbolType type,
                               int cx, int cy, int r, Color sc,
                               int outlineWidth, Color outlineColor,
                               const std::string& baseDir,
                               const std::unordered_map<std::string, std::string>* odmData);
    static void recolorFlag(Image* img, const FlagRecolor& rc);

    static Image recolorImage(const Image& src, Color newColor);
};
