#include "MapRenderer.h"
// Not just the pad's cursor but its buttons: this renderer answers the clicks
// that select a province, drag an army and pan the map, and it asks raylib for
// them itself. Without the shims the stick moved a pointer nothing could click.
#include "../PadInput.h"
// A renderer reaching for the audio system looks wrong and is not. Two of the
// functions below are the longest uninterrupted stretches of work in the whole
// load, and on the web that is measured in whether the music survives them:
// nothing drains the audio buffer while this thread is inside a 33-million-pixel
// scan. Audio::pump() is the yield. See its comment for why a browser needs one.
#include "../Audio.h"
// T(): the debug overlays below are drawn text like any other.
#include "../i18n/Locale.h"
#include "raymath.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <utility>

static constexpr float RAD2DEGF = 180.0f / PI;

MapRenderer::MapRenderer(int screenW, int screenH, int mapW, int mapH)
    : m_screenW(screenW), m_screenH(screenH), m_mapW(mapW), m_mapH(mapH)
{
    m_camera.target = { mapW * 0.5f, mapH * 0.5f };
    m_camera.offset = { screenW * 0.5f, screenH * 0.5f };
    m_camera.rotation = 0.0f;
    m_camera.zoom = 1.0f;
    m_minZoom = std::max(m_screenW / static_cast<float>(m_mapW),
                         m_screenH / static_cast<float>(m_mapH));
    m_maxZoom = 5.0f;
}

Vector2 MapRenderer::getMouse() const {
    // The pad's virtual cursor has to arrive here too, not only in Game. The map
    // is where a controller player does the aiming -- selecting a province,
    // dragging an army, pointing artillery -- and this renderer asks for the
    // pointer itself rather than being handed one. Without this the stick moved
    // a cursor that every panel could see and the map could not.
    if (odPad::active()) { Vector2 c = odPad::cursor(); return { c.x * m_dpiScale, c.y * m_dpiScale }; }
    Vector2 m = GetMousePosition();
    return { m.x * m_dpiScale, m.y * m_dpiScale };
}

MapRenderer::~MapRenderer() {
    if (m_borderTex.id > 0) UnloadTexture(m_borderTex);
    if (m_politicalTex.id > 0) UnloadTexture(m_politicalTex);
    if (m_selectionTex.id > 0) UnloadTexture(m_selectionTex);
    if (m_bulkTex.id > 0) UnloadTexture(m_bulkTex);
    if (m_populationTex.id > 0) UnloadTexture(m_populationTex);
    if (m_resourceTex.id > 0) UnloadTexture(m_resourceTex);
    if (m_claimsTex.id > 0) UnloadTexture(m_claimsTex);
    if (m_editorOverlayTex.id > 0) UnloadTexture(m_editorOverlayTex);
    if (m_highlightTex.id > 0) UnloadTexture(m_highlightTex);
}

void MapRenderer::setEditorOverlay(Texture2D tex) {
    if (m_editorOverlayTex.id > 0) UnloadTexture(m_editorOverlayTex);
    m_editorOverlayTex = tex;
}

void MapRenderer::setHighlight(Texture2D tex, int x, int y) {
    if (m_highlightTex.id > 0) UnloadTexture(m_highlightTex);
    m_highlightTex = tex;
    m_highlightX = x;
    m_highlightY = y;
}

void MapRenderer::clearHighlight() {
    if (m_highlightTex.id > 0) UnloadTexture(m_highlightTex);
    m_highlightTex = Texture2D{};
}

void MapRenderer::setPoliticalTexture(Texture2D tex) {
    if (m_politicalTex.id > 0) UnloadTexture(m_politicalTex);
    m_politicalTex = tex;
}

void MapRenderer::updatePoliticalTexture(const void* data) {
    if (m_politicalTex.id > 0)
        UpdateTexture(m_politicalTex, data);
}

void MapRenderer::updatePoliticalTextureRec(const void* rectData, int x, int y, int w, int h) {
    if (m_politicalTex.id > 0)
        UpdateTextureRec(m_politicalTex, {(float)x, (float)y, (float)w, (float)h}, rectData);
}

// ─── THE BORDER LAYER'S PIXEL ─────────────────────────────────────────────
//
// Two bytes: a luminance the draw tint overrides anyway, and coverage.
//
// It was four, as 0xFFFFFF00|alpha written as a little-endian uint32 into an
// R8G8B8A8 texture. Read that back a byte at a time and it is R=alpha,
// G=B=A=255 -- NOT white-with-alpha, which is what the constant looks like it
// says. So every marked pixel has always drawn fully OPAQUE, and the 180 an
// edge computes and the 50 its neighbour computes have never reached the
// screen: the halo is in the arithmetic and has never been in the picture.
//
// THE LOOK IS UNCHANGED HERE, deliberately. kBorderAlpha is 255 for anything
// marked, which is the pixel that has always been drawn. Correcting it is a
// one-line change -- pass the computed alpha through instead -- and it
// lightens every border on every map, so it is a decision to take on its own
// rather than the side effect of a memory fix. Nothing about the format
// stands in the way: the byte is there either way.
//
// WHY TWO BYTES IS THE POINT. Both draw sites tint with
// ColorAlpha(BLACK, 0.15f), so this texture supplies coverage and nothing
// else; three of its four channels were a constant. At 8192x4096 that is
// 128 MB of heap and 128 MB of GPU, halved. The GPU half is the one that
// matters on a phone: a scenario load builds THREE full-map textures --
// land/sea, political, and this -- and a browser tab's texture budget is not
// the wasm heap the [MEM] lines in Game_Loading.cpp measure.
//
// GRAY_ALPHA is GL_LUMINANCE_ALPHA on the ES2 path the web and Android builds
// take, sampled as (L,L,L,A); the font atlas has always shipped in it, so it
// is a format this build is already known to accept. Desktop GL 3.3 gets
// GL_RG8 and a swizzle, which raylib sets for us.
static constexpr size_t kBorderBpp = 2;
static constexpr uint8_t kBorderMarked = 255;

