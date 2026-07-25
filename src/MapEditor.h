#pragma once
#include "GameStructs.h"
#include "map/LandSeaMap.h"
#include "map/ProvinceMap.h"
#include "map/CountryMap.h"
#include "renderer/MapRenderer.h"
#include "raylib.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <functional>

class MapEditor {
public:
    enum EditMode {
        MODE_LANDMASS, MODE_PROVINCES, MODE_COUNTRIES, MODE_NAVY,
        MODE_RELATIONS, MODE_SCRIPTS, MODE_GENERATOR, MODE_METADATA
    };
    enum Tool { TOOL_BRUSH, TOOL_RECT, TOOL_FILL, TOOL_ERASE };
    enum ProjectState { PROJ_STARTUP, PROJ_OPEN, PROJ_CREATE, PROJ_EDITING, PROJ_IMPORT };

    struct GeneratorParams {
        int seed = 12345;
        float landCoverage = 0.35f;
        int numContinents = 5;
        int numCountries = 50;
        float jaggedness = 0.3f;
        float provinceDensity = 1.0f; // 0.2 = large provinces, 5.0 = tiny
    };

    struct PendingProject { std::string name, path; long long lastModified = 0; };

    // Per-province editable game data (resources, buildings, garrison).
    // Parsed from generated/loaded JSON; serialized back on export/save.
    struct EditorProvinceData {
        float oil = 0, gold = 0, rubber = 0, gemstones = 0, metal = 0; // amounts 0-100
        float oilB = 5, goldB = 15, rubberB = 10, gemB = 12, metalB = 8; // industry boosts
        int industryLevel = 0;        // 0-10
        float industryIncome = 10.0f; // preserved for export
        int fortification = 0;        // 0-5
        int portLevel = 0;            // 0-3, 0 = none
        long long population = 1000;
        std::vector<ArmyUnit> troops; // garrison units, any nation
        float compassEconomic = 0.0f; // -100..100, province-level political compass
        float compassSocial = 0.0f;   // -100..100
        std::vector<std::pair<std::string, float>> ethnicGroups; // name -> percent (should sum to ~100)
    };

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
    bool consumeExitRequest() { bool v = m_wantsExit; m_wantsExit = false; return v; }

    std::string getMapName() const { return m_mapName; }
    void setMapName(const std::string& n) { m_mapName = n; m_dirty = true; }

    // Headless generate-and-export for AI self-play training: runs the full
    // generation chain (landmass -> provinces/countries -> game data) and
    // packages it as an .odmap, no renderer or UI involved. Call init() first
    // (it only sets sizes + data dir). Returns the exported path, "" on failure.
    std::string generateAndExportHeadless(const GeneratorParams& p, const std::string& mapName);

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
    std::string m_minoritiesJson;       // raw generated string (kept for reference; export uses buildMinoritiesJson)
    std::string m_minorityColorsJson;
    std::unordered_map<std::string, Color> m_ethnicColors; // ethnicity name -> display color
    std::string buildMinoritiesJson() const;      // from m_provinceData[...].ethnicGroups
    std::string buildMinorityColorsJson() const;  // from m_ethnicColors
    std::string buildProvinceCompassJson() const; // from m_provinceData[...].compassEconomic/Social
    void generateProvincesCountries();
    void generateGameData();
    void exportODMap();

    // ── Editable game data (Phase 0 data model) ──
    std::unordered_map<int, EditorProvinceData> m_provinceData; // pid -> data
    std::vector<NavyShip> m_editorShips;
    std::map<std::pair<int,int>, CountryRelation> m_editorRelations; // key: {min cid, max cid}
    std::string m_author = "MapEditor";
    std::string m_license = "CC-BY-4.0";

    void parseGeneratedGameData();       // m_resourcesJson/m_populationJson -> m_provinceData
    std::string buildResourcesJson() const;  // full game schema for every province
    std::string buildPopulationJson() const;
    std::string buildPortsJson() const;
    std::string buildArmiesJson() const;
    std::string buildShipsJson() const;
    std::string buildRelationsJson() const;  // iso-keyed, both directions per pair
    std::string buildCountriesJson() const;  // factored out of exportODMap
    void ensureIsoCodes();               // unique iso_a3 for all countries
    static std::vector<uint8_t> encodePng(const std::vector<Color>& px, int w, int h);

