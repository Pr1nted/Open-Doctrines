#include "MapRenderer.h"
#include "GlobeView.h"
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
#include <cstdint>
#include <vector>
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
    if (m_surface.id > 0) UnloadRenderTexture(m_surface);
    delete m_globe;
}

void MapRenderer::setEditorOverlay(Texture2D tex) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_editorOverlayTex.id > 0) UnloadTexture(m_editorOverlayTex);
    m_editorOverlayTex = tex;
}

void MapRenderer::setHighlight(Texture2D tex, int x, int y) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_highlightTex.id > 0) UnloadTexture(m_highlightTex);
    m_highlightTex = tex;
    m_highlightX = x;
    m_highlightY = y;
}

void MapRenderer::clearHighlight() {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_highlightTex.id > 0) UnloadTexture(m_highlightTex);
    m_highlightTex = Texture2D{};
}

void MapRenderer::setPoliticalTexture(Texture2D tex) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_politicalTex.id > 0) UnloadTexture(m_politicalTex);
    m_politicalTex = tex;
}

void MapRenderer::updatePoliticalTexture(const void* data) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_politicalTex.id > 0)
        UpdateTexture(m_politicalTex, data);
}

void MapRenderer::updatePoliticalTextureRec(const void* rectData, int x, int y, int w, int h) {
    m_surfaceDirty = true;   // the globe samples a composite of these
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
    m_surfaceDirty = true;   // the globe samples a composite of these
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
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_populationTex.id > 0) UnloadTexture(m_populationTex);
    m_populationTex = tex;
}

void MapRenderer::updatePopulationTexture(const void* data) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_populationTex.id > 0)
        UpdateTexture(m_populationTex, data);
}

void MapRenderer::setResourceTexture(Texture2D tex) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_resourceTex.id > 0) UnloadTexture(m_resourceTex);
    m_resourceTex = tex;
}

void MapRenderer::updateResourceTexture(const void* data) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_resourceTex.id > 0)
        UpdateTexture(m_resourceTex, data);
}

void MapRenderer::setClaimsTexture(Texture2D tex) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_claimsTex.id > 0) UnloadTexture(m_claimsTex);
    m_claimsTex = tex;
}

void MapRenderer::updateClaimsTexture(const void* data) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_claimsTex.id > 0)
        UpdateTexture(m_claimsTex, data);
}

void MapRenderer::updateClaimsTextureRec(const void* rectData, int x, int y, int w, int h) {
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_claimsTex.id > 0)
        UpdateTextureRec(m_claimsTex, {(float)x, (float)y, (float)w, (float)h}, rectData);
}

