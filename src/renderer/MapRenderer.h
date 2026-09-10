#pragma once
#include "raylib.h"
#include "GlobeView.h"
using GlobeViewSky = GlobeView::Sky;
#include "../map/LandSeaMap.h"
#include "../map/ProvinceMap.h"
#include "../map/CountryMap.h"
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <string>

struct CountryLabel {
    std::string name;
    Vector2 center;
    float angle = 0.0f;
    int fontSize = 14;
    float span = 0.0f;
    float curvature = 0.0f;
};

class MapRenderer {
public:
    MapRenderer(int screenW, int screenH, int mapW, int mapH);
    ~MapRenderer();

    void update(float dt);
    void draw(const LandSeaMap& landSea, const ProvinceMap& provinces, const CountryMap& countries);

    void setPoliticalTexture(Texture2D tex);
    void updatePoliticalTexture(const void* data);
    void updatePoliticalTextureRec(const void* rectData, int x, int y, int w, int h);
    // Recompute border/halo pixels in a map-space rect from raw province pixels
    // and patch just that region of the border texture (live painting feedback).
    void updateBorderRegion(const Color* provPixels, int mapW, int mapH,
                            int rx, int ry, int rw, int rh);
    void setPopulationTexture(Texture2D tex);
    void updatePopulationTexture(const void* data);
    void setResourceTexture(Texture2D tex);
    void updateResourceTexture(const void* data);
    void setShowClaims(bool on) { m_showClaims = on; }
    bool getShowClaims() const { return m_showClaims; }
    void setShowPolitical(bool on) { m_showPolitical = on; }
    // Editor overlays: generic overlay texture (buildings view) + a pulsing
    // selection-highlight texture positioned in map space. Both take ownership.
    void setEditorOverlay(Texture2D tex);
    void setShowEditorOverlay(bool on) { m_showEditorOverlay = on; }
    bool getShowEditorOverlay() const { return m_showEditorOverlay; }
    void setHighlight(Texture2D tex, int x, int y);
    void clearHighlight();
    void setShowPopulation(bool on) { m_showPopulation = on; }
    void setShowRelations(bool on) { m_showRelations = on; }
    void setShowIndustry(bool on) { m_showIndustry = on; }
    void setClaimsTexture(Texture2D tex);
    void updateClaimsTexture(const void* data);
    void updateClaimsTextureRec(const void* rectData, int x, int y, int w, int h);
    bool hasClaimsTexture() const { return m_claimsTex.id > 0; }
    void setShowResource(int idx) { m_showResource = idx; }
    int  getShowResource() const { return m_showResource; }
    /**
     * Screen pixels per map pixel, in whichever view is up.
     *
     * The flat camera's own zoom on the flat map. On the globe it is DERIVED
     * from the orbit distance, because the 2D camera is frozen while the globe
     * is up -- and seven call sites use this to size markers and pick levels of
     * detail. Left alone, a boat drawn on the globe kept whatever size it had
     * when you last looked at the flat map, however far in you zoomed.
     */
    float getZoom() const;
    // Fully-zoomed-out level, i.e. the whole map on screen. Depends on the map
    // and window size, so anything that wants "how far out are we, really"
    // has to measure against this rather than against a fixed number.
    float getMinZoom() const { return m_minZoom; }
    void computeBorderTexture(const Image& provImage);
    /**
     * Which view the map is drawn in.
     *
     * The globe is not a different map -- it is the same textures on a sphere.
     * Everything above this line (overlays, selection, borders, painting) is
     * unchanged by the switch, because all of it composites into one surface
     * before either view draws anything.
     */
    enum class ViewMode { Flat, Globe };
    void setViewMode(ViewMode m);
    /// Put the view somewhere with no animation. For harnesses that need a known
    /// starting state; setViewMode is what the game uses.
    void snapViewMode(ViewMode m);
    void drawCountryNames();
    /// 1 on the flat map, and on the globe how square-on the ground is.
    float faceCosine(float px, float py) const;
    void finishTransition();
    ViewMode viewMode() const { return m_view; }
    /// True while the planet is mid-unroll. Overlays sit this out: see the note
    /// on m_morph.
    bool inTransition() const { return m_morphing; }

    /// Turn and zoom the globe. No-ops in the flat view, so callers that handle
    /// input do not need to branch on the mode.
    /// The sky this map carries. Kept here rather than on the globe so it
    /// survives the globe not existing yet -- a map loads long before anyone
    /// presses F7.
    void setSky(const GlobeViewSky& sky);