static inline void writeBorderTexel(uint8_t* dst, uint8_t alpha) {
    dst[0] = 255;                                 // luminance; the tint wins
    dst[1] = alpha ? kBorderMarked : (uint8_t)0;  // coverage
}

// Is this pixel on a province edge? Wraps in x, because the map is a cylinder,
// and counts the top and bottom rows as edges -- which is what the flood fill
// this replaced did by construction, and what the live-paint path below has
// always done. ONE definition now, shared by both, instead of two that agreed
// only by inspection.
static inline bool provEdgeAt(const uint32_t* pixels, int mapW, int mapH,
                              int px, int py) {
    const uint32_t centre = pixels[(size_t)py * mapW + px];
    if (centre == 0) return false;
    const int l = (px == 0) ? mapW - 1 : px - 1;
    const int r = (px == mapW - 1) ? 0 : px + 1;
    return pixels[(size_t)py * mapW + l] != centre ||
           pixels[(size_t)py * mapW + r] != centre ||
           (py == 0 || pixels[(size_t)(py - 1) * mapW + px] != centre) ||
           (py == mapH - 1 || pixels[(size_t)(py + 1) * mapW + px] != centre);
}

// 180 on an edge pixel, 50 on the one-pixel halo around one, 0 elsewhere.
static inline uint8_t borderAlphaAt(const uint32_t* pixels, int mapW, int mapH,
                                    int px, int py) {
    if (provEdgeAt(pixels, mapW, mapH, px, py)) return 180;
    const int l = (px == 0) ? mapW - 1 : px - 1;
    const int r = (px == mapW - 1) ? 0 : px + 1;
    if (provEdgeAt(pixels, mapW, mapH, l, py) ||
        provEdgeAt(pixels, mapW, mapH, r, py) ||
        (py > 0 && provEdgeAt(pixels, mapW, mapH, px, py - 1)) ||
        (py < mapH - 1 && provEdgeAt(pixels, mapW, mapH, px, py + 1))) return 50;
    return 0;
}

void MapRenderer::updateBorderRegion(const Color* provPixels, int mapW, int mapH,
                                     int rx, int ry, int rw, int rh) {
    if (m_borderTex.id == 0 || provPixels == nullptr || m_borderPixels.empty()) return;
    // Expand by 2 so border/halo transitions at the rect edge recompute correctly
    int x0 = std::max(0, rx - 2), y0 = std::max(0, ry - 2);
    int x1 = std::min(mapW - 1, rx + rw + 1), y1 = std::min(mapH - 1, ry + rh + 1);
    if (x0 > x1 || y0 > y1) return;

    const auto* pixels = reinterpret_cast<const uint32_t*>(provPixels);
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<uint8_t> rect((size_t)w * h * kBorderBpp, 0);
    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            const uint8_t a = borderAlphaAt(pixels, mapW, mapH, px, py);
            writeBorderTexel(&rect[((size_t)(py - y0) * w + (px - x0)) * kBorderBpp], a);
            writeBorderTexel(&m_borderPixels[((size_t)py * mapW + px) * kBorderBpp], a);
        }
    }
    UpdateTextureRec(m_borderTex, {(float)x0, (float)y0, (float)w, (float)h}, rect.data());
}

void MapRenderer::setPopulationTexture(Texture2D tex) {
    if (m_populationTex.id > 0) UnloadTexture(m_populationTex);
    m_populationTex = tex;
}

void MapRenderer::updatePopulationTexture(const void* data) {
    if (m_populationTex.id > 0)
        UpdateTexture(m_populationTex, data);
}

void MapRenderer::setResourceTexture(Texture2D tex) {
    if (m_resourceTex.id > 0) UnloadTexture(m_resourceTex);
    m_resourceTex = tex;
}

void MapRenderer::updateResourceTexture(const void* data) {
    if (m_resourceTex.id > 0)
        UpdateTexture(m_resourceTex, data);
}

void MapRenderer::setClaimsTexture(Texture2D tex) {
    if (m_claimsTex.id > 0) UnloadTexture(m_claimsTex);
    m_claimsTex = tex;
}

void MapRenderer::updateClaimsTexture(const void* data) {
    if (m_claimsTex.id > 0)
        UpdateTexture(m_claimsTex, data);
}

void MapRenderer::updateClaimsTextureRec(const void* rectData, int x, int y, int w, int h) {
    if (m_claimsTex.id > 0)
        UpdateTextureRec(m_claimsTex, {(float)x, (float)y, (float)w, (float)h}, rectData);
}