    // Frame-deferred generation (0=idle, 1=awaiting overlay, 2=running)
    int m_genPending = 0;
    std::string m_genStatus;

    // Project flow
    ProjectState m_projectState = PROJ_STARTUP;
    int m_projChoice = 0;
    int m_projScroll = 0;
    int m_projDeleteArm = -1;      // index armed for delete confirmation (click "Delete" once, then "Confirm?")
    std::vector<std::string> m_projFiles;
    std::string m_dataDir;

    void drawStartupDialog();
    void drawOpenDialog();
    void drawCreateDialog();
    void drawImportDialog();

    // "Based on Existing Map": browse the game's standard + custom .odmap
    // maps and load one into the editor (distinct from .uodmap projects).
    struct ImportMapEntry { std::string label, path; bool isStandard = false; };
    std::vector<ImportMapEntry> m_importEntries;
    bool m_importScanned = false;
    int m_importChoice = -1;
    int m_importScroll = 0;
    void scanImportableMaps();
    bool loadExistingMap(const std::string& path); // .odmap -> editable state

    // UI
    EditMode m_mode = MODE_LANDMASS;
    Tool m_tool = TOOL_BRUSH;
    // Custom map thumbnail (Metadata panel). Empty = auto-generate from the
    // political map at export time, as before. Any dropped image is rescaled
    // to THUMB_W x THUMB_H PNG on export, so the drop is forgiving about size.
    static constexpr int THUMB_W = 160, THUMB_H = 80;
    std::string m_thumbnailPath;          // source image on disk, or empty
    Texture2D m_thumbnailTex{};
    std::string m_thumbnailTexPath;       // path the cached texture was built from
    bool setThumbnailFromFile(const std::string& srcPath); // copy into the project, refresh preview

    static constexpr float BRUSH_MAX = 60.0f; // shared by the size slider's drag mapping and its drawn fill
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
    // Headless self-play export: generate terrain at a coarser scale. Training
    // maps are throwaway geography the model sees once, so province-outline
    // fidelity matters far less than how many maps per hour it can chew
    // through. Never set for the interactive editor.
    bool m_fastGen = false;
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
    // ── Research/policy "set mode" overlay (mirrors in-game research tree /
    //    policy screen) ──
    std::vector<ResearchNode> m_researchNodes; // all research definitions
    bool  m_setModeOpen = false;
    bool  m_setModePolicyMode = false;   // true: policy screen, false: research tree
    bool  m_setModeIsEthnicity = false;  // true: target is m_setModeEthnicity, not a country
    std::string m_setModeEthnicity;
    std::string m_setModeEthnicityIso;   // owning country's ISO (keys m_ethnicRelations like the game does)
    int   m_setModeEthnicityCid = -1;    // owning country's cid, for "apply to all minorities in country"
    int   m_setModeEthnicTab = 0;        // 0 = starting policies, 1 = ethnic relations (in-game parity tab)
    bool  m_setModeEthnicFromList = false; // opened from the country's ethnic list -> closing returns there
    int   m_setModeCountry = -1;
    int   m_setModeTab = 0;              // 0-3 research categories
    float m_setModeZoom = 1.0f;
    float m_setModeCamX = 0, m_setModeCamY = 0;
    bool  m_setModeDragging = false;
    int   m_setModeDragPrevX = 0, m_setModeDragPrevY = 0;
    int   m_setModeHoveredNode = -1;
    std::vector<ResearchNode> m_setModeNodes; // working copy carrying .researched flags
    void  openSetMode(int cid, bool policyMode);
    // Policy-only overlay for an ethnic group. initialTab picks which tab it
    // opens on (0 = starting policies, 1 = ethnic relations); fromList makes
    // closing it return to the country's ethnic list instead of the map.
    void  openSetModeEthnicity(const std::string& name, const std::string& iso, int cid = -1,
                               int initialTab = 0, bool fromList = false);
    void  closeSetModeEthnicity();       // honours the return-to-list behaviour
    std::vector<std::string> minoritiesOfCountry(int cid) const; // every ethnic group name found in that country's provinces
    void  applySetModeResearch();        // working-copy flags -> Country::research
    void  toggleSetModeNode(int idx);    // toggle with dep/mutex cascades
    void  drawSetModeOverlay();          // immediate-mode input + draw

