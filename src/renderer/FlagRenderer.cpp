#include "FlagRenderer.h"
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <cstring>
#include "nanosvg.h"
#include "nanosvgrast.h"

// ══════════════════════════════════════════════════════
//  Color helper
// ══════════════════════════════════════════════════════
static Color getColor(const std::vector<Color>& colors, int index, Color fallback) {
    return (index < (int)colors.size()) ? colors[index] : fallback;
}

// ══════════════════════════════════════════════════════
//  Pixel-level primitives
// ══════════════════════════════════════════════════════
void FlagRenderer::fillImage(Image* img, Color c) {
    for (int y = 0; y < img->height; ++y)
        for (int x = 0; x < img->width; ++x)
            ImageDrawPixel(img, x, y, c);
}

void FlagRenderer::fillCircle(Image* img, int cx, int cy, int r, Color c) {
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r * r) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < img->width && py >= 0 && py < img->height)
                    ImageDrawPixel(img, px, py, c);
            }
}

void FlagRenderer::fillRect(Image* img, int x, int y, int w, int h, Color c) {
    for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx) {
            int px = x + dx, py = y + dy;
            if (px >= 0 && px < img->width && py >= 0 && py < img->height)
                ImageDrawPixel(img, px, py, c);
        }
}

void FlagRenderer::fillTriangle(Image* img, int x1, int y1, int x2, int y2, int x3, int y3, Color c) {
    // Rasterize via barycentric
    int minX = std::max(0, std::min({x1, x2, x3}));
    int maxX = std::min(img->width - 1, std::max({x1, x2, x3}));
    int minY = std::max(0, std::min({y1, y2, y3}));
    int maxY = std::min(img->height - 1, std::max({y1, y2, y3}));
    auto orient = [](int ax, int ay, int bx, int by, int cx, int cy) {
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };
    int o1 = orient(x1, y1, x2, y2, x3, y3);
    if (o1 == 0) return;
    for (int py = minY; py <= maxY; ++py)
        for (int px = minX; px <= maxX; ++px) {
            int o2 = orient(x2, y2, x3, y3, px, py);
            int o3 = orient(x3, y3, x1, y1, px, py);
            if ((o1 > 0 && o2 >= 0 && o3 >= 0) || (o1 < 0 && o2 <= 0 && o3 <= 0))
                ImageDrawPixel(img, px, py, c);
        }
}

void FlagRenderer::drawLine(Image* img, int x1, int y1, int x2, int y2, int thick, Color c) {
    // Bresenham-style thick line using small rectangles
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    int px = x1, py = y1;
    while (true) {
        for (int t = -thick/2; t <= thick/2; ++t)
            for (int tt = -thick/2; tt <= thick/2; ++tt) {
                int tx = px + t, ty = py + tt;
                if (tx >= 0 && tx < img->width && ty >= 0 && ty < img->height)
                    ImageDrawPixel(img, tx, ty, c);
            }
        if (px == x2 && py == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; px += sx; }
        if (e2 <= dx) { err += dx; py += sy; }
    }
}

void FlagRenderer::imagePixelate(Image* img, int blockSize) {
    for (int y = 0; y < img->height; y += blockSize)
        for (int x = 0; x < img->width; x += blockSize) {
            int r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int dy = 0; dy < blockSize && y + dy < img->height; ++dy)
                for (int dx = 0; dx < blockSize && x + dx < img->width; ++dx) {
                    Color c = GetImageColor(*img, x + dx, y + dy);
                    r += c.r; g += c.g; b += c.b; a += c.a; count++;
                }
            r /= count; g /= count; b /= count; a /= count;
            Color avg = {(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a};
            for (int dy = 0; dy < blockSize && y + dy < img->height; ++dy)
                for (int dx = 0; dx < blockSize && x + dx < img->width; ++dx)
                    ImageDrawPixel(img, x + dx, y + dy, avg);
        }
}