void MapRenderer::computeBorderTexture(const Image& provImage) {
    m_surfaceDirty = true;   // the globe samples a composite of these
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

void MapRenderer::snapTo(float x, float y, float zoom) {
    m_camera.target = { x, y };
    m_camera.zoom   = std::clamp(zoom, m_minZoom, m_maxZoom);
    m_flying = false;
    m_wasDragged = false;
    m_isDragging = false;
}

void MapRenderer::update(float dt) {
    bool userInteracted = false;

    if (m_morphing) {
        // ~0.7s each way. Eased at both ends rather than run at a constant rate:
        // the sheet starts and stops without a jolt, which is most of what makes
        // the move read as one object turning rather than two frames spliced.
        const float step = dt / 0.70f;
        m_morph += (m_morphTo > m_morph) ? step : -step;
        if (m_morph >= 1.0f) { m_morph = 1.0f; m_morphing = false; }
        if (m_morph <= 0.0f) { m_morph = 0.0f; m_morphing = false; finishTransition(); }
    }

    // Input is the globe's only while the globe is actually a globe. Orbiting a
    // half-unrolled sheet moves the camera along an arc that is itself being
    // interpolated, and the two fight.
    if (!m_paused && !m_morphing && m_view == ViewMode::Globe && m_globe) {
        // The globe takes the same gestures as the flat map -- drag to move,
        // wheel to zoom -- so the hand does not have to learn a second map.
        // Handled here rather than in the caller so no input code has to know
        // which view is up.
        const Vector2 d = GetMouseDelta();
        const bool panning = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ||
                             (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !m_blockLeftPan);
        if (panning) {
            if (!m_isDragging) m_isDragging = true;
            if (fabs(d.x) > 3.0f || fabs(d.y) > 3.0f) m_wasDragged = true;
            // Negated: dragging left should turn the globe so the ground moves
            // WITH the cursor, the way dragging the flat map does.
            m_globe->orbit(-d.x, -d.y);
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE) ||
            IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_isDragging = false;

        const float w = GetMouseWheelMove();
        if (w != 0.0f &&
            (m_provincePanelRect.height <= 0 ||
             !CheckCollisionPointRec(getMouse(), m_provincePanelRect))) {
            m_globe->zoom(w);
        }
        return;
    }

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

    // ── ACCUMULATORS INDEXED BY PROVINCE ID, NOT HASHED BY IT ──
    //
    // This loop visits every pixel of the province image -- 33.5 million of
    // them on an 8192x4096 map -- and it used to do a dozen unordered_map
    // operations at each one: a count() to test the id, three [] to add to the
    // sums, and up to eight more for the bounding box. Four hundred million
    // hash lookups to compute what is, in the end, a sum and a rectangle per
    // province.
    //
    // Natively that is a few seconds. In wasm, where the hashing has none of
    // the native optimisations behind it, it is the thirty seconds the browser
    // build spent frozen at "82%" -- the bar reads 82% because the frame this
    // work belongs to cannot be presented until it returns.
    //
    // Province ids are dense and bounded (m_provinceCountryLookup is already a
    // vector indexed by them), so a flat array is the right shape: one bounds
    // check and one indexed read where there were twelve hash lookups.
    int maxPid = 0;
    for (const auto& kv : all) maxPid = std::max(maxPid, kv.first);
    const size_t nPid = (size_t)maxPid + 1;

    std::vector<uint8_t>   known(nPid, 0);
    for (const auto& kv : all)
        if (kv.first > 0) known[(size_t)kv.first] = 1;

    std::vector<long long> sx(nPid, 0), sy(nPid, 0), cnt(nPid, 0);
    std::vector<int32_t>   minX(nPid, INT32_MAX), maxX(nPid, INT32_MIN);
    std::vector<int32_t>   minY(nPid, INT32_MAX), maxY(nPid, INT32_MIN);

    for (int y = 0; y < m_mapH; ++y) {
        Audio::get().pump();          // as in computeBorderTexture above
        int rowOff = y * stride;
        for (int x = 0; x < m_mapW; ++x) {
            int pi = rowOff + x * 4;
            int r = provPixels[pi], g = provPixels[pi + 1], b = provPixels[pi + 2];
            int pid = Province::colorToId(r, g, b);

            // Accumulate center/bbox data
            if (pid > 0 && (size_t)pid < nPid && known[(size_t)pid]) {
                const size_t k = (size_t)pid;
                sx[k] += x;
                sy[k] += y;
                cnt[k]++;
                if (x < minX[k]) minX[k] = x;
                if (x > maxX[k]) maxX[k] = x;
                if (y < minY[k]) minY[k] = y;
                if (y > maxY[k]) maxY[k] = y;
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
        const int id = kv.first;
        if (id <= 0 || (size_t)id >= nPid) continue;
        const size_t k = (size_t)id;
        if (cnt[k] <= 0) continue;
        centers_out[id] = {
            (float)sx[k] / (float)cnt[k],
            (float)sy[k] / (float)cnt[k]
        };
        const float w = (float)(maxX[k] - minX[k]);
        const float h = (float)(maxY[k] - minY[k]);
        radii_out[id] = std::max(w, h) * 0.5f;
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
    m_surfaceDirty = true;   // the globe samples a composite of these
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
    m_surfaceDirty = true;   // the globe samples a composite of these
    if (m_bulkTex.id > 0) UnloadTexture(m_bulkTex);
    m_bulkTex = {};
}

std::vector<MapRenderer::Layer> MapRenderer::layerStack(const LandSeaMap& landSea) const {
    std::vector<Layer> out;
    out.push_back({landSea.getTexture(), WHITE});

    // Claims mode paints the political base and then a semi-transparent claims
    // pattern over it; every other mode picks ONE base layer.
    if (m_showClaims && m_claimsTex.id > 0) {
        if (m_politicalTex.id > 0) out.push_back({m_politicalTex, WHITE});
        out.push_back({m_claimsTex, WHITE});
    } else {
        Texture2D mainTex = m_politicalTex;
        if (m_showResource >= 0 && m_resourceTex.id > 0)      mainTex = m_resourceTex;
        else if (m_showRelations && m_populationTex.id > 0)   mainTex = m_populationTex;
        else if (m_showPopulation && m_populationTex.id > 0)  mainTex = m_populationTex;
        if (mainTex.id > 0) out.push_back({mainTex, WHITE});
    }

    if (!m_showCountryNames && m_borderTex.id > 0)
        out.push_back({m_borderTex, ColorAlpha(BLACK, 0.15f)});
    if (m_selectionTex.id > 0) out.push_back({m_selectionTex, WHITE});
    // Painted for a bulk action but not committed: drawn last so a province
    // that is both selected and painted reads as painted, which is what the
    // player is deciding about.
    if (m_bulkTex.id > 0) out.push_back({m_bulkTex, WHITE});
    return out;
}

// ── What the composite has to hold, and how big to make it ──
//
// Far out, the whole planet is on screen and the answer is the whole map at a
// resolution the view can actually use. Zoomed in, the visible ground is a small
// cap: compositing the whole map to serve it spends every texel outside the cap
// on nothing, which is why the close view needed a 134 MB full-resolution copy
// to look sharp. A window over just that cap gets the same sharpness out of a
// target a quarter the size, because none of it is wasted.
//
// The window is deliberately larger than the cap. Back faces are culled but the
// near hemisphere is not clipped to what is visible, so fragments beyond the
// horizon are still rasterised; they end up off screen, but a window that only
// just covered the cap would have them sampling past its edge, and the bilinear
// filter would drag that edge inward into view.
MapRenderer::SurfaceWindow MapRenderer::surfaceWindow() const {
    SurfaceWindow w;
    const int half = m_mapW > 4096 ? m_mapW / 2 : m_mapW;
    if (m_view != ViewMode::Globe || !m_globe) {
        w.texW = half; w.texH = std::max(1, half * m_mapH / m_mapW);
        return w;
    }

    const float dist = std::max(1.0001f, m_globe->distance());
    const float lat  = m_globe->latitude();

    // ── How far round the planet the SCREEN reaches ──
    //
    // Not how far the horizon reaches. Close in they are very different: at the
    // nearest zoom the horizon is 42 degrees away and the viewport shows barely
    // 21, so a window sized to the horizon is four times the ground it needs.
    // Sized to the horizon it also reached over the pole, which forces the whole
    // map -- the first version of this fell back to the full composite every
    // time and was byte-for-byte the thing it was replacing.
    //
    // A point `th` round from under the camera projects to radius
    // f*sin(th)/(dist-cos(th)), which climbs with th, so walking outwards and
    // stopping at the corner of the screen finds the angle wanted.
    const float horizon = acosf(1.0f / dist);
    const float f = ((float)m_screenH * 0.5f) / tanf(45.0f * 0.5f * DEG2RAD);
    const float rMax = 0.5f * sqrtf((float)m_screenW * (float)m_screenW +
                                    (float)m_screenH * (float)m_screenH) * 1.06f;
    float cap = horizon;
    for (int i = 1; i <= 48; ++i) {
        const float th = horizon * (float)i / 48.0f;
        const float den = dist - cosf(th);
        const float r = (den > 0.01f) ? f * sinf(th) / den : 1e9f;
        if (r > rMax) { cap = th; break; }
    }
    // A margin, then clamped to the horizon: there is nothing beyond it to hold.
    cap = std::min(cap * 1.12f + 0.02f, horizon);

    // Longitude covers more ground per radian the nearer the pole. A cap that
    // reaches over the pole spans every longitude, and there the wrap stops
    // meaning anything at all.
    const float alat = fabsf(lat);
    float dLon;
    if (alat + cap >= PI * 0.5f - 0.01f) dLon = PI;
    else dLon = asinf(std::min(1.0f, sinf(cap) / cosf(alat)));

    w.du = dLon / PI;              // dLon is a half-width; the map spans 2*PI
    w.dv = (cap * 2.0f) / PI;
    if (w.du >= 0.75f || w.dv >= 0.75f) {
        // Most of the map anyway: a window would cost the wrap handling and buy
        // nothing.
        w.texW = half; w.texH = std::max(1, half * m_mapH / m_mapW);
        return w;
    }

    const float uc = (m_globe->longitude() + PI) / (2.0f * PI);
    const float vc = (PI * 0.5f - lat) / PI;
    w.u0 = uc - w.du * 0.5f;
    w.v0 = vc - w.dv * 0.5f;

    // ── Quantised, so panning does not recomposite every frame ──
    //
    // The window follows the camera, and a window that follows exactly is a
    // full rebuild of a multi-megabyte target on every mouse move. Snapped to a
    // grid an eighth of its own size, it rebuilds once per eighth of a turn.
    const float gx = w.du / 8.0f, gy = w.dv / 8.0f;
    w.u0 = floorf(w.u0 / gx) * gx;
    w.v0 = floorf(w.v0 / gy) * gy;
    // Grown by one grid step each way so the snap can never uncover an edge.
    w.u0 -= gx; w.du += gx * 2.0f;
    w.v0 -= gy; w.dv += gy * 2.0f;
    // Vertically the map does not wrap, so a window off the top or bottom is
    // clamped rather than folded.
    if (w.v0 < 0.0f) { w.dv += w.v0; w.v0 = 0.0f; }
    if (w.v0 + w.dv > 1.0f) w.dv = 1.0f - w.v0;
    if (w.dv <= 0.001f) { w.texW = half; w.texH = std::max(1, half * m_mapH / m_mapW); return w; }

    // ── How many texels, and why not a power of two ──
    //
    // Two ceilings: what the screen can show, and what the raster can supply.
    // The smaller wins, and here it is almost always the raster -- close in the
    // ground is magnified, so the source runs out of detail long before the
    // screen runs out of pixels.
    //
    // The scale that matters for the first is the one directly under the camera,
    // f/(dist-1) pixels per radian, NOT the projected radius of the whole sphere:
    // sizing by the latter asks for a quarter of the texels the middle of the
    // view actually uses, because that is where the ground is nearest.
    //
    // Sized to that, in steps of 256 rather than doublings. Rounding up to a
    // power of two was asking for two to four times the texels the window could
    // use, which ran into the memory ceiling and got halved back to barely more
    // than the full composite gave -- a window costing an extra resample to
    // deliver the same picture. Nothing here needs a power of two: the patch is
    // clamped, not wrapped, and carries no mipmaps.
    const float perRadian = f / std::max(dist - 1.0f, 0.05f);
    const float wantW = std::min(w.du * 2.0f * PI * perRadian, w.du * (float)m_mapW);
    auto step256 = [](float v, int lo, int hi) {
        int n = ((int)(v + 128.0f) / 256) * 256;
        return std::clamp(n, lo, hi);
    };
    w.texW = step256(wantW, 512, 4096);
    w.texH = step256(w.texW * (w.dv * (float)m_mapH) / (w.du * (float)m_mapW), 256, 4096);

    // A last ceiling on memory, for a window that is large AND close. The full
    // composite costs 34 MB; a window is allowed more, because it is the close
    // view and the only one that can use it, but not without limit.
    const long long budget = 4096LL * 3072LL;
    while ((long long)w.texW * w.texH > budget && w.texW > 512) {
        w.texW = step256(w.texW * 0.75f, 512, 4096);
        w.texH = step256(w.texH * 0.75f, 256, 4096);
    }
    w.full = false;
    return w;
}

void MapRenderer::buildSurface(const LandSeaMap& landSea) {
    const SurfaceWindow win = surfaceWindow();
    if (m_surface.id == 0 || m_surface.texture.width != win.texW ||
        m_surface.texture.height != win.texH) {
        if (m_surface.id > 0) UnloadRenderTexture(m_surface);
        m_surface = LoadRenderTexture(win.texW, win.texH);
        // Bilinear, or the sphere shows the raster's texels at the limb where
        // it is most compressed.
        SetTextureFilter(m_surface.texture, TEXTURE_FILTER_BILINEAR);
    }
    if (m_globe) m_globe->setSurfaceWindow({win.u0, win.v0}, {win.du, win.dv});

    BeginTextureMode(m_surface);
    ClearBackground(BLANK);
    const float W = (float)win.texW, H = (float)win.texH;
    for (const Layer& l : layerStack(landSea)) {
        if (l.tex.id == 0) continue;
        const float tw = (float)l.tex.width, th = (float)l.tex.height;
        // NEGATIVE source height, and it is not a trick: a render target is
        // stored bottom-up, so anything drawn into it arrives upside down when
        // sampled. Flipping each layer on the way in cancels that exactly, and
        // costs nothing -- the alternative is a flag the globe has to carry and
        // every future consumer of this surface has to remember.
        if (win.full) {
            DrawTexturePro(l.tex, {0.0f, 0.0f, tw, -th}, {0.0f, 0.0f, W, H},
                           {0.0f, 0.0f}, 0.0f, l.tint);
            continue;
        }
        // A window can straddle the antimeridian, where the source wraps and the
        // patch does not, so it goes in as up to two pieces.
        float x0 = fmodf(win.u0, 1.0f);
        if (x0 < 0.0f) x0 += 1.0f;
        const float srcY = win.v0 * th, srcH = win.dv * th;
        const float firstU = std::min(win.du, 1.0f - x0);
        DrawTexturePro(l.tex,
                       {x0 * tw, srcY, firstU * tw, -srcH},
                       {0.0f, 0.0f, W * (firstU / win.du), H},
                       {0.0f, 0.0f}, 0.0f, l.tint);
        if (firstU < win.du) {
            const float restU = win.du - firstU;
            DrawTexturePro(l.tex,
                           {0.0f, srcY, restU * tw, -srcH},
                           {W * (firstU / win.du), 0.0f, W * (restU / win.du), H},
                           {0.0f, 0.0f}, 0.0f, l.tint);
        }
    }
    EndTextureMode();
    m_surfaceDirty = false;
}


// Country names, drawn from BOTH views. Lifted out of draw() because the globe
// path returns before the end of it, so for as long as this was inline the
// globe silently had no labels at all -- the code was right and simply never
// ran. Every position goes through pixelToScreen, which is what makes the same
// routine curve the names round a sphere and lay them flat on a map.
float MapRenderer::faceCosine(float px, float py) const {
    if (m_view != ViewMode::Globe || !m_globe) return 1.0f;
    return m_globe->facing(px, py);
}

void MapRenderer::drawCountryNames() {
    if (m_showCountryNames && m_countryLabels) {
        float t = (getZoom() - m_minZoom) / (m_maxZoom - m_minZoom);
        t = std::clamp(t, 0.0f, 1.0f);
        uint8_t alpha = (uint8_t)(255.0f * (1.0f - t));
        if (alpha < 25) alpha = 25;
        Font baseFont = GetFontDefault();
        bool haveFallback = (m_fallbackFont.texture.id > 0);
        float spacing = 3.0f;

        // ── Decluttering, on the globe only ──
        //
        // At whole-planet zoom Europe is thirty countries inside a hand's
        // breadth, and their names overprint into a grey smear. The flat map
        // solves this by being zoomed in; the globe has no such escape, because
        // seeing the whole planet at once is the point of it.
        //
        // So: biggest country first, and a name is kept only if its box is clear
        // of every name already kept. Greedy and one pass -- the ordering is
        // what makes it look deliberate rather than arbitrary, because the name
        // that survives a collision is always the more important one.
        //
        // Scoped to the globe. The flat map's labelling is not broken and is not
        // this routine's to change.
        std::vector<PlacedLabel> taken;
        const bool declutter = (m_view == ViewMode::Globe);
        std::vector<const CountryLabel*> order;
        order.reserve(m_countryLabels->size());
        for (auto& l : *m_countryLabels) order.push_back(&l);
        if (declutter) {
            std::sort(order.begin(), order.end(),
                      [](const CountryLabel* a, const CountryLabel* b) {
                          if (a->fontSize != b->fontSize) return a->fontSize > b->fontSize;
                          return a->name < b->name;   // stable across frames
                      });
        }

        for (const CountryLabel* lp : order) {
            const CountryLabel& label = *lp;
            // Wrap label center to the copy closest to the camera
            Vector2 center = label.center;
            {
                float dx = center.x - m_camera.target.x;
                while (dx > m_mapW * 0.5f) { center.x -= m_mapW; dx -= m_mapW; }
                while (dx < -m_mapW * 0.5f) { center.x += m_mapW; dx += m_mapW; }
            }

            // Whole-label cull, through the seam so the globe drops a country
            // that has turned past the horizon rather than smearing its name
            // across the limb.
            Vector2 sp{};
            if (pixelToScreen(center.x, center.y, sp.x, sp.y) == Facing::Behind) continue;

            // ── The limb ──
            //
            // Ground near the edge of the disc is seen almost edge-on, so a
            // continent's worth of it lands in a few pixels. Names sized for the
            // flat map pile into an illegible band there. Sized and faded by how
            // square-on the ground is, they thin out into the limb instead --
            // which is also what a label on a real curved surface would do.
            // Exactly 1 on the flat map, so nothing there changes.
            const float face = faceCosine(center.x, center.y);
            if (face < 0.30f) continue;
            if (sp.x < -300 || sp.x > m_screenW + 300) continue;
            if (sp.y < -100 || sp.y > m_screenH + 100) continue;

            uint8_t a = alpha;
            {   // fade the last of it out rather than dropping names on a hard edge
                const float f = std::clamp((face - 0.30f) / 0.22f, 0.0f, 1.0f);
                a = (uint8_t)(alpha * f);
                if (a == 0) continue;
            }
            Color col = {255, 255, 255, a};

            const char* text = label.name.c_str();
            int len = (int)strlen(text);
            if (len < 1) continue;

            // Display font size: scales with zoom but never tiny
            float displayFs = (float)label.fontSize * getZoom() * (0.55f + 0.45f * face);
            // Clamped BEFORE the collision test, not after. Measuring a box from
            // the unclamped size gives a 4px box for text that draws at 14, so
            // nothing ever overlaps anything and the whole test quietly passes
            // everything through.
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

            // ── As long as the letters it actually draws ──
            //
            // The run is laid out in MAP units, from advances taken at
            // label.fontSize; the glyphs are then drawn at a SCREEN size with a
            // floor under it so they stay readable. Where a country is small on
            // screen those two disagree and the name prints on top of itself.
            //
            // That is what the smears near the limb were. Not two names
            // colliding -- one name colliding with itself, which no amount of
            // decluttering between names could ever have fixed, because from the
            // outside the two look identical.
            //
            // So the run is stretched until it is at least as long as the glyphs
            // need. A small country's name then reaches past its own borders,
            // which is the right trade: an unreadable name inside the lines
            // tells you nothing at all.
            // How long a run of `w` map units, centred here and along the
            // label's axis, comes out ON SCREEN. Measured as a polyline through
            // the middle rather than end to end: over a long arc the chord
            // between the ends is meaningfully shorter than the text drawn along
            // it, and that difference is what the fit below is chasing.
            auto runLength = [&](float w) -> float {
                const Vector2 h{cosf(label.angle) * w * 0.5f,
                                sinf(label.angle) * w * 0.5f};
                Vector2 e0{}, e1{};
                if (pixelToScreen(center.x - h.x, center.y - h.y, e0.x, e0.y) == Facing::Behind)
                    return 0.0f;
                if (pixelToScreen(center.x + h.x, center.y + h.y, e1.x, e1.y) == Facing::Behind)
                    return 0.0f;
                const float a1 = sqrtf((sp.x - e0.x) * (sp.x - e0.x) + (sp.y - e0.y) * (sp.y - e0.y));
                const float a2 = sqrtf((e1.x - sp.x) * (e1.x - sp.x) + (e1.y - sp.y) * (e1.y - sp.y));
                return a1 + a2;
            };

            // How much screen the glyphs will actually take at the size they
            // will actually be drawn.
            // Constant: the sum of the glyph advances at the size they are
            // drawn. It does NOT move when the run is stretched -- the letters
            // do not grow, the space they are given does.
            const float needPx = totalW * displayFs / (float)label.fontSize;
            float runPx = 0.0f;

            // ── Set along the surface, or not set at all ──
            //
            // The run is laid out in MAP units from advances taken at the
            // label's own size; the glyphs are drawn at a SCREEN size with a
            // floor under it so they stay readable. Where the ground is small or
            // seen edge-on those two disagree and every letter lands on the last
            // one -- which is what the smears near the limb were: not two names
            // colliding, one name colliding with itself.
            //
            // The run is therefore stretched until the letters fit it. The name
            // then reaches past its own borders, which is ordinary cartography
            // and the right trade -- an unreadable name inside the lines says
            // nothing.
            //
            // But a name near the limb needs a stretch of fifty, and fifty times
            // a short run is half the planet: the name would wrap round the
            // world to be legible. That country does not get a name. Dropping it
            // is the honest answer and the one an atlas gives; the alternative
            // tried first was to set it straight across the screen instead,
            // which reads as a caption floating in front of the globe rather
            // than a name written on it.
            float stretch = 1.0f;
            if (m_view == ViewMode::Globe) {
                // ── Fitting the run to the letters, on the surface ──
                //
                // Solved by iteration, not by one multiplication. Scaling the
                // run by need/have assumes the map-to-screen relation is linear
                // along it, and on a sphere it is not: the answer undershoots,
                // sometimes by a lot, and the letters stay piled up. That was
                // the second wrong fix -- and it looked plausible because the
                // arithmetic is right, just about the wrong geometry.
                //
                // Each pass measures what the current run really projects to and
                // corrects from there, damped so a wild first estimate cannot
                // throw the run round the far side in one go.
                const float maxSpan = (float)m_mapW * 0.18f;
                float w = totalW;
                float got = runLength(w);
                for (int pass = 0; pass < 5 && got < needPx * 0.98f; ++pass) {
                    if (got <= 0.5f) { w = 0.0f; break; }     // edge-on: no room
                    w *= std::min(needPx / got, 4.0f);
                    if (w > maxSpan) { w = 0.0f; break; }
                    got = runLength(w);
                }
                // Still short after five passes means the surface here cannot
                // hold the name at a readable size, however far it is spread.
                // That country does not get a name -- which is the answer an
                // atlas gives, and the only one that keeps every name that IS
                // drawn written on the globe rather than floating over it.
                if (w <= 0.0f || got < needPx * 0.80f) continue;
                stretch = w / totalW;
                totalW  = w;
                runPx   = got;
            }

            // Scale curvature proportionally -- to the run as it was LAID OUT,
            // grown by the stretch but not without limit. The bow has to grow
            // with the run or the arc flattens out of shape, and it must not
            // grow with the whole of a fivefold stretch or it throws the name
            // clean off the country it belongs to.
            const float laidW = totalW / stretch;
            float curvScale = (label.span > 0)
                            ? (laidW * std::min(stretch, 2.0f)) / label.span
                            : 1.0f;
            float useCurvature = label.curvature * curvScale;

            Vector2 dir = {cosf(label.angle), sinf(label.angle)};
            Vector2 perp = {-dir.y, dir.x};
            float cursor = -totalW * 0.5f;

            if (declutter) {
                // ── Deciding what overlaps, from the name as it is DRAWN ──
                //
                // The first version bounded each name with an axis-aligned box
                // sized from its character count. That is wrong in three ways at
                // once, and all three bite hardest at the limb: a country's name
                // is set along its OWN axis, it is usually bowed, and the
                // projection compresses a run of map near the edge of the disc
                // to a fraction of the length a character count implies. So the
                // box claimed room the name did not use, missed room it did, and
                // was worst exactly where the crowding is.
                //
                // Sampled instead: the same map positions the glyph loop below
                // walks, through the same projection, and two names collide when
                // their sampled runs pass within a line's height of each other.
                // After the stretch the run occupies roughly the screen length
                // the glyphs asked for.
                const float span = std::max(needPx, runPx);
                const float rad = displayFs * 0.5f;
                // Close enough together that two names cannot cross between
                // consecutive samples without one of them noticing.
                const int NS = std::clamp((int)(span / std::max(rad, 1.0f)) + 2, 3, 24);

                m_labelPts.clear();
                for (int si = 0; si < NS; ++si) {
                    const float pf = (float)si / (float)(NS - 1);
                    const float cur = (pf - 0.5f) * totalW;
                    const float off = sinf(pf * PI) * useCurvature;
                    Vector2 w{center.x + dir.x * cur + perp.x * off,
                              center.y + dir.y * cur + perp.y * off};
                    Vector2 sp2{};
                    if (pixelToScreen(w.x, w.y, sp2.x, sp2.y) == Facing::Behind) continue;
                    m_labelPts.push_back(sp2);
                }
                if (m_labelPts.empty()) continue;

                bool clash = false;
                for (const PlacedLabel& q : taken) {
                    const float lim = (rad + q.radius) * (rad + q.radius);
                    for (size_t ia = 0; ia < m_labelPts.size() && !clash; ++ia) {
                        for (size_t ib = 0; ib < q.pts.size(); ++ib) {
                            const float ddx = m_labelPts[ia].x - q.pts[ib].x;
                            const float ddy = m_labelPts[ia].y - q.pts[ib].y;
                            if (ddx * ddx + ddy * ddy < lim) { clash = true; break; }
                        }
                    }
                    if (clash) break;
                }
                if (clash) continue;
                taken.push_back({m_labelPts, rad});
            }

            for (size_t ci = 0; ci < chars.size(); ci++) {
                // The STRETCHED advance. Widening the run without widening the
                // steps taken across it moves where the name starts and leaves
                // the letters as tightly packed as they were -- which is what
                // the last two attempts at this actually did, and why the smears
                // survived a fix that measured correct every time: the run was
                // the right length and nothing walked along it.
                float cw = chars[ci].advance * stretch;
                float charCenter = cursor + cw * 0.5f;
                float p = (charCenter + totalW * 0.5f) / totalW;

                float curvatureOffset = sinf(p * PI) * useCurvature;
                Vector2 pos = {
                    center.x + dir.x * cursor + perp.x * curvatureOffset,
                    center.y + dir.y * cursor + perp.y * curvatureOffset
                };

                // ── Orientation, taken from the PROJECTION rather than from
                // the map ──
                //
                // In map space a label's angle is a constant. On a sphere the
                // same line of text runs uphill at one end of a country and
                // downhill at the other, and a fixed angle makes the name slide
                // off the surface it is naming. So the character's direction is
                // measured on SCREEN: project the glyph and a point just along
                // the text from it, and take the angle between them. On the flat
                // map the projection is affine and this reproduces exactly what
                // the constant angle gave.
                const float tangent = atan2f(cosf(p * PI) * PI * useCurvature, totalW);
                const float aheadA = label.angle + tangent;
                const Vector2 ahead = {pos.x + cosf(aheadA) * 6.0f,
                                       pos.y + sinf(aheadA) * 6.0f};

                Vector2 screenPos{}, screenAhead{};
                if (pixelToScreen(pos.x, pos.y, screenPos.x, screenPos.y) == Facing::Behind) {
                    cursor += cw;
                    if (ci < chars.size() - 1) cursor += spacing;
                    continue;      // this glyph is round the back
                }
                float deg;
                if (pixelToScreen(ahead.x, ahead.y, screenAhead.x, screenAhead.y) == Facing::Front) {
                    deg = atan2f(screenAhead.y - screenPos.y,
                                 screenAhead.x - screenPos.x) * RAD2DEGF;
                } else {
                    deg = aheadA * RAD2DEGF;   // at the very limb, fall back
                }
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
                if (ci < chars.size() - 1) cursor += spacing * stretch;
            }
        }
    }
}

void MapRenderer::draw(const LandSeaMap& landSea, const ProvinceMap& provinces, const CountryMap& countries) {
    if (m_view == ViewMode::Globe) {
        if (!m_globe) m_globe = new GlobeView(m_mapW, m_mapH);
        // WHICH layers are drawn is derived, not announced. Every show/hide
        // toggle changes the stack, and requiring each one to remember to mark
        // the composite stale is a rule that gets broken by the next overlay
        // somebody adds -- the symptom being a globe that quietly shows the
        // previous view's layers. A signature over the stack cannot be
        // forgotten. Pixel changes still mark themselves: see above.
        unsigned long long sig = 1469598103934665603ULL;
        // The wanted resolution is part of the signature, so zooming in rebuilds
        // the composite at the size the new view needs without anyone having to
        // remember to say so.
        {
            const SurfaceWindow sw = surfaceWindow();
            auto q = [](float f) { return (unsigned long long)(long long)(f * 100000.0f); };
            sig = (sig ^ q(sw.u0)) * 1099511628211ULL;
            sig = (sig ^ q(sw.v0)) * 1099511628211ULL;
            sig = (sig ^ q(sw.du)) * 1099511628211ULL;
            sig = (sig ^ q(sw.dv)) * 1099511628211ULL;
            sig = (sig ^ (unsigned long long)sw.texW) * 1099511628211ULL;
        }
        for (const Layer& l : layerStack(landSea)) {
            sig = (sig ^ l.tex.id) * 1099511628211ULL;
            sig = (sig ^ ColorToInt(l.tint)) * 1099511628211ULL;
        }
        if (sig != m_surfaceSig) { m_surfaceSig = sig; m_surfaceDirty = true; }
        if (m_surfaceDirty || m_surface.id == 0) buildSurface(landSea);
        m_globe->setSurface(m_surface.texture);
        // smoothstep: zero slope at both ends.
        const float e = m_morph * m_morph * (3.0f - 2.0f * m_morph);
        m_globe->setMorph(e);
        m_globe->draw(m_screenW, m_screenH);
        // Labels ride on the sphere. Suppressed mid-unroll along with every
        // other overlay, by the guard in pixelToScreen.
        drawCountryNames();
        return;
    }

    BeginMode2D(m_camera);

    float viewW = m_screenW / m_camera.zoom;
    float left = m_camera.target.x - viewW * 0.5f;
    float right = m_camera.target.x + viewW * 0.5f;
    int tileStart = static_cast<int>(std::floor(left / m_mapW));
    int tileEnd = static_cast<int>(std::ceil(right / m_mapW));

    for (const Layer& l : layerStack(landSea)) {
        if (l.tex.id == 0) continue;
        for (int tx = tileStart; tx < tileEnd; ++tx) {
            DrawTexture(l.tex, tx * m_mapW, 0, l.tint);
        }
    }

    EndMode2D();

    drawCountryNames();

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
    if (m_view == ViewMode::Globe && m_globe) {
        // A click that misses the planet lands on empty space. Reported as
        // province 0 by leaving the coordinates outside the raster, which is
        // what callers already treat as "nothing there".
        // Same rule as pixelToScreen: while the planet is unrolling, a click
        // resolves to nowhere rather than to a province that is not under the
        // cursor.
        if (m_morphing || !m_globe->screenToPixel(sx, sy, m_screenW, m_screenH, px, py)) {
            px = -1; py = -1;
        }
        return;
    }
    Vector2 screenPos = { sx, sy };
    Vector2 worldPos = GetScreenToWorld2D(screenPos, m_camera);
    py = static_cast<int>(worldPos.y);
    px = static_cast<int>(worldPos.x);
    while (px < 0) px += m_mapW;
    while (px >= m_mapW) px -= m_mapW;
}

bool MapRenderer::globeCentreOnScreen(float& sx, float& sy) const {
    if (m_view != ViewMode::Globe || !m_globe || m_morphing) return false;
    const float u = (m_globe->longitude() + PI) / (2.0f * PI);
    const float v = (PI * 0.5f - m_globe->latitude()) / PI;
    return pixelToScreen(u * (float)m_mapW, v * (float)m_mapH, sx, sy) == Facing::Front;
}

MapRenderer::Facing MapRenderer::shellPoint(Vector2 from, Vector2 to, float t,
                                            float& sx, float& sy) const {
    if (m_view == ViewMode::Globe && m_globe) {
        if (m_morphing) return Facing::Behind;    // same rule as every overlay
        return m_globe->arcPoint(from.x, from.y, to.x, to.y, t,
                                 m_screenW, m_screenH, sx, sy)
             ? Facing::Front : Facing::Behind;
    }
    // Flat: the short way round, and a straight line, exactly as before.
    //
    // Anchored on the SOURCE's tile copy and stepped from there, rather than
    // each point choosing its own nearest copy. That is what the old arrow code
    // did -- it corrected the destination back onto the source's copy by hand --
    // and a shot across the antimeridian is the case where the two disagree.
    float dx = to.x - from.x;
    while (dx >  (float)m_mapW * 0.5f) dx -= (float)m_mapW;
    while (dx < -(float)m_mapW * 0.5f) dx += (float)m_mapW;
    const float anchored = from.x + roundf((m_camera.target.x - from.x) / (float)m_mapW)
                                    * (float)m_mapW;
    const Vector2 v = GetWorldToScreen2D({anchored + dx * t,
                                          from.y + (to.y - from.y) * t}, m_camera);
    sx = v.x;
    sy = v.y;
    return Facing::Front;
}

MapRenderer::Facing MapRenderer::pixelToScreen(float px, float py,
                                              float& sx, float& sy) const {
    if (m_view == ViewMode::Globe && m_globe) {
        // Mid-unroll every point is between its two homes and neither answer is
        // right, so nothing is placed at all until the planet settles.
        if (m_morphing) return Facing::Behind;
        if (!m_globe->pixelToScreen(px, py, m_screenW, m_screenH, sx, sy))
            return Facing::Behind;
        return Facing::Front;
    }
    // The map wraps horizontally: project the tile copy nearest the camera
    float wrapped = px + roundf((m_camera.target.x - px) / (float)m_mapW) * (float)m_mapW;
    Vector2 v = GetWorldToScreen2D({wrapped, py}, m_camera);
    sx = v.x;
    sy = v.y;
    // The flat map has no hidden half, so there is nothing here to hide behind.
    return Facing::Front;
}

float MapRenderer::getZoom() const {
    if (m_view != ViewMode::Globe || !m_globe) return m_camera.zoom;
    // Pixels the planet's radius covers on screen, from the perspective
    // projection, divided by the map pixels one radius represents. That makes
    // the number mean the same thing in both views -- which is the only reason
    // the call sites can stay as they are.
    const float halfFovTan = tanf(45.0f * 0.5f * DEG2RAD);
    const float pixelsPerRadius = (m_screenH * 0.5f) / (halfFovTan * m_globe->distance());
    const float mapPixelsPerRadius = (float)m_mapW / (2.0f * PI);
    return pixelsPerRadius / mapPixelsPerRadius;
}

void MapRenderer::setViewMode(ViewMode m) {
    if (m == m_view) return;

    // Carry the view across, BOTH WAYS. Switching is a change of projection,
    // not of place: whatever ground was in front of you stays in front of you.
    // Done here rather than by the caller so there is one definition of what
    // "the same place" means, and so the round trip actually returns you where
    // you started instead of drifting a little each time.
    if (m == ViewMode::Globe) {
        if (!m_globe) { m_globe = new GlobeView(m_mapW, m_mapH); if (m_haveSky) m_globe->setSky(m_sky); }
        m_globe->lookAt(m_camera.target.x, m_camera.target.y);
        anchorSheetToFlat();
        m_surfaceDirty = true;
        m_view = ViewMode::Globe;
        m_flatPending = false;
        // Wound back to flat explicitly. Without this the very first switch of a
        // session finds m_morph already at 1 -- its initial value, because the
        // globe IS a sphere whenever it is drawn normally -- and the animation
        // completes on the frame it starts, so the first F7 a player presses is
        // the only one that cuts.
        m_morph = 0.0f;
        m_morphTo = 1.0f;
        m_morphing = true;
    } else if (m_globe) {
        // Going the other way the view does NOT flip yet: the sphere has to
        // unroll first, and only the 3D path can draw that. m_flatPending marks
        // the intent; the flip happens in finishTransition() once it is flat.
        //
        // The destination framing is settled HERE, before the animation starts,
        // and the sheet anchored on it -- otherwise the unroll ends on one
        // framing and the 2D view takes over with another, which is the same
        // jump at the other end. Zoom comes across as well as position: matching
        // half the framing is matching none of it.
        const float globeZoom = getZoom();     // still the globe's, m_view unchanged
        const float u = (m_globe->longitude() + PI) / (2.0f * PI);
        const float v = (PI * 0.5f - m_globe->latitude()) / PI;
        m_camera.target = { u * (float)m_mapW, v * (float)m_mapH };
        m_camera.zoom   = std::clamp(globeZoom, m_minZoom, m_maxZoom);
        anchorSheetToFlat();
        m_flatPending = true;
        m_morph = 1.0f;      // the sphere it is unrolling FROM
        m_morphTo = 0.0f;
        m_morphing = true;
    } else {
        m_view = m;
    }
}

// Frame a 3D camera onto the flat sheet so it reproduces, as closely as a
// perspective camera can, exactly what the 2D view is showing right now. This
// is what makes the unroll begin where the eye already is: without it the
// animation opens on a whole-world shot and the first frame is a jump.
void MapRenderer::anchorSheetToFlat() {
    if (!m_globe) return;
    // Wrapped first: the flat map repeats horizontally and its camera target is
    // free to wander outside the raster.
    float tpx = m_camera.target.x;
    while (tpx < 0.0f) tpx += (float)m_mapW;
    while (tpx >= (float)m_mapW) tpx -= (float)m_mapW;

    Vector3 eye{}, at{};
    globe::sheetAnchor(tpx, m_camera.target.y, m_camera.zoom,
                       m_mapW, m_mapH, m_screenW, m_screenH, eye, at);
    m_globe->setSheetAnchor(eye, at);
}

void MapRenderer::snapViewMode(ViewMode m) {
    setViewMode(m);
    if (!m_morphing) return;
    m_morph = m_morphTo;
    m_morphing = false;
    finishTransition();
}

void MapRenderer::finishTransition() {
    if (!m_flatPending) return;
    // The camera was carried across when the switch was ASKED FOR, not here --
    // the sheet has to be framed on the destination before the animation runs,
    // or the last frame of the unroll and the first frame of the flat map are
    // two different pictures.
    m_view = ViewMode::Flat;
    m_flatPending = false;
    // The vertical clamp in update() pulls the target inside the map edges on
    // the next frame, which is where that rule already lives.
}

void MapRenderer::setSky(const GlobeViewSky& sky) {
    m_sky = sky;
    m_haveSky = true;
    if (m_globe) m_globe->setSky(m_sky);
}

void MapRenderer::orbitGlobe(float dx, float dy) {
    if (m_view == ViewMode::Globe && m_globe) m_globe->orbit(dx, dy);
}

void MapRenderer::zoomGlobe(float amount) {
    if (m_view == ViewMode::Globe && m_globe) m_globe->zoom(amount);
}