    // ── Policy screen (starting policies per country) ──
    // The master policy list is per-map data (mirrors policies.json inside
    // .odmap); loaded once from data/policies.json as an editable default.
    std::vector<Policy> m_editorPolicies;
    bool m_editorPoliciesLoaded = false;
    std::unordered_map<int, std::vector<std::string>> m_countryPolicies; // cid -> policy ids
    std::unordered_map<std::string, std::vector<std::string>> m_ethnicityPolicies; // ethnicity name -> policy ids
    // Government-to-ethnicity relations, mirroring the in-game Ethnic tab:
    // iso -> ethnicity name -> one option index per fixed category (see
    // ethnicRelationCategories()). Matches starting_minority_policies.json.
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<int>>> m_ethnicRelations;
    // Country-panel overview: every minority found anywhere in that country's
    // provinces, so you don't have to hunt through provinces one at a time
    // to define how the country reacts to each of them.
    bool m_countryEthnicListOpen = false;
    int m_countryEthnicListCid = -1;
    int m_countryEthnicListScroll = 0;
    void openCountryEthnicList(int cid);
    void drawCountryEthnicListOverlay();
    void drawEthnicRelationsScreen(const std::string& iso, const std::string& ethnicity, int topY = 16);
    std::string buildStartingMinorityPoliciesJson() const;
    std::unordered_set<std::string> m_policyFoldersOpen;
    int m_policyScroll = 0;
    void loadEditorPolicies();           // data/policies.json -> m_editorPolicies (once)
    void togglePolicyInList(std::vector<std::string>& list, const std::string& policyId);
    void togglePolicyForCountry(int cid, const std::string& policyId);
    void togglePolicyForEthnicity(const std::string& name, const std::string& policyId);
    void drawPolicyScreen(int cid);      // the policy-screen body of the overlay
    void drawPolicyScreenFor(std::vector<std::string>& selected, const std::function<void(const std::string&)>& onToggle, int topY = 16);
    void drawPolicyScreenEthnicity(const std::string& name, int topY = 16);
    std::string buildPoliciesJson() const;
    std::string buildStartingPoliciesJson() const;

    bool anyModalOpen() const { return m_setModeOpen || m_exitDialogOpen || m_scriptEdOpen || m_pickerOpen || m_countryEthnicListOpen; }

    // Self-contained blocking-operation loading screen (throbber, status,
    // percentage, progress bar) — mirrors Game::drawLoadingScreen()'s look
    // so save/load/import in the editor feels consistent with in-game
    // loading. Calls BeginDrawing/EndDrawing itself since it's invoked
    // synchronously mid-function, outside the normal per-frame draw pass.
    void drawMiniLoadingScreen(float progress, const std::string& status);

    // Exit request (consumed by Game::updateMapEditor)
    bool m_wantsExit = false;

    // ── Relations editor ──
    int m_relCountryA = -1, m_relCountryB = -1;
    int m_relScrollA = 0, m_relScrollB = 0;
    // Immediate-mode scrollable country list; returns clicked cid or -1.
    int drawCountryList(int x, int y, int w, int rows, int& scroll, int selected, const std::string& filter = "");

    // ── Province editor ──
    int m_provPanelScroll = 0;
    int m_provTool = 0;                // 0 = select, 1 = paint shape
    bool m_provPopEditing = false;
    int m_provPopEditPid = -1;
    std::string m_provPopEditText;
    bool m_provTroopEditing = false;
    int m_provTroopEditPid = -1;
    int m_provTroopEditIdx = -1;
    std::string m_provTroopEditText;

