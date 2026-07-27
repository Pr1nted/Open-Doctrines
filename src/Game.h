#pragma once
#include "GameStructs.h"
#include "map/LandSeaMap.h"
#include "map/ProvinceMap.h"
#include "map/CountryMap.h"
#include "renderer/MapRenderer.h"
#include "Config.h"
#include "ScriptEngine.h"
#include "MapEditor.h"
#include "raymath.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

class AISystem;

class Game {
public:
    static constexpr int SPC_CID = 65533;
    static constexpr int UNC_CID = 65534;
    static constexpr int BLC_CID = 65535;

    friend class ScriptEngine;
    friend class AISystem;

    Game();
    ~Game();

    bool init(int screenW, int screenH, const char* title);
    void run();
    void shutdown();

    // Public wrapper for loading a save file from command line
    void loadSaveAndStart(const std::string& savePath);

    // Headless AI self-play training (`--train-ai`): generate a procedural
    // map, play N turns with every country AI-driven, then rotate to a fresh
    // map so the model never overfits one geography. Model persists to
    // data/ai/model.bin between maps and runs.
    void runAITraining(int numMaps, int turnsPerMap, int numCountries, unsigned int baseSeed);

private:
    enum ScreenState {
        SCREEN_SPLASH,         // startup "Pr1nted presents" fade, shown before the main menu
        SCREEN_MENU,
        SCREEN_SINGLEPLAYER,   // submenu: New World / Load World
        SCREEN_FILE_BROWSER,
        SCREEN_MAP_SELECT,
        SCREEN_COUNTRY_SELECT, // pick a country to play (after loading)
        SCREEN_PLAYING,
        SCREEN_LOADING,
        SCREEN_CREDITS,
        SCREEN_COMMUNITY,
        SCREEN_MAP_EDITOR,
        SCREEN_MODS
    };
    ScreenState m_currentScreen = SCREEN_MENU;

    // Startup splash ("Pr1nted presents" fade before the main menu)
    void updateSplashScreen(float dt);
    void drawSplashScreen();
    float m_splashTimer = 0.0f;
    // Main-menu intro: 0 -> 1 slides/fades the UI in after the splash so the
    // menu eases in instead of popping. 1 = finished (normal drawing).
    // Input is suppressed while it plays, so the drawn positions and the
    // click rects in updateMainMenu() can never disagree mid-slide.
    float m_menuIntro = 1.0f;

    // Loading screen
    void drawLoadingScreen();
    void setLoadingProgress(float progress, const std::string& status);
    void showLoadingScreen();
    void hideLoadingScreen();
    bool m_showLoadingScreen = false;
    float m_loadingProgress = 0.0f;
    std::string m_loadingStatus;
    std::vector<std::string> m_loadingTips;
    int m_currentTipIndex = 0;
    float m_tipTimer = 0;
    
    // Async loading state machine (runs one step per frame in the game loop)
    enum LoadingPhase {
        LOAD_NONE = 0,
        LOAD_ODM_SAVE,       // extract .odmap from .odsv into temp file
        LOAD_ODM,            // loadFromODM or loadFromFiles
        LOAD_GAME_DATA_RESOURCES,  // loadGameData step 1: resources.json + fortification
        LOAD_GAME_DATA_OTHER,      // loadGameData step 2: relations + claims + ports + armies + ships
        LOAD_INIT_RENDERER,  // new MapRenderer, computeBorderTexture, setPoliticalTexture
        LOAD_BUILD_POP,      // buildPopulationLookups
        LOAD_GEN_RESOURCE_TEXTURES,  // generate resource buffers
        LOAD_GEN_ICONS,      // generateIcons
        LOAD_BUILD_PROV_DATA, // buildProvinceData + country centers
        LOAD_COMPUTE_LABELS, // computeCountryLabels + setCountryLabels + rebuildFlags
        LOAD_CREATE_SAVE,
        LOAD_SAVE_FINALIZE,
        LOAD_FINALIZE,
        LOAD_DONE
    };
    LoadingPhase m_loadingPhase = LOAD_NONE;
    std::string m_loadingOdmPath;
    std::string m_loadingSavePath;
    std::string m_loadingTempOdm;
    int m_loadingResIdx = 0;
    bool m_loadingShouldCreateSave = false;
    bool m_loadingFailed = false;
    double m_lastLoadingWork = 0.0;  // for work throttling
    std::string m_loadingWorldName;
    void startLoading(const std::string& odmPath);
    void startLoadingSave(const std::string& savePath);
    void updateLoading();

    bool loadFromODM(const std::string& odmPath);
    bool loadFromFiles();
    void loadGameData();           // Load all non-map game data (resources, ships, etc.)
    bool loadGameDataStep1();      // Load only resources.json (resources + industry + fortification)
    bool loadGameDataStep2();      // Load remaining data (relations, claims, ports, armies, ships)
    void unloadGameData();         // Free all loaded game data
    bool loadMapPack(const std::string& odmPath);  // Load .odmap and init renderer
    bool loadSaveFile(const std::string& savePath); // Load .odsv, extract .odmap, replay
    bool replaySaveTurns(const std::string& savePath); // Replay turn deltas from a save
    void startNewGame(const std::string& mapName);
    void startNewGameWithName(const std::string& mapName, const std::string& worldName);
    void startLoadedGame(const std::string& saveName);
    void scanDirectory(const std::string& dir, const std::string& ext, std::vector<std::string>& out);

