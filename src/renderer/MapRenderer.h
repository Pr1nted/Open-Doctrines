#pragma once
#include "raylib.h"
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
    void setPopulationTexture(Texture2D tex);
    void updatePopulationTexture(const void* data);
    void setResourceTexture(Texture2D tex);
    void updateResourceTexture(const void* data);
    void setShowClaims(bool on) { m_showClaims = on; }
    void setShowPopulation(bool on) { m_showPopulation = on; }
    void setShowRelations(bool on) { m_showRelations = on; }
    void setShowIndustry(bool on) { m_showIndustry = on; }
    void setClaimsTexture(Texture2D tex);
    void updateClaimsTexture(const void* data);
    void setShowResource(int idx) { m_showResource = idx; }
    int  getShowResource() const { return m_showResource; }
    float getZoom() const { return m_camera.zoom; }
    void computeBorderTexture(const Image& provImage);
    void screenToPixel(float sx, float sy, int& px, int& py) const;
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
    Vector2 getMouse() const;
    int m_screenW, m_screenH;
    float m_dpiScale = 1.0f;
    int m_mapW, m_mapH;
    bool m_isDragging = false;
    bool m_wasDragged = false;
    bool m_paused = false;
    bool m_debugMode = false;
    int m_selectedProvinceId = 0;
    Texture2D m_selectionTex{};
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
    bool m_showPopulation = false;
    bool m_showRelations = false;
    bool m_showClaims = false;
    bool m_showIndustry = false;
    int m_showResource = -1;  // -1 = off, 0-3 = resource index
    const std::vector<struct CountryLabel>* m_countryLabels = nullptr;
    Font m_fallbackFont{};
};