void MapRenderer::computeBorderTexture(const Image& provImage) {
    if (provImage.data == nullptr) return;

    const int mapW = provImage.width;
    const int mapH = provImage.height;
    const auto* pixels = static_cast<const uint32_t*>(provImage.data);

    // ─── WHAT THIS USED TO COST, AND WHY IT MATTERED ──────────────────────
    //
    // This is the phase an iPhone died in: the loading bar reached 40%,
    // "Initializing renderer", and the tab was gone. It was not the texture
    // upload. It was the three full-map working buffers this function held at
    // once, on an 8192x4096 map:
    //
    //     borderDist    vector<int>       128 MB   a distance field
    //     queue         vector<QEntry>     31 MB   4.1 M BFS entries, and a
    //                                              push_back doubling makes
    //                                              the peak twice that
    //     borderPixels  vector<uint32_t>  128 MB   built, then COPIED into
    //     m_borderPixels                  128 MB   ...this, both live at once
    //
    // ~450 MB, all transient. Transient does not help: the wasm heap only ever
    // grows, so a spike the allocator hands straight back still raises the
    // tab's high-water mark for good, and the high-water mark is what Safari
    // kills on.
    //
    // None of it was needed. The distance field was only ever read as "is d
    // 0, 1, or more" -- and the BFS never expanded past 1, so it computed
    // nothing beyond "is this pixel an edge, or next to one". That is two
    // local tests, and the live-paint path above had been computing them
    // directly all along.
    //
    // So: no distance field, no queue, no second output buffer. Three rows of
    // edge flags -- 24 KB at this width -- rolled down the image, written
    // straight into m_borderPixels. One pass over the pixels instead of three,
    // and the same picture out.
    m_borderPixels.assign((size_t)mapW * mapH * kBorderBpp, 0);

    std::vector<uint8_t> flagRows((size_t)mapW * 3, 0);
    uint8_t* rows[3] = { flagRows.data(), flagRows.data() + mapW, flagRows.data() + 2 * mapW };
    auto fillFlags = [&](uint8_t* out, int y) {
        if (y < 0 || y >= mapH) { std::fill(out, out + mapW, (uint8_t)0); return; }
        for (int px = 0; px < mapW; ++px)
            out[px] = provEdgeAt(pixels, mapW, mapH, px, y) ? 1 : 0;
    };
    fillFlags(rows[0], -1);
    fillFlags(rows[1], 0);
    fillFlags(rows[2], 1);

    for (int py = 0; py < mapH; ++py) {
        // Once a row. pump() rate-limits itself and costs a clock read when it
        // is not due, so the scan pays almost nothing and the stream stays fed
        // throughout instead of between phases. Every 64 rows was the old
        // interval and was longer than an audio period at this raster size,
        // which is how the buffer ran dry with the instrumentation in place.
        Audio::get().pump();
        const uint8_t* prev = rows[0];
        const uint8_t* cur  = rows[1];
        const uint8_t* next = rows[2];
        uint8_t* dst = m_borderPixels.data() + (size_t)py * mapW * kBorderBpp;
        for (int px = 0; px < mapW; ++px) {
            uint8_t a = 0;
            if (cur[px]) {
                a = 180;
            } else {
                const int l = (px == 0) ? mapW - 1 : px - 1;
                const int r = (px == mapW - 1) ? 0 : px + 1;
                if (cur[l] || cur[r] || prev[px] || next[px]) a = 50;
            }
            writeBorderTexel(dst + (size_t)px * kBorderBpp, a);
        }
        // Roll the window down and refill the row that just fell off the top.
        uint8_t* recycled = rows[0];
        rows[0] = rows[1];
        rows[1] = rows[2];
        rows[2] = recycled;
        fillFlags(rows[2], py + 2);
    }

    // The scan above yields every row; this does not and cannot. Handing a
    // 8192x4096 surface to the driver is one opaque call with no iteration of
    // ours inside it.
    //
    // NOT a BlockingCall, though it was one. Measured, this region is about
    // 130 ms -- three audio periods. Suspending the device for that costs a
    // stop and a restart of the music to save three repeated blocks, which is
    // a worse trade than the thing it was fixing. The guard is for the
    // multi-second regions.
    //
    // A top-up instead: refilling immediately before the stall is the most
    // headroom the stream can be given, and it costs nothing.
    Audio::get().pump();
    if (m_borderTex.id > 0) UnloadTexture(m_borderTex);
    // STRAIGHT FROM THE VECTOR WE ALREADY HOLD. LoadTextureFromImage only
    // reads the pixels, so an Image header pointing at m_borderPixels does the
    // job with no copy and nothing to free. It is not owned, so it must NOT be
    // unloaded.
    Image img{};
    img.data = m_borderPixels.data();
    img.width = mapW;
    img.height = mapH;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
    m_borderTex = LoadTextureFromImage(img);
}

void MapRenderer::resize(int screenW, int screenH) {
    m_screenW = screenW;
    m_screenH = screenH;
    m_camera.offset = { screenW * 0.5f, screenH * 0.5f };
    m_minZoom = std::max(m_screenW / static_cast<float>(m_mapW),
                         m_screenH / static_cast<float>(m_mapH));
    m_camera.zoom = std::clamp(m_camera.zoom, m_minZoom, m_maxZoom);
    m_wasDragged = false;
    m_isDragging = false;
}

void MapRenderer::addZoom(float amount) {
    Vector2 mouseWorldBefore = GetScreenToWorld2D(getMouse(), m_camera);
    m_camera.zoom += amount;
    if (m_camera.zoom < m_minZoom) m_camera.zoom = m_minZoom;
    if (m_camera.zoom > m_maxZoom) m_camera.zoom = m_maxZoom;
    Vector2 mouseWorldAfter = GetScreenToWorld2D(getMouse(), m_camera);
    Vector2 diff = Vector2Subtract(mouseWorldBefore, mouseWorldAfter);
    m_camera.target = Vector2Add(m_camera.target, diff);
    m_flying = false;
}

void MapRenderer::flyTo(float x, float y, float zoom, float speed) {
    // Pick shortest horizontal path from current camera position
    m_flyTarget = { x, y };
    while (m_flyTarget.x - m_camera.target.x > m_mapW * 0.5f) m_flyTarget.x -= m_mapW;
    while (m_flyTarget.x - m_camera.target.x < -m_mapW * 0.5f) m_flyTarget.x += m_mapW;

    m_flyZoom = std::clamp(zoom, m_minZoom, m_maxZoom);
    m_flySpeed = speed;
    m_flying = true;
    m_wasDragged = false;
    m_isDragging = false;
}