    void update(float dt);
    void draw();
    void drawInner();   // inner rendering body (no Begin/EndDrawing); used by draw() and popup overlay path
    void handlePauseMenu();
    void reloadBorders();
    void generateIcons();
    void drawBottomPanel();
    void computeCountryLabels();
    void flyToProvince(int provinceId);
    void buildCountryProvinceList(int countryId);
    void cycleProvince(int direction);
    void drawPauseMenu();
    void drawMenuList(const std::vector<std::string>& items, int selectedIndex);
    void drawCountryPanel();
    void drawSidebarButtons();
    void drawEconomy();
    void updateEconomy();
    void drawEconomyGlobal(int centerX, int startY);
    void drawEconomyLocal(int centerX, int startY);
    int drawBreakdownRow(int x, int y, int valX, const char* label, const char* value, Color col, bool highlight);
    void recordIncomeSnapshot();

    // Main menu
    void drawMainMenu();
    void updateMainMenu();
    void drawSingleplayerMenu();
    void updateSingleplayerMenu();
    void drawCountrySelect();
    void updateCountrySelect();
    void drawSettingsFromMenu();
    void updateSettingsFromMenu();
    void drawMenuBackground(bool dimmed = false);
    void updateMenuBackground();
    void drawDebugOverlay();
    int m_menuIndex = 0;
    std::string m_menuFeedback;
    float m_menuFeedbackTimer = 0;

    // Credits
    void loadCredits();
    void updateCredits();
    void drawCredits();
    void updateCommunityMenu();
    void drawCommunityMenu();

    // --- Mods (Gearbox). See src/Game_Mods.cpp and docs/modding.md ---
    void initModSystem();
    void updateModsMenu();
    void drawModsMenu();
    void drawModAdvanced();
    void drawModDeleteConfirm();
    void drawModAiWarning();
    void drawModReloadingOverlay();
    void drawModPanels();
    void clearModThumbnails();

    // Backing for the GameState.Read capability. Kept as plain accessors so the
    // mod layer never sees a game header. Public because the bridge that
    // implements ModGameAccess lives outside the class.
public:
    int         modTurnNumber() const;
    const std::vector<int>& modCountryIds() const;
    bool        modCountryExists(int cid) const;
    std::string modCountryName(int cid) const;
    double      modCountryTreasury(int cid) const;
    int         modCountryProvinceCount(int cid) const;
    long long   modProvincePopulation(int pid) const;
    int         modProvinceOwner(int pid) const;

    // Backing for the Map capability. Geometry only, and read-only: adjacency
    // and centres are already computed at load (m_provinceNeighbors,
    // m_provinceCenters), so none of this costs anything to expose.
    int         modMapWidth() const;
    int         modMapHeight() const;
    const std::vector<int>& modProvinceIds() const;
    std::string modProvinceName(int pid) const;
    bool        modProvinceExists(int pid) const;
    float       modProvinceCenterX(int pid) const;
    float       modProvinceCenterY(int pid) const;
    bool        modProvinceIsLand(int pid) const;
    int         modProvinceNeighborCount(int pid) const;
    int         modProvinceNeighborAt(int pid, int index) const;

    // Backing for the Diplomacy capability. Relations are stored by isoA3, so
    // these take country ids and do the lookup, keeping the mod layer free of
    // the game's keying.
    bool        modAtWar(int a, int b) const;
    bool        modAllied(int a, int b) const;
    bool        modNonAggression(int a, int b) const;
    bool        modGuaranteed(int a, int b) const;
    // Proposes, rather than performs: routed through declareWar so guarantee
    // chains and every other consequence happen exactly as they would for any
    // other actor. Returns false when the game refuses it.
    bool        modProposeWar(int attacker, int defender);

    // Backing for GameState.Write. Every one of these goes through the same
    // code the game itself uses, so a mod cannot reach a state the game could
    // not; each validates and returns false rather than trapping.
    bool        modSetCountryTreasury(int cid, double value);
    bool        modAddCountryTreasury(int cid, double delta);
    bool        modSetProvinceOwner(int pid, int toCid);
    bool        modSetProvincePopulation(int pid, long long value);

    // Backing for the Neural capability. OBSERVE ONLY: there is deliberately
    // no path here that can write to the model or to training state.
    int         modNeuralFeatureCount() const;
    int         modNeuralFeatures(int cid, float* out, int cap) const;
    int         modNeuralRewardCount() const;
    double      modNeuralRewardMean(int index) const;
private:
    const std::string* modIsoFor(int cid) const;
    bool        modRelationFlag(int a, int b, int which) const;
public:
private:
    // Dense, stable id list for the Map capability, built on demand because
    // m_provinces is an unordered_map and a mod needs a fixed iteration order.
    mutable std::vector<int> m_modProvinceIds;
public:

    int   m_modIndex = 0;
    int   m_modScroll = 0;
    int   m_modAdvancedFor = -1;      // index whose Advanced panel is open
    // Reset when the mod menu is left, so each visit checks for updates
    // once rather than every frame.
    bool  m_modUpdatesAsked = false;
    int   m_modDeleteFor = -1;
    int   m_modAiWarnFor = -1;        // index awaiting the AI-learning interlock
    bool  m_modReloading = false;
    int   m_modReloadFrames = 0;
    std::string m_modFeedback;
    float m_modFeedbackTimer = 0.0f;
    std::unordered_map<std::string, Texture2D> m_modThumbs;

    // Caches for the read capability, rebuilt when the turn changes.
    mutable std::vector<int> m_modCountryIds;
    mutable int m_modCountryIdsTurn = -1;
    mutable std::unordered_map<int, int> m_modProvCounts;
    std::vector<CreditEntry> m_credits;
    float m_creditsScroll = 0.0f;
    float m_creditsSpeed = 60.0f;
    bool m_creditsLoaded = false;