    // ── Generic searchable picker overlay (ethnicities / country allegiance) ──
    bool m_pickerOpen = false;
    int m_pickerMode = 0;              // 0 = ethnicity picker, 1 = country picker
    std::string m_pickerQuery;
    int m_pickerScroll = 0;
    int m_pickerProvId = -1;
    int m_pickerSlot = -1;             // ethnic-group index, or troop-unit index; -1 = add new (ethnicity mode)
    std::vector<std::string> m_pickerEthnicityPool; // cached from m_minorityColorsJson when opened
    void openEthnicityPicker(int provId, int slotIdx); // slotIdx<0 adds a new group
    void openCountryPicker(int provId, int troopIdx);
    void drawPickerOverlay();
    std::vector<std::string> ethnicityNamePool() const; // parses m_minorityColorsJson keys
    bool m_provStrokeActive = false;   // shape-paint stroke in progress
    bool m_landStrokeActive = false;   // landmass stroke while provinces exist
    // Per-province pixel counts, maintained incrementally so stroke-end
    // cleanup never needs a full-map scan.
    std::unordered_map<int, int> m_provincePixelCounts;
    // Pixel-space centroids of provinces with industry/fort/port, cached by
    // rebuildBuildingsOverlay() and drawn as screen-space badges each frame.
    std::unordered_map<int, Vector2> m_buildingCentroids;
    // Dirty bounding box of the current stroke (union across frames).
    int m_strokeMinX = 0, m_strokeMinY = 0, m_strokeMaxX = -1, m_strokeMaxY = -1;
    bool m_strokeWrapped = false;
    void resetStrokeBBox() { m_strokeMinX = MAP_W; m_strokeMinY = MAP_H; m_strokeMaxX = -1; m_strokeMaxY = -1; m_strokeWrapped = false; }
    void growStrokeBBox(int x0, int y0, int x1, int y1) {
        m_strokeMinX = std::min(m_strokeMinX, x0); m_strokeMinY = std::min(m_strokeMinY, y0);
        m_strokeMaxX = std::max(m_strokeMaxX, x1); m_strokeMaxY = std::max(m_strokeMaxY, y1);
    }
    bool strokeBBoxValid() const { return m_strokeMaxX >= m_strokeMinX && m_strokeMaxY >= m_strokeMinY; }

    // Rect-tool drag (anchor in map coords, captured at press)
    bool m_rectDragActive = false;
    int m_rectAnchorX = 0, m_rectAnchorY = 0;
    bool m_rectLand = true;

    // Stands up an empty province/political layer on a blank project so a map
    // can be built entirely by hand, without first running the generator.
    void initEmptyProvinceLayer();
    void rebuildProvinceCounts();
    void sanitizeProvincePixels();  // strip province colors from sea, fix orphan land
    // Recomputes m_politicalPixels with the same border-distance glow the
    // in-game renderer applies (Game::generatePoliticalTexture), so the
    // editor's canvas preview matches gameplay instead of flat fills.
    void computePoliticalGradient();
    // Region-limited version: recomputes the border-distance gradient (and
    // re-uploads just that rect) only within [x0,y0]-[x1,y1] plus enough
    // padding for the BFS's 60-unit cap to settle at the region edges —
    // this is what keeps per-stroke updates fast instead of re-scanning
    // all ~33M map pixels after every single paint stroke.
    void computePoliticalGradientRegion(int x0, int y0, int x1, int y1);
    // Set whenever an edit could change the political layer (ownership,
    // province shape, land/sea, or country color); consumed once per frame
    // in updateEdit() as soon as the mouse is released, so the border-glow
    // gradient stays live without recomputing mid-drag every frame.
    bool m_polGradientDirty = false;
    bool m_polGradientFullDirty = false; // true: next refresh must scan the whole map
    int m_polDirtyX0 = 0, m_polDirtyY0 = 0, m_polDirtyX1 = -1, m_polDirtyY1 = -1; // accumulated region
    void markPoliticalDirty(int x0, int y0, int x1, int y1);
    void markPoliticalDirtyFull() { m_polGradientDirty = true; m_polGradientFullDirty = true; }
    void applyProvinceBrush(int cx, int cy); // paint selected province's shape
    void applyProvinceRect(int x1, int y1, int x2, int y2); // assign land in rect
    void applyProvinceFill(int cx, int cy);  // reassign a contiguous chunk (engulf)
    void applyCountryOwnershipBrush(int cx, int cy); // paint province ownership onto m_selectedCountry
    bool m_countryBrushActive = false;       // Countries-tab toggle: paint transfers ownership
    bool m_countryBrushStrokeActive = false; // drag in progress
    std::unordered_set<int> m_countryBrushTouched; // provinces already reassigned this stroke

