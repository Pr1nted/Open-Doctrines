#pragma once
#include "GameStructs.h"
#include "map/LandSeaMap.h"
#include "map/ProvinceMap.h"
#include "map/CountryMap.h"
#include "renderer/MapRenderer.h"
#include "raylib.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>

class MapEditor {
public:
    enum EditMode {
        MODE_LANDMASS, MODE_PROVINCES, MODE_COUNTRIES, MODE_RESOURCES,
        MODE_TROOPS, MODE_NAVY, MODE_RELATIONS, MODE_SCRIPTS,
        MODE_GENERATOR, MODE_METADATA
    };
    enum Tool { TOOL_BRUSH, TOOL_RECT, TOOL_FILL, TOOL_ERASE };
    enum ProjectState { PROJ_STARTUP, PROJ_OPEN, PROJ_CREATE, PROJ_EDITING };

    struct GeneratorParams {
        int seed = 12345;
        float landCoverage = 0.35f;
        int numContinents = 5;
        int numCountries = 50;
        float jaggedness = 0.3f;
        float provinceDensity = 1.0f; // 0.2 = large provinces, 5.0 = tiny
    };

    struct PendingProject { std::string name, path; long long lastModified = 0; };

    MapEditor();
    ~MapEditor();

    void init(int screenW, int screenH, const std::string& dataDir);
    void resize(int screenW, int screenH);
    void update(float dt);
    void draw();

    bool hasUnsavedChanges() const { return m_dirty; }
    void setDirty(bool d) { m_dirty = d; }
    void trackChange() { m_dirty = true; }
    bool isInProjectDialog() const { return m_projectState != PROJ_EDITING; }

    std::string getMapName() const { return m_mapName; }
    void setMapName(const std::string& n) { m_mapName = n; m_dirty = true; }

private:
    // Map display
    static const int MAP_W = 8192;
    static const int MAP_H = 4096;
    MapRenderer* m_renderer = nullptr;
    LandSeaMap m_editLandSea;
    ProvinceMap m_editProvinces;
    CountryMap m_editCountries;

    std::vector<Color> m_pixels;

    void initBlankMap();
    void applyBrush(int cx, int cy, bool land);
    void applyRect(int x1, int y1, int x2, int y2, bool land);
    void applyFloodFill(int sx, int sy, bool land);

    // Generator
    GeneratorParams m_genParams;
    void generateMap(const GeneratorParams& p);

    // Procedural province + country + game data generation
    bool m_hasProvinces = false;
    bool m_hasGameData = false;
    std::vector<Color> m_provincePixels;
    std::vector<Color> m_politicalPixels;
    std::string m_provinceJson;
    std::string m_countryJson;
    std::string m_populationJson;
    std::string m_resourcesJson;
    void generateProvincesCountries();
    void generateGameData();
    void exportODMap();

    // Frame-deferred generation (0=idle, 1=awaiting overlay, 2=running)
    int m_genPending = 0;
    std::string m_genStatus;

    // Project flow
    ProjectState m_projectState = PROJ_STARTUP;
    int m_projChoice = 0;
    int m_projScroll = 0;
    std::vector<std::string> m_projFiles;
    std::string m_dataDir;

    void drawStartupDialog();
    void drawOpenDialog();
    void drawCreateDialog();

    // UI
    EditMode m_mode = MODE_LANDMASS;
    Tool m_tool = TOOL_BRUSH;
    int m_brushSize = 20;
    bool m_drawAsLand = true;
    bool m_isPanMode = false;
    bool m_editingSeed = false;
    std::string m_seedText;
    bool m_editingCountryCount = false;
    std::string m_countryCountText;
    std::string m_warningMsg;
    float m_warningTimer = 0;
    std::string m_mapName = "New Map";
    bool m_dirty = false;

    int m_screenW = 1600, m_screenH = 900;
    int m_panelW = 280, m_toolbarH = 46, m_bottomH = 50;
    int m_canvasX, m_canvasY, m_canvasW, m_canvasH;

    int m_selectedProvince = -1;
    int m_selectedCountry = -1;
    int m_countryScroll = 0;
    bool m_editingCountryName = false;
    std::string m_editingNameText;
    std::string m_flagUploadPath;
    int m_renameCountryId = -1;
    int m_flagPreviewCountry = -1;
    Texture2D m_flagPreviewTex{};
    bool m_editingDoctrine = false;
    std::string m_editingDoctrineText;
    bool m_editingResearch = false;
    std::string m_editingResearchText;

    // Button helper
    bool drawButton(const char* label, Rectangle rect, bool selected, int fontSize);
    bool drawButtonCol(const char* label, Rectangle rect, Color accent, bool selected, int fontSize);

    // Update
    void updateEdit(float dt);
    void updateToolbar();
    void updateBottomBar();
    void updateSidePanel();
    void updateLandmassPanel();
    void updateGeneratorPanel();
    void updateProvincePanel();
    void updateCountryPanel();
    void updateResourcePanel();
    void updateRelationsPanel();
    void updateTroopsPanel();
    void updateNavyPanel();
    void updateScriptPanel();
    void updateMetadataPanel();

    // Draw
    void drawToolbar();
    void drawCanvas();
    void drawSidePanel();
    void drawBottomBar();
    void drawLandmassPanel();
    void drawGeneratorPanel();
    void drawProvincePanel();
    void drawCountryPanel();
    void drawResourcePanel();
    void drawRelationsPanel();
    void drawTroopsPanel();
    void drawNavyPanel();
    void drawScriptPanel();
    void drawMetadataPanel();

    void screenToCanvas(int sx, int sy, int& cx, int& cy) const;
};