void MapRenderer::update(float dt) {
    bool userInteracted = false;

    if (!m_paused) {
        // Handle drag/zoom — when paused, block all map interaction
        Vector2 delta = GetMouseDelta();
        bool panning = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ||
                       (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !m_blockLeftPan);
        if (panning) {
            if (!m_isDragging) m_isDragging = true;
            if (fabs(delta.x) > 3.0f || fabs(delta.y) > 3.0f) m_wasDragged = true;
            Vector2 move = Vector2Scale(delta, -1.0f / m_camera.zoom);
            m_camera.target = Vector2Add(m_camera.target, move);
            if (m_flying && (fabs(delta.x) > 0 || fabs(delta.y) > 0)) userInteracted = true;
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE) ||
            IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_isDragging = false;
        }

        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            // Don't zoom if mouse is over the province info / ship list panel
            if (m_provincePanelRect.height <= 0 || !CheckCollisionPointRec(getMouse(), m_provincePanelRect)) {
                Vector2 mouseWorldBefore = GetScreenToWorld2D(getMouse(), m_camera);
                m_camera.zoom += wheel * 0.1f;
                if (m_camera.zoom < m_minZoom) m_camera.zoom = m_minZoom;
                if (m_camera.zoom > m_maxZoom) m_camera.zoom = m_maxZoom;
                Vector2 mouseWorldAfter = GetScreenToWorld2D(getMouse(), m_camera);
                Vector2 diff = Vector2Subtract(mouseWorldBefore, mouseWorldAfter);
                m_camera.target = Vector2Add(m_camera.target, diff);
                if (m_flying) userInteracted = true;
            }
        }
    }

    if (userInteracted) m_flying = false;

    if (m_flying) {
        // Keep fly target close to camera (wrap-around compensation)
        while (m_flyTarget.x - m_camera.target.x > m_mapW * 0.5f) m_flyTarget.x -= m_mapW;
        while (m_flyTarget.x - m_camera.target.x < -m_mapW * 0.5f) m_flyTarget.x += m_mapW;

        // Exponential chase: always moves toward target, smoothly redirects on change
        float t = 1.0f - powf(0.5f, dt * m_flySpeed);
        m_camera.target = Vector2Lerp(m_camera.target, m_flyTarget, t);
        m_camera.zoom += (m_flyZoom - m_camera.zoom) * t;

        // Keep zoom within bounds during animation
        if (m_camera.zoom < m_minZoom) m_camera.zoom = m_minZoom;
        if (m_camera.zoom > m_maxZoom) m_camera.zoom = m_maxZoom;

        // Stop when close enough
        float dist = Vector2Distance(m_camera.target, m_flyTarget);
        if (dist < 0.5f && fabs(m_camera.zoom - m_flyZoom) < 0.01f) {
            m_camera.target = m_flyTarget;
            m_camera.zoom = m_flyZoom;
            m_flying = false;
        }
    }

    // Always clamp camera vertically to prevent showing beyond map edges
    while (m_camera.target.x < 0) m_camera.target.x += m_mapW;
    while (m_camera.target.x >= m_mapW) m_camera.target.x -= m_mapW;

    float visibleH = m_screenH / m_camera.zoom;
    float minTargetY = visibleH * 0.5f;
    float maxTargetY = m_mapH - visibleH * 0.5f;
    if (maxTargetY < minTargetY) {
        m_camera.target.y = m_mapH * 0.5f;
    } else {
        if (m_camera.target.y < minTargetY) m_camera.target.y = minTargetY;
        if (m_camera.target.y > maxTargetY) m_camera.target.y = maxTargetY;
    }
}

void MapRenderer::buildProvinceData(
    const ProvinceMap& provinces,
    std::unordered_map<int, Vector2>& centers_out,
    std::unordered_map<int, float>& radii_out)
{
    m_provinceGlow.clear();
    centers_out.clear();
    radii_out.clear();
    if (m_borderPixels.empty()) return;

    const auto* provPixels = static_cast<const unsigned char*>(provinces.getImage().data);
    if (!provPixels) return;

    auto& all = provinces.getAllProvinces();

    int stride = m_mapW * 4;

    // Accumulators for centers
    std::unordered_map<int, long long> sx, sy, cnt;
    std::unordered_map<int, int> minX, maxX, minY, maxY;

    for (int y = 0; y < m_mapH; ++y) {
        Audio::get().pump();          // as in computeBorderTexture above
        int rowOff = y * stride;
        for (int x = 0; x < m_mapW; ++x) {
            int pi = rowOff + x * 4;
            int r = provPixels[pi], g = provPixels[pi + 1], b = provPixels[pi + 2];
            int pid = Province::colorToId(r, g, b);

            // Accumulate center/bbox data
            if (pid > 0 && all.count(pid)) {
                sx[pid] += x;
                sy[pid] += y;
                cnt[pid]++;
                if (!minX.count(pid) || x < minX[pid]) minX[pid] = x;
                if (!maxX.count(pid) || x > maxX[pid]) maxX[pid] = x;
                if (!minY.count(pid) || y < minY[pid]) minY[pid] = y;
                if (!maxY.count(pid) || y > maxY[pid]) maxY[pid] = y;
            }

            // Build glow map
            //
            // INDEXED BY THE BORDER LAYER'S OWN STRIDE, not the province
            // image's. These two buffers cover the same pixels and no longer
            // have the same pixel: `pi` is a byte offset into a 4-byte RGBA
            // image, and reading the coverage byte at pi+3 was only ever right
            // while the border layer was also four bytes wide.
            uint8_t ba = m_borderPixels[((size_t)y * m_mapW + x) * kBorderBpp + 1];
            if (ba > 0) {
                int foundPid = 0;
                for (int pass = 0; pass < 5 && foundPid == 0; ++pass) {
                    int nx = x, ny = y;
                    if (pass == 1) nx = (x == 0) ? m_mapW - 1 : x - 1;
                    if (pass == 2) nx = (x == m_mapW - 1) ? 0 : x + 1;
                    if (pass == 3) { nx = x; ny = y - 1; }
                    if (pass == 4) { nx = x; ny = y + 1; }
                    if (ny < 0 || ny >= m_mapH) continue;
                    int ni = ny * stride + nx * 4;
                    int nid = Province::colorToId(provPixels[ni], provPixels[ni + 1], provPixels[ni + 2]);
                    if (nid > 0) foundPid = nid;
                }
                if (foundPid > 0)
                    m_provinceGlow[foundPid].push_back({y * m_mapW + x, ba});
            }
        }
    }

    // Compute centers and radii from accumulated data
    for (auto& kv : all) {
        int id = kv.first;
        auto cit = cnt.find(id);
        if (cit != cnt.end() && cit->second > 0) {
            centers_out[id] = {
                (float)sx[id] / cit->second,
                (float)sy[id] / cit->second
            };
            float w = (float)(maxX[id] - minX[id]);
            float h = (float)(maxY[id] - minY[id]);
            radii_out[id] = std::max(w, h) * 0.5f;
        }
    }
}