    // ── Claims painting (Countries tab) ──
    // cid -> claimed province ids. A claim is one country asserting a right to
    // a province it doesn't own; exported as claims.json ({ISO: [pid, ...]}),
    // the same shape Game::loadGameData() reads.
    std::map<int, std::set<int>> m_editorClaims;
    bool m_claimsBrushActive = false;        // paint mode: adds/removes claims
    bool m_claimsBrushErase = false;         // true: strokes remove claims instead
    bool m_claimsStrokeActive = false;       // drag in progress
    std::unordered_set<int> m_claimsBrushTouched; // provinces already toggled this stroke
    std::vector<Color> m_claimsPixels;       // full-map RGBA overlay buffer
    int m_claimsOverlayCid = -2;             // country the overlay was last built for
    void applyClaimsBrush(int cx, int cy);
    void rebuildClaimsOverlay();             // repaint the whole overlay for m_selectedCountry
    void paintClaimsProvince(int pid, bool claimed, int& bx0, int& by0, int& bx1, int& by1);
    std::string buildClaimsJson() const;
    void commitProvincePixels();             // full push: pixels -> province map + textures
    void liveUpdateRegion(int minX, int minY, int maxX, int maxY); // patch textures mid-stroke
    void localAssignNewLand(int x0, int y0, int x1, int y1); // live: new land joins nearby provinces
    void finalizeLandStroke();               // stroke end: orphan land -> new provinces, cleanup
    void removeProvinceEntry(int pid);       // drop from JSON + maps + editor data
    int  createProvinceEntry(int countryId); // register a fresh province, returns its id
    void createNewProvince();                // UI action: new province, ready to paint
    void deleteSelectedProvince();           // merge pixels into best neighbor
    bool garbageCollectProvinces();          // drop zero-pixel provinces (count-based)

    // ── Country appearance ──
    // Live recolor cache: the country's political-pixel indices + bbox, built
    // once at drag start so every slider frame is a cheap rewrite + rect upload.
    int m_recolorCid = -1;
    std::vector<int> m_recolorIdx;
    int m_recolorX0 = 0, m_recolorY0 = 0, m_recolorX1 = -1, m_recolorY1 = -1;
    void regenerateFlag(Country& c);         // fresh random flag from country color
    void liveRecolorCountry(int cid);        // live repaint while a slider drags

    // ── Map scripts (Scripts tab) ──
    std::map<std::string, std::string> m_scripts; // filename -> content
    int m_scriptSel = -1;              // selected row in the project list
    int m_scriptScroll = 0;
    int m_scriptDiskScroll = 0;
    double m_scriptLastClickTime = 0;  // double-click detection
    int m_scriptLastClickRow = -1;
    int m_scriptDeleteArm = -1;        // click Delete twice to confirm
    std::vector<std::string> m_diskScripts; // data/scripts/*.txt on disk
    bool m_diskScriptsScanned = false;

    // ── Script editor overlay (the IDE) ──
    bool m_scriptEdOpen = false;
    std::string m_scriptEdName;
    std::vector<std::string> m_scriptEdLines;
    int m_scriptEdCurLine = 0, m_scriptEdCurCol = 0;
    // Selection anchor; selLine < 0 means "no selection". The selected range
    // is always [anchor, cursor) in document order, normalized on use.
    int m_scriptEdSelLine = -1, m_scriptEdSelCol = 0;
    bool m_scriptEdMouseSelecting = false;
    bool scriptEdHasSelection() const;
    void scriptEdSelectionRange(int& l0, int& c0, int& l1, int& c1) const; // normalized
    std::string scriptEdSelectedText() const;
    void scriptEdDeleteSelection(); // deletes + moves cursor to range start + clears anchor
    int m_scriptEdScroll = 0;          // first visible line
    float m_scriptEdBlink = 0;
    std::vector<std::string> m_scriptEdHints;   // current completions
    std::string m_scriptEdHintDoc;              // doc line for the top hint
    void openScriptEditor(const std::string& name);
    void saveScriptEditor();           // buffer -> m_scripts
    void drawScriptEditorOverlay();    // immediate-mode input + draw
    void scriptEdInsertText(const std::string& text);
    void refreshScriptHints();
    std::string scriptEdCurrentWord(int* startCol = nullptr) const;

    // Full documentation viewer (button inside the script editor overlay)
    bool m_scriptDocsOpen = false;
    int m_scriptDocsSel = 0;
    int m_scriptDocsScroll = 0;
    int m_scriptDocsBodyScroll = 0;
    void drawScriptDocsOverlay();