void FlagRenderer::imageBlur(Image* img, int radius) {
    if (radius < 1 || img->width < 1 || img->height < 1) return;
    // Simple 2D box blur approximated as separable Gaussian blur
    // Using a 3-sigma kernel (radius = 3 * sigma)
    int size = radius * 2 + 1;
    std::vector<float> kernel(size * size);
    float sigma = radius / 3.0f;
    float sum = 0.0f;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            float g = expf(-(x*x + y*y) / (2.0f * sigma * sigma));
            kernel[(y + radius) * size + (x + radius)] = g;
            sum += g;
        }
    }
    for (float& k : kernel) k /= sum;
    // Create temporary image for horizontal pass
    Image tmp = GenImageColor(img->width, img->height, BLANK);
    // Horizontal blur pass
    for (int y = 0; y < img->height; ++y) {
        for (int x = 0; x < img->width; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int kx = -radius; kx <= radius; ++kx) {
                int sx = x + kx;
                if (sx < 0) sx = 0;
                if (sx >= img->width) sx = img->width - 1;
                Color c = GetImageColor(*img, sx, y);
                float w = kernel[(radius) * size + (kx + radius)];
                r += c.r * w; g += c.g * w; b += c.b * w; a += c.a * w;
            }
            ImageDrawPixel(&tmp, x, y, {(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
        }
    }
    // Vertical blur pass and store back
    for (int y = 0; y < img->height; ++y) {
        for (int x = 0; x < img->width; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int ky = -radius; ky <= radius; ++ky) {
                int sy = y + ky;
                if (sy < 0) sy = 0;
                if (sy >= img->height) sy = img->height - 1;
                Color c = GetImageColor(tmp, x, sy);
                float w = kernel[(ky + radius) * size + (radius)];
                r += c.r * w; g += c.g * w; b += c.b * w; a += c.a * w;
            }
            ImageDrawPixel(img, x, y, {(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
        }
    }
    UnloadImage(tmp);
}

// ══════════════════════════════════════════════════════
//  Text block (for non-ASCII text placeholder)
// ══════════════════════════════════════════════════════
void FlagRenderer::drawTextBlock(Image* img, int cx, int cy, int w, int h, Color color) {
    if (w < 2 || h < 2) { if (cx >= 0 && cx < img->width && cy >= 0 && cy < img->height) ImageDrawPixel(img, cx, cy, color); return; }
    Color bg = { (unsigned char)(color.r * 0.4f), (unsigned char)(color.g * 0.4f),
                 (unsigned char)(color.b * 0.4f), color.a };
    fillRect(img, cx - w/2, cy - h/2, w, h, bg);
    int lineH = (h > 6) ? 2 : 1;
    int gap = (h > 6) ? (h - 3 * lineH) / 4 : 1;
    for (int i = 0; i < 3 && (i * (lineH + gap) + lineH) <= h; ++i) {
        int ly = cy - h/2 + gap + i * (lineH + gap);
        fillRect(img, cx - w/2 + 2, ly, w - 4, lineH, color);
    }
}

void FlagRenderer::drawCensorBar(Image* img, int cx, int cy, int rad) {
    if (rad < 2) { if (cx >= 0 && cx < img->width && cy >= 0 && cy < img->height) ImageDrawPixel(img, cx, cy, BLACK); return; }
    fillCircle(img, cx, cy, rad, BLACK);
    if (rad > 3) {
        int d = rad / 2;
        ImageDrawLine(img, cx - d, cy - d, cx + d, cy + d, WHITE);
        ImageDrawLine(img, cx + d, cy - d, cx - d, cy + d, WHITE);
    }
}

// ══════════════════════════════════════════════════════
//  BACKGROUND PATTERNS
// ══════════════════════════════════════════════════════

void FlagRenderer::drawSolid(Image* img, int w, int h, const std::vector<Color>& colors) {
    fillImage(img, getColor(colors, 0, DARKGRAY));
}

void FlagRenderer::drawStripesH(Image* img, int w, int h, const std::vector<Color>& colors) {
    int n = (int)colors.size();
    if (n == 0) { fillImage(img, DARKGRAY); return; }
    int bandH = h / n;
    for (int i = 0; i < n; ++i) {
        Color c = getColor(colors, i, DARKGRAY);
        ImageDrawRectangle(img, 0, i * bandH, w, (i == n - 1) ? h - i * bandH : bandH, c);
    }
}

void FlagRenderer::drawStripesV(Image* img, int w, int h, const std::vector<Color>& colors) {
    int n = (int)colors.size();
    if (n == 0) { fillImage(img, DARKGRAY); return; }
    int bandW = w / n;
    for (int i = 0; i < n; ++i) {
        Color c = getColor(colors, i, DARKGRAY);
        ImageDrawRectangle(img, i * bandW, 0, (i == n - 1) ? w - i * bandW : bandW, h, c);
    }
}

void FlagRenderer::drawDiagonal(Image* img, int w, int h, bool rightDown, const std::vector<Color>& colors) {
    Color c1 = getColor(colors, 0, DARKGRAY);
    Color c2 = getColor(colors, 1, LIGHTGRAY);
    // Simple two-color diagonal split
    for (int y = 0; y < h; ++y) {
        float t = (float)y / h;
        int splitX = rightDown ? (int)(t * w) : (int)((1.0f - t) * w);
        if (splitX < 0) splitX = 0;
        if (splitX > w) splitX = w;
        for (int x = 0; x < splitX; ++x)
            ImageDrawPixel(img, x, y, c1);
        for (int x = splitX; x < w; ++x)
            ImageDrawPixel(img, x, y, c2);
    }
}

void FlagRenderer::drawTriangle(Image* img, int w, int h, bool doubleTri, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    Color tri = getColor(colors, 1, WHITE);
    fillImage(img, bg);
    // Triangle from left edge to center
    for (int y = 0; y < h; ++y) {
        int triW = (int)((float)y / h * w * 0.45f + (float)(h - y) / h * w * 0.45f);
        if (triW < 0) triW = 0;
        for (int x = 0; x < triW && x < w; ++x)
            ImageDrawPixel(img, x, y, tri);
    }
    if (doubleTri && colors.size() >= 3) {
        // Second triangle (smaller, different color) inset
        Color tri2 = getColor(colors, 2, WHITE);
        for (int y = h / 4; y < 3 * h / 4; ++y) {
            float t = (float)(y - h / 4) / (h / 2);
            int triW = (int)((1.0f - fabs(t - 0.5f) * 2.0f) * w * 0.2f);
            for (int x = 0; x < triW && x < w; ++x)
                ImageDrawPixel(img, x, y, tri2);
        }
    }
}

void FlagRenderer::drawQuartered(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color q1 = getColor(colors, 0, DARKGRAY);
    Color q2 = getColor(colors, 1, WHITE);
    Color q3 = getColor(colors, 2, q2);
    Color q4 = getColor(colors, 3, q1);
    int hw = w / 2, hh = h / 2;
    fillRect(img, 0, 0, hw, hh, q1);
    fillRect(img, hw, 0, w - hw, hh, q2);
    fillRect(img, 0, hh, hw, h - hh, q3);
    fillRect(img, hw, hh, w - hw, h - hh, q4);
}

void FlagRenderer::drawSaltir(Image* img, int w, int h, const std::vector<Color>& colors) {
    // X cross splitting flag into 4 triangles
    Color c1 = getColor(colors, 0, DARKGRAY);
    Color c2 = getColor(colors, 1, WHITE);
    Color c3 = getColor(colors, 2, c1);
    Color c4 = getColor(colors, 3, c2);
    int cx = w / 2, cy = h / 2;
    // Top, right, bottom, left triangles
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // Which side of the X?  Sign of (x - cx) and (y - cy) determines
            if (abs(x - cx) > abs(y - cy)) {
                // Left/right of X — check left triangle or right
                ImageDrawPixel(img, x, y, (x < cx) ? c1 : c2);
            } else {
                ImageDrawPixel(img, x, y, (y < cy) ? c3 : c4);
            }
        }
    // Draw the X stripes
    int bw = std::max(1, w / 14);
    for (int t = -bw/2; t <= bw/2; ++t) {
        for (int y = 0; y < h; ++y) {
            int xx1 = y + t;
            int xx2 = (h - y) + t;
            if (xx1 >= 0 && xx1 < w) ImageDrawPixel(img, xx1, y, c3);
            if (xx2 >= 0 && xx2 < w) ImageDrawPixel(img, xx2, y, c4);
        }
    }
}

void FlagRenderer::drawCanton(Image* img, int w, int h, const std::vector<Color>& colors) {
    // Colors[0] = flag background, colors[1] = canton background
    Color bg = getColor(colors, 0, DARKGRAY);
    Color canton = getColor(colors, 1, BLUE);
    fillImage(img, bg);
    int cw = w * 2 / 5, ch = h * 2 / 5;
    fillRect(img, 0, 0, cw, ch, canton);
}

void FlagRenderer::drawPale(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color left = getColor(colors, 0, DARKGRAY);
    Color center = getColor(colors, 1, WHITE);
    Color right = getColor(colors, 2, left);
    int cw = w / 3;
    int rw = w - cw * 2;
    fillRect(img, 0, 0, cw, h, left);
    fillRect(img, cw, 0, cw, h, center);
    fillRect(img, cw * 2, 0, rw, h, right);
}

void FlagRenderer::drawFess(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color top = getColor(colors, 0, DARKGRAY);
    Color center = getColor(colors, 1, WHITE);
    Color bot = getColor(colors, 2, top);
    int ch = h / 3;
    int rh = h - ch * 2;
    fillRect(img, 0, 0, w, ch, top);
    fillRect(img, 0, ch, w, ch, center);
    fillRect(img, 0, ch * 2, w, rh, bot);
}

void FlagRenderer::drawNordicCross(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    Color cross = getColor(colors, 1, WHITE);
    fillImage(img, bg);
    int vw = std::max(1, w / 8);        // vertical bar width
    int hh = std::max(1, h / 6);        // horizontal bar height
    int vx = w / 4;                     // offset to hoist
    int hy = h / 2 - hh / 2;
    fillRect(img, vx, 0, vw, h, cross);
    fillRect(img, 0, hy, w, hh, cross);
    // If 3+ colors, draw inner cross (like Norway/Sweden)
    if (colors.size() >= 3) {
        Color inner = getColor(colors, 2, RED);
        int ivw = std::max(1, vw / 3);
        int ihh = std::max(1, hh / 3);
        fillRect(img, vx + (vw - ivw) / 2, 0, ivw, h, inner);
        fillRect(img, 0, hy + (hh - ihh) / 2, w, ihh, inner);
    }
}

void FlagRenderer::drawGreekCross(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    Color cross = getColor(colors, 1, WHITE);
    fillImage(img, bg);
    int vw = std::max(1, w / 5);
    int hh = std::max(1, h / 5);
    int cx = w / 2 - vw / 2;
    int cy = h / 2 - hh / 2;
    fillRect(img, cx, 0, vw, h, cross);
    fillRect(img, 0, cy, w, hh, cross);
}

void FlagRenderer::drawBorder(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    Color border = getColor(colors, 1, WHITE);
    fillImage(img, bg);
    int bw = std::max(1, w / 20);
    fillRect(img, 0, 0, w, bw, border);
    fillRect(img, 0, h - bw, w, bw, border);
    fillRect(img, 0, 0, bw, h, border);
    fillRect(img, w - bw, 0, bw, h, border);
}

void FlagRenderer::drawSunburst(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    Color ray = getColor(colors, 1, WHITE);
    fillImage(img, bg);
    int cx = w / 2, cy = h / 2;
    int maxR = (int)sqrtf((float)(cx * cx + cy * cy));
    // Draw alternating rays
    for (int a = 0; a < 360; a += 15) {
        float rad = a * 3.14159f / 180.0f;
        float cosA = cos(rad), sinA = sin(rad);
        for (int r = 0; r < maxR; ++r) {
            int px = cx + (int)(cosA * r);
            int py = cy + (int)(sinA * r);
            if (px >= 0 && px < w && py >= 0 && py < h)
                ImageDrawPixel(img, px, py, ray);
        }
    }
}

// ══════════════════════════════════════════════════════
//  SYMBOL DRAWING — STARS
// ══════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════
//  CRESCENT & SUN
// ══════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════
//  CROSSES
// ══════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════
//  GEAR, SWORDS, MOUNTAIN, DIAMOND
// ══════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════
//  MAIN RENDER ENTRY POINT
// ══════════════════════════════════════════════════════

void FlagRenderer::drawFlagBackground(Image* img, const FlagPattern& pattern, int width, int height) {
    switch (pattern.type) {
        case FlagType::SOLID:         drawSolid(img, width, height, pattern.colors); break;
        case FlagType::HSTRIPES_2:
        case FlagType::HSTRIPES_3:
        case FlagType::HSTRIPES_N:    drawStripesH(img, width, height, pattern.colors); break;
        case FlagType::VSTRIPES_2:
        case FlagType::VSTRIPES_3:
        case FlagType::VSTRIPES_N:    drawStripesV(img, width, height, pattern.colors); break;
        case FlagType::DIAGONAL_L:    drawDiagonal(img, width, height, false, pattern.colors); break;
        case FlagType::DIAGONAL_R:    drawDiagonal(img, width, height, true, pattern.colors); break;
        case FlagType::TRIANGLE:      drawTriangle(img, width, height, false, pattern.colors); break;
        case FlagType::TRIANGLE_DOUBLE: drawTriangle(img, width, height, true, pattern.colors); break;
        case FlagType::QUARTERED:     drawQuartered(img, width, height, pattern.colors); break;
        case FlagType::SALTIR:        drawSaltir(img, width, height, pattern.colors); break;
        case FlagType::CANTON:        drawCanton(img, width, height, pattern.colors); break;
        case FlagType::PALE:          drawPale(img, width, height, pattern.colors); break;
        case FlagType::FESS:          drawFess(img, width, height, pattern.colors); break;
        case FlagType::CROSS_NORDIC:  drawNordicCross(img, width, height, pattern.colors); break;
        case FlagType::CROSS_GREEK:   drawGreekCross(img, width, height, pattern.colors); break;
        case FlagType::STRIPED_EDGE:  drawBorder(img, width, height, pattern.colors); break;
        case FlagType::SUNBURST:      drawSunburst(img, width, height, pattern.colors); break;
    }
}

void FlagRenderer::drawSymbol(Image* img, const FlagSymbol& sym, int width, int height, const std::string& baseDir,
                               const std::unordered_map<std::string, std::string>* odmData) {
    int cx = (int)(sym.x * width);
    int cy = (int)(sym.y * height);
    int r = (int)(sym.size * std::min(width, height));
    if (r < 1) r = 1;

    Color sc = getColor(sym.colors, 0, WHITE);

    // ── SVG redirect: map old procedural types to SVG_FILE ──
    SymbolType effectiveType = sym.type;
    std::string svgPath;
    auto mapToSVG = [&](SymbolType t, const char* svgName) {
        if (effectiveType == t) {
            // Symbol SVGs live in <data>/symbols/ (data/flags/ holds country flags)
            svgPath = std::string("symbols/") + svgName;
            effectiveType = SymbolType::SVG_FILE;
        }
    };
    if (!baseDir.empty()) {
        mapToSVG(SymbolType::STAR_5,        "star5.svg");
        mapToSVG(SymbolType::STAR_6,        "star6.svg");
        mapToSVG(SymbolType::STAR_7,        "star7.svg");
        mapToSVG(SymbolType::CRESCENT,      "crescent.svg");
        mapToSVG(SymbolType::CRESCENT_STAR, "crescent_star.svg");
        mapToSVG(SymbolType::SUN,           "sun.svg");
        mapToSVG(SymbolType::SUN_RAYS,      "sun_wavy.svg");
        mapToSVG(SymbolType::CROSS_LATIN,   "cross_latin.svg");
        mapToSVG(SymbolType::CROSS_SALTIR,  "cross_saltir.svg");
        mapToSVG(SymbolType::CROSS_MALTESE, "cross_maltese.svg");
        mapToSVG(SymbolType::DIAMOND,       "diamond.svg");
        mapToSVG(SymbolType::GEAR,          "gear.svg");
        mapToSVG(SymbolType::HAMMER_SICKLE, "hammer_sickle.svg");
        mapToSVG(SymbolType::SWASTIKA,      "swastika.svg");
        mapToSVG(SymbolType::SWORD,         "sword.svg");
        mapToSVG(SymbolType::CROSSED_SWORDS,"crossed_swords.svg");
        mapToSVG(SymbolType::MOUNTAIN,      "mountain.svg");
        mapToSVG(SymbolType::TREE,          "tree.svg");
    }

    switch (effectiveType) {
        case SymbolType::SVG_FILE: {
            const std::string& svgName = (sym.type == SymbolType::SVG_FILE) ? sym.text : svgPath;
            if (!svgName.empty()) {
                                // Clamp position so symbol stays within flag bounds
                int halfRaster = r;  // raster is r*2, half is r
                if (cx - halfRaster < 0) cx = halfRaster;
                if (cy - halfRaster < 0) cy = halfRaster;
                if (cx + halfRaster > width) cx = width - halfRaster;
                if (cy + halfRaster > height) cy = height - halfRaster;
                if (cx < 0) cx = halfRaster;
                if (cy < 0) cy = halfRaster;
Image svgImg = rasterizeSVG(svgName, r * 2, r * 2, baseDir, odmData);
                if (svgImg.data != nullptr) {
                    // Recolor SVG to match symbol color
                    Color targetColor = getColor(sym.colors, 0, WHITE);
                    if (!(targetColor.r == 255 && targetColor.g == 255 && targetColor.b == 255)) {
                        Image recolored = recolorImage(svgImg, targetColor);
                        UnloadImage(svgImg);
                        svgImg = recolored;
                    }
                    // Composite onto flag at (cx, cy) centered
                    int hw = svgImg.width / 2;
                    int hh = svgImg.height / 2;
                    for (int sy = 0; sy < svgImg.height; ++sy) {
                        for (int sx = 0; sx < svgImg.width; ++sx) {
                            Color pc = GetImageColor(svgImg, sx, sy);
                            if (pc.a > 0) {
                                int px = cx + sx - hw;
                                int py = cy + sy - hh;
                                if (px >= 0 && px < img->width && py >= 0 && py < img->height)
                                    ImageDrawPixel(img, px, py, pc);
                            }
                        }
                    }
                    UnloadImage(svgImg);
                }
            }
            break;
        }

        // ── Procedural fallbacks (for symbols without SVG) ──
        case SymbolType::CIRCLE:         fillCircle(img, cx, cy, r, sc); break;
        case SymbolType::DISC:
            fillCircle(img, cx, cy, r, sc);
            fillCircle(img, cx, cy, r * 3 / 5, getColor(sym.colors, 1, DARKGRAY));
            break;
        case SymbolType::TRIANGLE: {
            int hh = r, hw = r;
            fillTriangle(img, cx, cy - hh, cx - hw, cy + hh, cx + hw, cy + hh, sc);
            break;
        }
        case SymbolType::TEXT_BLOCK:
            drawTextBlock(img, cx, cy, r * 2, r, sc);
            break;
        case SymbolType::CENSOR_BAR:
            drawCensorBar(img, cx, cy, r);
            break;
        default: break;
    }
}

Texture2D FlagRenderer::render(const FlagPattern& pattern, int width, int height, const std::string& baseDir,
                                 const std::unordered_map<std::string, std::string>* odmData) {
    if (!pattern.imagePath.empty()) {
        std::string fileExt;
        std::string::size_type dot = pattern.imagePath.rfind('.');
        if (dot != std::string::npos)
            fileExt = pattern.imagePath.substr(dot);
        if (fileExt == ".svg") {
            Image svgImg = rasterizeSVG(pattern.imagePath, width, height, baseDir, odmData);
            if (svgImg.data != nullptr) {
                Image dst;
                if (svgImg.width != width || svgImg.height != height) {
                    dst = GenImageColor(width, height, BLANK);
                    for (int y = 0; y < height; ++y)
                        for (int x = 0; x < width; ++x) {
                            int sx = x * svgImg.width / width;
                            int sy = y * svgImg.height / height;
                            if (sx >= svgImg.width) sx = svgImg.width - 1;
                            if (sy >= svgImg.height) sy = svgImg.height - 1;
                            Color c = GetImageColor(svgImg, sx, sy);
                            if (c.a > 0) ImageDrawPixel(&dst, x, y, c);
                        }
                    UnloadImage(svgImg);
                } else {
                    dst = svgImg;
                }
                if (pattern.censored) imageBlur(&dst, 4);
                Texture2D tex = LoadTextureFromImage(dst);
                UnloadImage(dst);
                return tex;
            }
        } else {
            std::string fullPath = baseDir.empty() ? pattern.imagePath : baseDir + "/" + pattern.imagePath;
            Image src = {};
            // Try loading from archive first
            if (odmData) {
                auto it = odmData->find(pattern.imagePath);
                if (it != odmData->end()) {
                    src = LoadImageFromMemory(fileExt.c_str(), 
                        reinterpret_cast<const unsigned char*>(it->second.data()), 
                        static_cast<int>(it->second.size()));
                }
            }
            // Fall back to disk if not in archive
            if (src.data == nullptr) {
                src = LoadImage(fullPath.c_str());
            }
            if (src.data != nullptr) {
                ImageResize(&src, width, height);
                if (pattern.censored) imageBlur(&src, 4);
                Texture2D tex = LoadTextureFromImage(src);
                UnloadImage(src);
                return tex;
            }
        }
    }

    Image img = GenImageColor(width, height, BLANK);
    drawFlagBackground(&img, pattern, width, height);
    for (const auto& sym : pattern.symbols)
        drawSymbol(&img, sym, width, height, baseDir, odmData);

    if (pattern.censored) imageBlur(&img, 4);
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    return tex;
}

// ══════════════════════════════════════════════════════
//  SVG rasterization with cache
// ══════════════════════════════════════════════════════

Image FlagRenderer::rasterizeSVG(const std::string& filePath, int width, int height, const std::string& baseDir,
                                  const std::unordered_map<std::string, std::string>* odmData) {
    std::string fullPath = baseDir.empty() ? filePath : baseDir + "/" + filePath;
    static std::unordered_map<std::string, Image> s_cache;
    auto cit = s_cache.find(fullPath);
    if (cit != s_cache.end()) {
        Image copy = GenImageColor(cit->second.width, cit->second.height, BLANK);
        for (int y = 0; y < cit->second.height; ++y)
            for (int x = 0; x < cit->second.width; ++x)
                ImageDrawPixel(&copy, x, y, GetImageColor(cit->second, x, y));
        return copy;
    }

    NSVGimage* svgImage = nullptr;
    if (odmData) {
        auto it = odmData->find(filePath);
        if (it != odmData->end()) {
            std::string copy = it->second;
            svgImage = nsvgParse(&copy[0], "px", 96.0f);
        }
        if (!svgImage) {
            // Try with "symbols/" prefix — SVGs may be stored there in the archive
            std::string symPath = "symbols/" + filePath;
            auto sit = odmData->find(symPath);
            if (sit != odmData->end()) {
                std::string copy = sit->second;
                svgImage = nsvgParse(&copy[0], "px", 96.0f);
            }
        }
    }
    if (!svgImage) {
        svgImage = nsvgParseFromFile(fullPath.c_str(), "px", 96.0f);
    }
    if (!svgImage) {
        printf("[SVG] Failed to parse: %s\n", fullPath.c_str());
        return {0};
    }

    int w = width;
    int h = height;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    unsigned char* rgba = (unsigned char*)malloc(w * h * 4);
    if (!rgba) { nsvgDelete(svgImage); return {0}; }
    memset(rgba, 0, w * h * 4);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { free(rgba); nsvgDelete(svgImage); return {0}; }
    // Scale-to-fit: preserve aspect ratio, center within (w, h) with transparent padding.
    // This fixes clipping for non-2:1 SVGs (e.g., 3:2, 1:1, Nepal 5:6) that nanosvg would
    // otherwise clip vertically when using width-only scaling.
    float sx = (float)w / svgImage->width;
    float sy = (float)h / svgImage->height;
    float scale = sx < sy ? sx : sy;
    float tx = ((float)w - svgImage->width * scale) * 0.5f;
    float ty = ((float)h - svgImage->height * scale) * 0.5f;
    nsvgRasterize(rast, svgImage, tx, ty, scale, rgba, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svgImage);

    Image img = GenImageColor(w, h, BLANK);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = (y * w + x) * 4;
            unsigned char rr = rgba[idx], gg = rgba[idx+1], bb = rgba[idx+2], aa = rgba[idx+3];
            ImageDrawPixel(&img, x, y, {rr, gg, bb, aa});
        }
    }
    free(rgba);

    Image cached = ImageCopy(img);
    s_cache[fullPath] = cached;

    return img;
}

Image FlagRenderer::recolorImage(const Image& src, Color newColor) {
    Image dst = GenImageColor(src.width, src.height, BLANK);
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            Color c = GetImageColor(src, x, y);
            if (c.a > 0) {
                // Preserve luminance for shading detail, replace hue with newColor
                float luma = (c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f) / 255.0f;
                unsigned char nr = (unsigned char)std::min(255.0f, newColor.r * luma);
                unsigned char ng = (unsigned char)std::min(255.0f, newColor.g * luma);
                unsigned char nb = (unsigned char)std::min(255.0f, newColor.b * luma);
                ImageDrawPixel(&dst, x, y, {nr, ng, nb, c.a});
            }
        }
    }
    return dst;
}