void MapRenderer::rebuildGlowMap(const ProvinceMap& provinces) {
    m_provinceGlow.clear();
    if (m_borderPixels.empty()) return;

    const auto* provPixels = static_cast<const unsigned char*>(provinces.getImage().data);
    if (!provPixels) return;

    int stride = m_mapW * 4;

    for (int y = 0; y < m_mapH; ++y) {
        Audio::get().pump();          // as in computeBorderTexture above
        int rowOff = y * stride;
        for (int x = 0; x < m_mapW; ++x) {
            int pi = rowOff + x * 4;
            uint8_t ba = m_borderPixels[pi + 3];
            if (ba == 0) continue;

            int foundPid = 0;
            for (int pass = 0; pass < 5 && foundPid == 0; ++pass) {
                int nx = x, ny = y;
                if (pass == 1) nx = (x == 0) ? m_mapW - 1 : x - 1;
                if (pass == 2) nx = (x == m_mapW - 1) ? 0 : x + 1;
                if (pass == 3) { nx = x; ny = y - 1; }
                if (pass == 4) { nx = x; ny = y + 1; }
                if (ny < 0 || ny >= m_mapH) continue;
                int ni = ny * stride + nx * 4;
                int nid = Province::colorToId(provPixels[ni], provPixels[ni + 1], provPixels[ni + 2]);
                if (nid > 0) foundPid = nid;
            }
            if (foundPid > 0)
                m_provinceGlow[foundPid].push_back({y * m_mapW + x, ba});
        }
    }
}

void MapRenderer::buildSelectionGlow() {
    if (m_selectionTex.id > 0) {
        UnloadTexture(m_selectionTex);
        m_selectionTex = {};
    }
    if (m_selectedProvinceId <= 0) return;

    auto it = m_provinceGlow.find(m_selectedProvinceId);
    if (it == m_provinceGlow.end() || it->second.empty()) return;

    Image glowImg = GenImageColor(m_mapW, m_mapH, {0, 0, 0, 0});
    auto* glowData = static_cast<unsigned char*>(glowImg.data);

    for (auto& [idx, alpha] : it->second) {
        int bi = idx * 4;
        glowData[bi] = 255;
        glowData[bi + 1] = 255;
        glowData[bi + 2] = 255;
        glowData[bi + 3] = alpha;
    }

    m_selectionTex = LoadTextureFromImage(glowImg);
    UnloadImage(glowImg);
}

void MapRenderer::setBulkSelection(const std::vector<int>& provinceIds, Color tint) {
    clearBulkSelection();
    if (provinceIds.empty()) return;

    Image img = GenImageColor(m_mapW, m_mapH, {0, 0, 0, 0});
    auto* data = static_cast<unsigned char*>(img.data);

    bool any = false;
    for (int pid : provinceIds) {
        auto it = m_provinceGlow.find(pid);
        if (it == m_provinceGlow.end()) continue;
        for (auto& [idx, alpha] : it->second) {
            const int bi = idx * 4;
            data[bi]     = tint.r;
            data[bi + 1] = tint.g;
            data[bi + 2] = tint.b;
            // The province's own edge falloff, used AS IS -- exactly what
            // buildSelectionGlow does for the single selection. Scaling it down
            // by a tint alpha as well made forty painted provinces almost
            // invisible, which is the one thing this overlay may not be.
            data[bi + 3] = alpha;
            any = true;
        }
    }

    if (any) m_bulkTex = LoadTextureFromImage(img);
    UnloadImage(img);
}

void MapRenderer::clearBulkSelection() {
    if (m_bulkTex.id > 0) UnloadTexture(m_bulkTex);
    m_bulkTex = {};
}