    void orbitGlobe(float dx, float dy);
    void zoomGlobe(float amount);

    void screenToPixel(float sx, float sy, int& px, int& py) const;

    /**
     * Whether a map point is in front of the camera or behind the planet.
     *
     * The flat map has no hidden half and always answers Front. The globe does,
     * and it is answered HERE rather than at each call site: a marker whose
     * province has turned out of view must not draw, and fifteen call sites
     * working that out for themselves is fifteen chances to get it subtly wrong.
     */
    enum class Facing { Front, Behind };
    Facing pixelToScreen(float px, float py, float& sx, float& sy) const;
    void setSelectedProvince(int id) { m_selectedProvinceId = id; }
    int getSelectedProvinceId() const { return m_selectedProvinceId; }
    void setPaused(bool paused) { m_paused = paused; }
    void setDebugMode(bool on) { m_debugMode = on; }
    void setCountryFlags(const std::unordered_map<int, Texture2D>* flags) { m_countryFlags = flags; }

    void flyTo(float x, float y, float zoom, float speed = 2.0f);
    void addZoom(float amount);
    void resize(int screenW, int screenH);
    void setMaxZoom(float zoom) { m_maxZoom = zoom; }
    void setDpiScale(float scale) { m_dpiScale = scale; }
    // Single-scan build: computes glow map AND province centers/radii
    void buildProvinceData(const ProvinceMap& provinces,
                           std::unordered_map<int, Vector2>& centers_out,
                           std::unordered_map<int, float>& radii_out);
    // Glow-only rebuild (when borders reload but provinces don't change)
    void rebuildGlowMap(const ProvinceMap& provinces);
    void rebuildSelectionGlow() { buildSelectionGlow(); }
    void setBottomPanelRect(Rectangle r) { m_bottomPanelRect = r; }
    void setSkipClickRect(Rectangle r) { m_skipClickRect = r; }
    void setProvincePanelRect(Rectangle r) { m_provincePanelRect = r; }
    const Rectangle& getProvincePanelRect() const { return m_provincePanelRect; }

    /**
     * Whether a point is over UI that eats map clicks rather than the map.
     *
     * The rule this class already applies to its own click-to-select, asked
     * out loud so callers who interpret a click themselves apply the SAME one.
     * A caller that forgets it reads a click on the army panel as a click on
     * whatever province happens to lie behind the panel.
     */
    bool pointOverPanels(Vector2 p) const {
        return (m_bottomPanelRect.height   > 0 && CheckCollisionPointRec(p, m_bottomPanelRect)) ||
               (m_skipClickRect.height     > 0 && CheckCollisionPointRec(p, m_skipClickRect)) ||
               (m_provincePanelRect.height > 0 && CheckCollisionPointRec(p, m_provincePanelRect));
    }
    /**
     * Light up a set of provinces, for an action being composed but not yet
     * taken -- the bulk upgrade paint.
     *
     * Rebuilt whole on every change rather than diffed. A stroke adds a few
     * provinces a second at most, and this is one image the GPU uploads once;
     * tracking incremental edits would be more code to be wrong about.
     *
     * Only the RGB of `tint` is used. The alpha comes from each province's own
     * edge falloff, the same as the single selection's glow -- multiplying the
     * two instead made a large selection nearly invisible.
     */
    void setBulkSelection(const std::vector<int>& provinceIds, Color tint);
    void clearBulkSelection();

    /**
     * The province under the cursor as of the last frame drawn, or 0.
     *
     * Zero also means "over open sea" and "over a panel is irrelevant" -- this
     * is what is under the pointer, not what may be acted on. Callers that care
     * about panels ask `pointOverPanels` as well.
     */
    int hoveredProvinceId() const { return m_hoveredProvinceId; }

    void setShowCountryNames(bool on) { m_showCountryNames = on; }
    void setCountryLabels(const std::vector<struct CountryLabel>* labels) { m_countryLabels = labels; }
    void setFallbackFont(Font font) { m_fallbackFont = font; }
    void drawSubregion(int sx, int sy, int sw, int sh,
                       float worldX, float worldY, float zoom,
                       const LandSeaMap& landSea, const ProvinceMap& provinces, const CountryMap& countries);
    void setBlockLeftPan(bool v) { m_blockLeftPan = v; }
    void setWasDragged(bool v) { m_wasDragged = v; }
    bool getWasDragged() const { return m_wasDragged; }
    const Vector2& getCameraTarget() const { return m_camera.target; }
    const Camera2D& getCamera() const { return m_camera; }

private:
    void buildSelectionGlow();