    // Save / unsaved changes
    std::string m_currentSavePath;
    int m_turnCount = 0;
    bool m_unsavedChanges = false;
    bool m_showUnsavedWarning = false;
    int m_unsavedChoice = 0;  // 0=Save&Quit, 1=Quit anyway, 2=Cancel
    bool m_autoCreatedSave = false;
    std::string m_saveFeedback;
    float m_saveFeedbackTimer = 0;

    bool trySaveGame();
    void trackChange();
    void drawUnsavedWarning();

    std::vector<MapEntry> m_mapEntries;
    int m_mapTabIndex = 0;        // 0=Standard, 1=Custom
    int m_mapIndex = 0;
    int m_mapScroll = 0;
    bool m_showMapDeleteConfirm = false;
    int m_mapDeleteIndex = -1;
    // Map info popup
    bool m_showMapInfoPopup = false;
    int m_mapInfoIndex = -1;
    std::vector<Notification> m_notifications;
    void addNotification(const std::string& msg, Color color = WHITE, float duration = 6.0f);
    void updateNotifications();

    std::vector<PopupEntry> m_popupQueue;
    // Ceasefire popup: whether the itemised terms panel is expanded. Reset
    // whenever a popup is dismissed so the next one starts collapsed.
    bool m_popupShowTerms = false;
    void pushPopup(PopupType type, const std::string& title, const std::string& message,
                    int countryId = 0, const std::string& action = "",
                    const std::string& sourceIso = "", const std::string& targetIso = "");
    void drawPopup();
    void updatePopup();

    // License popup
    bool m_showLicensePopup = false;
    int m_licenseEntryIndex = -1;
    int m_licenseScroll = 0;
    std::string m_cachedLicenseText;

    // Scripts detection from loaded .odmap
    bool m_loadedMapHasScripts = false;

    // Map import state
    std::string m_importPath;
    std::string m_importName;
    bool m_showImportNameDialog = false;
    void executeMapImport();

    // New world name dialog
    std::string m_newWorldName;
    std::string m_newWorldMapPath;
    bool m_showNewWorldDialog = false;

    // World rename dialog
    std::string m_renameWorldOldName;
    std::string m_renameWorldNewName;
    int m_renameWorldIndex = -1;
    bool m_showRenameDialog = false;

    void loadMapEntries();
    void clearThumbCache();
    Texture2D getThumbTexture(const std::string& path);
    Texture2D getThumbTextureFromODM(const std::string& odmPath);
    std::unordered_map<std::string, Texture2D> m_thumbCache;
    void drawMapBrowser();
    void updateMapBrowser();

    // File browser (save world browsing + .odmap import)
    int m_fileIndex = 0;
    int m_fileScroll = 0;
    std::vector<std::string> m_fileItems;
    bool m_browsingSaves = false;
    void drawFileBrowser();
    void updateFileBrowser();

    std::vector<SaveWorldInfo> m_worldInfos;
    bool m_showDeleteConfirm = false;
    int m_deleteWorldIndex = -1;
    bool m_showWorldSettings = false;
    int m_worldSettingsIndex = -1;
    void drawWorldBrowser();
    void updateWorldBrowser();

    Vector2 getMouse() const { return { GetMousePosition().x * m_dpiScale, GetMousePosition().y * m_dpiScale }; }

    LandSeaMap m_landSea;
    ProvinceMap m_provinces;
    CountryMap m_countries;
    MapRenderer* m_renderer = nullptr;
    Texture2D m_politicalTex{};
    int m_screenW = 1600;
    int m_screenH = 900;
    float m_dpiScale = 1.0f;
    bool m_running = false;
    std::string m_dataDir;

    std::unordered_map<int, Vector2> m_provinceCenters;
    std::unordered_map<int, float> m_provinceRadius;
    std::unordered_map<int, Vector2> m_countryCenters;
    std::vector<CountryLabel> m_countryLabels;
    bool m_labelsDirty = false; // set by rebellions/ceasefires; labels rebuilt once per turn
    int m_lastSelectedProvince = 0;
    std::vector<int> m_countryProvinceIds;
    int m_countryProvinceIndex = -1;

    std::unordered_map<int, Texture2D> m_countryFlags;
    void rebuildFlags();

    bool m_paused = false;
    bool m_inSettings = false;
    int m_settingsTab = 0;
    int m_settingsIndex = 0;
    bool m_editingValue = false;
    std::string m_editBuffer;
    Config m_config;
    std::string m_configPath;
    bool m_draggingFpsSlider = false;
    bool m_waitingForKey = false;
    int m_rebindingAction = -1;
    int m_settingsScroll = 0;

    // ─── Debug/Advanced settings ─────────────────
    struct ConsoleBuf : std::streambuf {
        Game* game;
        std::string buf;
        ConsoleBuf(Game* g) : game(g) {}
    protected:
        int overflow(int c) override {
            if (c == '\n') { flushLine(); }
            else if (c != '\r') { buf += (char)c; }
            return c;
        }
        std::streamsize xsputn(const char* s, std::streamsize n) override {
            for (std::streamsize i = 0; i < n; ++i) {
                if (s[i] == '\n') { flushLine(); }
                else if (s[i] != '\r') { buf += s[i]; }
            }
            return n;
        }
        void flushLine() {
            if (!buf.empty()) { 
                game->addConsoleLine(buf); 
                // Also write to original stdout for debugging
                if (game && game->m_origCout) {
                    std::ostream origOut(game->m_origCout);
                    origOut << buf << '\n';
                }
                buf.clear(); 
            }
        }
    };
    struct ConsoleWindow {
        Rectangle rect{100, 100, 600, 300};
        bool dragging = false;
        Vector2 dragOffset{0, 0};
        bool resizing = false;
        int resizeEdge = 0;
        std::vector<std::string> lines;
        std::mutex mutex;
        int scrollOffset = 0;
    };
    ConsoleWindow m_console;
    ConsoleBuf* m_consoleBuf = nullptr;
    std::streambuf* m_origCout = nullptr;
    std::streambuf* m_origCerr = nullptr;
    void addConsoleLine(const std::string& line);
    void drawConsoleWindow();
    void drawNotifications();
    bool isMouseOverConsole();
    int m_windowedX = 0;
    int m_windowedY = 0;
    int m_windowedW = 1600;
    int m_windowedH = 900;
    int m_activeViewTab = 0;
    Font m_gameFont{};        // Fallback font for non-ASCII characters (Unifont)
    Font m_defaultFont{};     // Cached default raylib font