void MapRenderer::draw(const LandSeaMap& landSea, const ProvinceMap& provinces, const CountryMap& countries) {
    BeginMode2D(m_camera);

    const Texture2D& tex = landSea.getTexture();
    float viewW = m_screenW / m_camera.zoom;
    float left = m_camera.target.x - viewW * 0.5f;
    float right = m_camera.target.x + viewW * 0.5f;

    int tileStart = static_cast<int>(std::floor(left / m_mapW));
    int tileEnd = static_cast<int>(std::ceil(right / m_mapW));
    for (int tx = tileStart; tx < tileEnd; ++tx) {
        DrawTexture(tex, tx * m_mapW, 0, WHITE);
    }

    // Claims overlay mode: political base + semi-transparent claims pattern
    if (m_showClaims && m_claimsTex.id > 0) {
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_politicalTex, tx * m_mapW, 0, WHITE);
        }
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_claimsTex, tx * m_mapW, 0, WHITE);
        }
    } else {
        Texture2D mainTex = m_politicalTex;
        if (m_showResource >= 0 && m_resourceTex.id > 0)
            mainTex = m_resourceTex;
        else if (m_showRelations && m_populationTex.id > 0)
            mainTex = m_populationTex;
        else if (m_showPopulation && m_populationTex.id > 0)
            mainTex = m_populationTex;
        if (mainTex.id > 0) {
            for (int tx = tileStart; tx < tileEnd; ++tx) {
                DrawTexture(mainTex, tx * m_mapW, 0, WHITE);
            }
        }
    }

    // Draw borders overlay (province borders — hidden in country-names mode)
    if (!m_showCountryNames && m_borderTex.id > 0) {
        Color borderColor = ColorAlpha(BLACK, 0.15f);
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_borderTex, tx * m_mapW, 0, borderColor);
        }
    }

    // Draw selection glow
    if (m_selectionTex.id > 0) {
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_selectionTex, tx * m_mapW, 0, WHITE);
        }
    }

    // Provinces painted for a bulk action but not yet committed. Drawn after
    // the single selection so a province that is both still reads as painted --
    // which is what the player is deciding about.
    if (m_bulkTex.id > 0) {
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_bulkTex, tx * m_mapW, 0, WHITE);
        }
    }

    EndMode2D();

    // Draw country names overlay — curved per-character text
    if (m_showCountryNames && m_countryLabels) {
        float t = (m_camera.zoom - m_minZoom) / (m_maxZoom - m_minZoom);
        t = std::clamp(t, 0.0f, 1.0f);
        uint8_t alpha = (uint8_t)(255.0f * (1.0f - t));
        if (alpha < 25) alpha = 25;
        Font baseFont = GetFontDefault();
        bool haveFallback = (m_fallbackFont.texture.id > 0);
        float spacing = 3.0f;

        for (auto& label : *m_countryLabels) {
            // Wrap label center to the copy closest to the camera
            Vector2 center = label.center;
            {
                float dx = center.x - m_camera.target.x;
                while (dx > m_mapW * 0.5f) { center.x -= m_mapW; dx -= m_mapW; }
                while (dx < -m_mapW * 0.5f) { center.x += m_mapW; dx += m_mapW; }
            }

            Vector2 sp = GetWorldToScreen2D(center, m_camera);
            if (sp.x < -300 || sp.x > m_screenW + 300) continue;
            if (sp.y < -100 || sp.y > m_screenH + 100) continue;

            Color col = {255, 255, 255, alpha};

            const char* text = label.name.c_str();
            int len = (int)strlen(text);
            if (len < 1) continue;

            // Display font size: scales with zoom but never tiny
            float displayFs = (float)label.fontSize * m_camera.zoom;
            displayFs = std::clamp(displayFs, 14.0f, (float)label.fontSize);

            // Decode UTF-8 into codepoints, determine per-char font
            struct CharInfo { int cp; Font* font; float advance; };
            std::vector<CharInfo> chars;
            const unsigned char* up = (const unsigned char*)text;
            while (*up) {
                int cp = 0, sz = 0;
                if (*up < 0x80) { cp = *up; sz = 1; }
                else if ((*up & 0xE0) == 0xC0) { cp = (*up & 0x1F) << 6 | (up[1] & 0x3F); sz = 2; }
                else if ((*up & 0xF0) == 0xE0) { cp = (*up & 0x0F) << 12 | (up[1] & 0x3F) << 6 | (up[2] & 0x3F); sz = 3; }
                else if ((*up & 0xF8) == 0xF0) { cp = (*up & 0x07) << 18 | (up[1] & 0x3F) << 12 | (up[2] & 0x3F) << 6 | (up[3] & 0x3F); sz = 4; }
                else { up++; continue; }

                bool isASCII = (cp >= 32 && cp < 127);
                Font* useFont = (isASCII || !haveFallback) ? &baseFont : &m_fallbackFont;
                int gi = GetGlyphIndex(*useFont, cp);
                float adv = (float)(*useFont).glyphs[gi].advanceX;
                if (adv <= 0) adv = (*useFont).recs[gi].width;
                float refScale = (float)label.fontSize / (float)(*useFont).baseSize;
                chars.push_back({cp, useFont, adv * refScale});
                up += sz;
            }
            if (chars.empty()) continue;

            // Compute total width
            float totalW = 0;
            for (size_t ci = 0; ci < chars.size(); ci++) {
                totalW += chars[ci].advance;
                if (ci < chars.size() - 1) totalW += spacing;
            }
            if (totalW < 1.0f) continue;

            // Scale curvature proportionally
            float curvScale = (label.span > 0) ? totalW / label.span : 1.0f;
            float useCurvature = label.curvature * curvScale;

            Vector2 dir = {cosf(label.angle), sinf(label.angle)};
            Vector2 perp = {-dir.y, dir.x};
            float cursor = -totalW * 0.5f;

            for (size_t ci = 0; ci < chars.size(); ci++) {
                float cw = chars[ci].advance;
                float charCenter = cursor + cw * 0.5f;
                float p = (charCenter + totalW * 0.5f) / totalW;

                float curvatureOffset = sinf(p * PI) * useCurvature;
                Vector2 pos = {
                    center.x + dir.x * cursor + perp.x * curvatureOffset,
                    center.y + dir.y * cursor + perp.y * curvatureOffset
                };

                float tangent = atan2f(cosf(p * PI) * PI * useCurvature, totalW);
                float deg = (label.angle + tangent) * RAD2DEGF;

                Vector2 screenPos = GetWorldToScreen2D(pos, m_camera);
                char buf[8] = {};
                int wpos = 0;
                int cpv = chars[ci].cp;
                if (cpv < 0x80) { buf[wpos++] = (char)cpv; }
                else if (cpv < 0x800) { buf[wpos++] = (char)(0xC0 | (cpv >> 6)); buf[wpos++] = (char)(0x80 | (cpv & 0x3F)); }
                else if (cpv < 0x10000) { buf[wpos++] = (char)(0xE0 | (cpv >> 12)); buf[wpos++] = (char)(0x80 | ((cpv >> 6) & 0x3F)); buf[wpos++] = (char)(0x80 | (cpv & 0x3F)); }
                else { buf[wpos++] = (char)(0xF0 | (cpv >> 18)); buf[wpos++] = (char)(0x80 | ((cpv >> 12) & 0x3F)); buf[wpos++] = (char)(0x80 | ((cpv >> 6) & 0x3F)); buf[wpos++] = (char)(0x80 | (cpv & 0x3F)); }
                buf[wpos] = 0;

                DrawTextPro(*chars[ci].font, buf, screenPos,
                            {0, displayFs * 0.5f}, deg, displayFs, 0, col);

                cursor += cw;
                if (ci < chars.size() - 1) cursor += spacing;
            }
        }
    }

    // Skip click/tooltip when paused (menu overlay handles input)
    if (m_paused) return;

    Vector2 mouseWorld = GetScreenToWorld2D(getMouse(), m_camera);
    int px = static_cast<int>(mouseWorld.x);
    int py = static_cast<int>(mouseWorld.y);

    int pxWrapped = px;
    while (pxWrapped < 0) pxWrapped += m_mapW;
    while (pxWrapped >= m_mapW) pxWrapped -= m_mapW;

    auto findProvince = [&](int cx, int cy, int radius) -> const Province* {
        const Province* p = provinces.getProvince(cx, cy);
        if (p) return p;
        for (int r = 1; r <= radius; ++r) {
            for (int dx = -r; dx <= r; ++dx) {
                int nx = cx + dx;
                while (nx < 0) nx += m_mapW;
                while (nx >= m_mapW) nx -= m_mapW;
                int ny1 = cy - r, ny2 = cy + r;
                if (ny1 >= 0) { p = provinces.getProvince(nx, ny1); if (p) return p; }
                if (ny2 < m_mapH) { p = provinces.getProvince(nx, ny2); if (p) return p; }
            }
            for (int dy = -(r - 1); dy <= r - 1; ++dy) {
                int ny = cy + dy;
                if (ny < 0 || ny >= m_mapH) continue;
                int nx1 = cx - r, nx2 = cx + r;
                while (nx1 < 0) nx1 += m_mapW;
                while (nx1 >= m_mapW) nx1 -= m_mapW;
                while (nx2 < 0) nx2 += m_mapW;
                while (nx2 >= m_mapW) nx2 -= m_mapW;
                p = provinces.getProvince(nx1, ny); if (p) return p;
                p = provinces.getProvince(nx2, ny); if (p) return p;
            }
        }
        return nullptr;
    };

    const Province* prov = (py >= 0 && py < m_mapH) ? findProvince(pxWrapped, py, 10) : nullptr;
    // Recorded rather than recomputed by anyone who wants it. Finding the
    // province under the cursor is a search this already does every frame, and
    // a second caller doing it again would double that cost for an answer that
    // is sitting right here.
    m_hoveredProvinceId = prov ? prov->id : 0;

    if (prov) {
        const Country* c = countries.getCountry(prov->countryId);
        int pad = 4;
        int flagW = 96, flagH = 48;
        int fontSize = 20;
        int tipPad = pad;

        if (m_debugMode) {
            // Debug ON: show province name, no flag in tooltip
            std::string tooltip = prov->name;
            if (c) tooltip += " (" + c->name + ")";
            int totalW = MeasureText(tooltip.c_str(), fontSize) + 2 * tipPad + tipPad;
            int totalH = flagH + 2 * tipPad;
            int tipX = m_screenW - totalW - tipPad;
            int tipY = tipPad;
            DrawRectangle(tipX, tipY, totalW, totalH, {0, 0, 0, 180});
            DrawText(tooltip.c_str(), tipX + tipPad, (totalH - fontSize) / 2 + tipY, fontSize, WHITE);
        } else {
            // Debug OFF: show country name + flag only
            std::string tooltip = c ? c->name : prov->name;
            int totalW = 2 * tipPad + flagW + tipPad + MeasureText(tooltip.c_str(), fontSize) + 2 * tipPad;
            int totalH = flagH + 2 * tipPad;
            int tipX = m_screenW - totalW - tipPad;
            int tipY = tipPad;
            DrawRectangle(tipX, tipY, totalW, totalH, {0, 0, 0, 180});

            bool flagDrawn = false;
            if (c && m_countryFlags) {
                auto fit = m_countryFlags->find(c->id);
                if (fit != m_countryFlags->end() && fit->second.id > 0) {
                    DrawTexturePro(fit->second,
                        {0, 0, (float)fit->second.width, (float)fit->second.height},
                        {(float)(tipX + tipPad), (float)(tipY + tipPad), (float)flagW, (float)flagH},
                        {0, 0}, 0.0f, WHITE);
                    flagDrawn = true;
                }
            }
            int textX = flagDrawn ? tipX + tipPad + flagW + tipPad : tipX + tipPad;
            DrawText(tooltip.c_str(), textX, (totalH - fontSize) / 2 + tipY, fontSize, WHITE);
        }
    } else if (m_debugMode && py >= 0 && py < m_mapH) {
        float lon, lat;
        landSea.pixelToLonLat(pxWrapped, py, lon, lat);
        DrawText(TextFormat(T("Ocean (%.1f, %.1f)"), lon, lat), 10, 5, 20, SKYBLUE);
    }

    // Click to select province (skip if clicking bottom panel or province info panel)
    bool onPanel = pointOverPanels(getMouse());
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !m_wasDragged && !m_blockLeftPan && !onPanel) {
        if (prov) {
            int pid = prov->id;
            m_selectedProvinceId = (m_selectedProvinceId == pid) ? 0 : pid;
        } else {
            m_selectedProvinceId = 0;
        }
        buildSelectionGlow();
        m_wasDragged = false;
    }

    if (m_debugMode) {
        // Show selection info with flag
        if (m_selectedProvinceId > 0) {
            auto it = provinces.getAllProvinces().find(m_selectedProvinceId);
            if (it != provinces.getAllProvinces().end()) {
                const Province& sel = it->second;
                const Country* c = countries.getCountry(sel.countryId);
                std::string info = "Selected: " + sel.name;
                if (c) info += " (" + c->name + ")";
                DrawText(info.c_str(), m_screenW / 2 - 200, m_screenH - 25, 20, YELLOW);
            }
        }

        DrawText(TextFormat(T("Zoom: %.1fx  |  Scroll to zoom, Left-drag to pan"),
                            m_camera.zoom),
                 10, m_screenH - 25, 16, LIGHTGRAY);

        int provCount = provinces.getAllProvinces().size();
        DrawText(TextFormat(T("Provinces: %d"), provCount), 10, m_screenH - 45, 16, LIGHTGRAY);
    }

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
        m_wasDragged = false;
    }
}