    // Lint pass: line indices (within m_scriptEdLines) with an error, and the message
    std::unordered_map<int, std::string> m_scriptEdErrors;
    void lintScriptEditor();           // static syntax check -> m_scriptEdErrors

    // Rename a project script and rewrite every `include "old"` reference to
    // the new name across the other project scripts.
    bool m_scriptRenaming = false;
    std::string m_scriptRenameText;
    void renameScript(const std::string& oldName, const std::string& newName);

    // ── Selection highlight + buildings overlay ──
    int m_hlMode = -1, m_hlProv = -2, m_hlCountry = -2;
    int m_hlRelA = -2, m_hlRelB = -2;
    bool m_hlDirty = false;
    void updateSelectionHighlight();         // pulsing highlight of selection
    void rebuildBuildingsOverlay();          // caches per-province building badge centroids
    void drawBuildingBadges();               // draws side-by-side industry/fort/port badges

    // ── Navy editor ──
    int m_navyCountry = -1;
    int m_navyCountryScroll = 0;
    std::string m_navyType = "destroyer";
    int m_selectedShip = -1;
    std::string m_navySearchQuery;      // filters the country list
    bool m_navySearchFocused = false;   // gates search box's char capture
    int m_navyDefaultHealth = 100;      // applied to the next placed ship
    long long m_navyDefaultTroops = 200; // applied to the next placed boat (crew)
    bool m_navyDefTroopsEditing = false;
    std::string m_navyDefTroopsText;
    bool m_navyShipTroopsEditing = false;
    int m_navyShipTroopsEditingIdx = -1;
    std::string m_navyShipTroopsText;
    bool m_navyDraggingShip = false;    // dragging the selected ship on the canvas
    // Drag-to-set 0-100 bar (health). Returns true when the value changed.
    bool drawHealthBar(int px, int& y, int listW, const char* label, int& health, bool inputOk);
    // Generic click-to-edit numeric text field (digits only); returns true
    // when a new value was committed (Enter or click-away) this frame.
    bool drawIntField(Rectangle r, long long& value, long long lo, long long hi,
                       bool& editing, std::string& editBuf, bool inputOk);
    void canvasToScreen(int cx, int cy, float& sx, float& sy) const;

    // ── Metadata editor ──
    int m_metaEditField = -1;   // -1 none, 0=name, 1=author, 2=start date, 3=custom license name
    std::string m_metaEditText;
    std::string m_mapDate = "January 2000"; // composed: "<Month> <Year> <AD|BC>"
    int m_dateMonth = 0;         // 0-11
    bool m_dateBC = false;
    std::string m_dateYearText = "2000";
    bool m_editingDateYear = false;
    bool m_dateMonthDropdownOpen = false;
    void syncDateFromString();   // m_mapDate -> month/year/era (on load)
    void syncDateToString();     // month/year/era -> m_mapDate (on edit)
    bool m_licenseCustom = false;   // custom license (own name + pasted/typed text)
    std::string m_licenseText;      // full custom license text
    bool m_licenseTextFocus = false;

    // ── .uodmap project save/load ──
    bool saveProject();                        // -> data/projects/<name>.uodmap
    bool loadProject(const std::string& path); // restore full editor state
    void rebuildFromPixelState();              // textures/maps from m_pixels etc.
    std::string m_saveStatus;                  // transient "Saved!" feedback
    float m_saveStatusTimer = 0;

    // ── Exit confirm dialog ──
    bool m_exitDialogOpen = false;
    void drawExitDialog();

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
    void updateRelationsPanel();
    void updateNavyPanel();
    void updateScriptPanel();
    void updateMetadataPanel();

    // Draw
    void drawToolbar();
    // Shared by the toolbar's Menu button and the ESC dispatcher: warns about
    // unsaved changes, otherwise returns straight to the editor's map menu.
    void requestReturnToMapMenu();
    void drawCanvas();
    void drawSidePanel();
    void drawBottomBar();
    void drawLandmassPanel();
    void drawGeneratorPanel();
    void drawProvincePanel();
    void drawCountryPanel();
    void drawRelationsPanel();
    void drawNavyPanel();
    void drawScriptPanel();
    void drawMetadataPanel();

    void screenToCanvas(int sx, int sy, int& cx, int& cy) const;
};