    // Draw text with per-character font selection:
    // default raylib font for ASCII (32-126), m_gameFont for non-ASCII
    void drawHybridText(int x, int y, int fontSize, const char* text, Color color);
    static float glyphAdvance(Font font, int glyphIndex, int fontSize);
    Texture2D m_iconPopulation{};
    Texture2D m_iconIndustry{};
    Texture2D m_iconDefence{};
    Texture2D m_iconRelations{};
    Texture2D m_iconArmyNav{};
    Texture2D m_iconNavy{};
    Texture2D m_iconResources{};
    Texture2D m_iconCountryNames{};
    Texture2D m_iconPolicies{};
    Texture2D m_iconEconomy{};
    Texture2D m_iconClaims{};
    Texture2D m_iconResearch{};
    int m_activeSidebarTab = 0; // 0=none, 1=Policies, 2=Economy, 3=Claims, 4=Research
    bool m_inResearch = false;
    // Sidebar "needs attention" markers: set when something finishes for the
    // player (research completed / a policy finished implementing), cleared
    // once they actually open that panel. Purely a UI hint.
    bool m_researchAlert = false;
    bool m_politicsAlert = false;

    bool m_inPolitics = false;

    // ─── Economy overlay ──────────────────────────
    bool m_inEconomy = false;
    int m_economyTab = 0;           // 0=Global, 1=Local
    int m_economyScroll = 0;
    int m_economyExpScroll = 0;
    int m_economyGrossScroll = 0;
    bool m_economyShowWorst = false; // show bottom 10 instead of top 10
    std::string m_economyFeedback;
    float m_economyFeedbackTimer = 0;
    CountryIncomeSnapshot computeCountryIncome(int countryId) const;
    void refreshIncomeCache();
    mutable std::unordered_map<int, CountryIncomeSnapshot> m_countryIncomeCache;
    std::unordered_map<int, std::vector<CountryIncomeSnapshot>> m_incomeHistory;
    std::string m_mapDate;
    std::unordered_map<int, long long> m_provincePopulations;
    std::unordered_map<int, Vector2> m_provinceCompass;
    std::unordered_map<int, std::vector<MinorityGroup>> m_provinceMinorities;
    std::unordered_map<std::string, Color> m_minorityColors;
    int m_playerCountryId = 0;
    std::vector<int> m_playableCountryIds;
    int m_countrySelectIndex = 0;
    float m_countrySelectScroll = 0;
    int m_pendingCountryId = 0;
    std::unordered_map<int, float> m_countryBalances;
    std::vector<int> m_provinceCountryLookup;
    std::unordered_map<int, int> m_provinceConquestTurn; // turn# when province was conquered (0 = not conquered)
    std::unordered_map<int, int> m_conqueredProvincePrevOwner; // previous owner of conquered province (for ongoing war debuff)
    std::vector<long long> m_provincePopArray;
    // Per-pixel lookups for fast population texture updates
    std::vector<int> m_pixelCountryArray;
    std::vector<std::vector<int>> m_countryPixels;
    std::unordered_map<int, std::vector<int>> m_provincePixels;
    std::vector<Color> m_populationPixelBuffer;
    std::vector<Color> m_politicalPixelBuffer;
    std::vector<uint8_t> m_gradientDist; // distance-to-border (0-255, capped at ~30)
    void generatePopulationTexture(int countryId, int prevCountryId);
    void generatePoliticalTexture();
    void buildPopulationLookups();
    int m_lastPopCountryId = -1;

    std::unordered_map<std::string, std::unordered_map<std::string, CountryRelation>> m_relations;
    void generateRelationsTexture(int countryId, int prevCountryId);
    int m_lastRelationsCountryId = -1;
    std::vector<Color> m_countryRelationColors;
    std::unordered_map<int, ProvinceResources> m_provinceResources;
    int m_activeResourceIdx = 0;   // 0=oil, 1=gold, 2=rubber, 3=gemstones, 4=metal
    std::array<std::vector<Color>, 5> m_resourceBuffers;
    void generateResourceTexture();
    void generateResourceTextureFor(int resIdx);
    Texture2D m_resourceTex{};
    static constexpr const char* RESOURCE_NAMES[5] = {"Oil", "Gold", "Rubber", "Gemstones", "Metal"};

