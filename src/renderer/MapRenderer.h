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
    // ── Province-coloured overlays ──
    //
    // Population, relations, resources and claims colour whole provinces, so
    // they are drawn from one texture of province ids and a colour table per
    // overlay (one entry per id) instead of a full-map image each. Four of
    // those images were 128 MB of CPU memory and 128 MB of texture apiece, and
    // every change -- even hovering a different country in the population
    // view -- rewrote and re-uploaded one whole. A table is 256 KB.
    enum class Overlay { Population = 0, Resource = 1, Claims = 2, Count_ };
    static constexpr int kOverlayEntries = 65536;   ///< one per 16-bit province id
    /// The province ids, read from the province map (id = RGB colour). Built
    /// once per map, the first time an overlay is needed.
    void setProvinceIndex(const Image& provinces);
    bool hasProvinceIndex() const { return m_provinceIndexTex.id > 0; }
    /// base[id] colours province id; stripe[id], where its alpha is non-zero,
    /// replaces it on the claims hatching ((x + y) % 10 < 4). Both at most
    /// kOverlayEntries long; missing entries are transparent.
    void setOverlayColours(Overlay which, const std::vector<Color>& base,
                           const std::vector<Color>* stripe = nullptr);
    /// Draw nothing for this overlay until its colours are set again.
    void clearOverlay(Overlay which);

    // ── The game's political layer ──
    //
    // Painted on the GPU into a render target from the province ids, a table
    // of province owners, a table of colours and the distance-to-border field,
    // whenever one of those changes. It was a CPU image uploaded every turn,
    // which cost 128 MB of CPU memory and -- because the Mac's OpenGL keeps
    // copies of a texture that is updated after it is made -- 384 MB of GPU
    // memory. The map editor still sets an image with setPoliticalTexture().
    //
    // owners[id]: the owning country of province id (0 for none), compared
    //             between neighbours to find the 1px border line.
    // rows[id]:   which row of colours paints province id. One row per
    //             distinct owner, row 0 for sea and unowned land: owner ids
    //             are not a dense range (a special id is stored as 65534).
    // colours:    one row of kPoliticalColumns per owner. Column d (0..63) is the colour at
    //             distance d from a border; column 64 + d the same on a
    //             border pixel (the 1px dark line). Exact CPU values: the
    //             shader only looks them up.
    // distance:   one byte per map pixel, the distance field, clamped to 63.
    static constexpr int kPoliticalColumns = 128;
    void setPoliticalOwners(const std::vector<uint16_t>& owners, const std::vector<uint16_t>& rows);
    void setPoliticalColours(const std::vector<Color>& colours, int rows);
    void setBorderDistance(const std::vector<uint8_t>& distance, int w, int h);
    /// The painted layer's texture, for panels that draw the map small. Owned
    /// by the renderer; its id does not change once made.
    Texture2D gamePoliticalTexture() const { return m_politicalTarget.texture; }
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
    /// A name already placed this frame, as the run of screen points it
    /// occupies. A run rather than a box because names are rotated and bowed.
    struct PlacedLabel { std::vector<Vector2> pts; float radius; };
    mutable std::vector<Vector2> m_labelPts;   ///< scratch, kept to avoid a per-label allocation

    void drawCountryNames();
    void anchorSheetToFlat();
    /// The part of the map the composite needs to hold, and how big a target to
    /// hold it in. `full` means the whole map, which is what the far view wants.
    struct SurfaceWindow {
        float u0 = 0.0f, v0 = 0.0f, du = 1.0f, dv = 1.0f;
        int   texW = 1024, texH = 512;
        bool  full = true;
    };
    SurfaceWindow surfaceWindow() const;
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
    /// How dark the unlit half goes, and how much of that the political
    /// colouring is spared. Carried by the map, like the rest of the sky.
    void setNight(const GlobeView::Night& n);

    void orbitGlobe(float dx, float dy);
    void zoomGlobe(float amount);
    void setGlobeDistance(float d);
    float distanceForZoom(float zoom) const;

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

    /// Sample the flight of a shell from one map point to another, `t` in [0,1].
    /// On the globe it arcs over the surface; on the flat map it is the straight
    /// line it has always been, because a flat map has no above to rise into.
    Facing shellPoint(Vector2 from, Vector2 to, float t, float& sx, float& sy) const;

    /// Where the point directly under the camera lands on screen -- the middle
    /// of the disc. False on the flat map, which has no such point.
    bool globeCentreOnScreen(float& sx, float& sy) const;
    void setSelectedProvince(int id) { m_selectedProvinceId = id; }
    int getSelectedProvinceId() const { return m_selectedProvinceId; }
    void setPaused(bool paused) { m_paused = paused; }
    void setDebugMode(bool on) { m_debugMode = on; }
    void setCountryFlags(const std::unordered_map<int, Texture2D>* flags) { m_countryFlags = flags; }

    void flyTo(float x, float y, float zoom, float speed = 2.0f);
    /// Put the camera somewhere with no travel. flyTo is what the game uses;
    /// this is for harnesses that need two shots taken from the same framing.
    void snapTo(float x, float y, float zoom);
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
    /// The 2D camera, with a zoom that means what the caller thinks it means.
    ///
    /// Thirty-nine places size markers, fonts and bars from `getCamera().zoom`,
    /// asking a reasonable question -- how big is a map pixel on screen -- and
    /// on the globe getting the answer for a view that is not on screen: the
    /// flat camera is frozen at wherever it was left when you switched. So the
    /// zoom is answered for the view that IS up. Target and offset stay the flat
    /// camera's, because the only code that uses them is flat-only anyway.
    const Camera2D& getCamera() const;