    Texture2D m_borderTex{};
    Texture2D m_politicalTex{};
    Texture2D m_populationTex{};
    Texture2D m_resourceTex{};
    Texture2D m_claimsTex{};
    Camera2D m_camera{};

    // ── The globe ──
    //
    // The surface is the whole layer stack composited once into a texture, so
    // the sphere samples exactly what the flat view draws -- claims, districts,
    // population, borders, selection and all. Rebuilt only when something in
    // the stack changes, because compositing 8192x4096 every frame is not free
    // on a phone and the map does not change every frame.
    ViewMode m_view = ViewMode::Flat;
    // ── The unroll ──
    //
    // Switching projection is a change of how the world is drawn, and cutting
    // between the two leaves you to work out for yourself that the province you
    // were looking at is the one now over there. The morph answers that
    // question by showing it, so nothing has to be re-found by hand.
    //
    // m_view flips to Globe at the START of the animation in BOTH directions --
    // the flat renderer cannot draw a half-sphere, so the 3D path owns the whole
    // transition and the flat view is only restored once the sheet is flat
    // again. Overlays (markers, labels, picking) sit the animation out entirely:
    // mid-morph a point on the map is at neither of the two positions those
    // paths know how to compute, and half a second of no counters reads as part
    // of the move where half a second of counters in the wrong place reads as a
    // bug.
    bool  m_morphing = false;
    float m_morph = 1.0f;      // 0 = flat sheet, 1 = sphere
    float m_morphTo = 1.0f;
    bool  m_flatPending = false;
    GlobeViewSky m_sky{};
    bool m_haveSky = false;
    GlobeView* m_globe = nullptr;
    RenderTexture2D m_surface{};
    bool m_surfaceDirty = true;
    unsigned long long m_surfaceSig = 0;  ///< which layers were last composited
    void buildSurface(const LandSeaMap& landSea);

    /**
     * The layer stack, in order, as textures and tints.
     *
     * ONE definition, used by both views: the flat map tiles each entry
     * horizontally, the globe composites them into a single surface. Written
     * this way because the alternative -- each view listing the layers itself --
     * drifts the moment somebody adds an overlay and updates only the view they
     * were looking at, and the symptom is a layer that exists in one view and
     * not the other.
     */
    struct Layer { Texture2D tex; Color tint; };
    std::vector<Layer> layerStack(const LandSeaMap& landSea) const;
    Vector2 getMouse() const;
    int m_screenW, m_screenH;
    float m_dpiScale = 1.0f;
    int m_mapW, m_mapH;
    bool m_isDragging = false;
    bool m_wasDragged = false;
    bool m_paused = false;
    bool m_debugMode = false;
    int m_selectedProvinceId = 0;
    int m_hoveredProvinceId = 0;
    Texture2D m_selectionTex{};
    Texture2D m_bulkTex{};
    std::vector<uint8_t> m_borderPixels;

    // Precomputed glow pixels per province (built once at init)
    std::unordered_map<int, std::vector<std::pair<int, uint8_t>>> m_provinceGlow;

    // Fly-to animation (exponential chase: always moves toward target, smooth redirects)
    Vector2 m_flyTarget{};
    float m_flyZoom = 1.0f;
    float m_flySpeed = 2.0f;
    float m_minZoom = 0.2f;
    float m_maxZoom = 5.0f;
    bool m_flying = false;

    const std::unordered_map<int, Texture2D>* m_countryFlags = nullptr;
    Rectangle m_bottomPanelRect{};
    Rectangle m_skipClickRect{};
    Rectangle m_provincePanelRect{};
    bool m_showCountryNames = false;
    bool m_blockLeftPan = false;
    bool m_showPolitical = true;
    Texture2D m_editorOverlayTex{};
    bool m_showEditorOverlay = false;
    Texture2D m_highlightTex{};
    int m_highlightX = 0, m_highlightY = 0;
    bool m_showPopulation = false;
    bool m_showRelations = false;
    bool m_showClaims = false;
    bool m_showIndustry = false;
    int m_showResource = -1;  // -1 = off, 0-3 = resource index
    const std::vector<struct CountryLabel>* m_countryLabels = nullptr;
    Font m_fallbackFont{};
};