    std::unordered_map<int, ProvinceIndustry> m_provinceIndustry;
    static std::string toRoman(int n);
    static std::string formatBalance(float val);
    static constexpr const char* ROMAN_NUMERALS[11] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};

    std::unordered_map<int, std::vector<ArmyUnit>> m_provinceArmies;

    std::unordered_map<int, PortInfo> m_provincePorts;
    std::unordered_map<int, std::vector<int>> m_provinceNeighbors;
    std::vector<NavyShip> m_ships;

    // Ship selection state
    std::vector<int> m_selectedShipIndices;
    Vector2 m_dragSelectStart{0, 0};
    bool m_isDragSelecting = false;
    float m_shipPanelScroll = 0.0f;
    int m_shipListFocusIndex = -1;  // -1 or index within m_selectedShipIndices for list highlight
    std::vector<int> m_countryShipIndices;
    int m_countryShipIndex = -1;
    void buildCountryShipList(int shipIdx);
    void cycleShip(int direction);

    // ─── Claims system ────────────────────────────
    std::unordered_map<std::string, std::vector<int>> m_claims;  // claimer ISO -> claimed province IDs
    std::unordered_map<int, std::vector<std::string>> m_claimsByProvince;  // province ID -> list of claimant ISOs
    bool m_showClaims = false;
    int m_lastClaimsCountryId = -1;
    std::vector<Color> m_claimsPixelBuffer;
    void generateClaimsTexture();
    void clearClaimsView();
    bool isCountryInvolvedInClaims(int countryId, int claimantCid);

    // ─── Claims overlay panel ─────────────────────
    bool m_inClaims = false;
    int m_claimsTab = 0;          // 0=My Claims, 1=Claims on Me, 2=Disputed
    int m_claimsScroll = 0;
    bool m_claimsEditMode = false;
    std::vector<int> m_claimsEditToAdd;
    std::vector<int> m_claimsEditToDrop;
    std::vector<int> m_claimsPendingAdd;   // claims queued for addition on next turn
    std::vector<int> m_claimsPendingDrop;  // claims queued for removal on next turn
    int m_claimsPovIndex = 0;     // selected POV country index for "Claims on Me"
    std::vector<std::string> m_claimsPovList; // claimant ISOs with claims on player
    int m_claimsMapSrcX = 0, m_claimsMapSrcY = 0;   // pan offset for inline map
    float m_claimsMapZoom = 1.0f;                     // zoom level for inline map
    bool m_claimsMapDragging = false;
    int m_claimsMapDragPrevX = 0, m_claimsMapDragPrevY = 0;
    bool m_claimsOverlayDirty = true;
    Texture2D m_claimsPanelTex{};   // full-map claims overlay texture for the panel
    void drawClaimsTab();

    std::unordered_map<int, PoliticalCompass> m_countryCompass;

    std::vector<Policy> m_allPolicies;
    // JSON data loaded from .odmap archive (loaded in-memory, never written to disk)
    std::unordered_map<std::string, std::string> m_odmJsonData;
    std::unordered_map<int, std::string> m_rebelFlagSvgs; // rebel CID → SVG string

    std::unordered_map<std::string, std::vector<std::string>> m_startingPolicies; // isoA3 -> [policyId]

    std::vector<ActivePolicy> m_activePolicies;  // implementing + active
    std::unordered_map<int, std::vector<int>> m_countryActivePolicyIndices; // countryId -> indices in m_activePolicies

    // Policy UI state
    int m_policyTab = 0;        // 0=Available, 1=Implementing, 2=Active, 3=Analysis
    int m_policyScroll = 0;
    int m_selectedPolicyIdx = -1;
    int m_policiesEnactedThisTurn = 0;
    std::unordered_set<std::string> m_openFolders; // expanded folder names in Available tab
    int m_analysisHotspotScroll = 0;
    int m_analysisMinorityScroll = 0;
    int m_analysisHotspotCount = 0;
    std::vector<std::pair<int, Rectangle>> m_analysisGoToButtons;

    std::vector<EthnicPolicyCategory> m_ethnicPolicyCategories;
    std::unordered_map<std::string, std::vector<int>> m_ethnicPolicies; // minorityName -> option indices
    std::unordered_map<std::string, float> m_minorityAlignmentDrift; // cumulative drift

    // Per-country starting minority ethnic policy defaults (isoA3 -> minorityName -> option indices)
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<int>>> m_startingMinorityPolicies;

    int m_selectedEthnicity = -1;
    int m_ethnicTabScroll = 0;
    int m_flyToLockTimer = 0; // prevents hover selection override during fly-to
    int m_blockLeftPanTimer = 0; // prevents MapRenderer click handler after Go-to

    void initPolicies();
    void initCountryCompass();
    void applyStartingPolicies();
    void updatePolicies();  // called each turn
    bool canCountryEnactPolicy(int countryId, const Policy& p) const;
    void enactPolicy(int countryId, const std::string& policyId, int targetProvince = -1, const std::string& targetMinority = "");
    void cancelPolicy(int activePolicyIndex);
    void applyPolicyEffects(int countryId);
    void shiftCountryCompass(int countryId, float econDelta, float socDelta);
    float getCountryUnrest(int countryId) const;
    void drawPoliciesTab();
    void updatePoliciesTab();
    void drawPoliticalCompass(int x, int y, int size, int countryId, bool showPopAverage = true);
    void drawAnalysisTab();
    float getProvinceRebellionChance(int provinceId) const;
    float getProvinceRebellionChance(int provinceId, int countryId) const;
    float getMinorityAlignment(const std::string& minorityName) const;
    float getMinorityAlignmentTrend(const std::string& minorityName) const;
    void initEthnicPolicyCategories();
    void drawEthnicTab();
    void updateEthnicTab();
    void applyEthnicPolicyEffects(int countryId);

    // ─── Menu background ──────────────────────────
    Texture2D m_menuBgTex{};
    int m_menuBgTexW = 0;
    int m_menuBgTexH = 0;
    float m_menuBgScroll = 0.0f;
    std::vector<BgParticle> m_menuParticles;
    float m_menuParticleTimer = 0.0f;
    std::vector<bool> m_menuBgLandPixels; // 1D bool array of land mask at rendered size
    std::vector<std::pair<int,int>> m_menuBgLandCoords; // list of (x,y) land pixels for fast spawning
    int m_menuBgPixelsW = 0;
    int m_menuBgPixelsH = 0;
    int m_menuBgInitScreenW = 0;
    int m_menuBgInitScreenH = 0;
    void initMenuBackground();
    
    std::vector<ResearchNode> m_researchNodes;
    std::unordered_map<int, std::unordered_set<std::string>> m_countryResearched;
    float m_researchCamX = 0, m_researchCamY = 0;
    float m_researchZoom = 1.0f;
    bool m_researchDragging = false;
    int m_researchDragPrevX = 0, m_researchDragPrevY = 0;
    int m_researchScroll = 0;
    float m_researchAllocation = 0.25f;
    float m_pacificationAllocation = 0.0f;
    int m_researchHoveredNode = -1;
    int m_researchActiveNode = -1;  // node being researched (-1 = none)
    int m_researchPoints = 0;
    int m_researchTab = 0; // 0=Buildings, 1=Army, 2=Population, 3=Misc
    float m_researchSliderHold = 0; // timer for slider hold
    void initResearchTrees();
    void drawResearchTab();
    void updateResearch(int countryId);
    bool hasResearched(const std::string& nodeId, int countryId = -1) const;
    void addResearchPoints(int countryId);
    
    // ─── Rebellion System ─────────────────────────
    int m_nextRebelCid = 60000;
    std::unordered_map<int, float> m_countryPacification;
    // ── Turn history / timelapse (Game_History.cpp) ──
    // Reconstructed purely from the .odsv, so browsing never mutates the
    // running game.
    struct HistShip { double lat = 0, lon = 0; int countryId = 0; };
    struct TurnSnapshot {
        int turn = 0;
        std::unordered_map<int, int> owner;            // pid -> cid
        std::unordered_map<int, long long> population; // pid -> pop
        std::unordered_map<int, long long> troops;     // pid -> total troops
        std::vector<HistShip> ships;
        bool hasState = false;   // has turns/s_NNNNN.json, so revert is faithful
    };
    enum HistoryView { HV_POLITICAL = 0, HV_POPULATION = 1, HV_TROOPS = 2 };
    bool buildTurnSnapshots(const std::string& savePath, std::vector<TurnSnapshot>& out);
    // Loads just the province image + countries from the save's embedded
    // .odmap, so history can be browsed/previewed/exported without a full
    // game load. Returns false if the save has no usable map data.
    bool loadHistoryMapData(const std::string& savePath);
    void renderHistoryFrame(const TurnSnapshot& a, const TurnSnapshot& b, float t,
                            int outW, int outH, std::vector<uint8_t>& rgba,
                            HistoryView view = HV_POLITICAL);
    bool exportHistoryGif(const std::string& savePath, int outW, int outH,
                          int subFrames, const std::string& destPath, std::string& outMsg);
    bool revertToTurn(int turn);
    // The rewind itself, split out because it may only run against a fully
    // built world (renderer included). revertToTurn() either calls it straight
    // away or defers it to the end of the async load.
    bool applyTurnRewind(const std::string& savePath, int turn);
    void updateHistoryScreen();
    void drawHistoryScreen();
    void openHistoryScreen(const std::string& savePath);
    void refreshHistoryPreview();
    std::string defaultTimelapsePath(const std::string& savePath, int w, int h) const;

    bool m_inHistory = false;
    std::string m_historySavePath;       // save being browsed
    bool m_historyFromGame = false;      // opened over a live game vs. from the browser
    int  m_historyIndex = 0;             // selected turn
    int  m_historyScroll = 0;
    int  m_historyResIndex = 1;          // index into the resolution presets
    int  m_historySubFrames = 4;         // interpolated frames per turn transition
    HistoryView m_historyView = HV_POLITICAL;
    std::string m_historyStatus;
    std::vector<TurnSnapshot> m_historySnaps;
    Texture2D m_historyPreviewTex{};
    int  m_historyPreviewTurn = -1;      // which turn the preview texture holds
    HistoryView m_historyPreviewView = HV_POLITICAL;
    bool m_historyEditingDest = false;   // destination text field focused
    std::string m_historyDestPath;       // where to write the GIF
    bool m_historyConfirmRevert = false; // two-step revert confirmation
    // Set when a revert needs the save loaded first: the async loader applies
    // the rewind on its final step, once the renderer and world exist again.
    int  m_pendingRevertTurn = -1;
    std::string m_pendingRevertSave;

    int allocateRebelCid();
    // Rebel countries are created at runtime, so unlike map countries they
    // exist nowhere on disk. Without persisting them, a reloaded save has
    // provinces pointing at a country id that no longer exists — the territory
    // renders as unowned limbo with no UNC/BLC tag. Serialized in the same
    // shape as countries.json so CountryMap::loadFromJson can merge them back.
    std::string buildRebelsJson() const;
    void restoreRebels(const std::string& savePath);
    // Creates placeholder countries for any rebel cid that provinces reference
    // but m_countries doesn't have (old saves with no rebels.json). Keeps such
    // territory rendering as a coloured state instead of grey limbo.
    void synthesizeMissingRebels();
    // ISO-A3 -> cid index. Many hot paths (rebellion-chance claims scan,
    // guarantee chains, diplomacy) used to find a country by ISO with a linear
    // scan over ALL countries — and the country map grows with every rebel
    // state, so those scans got slower every rebellion. Kept fresh by
    // rebuildIsoIndex() at load and by insertions at rebel creation.
    std::unordered_map<std::string, int> m_isoToCid;
    void rebuildIsoIndex();
    int cidForIso(const std::string& iso) const;

    // Single choke point for "X declares war on Y" (war flags were previously
    // set raw at scattered sites). Sets both relation directions, applies the
    // minority-kin alignment penalty, notifies the player when involved, and —
    // the new rule — pulls every guarantor of the defender into the war
    // against the attacker (one level; guarantors' own guarantors are not
    // chained, so a world war needs explicit guarantees, not transitivity).
    // Shared by the ceasefire path and the GameState.Write capability.
    void transferProvinceOwnership(int pid, int fromCid, int toCid);
    void declareWar(const std::string& attackerIso, const std::string& defenderIso,
                    bool chainGuarantees = true);
    void applyWarKinPenalty(const std::string& attackerIso, const std::string& defenderIso);
    // Inherent civil order subtracted from every province's rebellion chance —
    // makes stability the default and rebellion a grievance-driven exception.
    // Tuned so baseline provinces are stable but claim/war/minority hotspots
    // still revolt (see getProvinceRebellionChance).
    static constexpr float REBELLION_LOYALTY_FLOOR = 6.0f;
    static constexpr int REBEL_CID_MIN = 60000;
    void createRebelCountry(int rebelCid, int parentCid, const std::vector<int>& provinceIds);
    void processRebellions(int countryId);

    // ── Country AI (neural-net RL, see src/ai/) ──
    // Created lazily on the first processed turn; owns its model file.
    AISystem* m_ai = nullptr;
    // Self-play training mode: skips political-texture/label/delta work in
    // processTurn so turns run as fast as the simulation allows.
    bool m_aiTraining = false;
    // Rebellions that fired this turn, per country — cleared at processTurn
    // start. The AI reads it both as a feature and as a punishment signal.
    std::unordered_map<int, int> m_rebellionsThisTurnByCid;
    // Countries already reduced to zero provinces and disbanded, so the
    // per-turn elimination sweep does the (once-only) teardown and log line
    // exactly once instead of re-running it every turn for every dead shell —
    // a real cost on crowded maps with 100+ rebel breakaways. A country is
    // erased from this set if it ever holds land again (e.g. an amphibious
    // landing revives it), so it can be re-eliminated cleanly.
    std::unordered_set<int> m_eliminatedCids;

    // Research effect queries
    int getResearchedFortLevel(int countryId = -1) const;
    int getResearchedIndustryLevel(int countryId = -1) const;
    int getResearchedPortLevel(int countryId = -1) const;
    float getTotalEffect(const std::string& effectField) const;

    // ── Per-country research (AI countries; the player keeps the global tree
    // UI). Completion lands in m_countryResearched, which every effect query
    // above already consults, so finished nodes unlock features per country. ──
    std::unordered_map<int, float> m_countryResearchAllocation; // 0..1 share of income
    std::unordered_map<int, int>   m_countryResearchPoints;
    std::unordered_map<int, int>   m_countryResearchActive;     // index into m_researchNodes, -1 = none
    std::unordered_map<int, int>   m_countryResearchInvested;   // points sunk into the active node
    // Country-aware ResearchNode::isAvailable (that one reads the player-global
    // node flags; this reads m_countryResearched[cid]).
    bool isNodeAvailableFor(const ResearchNode& node, int countryId) const;
    void progressCountryResearch(int countryId);

    // ── Pending Actions (queued for processing on next turn) ──
    std::vector<PendingDiplomaticAction> m_pendingDiplomaticActions;
    std::vector<PendingUpgrade> m_pendingUpgrades;
    std::vector<PendingSpecialization> m_pendingSpecializations;
    std::vector<PendingRecruitment> m_pendingRecruitments;

    std::vector<PendingMoveOrder> m_pendingMoveOrders;
    std::vector<PendingDisbandOrder> m_pendingDisbandOrders;
    std::vector<PendingShipBuild> m_pendingShipBuilds;
    std::vector<PendingScrapShip> m_pendingScrapShips;
    std::vector<PendingEmbark> m_pendingEmbarkations;
    std::vector<PendingArtilleryOrder> m_pendingArtilleryOrders;
    std::vector<PendingShipMoveOrder> m_pendingShipMoveOrders;
    std::vector<PendingShipEngageOrder> m_pendingShipEngageOrders;
    std::vector<PendingShipBombardOrder> m_pendingShipBombardOrders;
    std::vector<PendingShipDisembark> m_pendingShipDisembarks;

    bool isProvinceCoastal(int pid) const;
    void processArtilleryOrders(int countryId);
    void processShipBombardOrders(int countryId);
    void processShipDisembarks(int countryId);
    void processRecruitments(int countryId);
    void processDisbandOrders(int countryId);
    void processEmbarkations(int countryId);
    void processScrapShips(int countryId);

    // UI state for action buttons
    int m_diplomaticActionScroll = 0;
    int m_armyRecruitPct = 50;  // slider for what % of max to recruit
    int m_armySplitPct = 50;    // slider for split percentage
    int m_specDropdownProvince = -1; // province id with open specialization dropdown
    int m_specDropdownHover = 0;
    bool m_armySliderActive = false;

    // Army move order state (right-click drag based)
    int m_armyMoveDragSource = -1;  // source province during drag (-1 = inactive)
    bool m_armyMoveDragActive = false; // drag in progress (button was pressed)
    bool m_armyMoveDragBtnDown = false; // button currently held (tracks press/release for keyboard keys)
    int m_armyMoveDragHoverPid = -1;   // province under cursor during drag
    bool m_armyMoveDragValidDest = false; // whether hover destination is valid for movement
    int m_armyMovePctSliderFrom = 0;  // from province of order whose slider is being dragged
    int m_armyMovePctSliderTo = 0;      // to province of order whose slider is being dragged

    // ─── Artillery UI state ───
    int m_artillerySourceProvince = -1;  // source province with open artillery UI (-1 = none)
    int m_artilleryTargetPid = -1;       // target province for pending artillery order
    std::string m_artillerySelectedType; // currently selected ammo type
    int m_artilleryDragSource = -1;      // source province during artillery drag (-1 = inactive)
    bool m_artilleryDragActive = false;  // drag in progress
    int m_artilleryWheelProvince = -1;   // province the wheel is open for (-1 = closed)
    int m_artilleryWheelHover = -1;      // hovered sector index (-1 = none)

    // ─── Navy ship action mode ───
    int m_shipActionMode = 0;  // 0=none, 1=move, 2=engage, 3=bombard
    int m_shipActionShipIdx = -1; // which ship is being ordered
    int m_shipActionHoverShipIdx = -1; // ship under cursor during engage targeting
    int m_shipActionHoverProvince = -1; // province under cursor during bombard/disembark targeting
    bool m_shipActionValidDest = false; // whether hover target is valid
    std::string m_shipBombardAmmo;      // selected artillery type for carrier bombardment
    bool m_shipBombardDropdownOpen = false;
    int m_shipWheelShipIdx = -1;        // ship the wheel is open for (-1 = closed)
    int m_shipWheelHover = -1;          // hovered sector index (-1 = none)
    std::string m_keybindFilter;         // search filter for keybinds tab
    bool m_keybindFilterActive = false;  // whether search box has focus
    std::unordered_set<int> m_collapsedSections; // indices of collapsed category headers
    int m_artilleryDragHoverPid = -1;    // province under cursor during drag
    bool m_artilleryDragValidDest = false;

    // Navy filter: 0=All, 1=Own, 2=Allies, 3=Enemies, 4=Neutral
    int m_navyFilter = 0;

    // ─── Province panel cached aggregates (recomputed when selected country changes) ──
    int m_lastPanelCountryId = -1;
    int m_cachedProvCount = 0;
    float m_cachedCountryIncome = 0;
    int m_cachedIndustryCount = 0;
    long long m_cachedCountryPop = 0;
    Vector2 m_cachedAvgCompass{0,0};
    int m_cachedAvgCompassCount = 0;

    // Cached computeCountryIncome (recomputed when player country changes)
    mutable int m_lastIncomeCountryId = -1;
    mutable CountryIncomeSnapshot m_cachedIncome;

    // ─── Ceasefire / Peace negotiation state ───
    bool m_inCeasefireScreen = false;
    std::string m_ceasefireTargetIso;   // ISO of country we're negotiating with
    int m_ceasefireOurMoney = 0;        // money we offer
    int m_ceasefireTheirMoney = 0;      // money we demand
    std::vector<int> m_ceasefireOurProvs;   // province IDs we cede
    std::vector<int> m_ceasefireTheirProvs; // province IDs they cede
    std::vector<int> m_ceasefireOurDropClaims;  // claims we drop (province IDs)
    std::vector<int> m_ceasefireTheirDropClaims; // claims they drop (province IDs)
    // Inline map state for ceasefire screen
    int m_ceasefireMapSrcX = 0, m_ceasefireMapSrcY = 0;
    float m_ceasefireMapZoom = 1.0f;
    bool m_ceasefireMapDragging = false;
    int m_ceasefireMapDragPrevX = 0, m_ceasefireMapDragPrevY = 0;
    int m_ceasefireSelectMode = 0; // 0=idle, 1=selecting our provinces to cede, 2=selecting claims to drop, 3=selecting their provinces to demand, 4=selecting claims they drop

    // Cached overlay buffer for ceasefire screen (rebuilt only when dirty)
    std::vector<Color> m_ceasefireOverlayBuf;
    bool m_ceasefireOverlayDirty = true;
    Texture2D m_ceasefireOverlayTex{};

    // Pending ceasefire offers sent by the player with terms.
    // Keyed by "sourceIso|targetIso". When the recipient accepts (next turn),
    // the offer is held one extra turn so effects apply on the turn after,
    // matching the request → review → apply flow described in the design.
    std::unordered_map<std::string, CeasefireTerms> m_pendingCeasefireTerms;
    // Pending ceasefire offers received and accepted by the player that are
    // now waiting to be applied on the next turn (turnsRemaining=1 → 0 then
    // applied). We store a parallel copy of the terms because the popup is
    // dismissed once the player clicks Approve.
    std::unordered_map<std::string, CeasefireTerms> m_acceptedCeasefireTerms;
    // Pulls each side's armies out of the other's territory when a war ends.
    // Returns the number of provinces cleared. See Game_TurnLogic.cpp.
    int  withdrawArmiesAfterPeace(int cidA, int cidB);
    void applyCeasefireTerms(const std::string& sourceIso, const std::string& targetIso, const CeasefireTerms& terms, bool alreadyDeducted = false);

    void drawCeasefireScreen();
    void updateCeasefireScreen();

    // ─── Turn processing state ───
    enum TurnState {
        TURN_NORMAL,        // Playing normally
    } m_turnState = TURN_NORMAL;
    int m_turnNumber = 0;

    // ─── Script engine ───
    ScriptEngine* m_scriptEngine = nullptr;
    std::vector<ScriptError> m_scriptErrors;
    float m_scriptErrorTimer = 0.0f;
    void runMapScripts();
    void drawScriptErrors();

    // ─── Map Editor ───
    MapEditor* m_mapEditor = nullptr;
    void drawMapEditor();
    void updateMapEditor();

    void processTurn();
    void processCountryTurn(int countryId);
    void processArmyMovement(int countryId);
    void processNavyMovement(int countryId);
    void processNavyCombat(int countryId);
    void cleanupSunkShips();
    void eliminateDefeatedCountries();
    void processDiplomaticRequests();
    void processUpgrades();
    void processEconomy(int countryId);
    void processPopulation();
    std::string saveStateJson();
    void loadStateJson(const std::string& json);
};