private:
    void buildSelectionGlow();

    Texture2D m_borderTex{};
    Texture2D m_politicalTex{};
    Texture2D m_claimsTex{};
    Texture2D m_provinceIndexTex{};
    struct OverlaySlot { Texture2D base{}, stripe{}; bool active = false; };
    OverlaySlot m_overlays[(int)Overlay::Count_];
    Texture2D m_noStripe{};          ///< 1x1 transparent, bound when a slot has none
    RenderTexture2D m_overlayTarget{};   ///< the shown overlay, painted from its table
    RenderTexture2D m_politicalTarget{}; ///< the game's political layer, painted
    Texture2D m_politicalOwners{}, m_politicalColours{}, m_borderDistance{};
    int m_politicalRows = 0;
    bool m_politicalStale = false;
    Shader m_politicalShader{};
    bool m_politicalShaderTried = false;
    int m_locPolOwners = -1, m_locPolColours = -1, m_locPolDistance = -1, m_locPolMapSize = -1, m_locPolRows = -1;
    void preparePolitical();
    RenderTexture2D makeTarget(int w, int h);
    int m_overlayPainted = -1;           ///< which Overlay m_overlayTarget holds
    Shader m_overlayShader{};
    bool m_overlayShaderTried = false;
    int m_locOverlayBase = -1, m_locOverlayStripe = -1, m_locOverlayMapSize = -1;
    Camera2D m_camera{};
    mutable Camera2D m_viewCamera{};

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
    GlobeView::Night m_night{};
    bool m_haveNight = false;
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
    /// overlay >= 0: tex is the province index, drawn through the overlay
    /// shader with that Overlay's colour table.
    /// scenery: the world under everything else (the land/sea layer). The
    /// globe dims scenery at night and keeps what is drawn on it readable.
    struct Layer { Texture2D tex; Color tint; bool scenery = false; };
    bool overlayReady(Overlay which) const;
    /// Which overlay the current view shows, or -1.
    int wantedOverlay() const;
    /// Paint that overlay's table into m_overlayTarget if it is not there yet.
    void prepareOverlay();
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