void MapRenderer::drawSubregion(int sx, int sy, int sw, int sh,
                                float worldX, float worldY, float zoom,
                                const LandSeaMap& landSea, const ProvinceMap& provinces, const CountryMap& countries) {
    Camera2D saved = m_camera;
    m_camera.target = {worldX, worldY};
    m_camera.zoom = zoom;
    m_camera.offset = {(float)sx + sw * 0.5f, (float)sy + sh * 0.5f};

    // Clamp Y axis (X loops horizontally)
    float viewH = (float)sh / m_camera.zoom;
    m_camera.target.y = std::max(viewH * 0.5f, std::min((float)m_mapH - viewH * 0.5f, m_camera.target.y));

    BeginScissorMode(sx, sy, sw, sh);
    BeginMode2D(m_camera);

    // Horizontal tiling so map wraps around (cylindrical projection)
    float viewW = (float)sw / m_camera.zoom;
    float left = m_camera.target.x - viewW * 0.5f;
    float right = m_camera.target.x + viewW * 0.5f;

    const Texture2D& lsTex = landSea.getTexture();
    int tileStart = static_cast<int>(std::floor(left / m_mapW));
    int tileEnd = static_cast<int>(std::ceil(right / m_mapW));
    for (int tx = tileStart; tx < tileEnd; ++tx) {
        DrawTexture(lsTex, tx * m_mapW, 0, WHITE);
    }

    if (m_politicalTex.id > 0 && m_showPolitical) {
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_politicalTex, tx * m_mapW, 0, WHITE);
        }
    }

    // Claims wash (map editor's claims brush). Sits above the political fill
    // so claimed provinces read clearly, but below borders/highlight.
    if (m_claimsTex.id > 0 && m_showClaims) {
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_claimsTex, tx * m_mapW, 0, WHITE);
        }
    }

    if (m_editorOverlayTex.id > 0 && m_showEditorOverlay) {
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_editorOverlayTex, tx * m_mapW, 0, WHITE);
        }
    }

    if (m_borderTex.id > 0) {
        Color bc = ColorAlpha(BLACK, 0.15f);
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_borderTex, tx * m_mapW, 0, bc);
        }
    }

    // Pulsing selection highlight (map editor)
    if (m_highlightTex.id > 0) {
        float pulse = 0.30f + 0.18f * sinf((float)GetTime() * 4.0f);
        Color hc = ColorAlpha(WHITE, pulse);
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(m_highlightTex, tx * m_mapW + m_highlightX, m_highlightY, hc);
        }
    }

    EndMode2D();
    EndScissorMode();

    m_camera = saved;
}

void MapRenderer::screenToPixel(float sx, float sy, int& px, int& py) const {
    Vector2 screenPos = { sx, sy };
    Vector2 worldPos = GetScreenToWorld2D(screenPos, m_camera);
    py = static_cast<int>(worldPos.y);
    px = static_cast<int>(worldPos.x);
    while (px < 0) px += m_mapW;
    while (px >= m_mapW) px -= m_mapW;
}

void MapRenderer::pixelToScreen(float px, float py, float& sx, float& sy) const {
    // The map wraps horizontally: project the tile copy nearest the camera
    float wrapped = px + roundf((m_camera.target.x - px) / (float)m_mapW) * (float)m_mapW;
    Vector2 v = GetWorldToScreen2D({wrapped, py}, m_camera);
    sx = v.x;
    sy = v.y;
}
