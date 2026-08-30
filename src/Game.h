#pragma once
#include "GameStructs.h"
#include "util/LoadLog.h"
#include "comms/Transmission.h"
#include "dialog/DialogBox.h"
#include "server/ServerRuntime.h"
#include "Gamepad.h"
#include "Touch.h"
#include "UiScale.h"
// The interface in another language, and the two raylib calls that draw it.
// After UiScale.h and before anything that draws: both headers shadow raylib
// functions and the order they do it in is the order they are applied.
#include "i18n/Locale.h"
#include "i18n/Text.h"
#include "map/LandSeaMap.h"
#include "map/ProvinceMap.h"
#include "map/CountryMap.h"
#include "renderer/MapRenderer.h"
#include "Config.h"
#include "Audio.h"
#include "net/TurnSeal.h"
#include "net/TurnStore.h"
#include "net/NetProtocol.h"
#include "ScriptEngine.h"
#include "MapEditor.h"
#include "raymath.h"
#include <deque>
#include <set>
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
    /**
     * Run as a dedicated server until told to stop. See Game_Server.cpp.
     *
     * Lives on Game, and not in src/server/, for the same reason
     * runHeadlessSimulation does: hosting is Game's own code path
     * (mpOpenHost, mpHostTurnUpdate, mpResolveTurn), and a server that
     * reimplemented it would be a second set of rules to keep in step with the
     * first. This drives the existing one with nobody at the keyboard.
     *
     * Returns the process exit code: 0 for a clean stop.
     */
    int runDedicatedServer(struct ServerConfig& config, class ServerConsole& console,
                           const std::string& configPath);

    /**
     * The same server, one step at a time, for a front end that draws.
     *
     * runDedicatedServer is begin + tick-until-stopped + end. A UI calls these
     * itself so it can draw a frame between ticks -- which a function owning
     * its own blocking loop cannot allow. serverBegin returns a process exit
     * code; 0 means it is running (or that --check finished, which leaves no
     * runtime behind, so serverTick would return false immediately).
     */
    int  serverBegin(struct ServerConfig& config, class ServerConsole& console,
                     const std::string& configPath);
    bool serverTick();
    void serverEnd();
    /** True while a session is up, so a front end knows there is something to draw. */
    bool serverRunning() const { return m_srv != nullptr; }

private:
    bool     srvInLobby() const;
    bool     srvInGame() const;
    uint32_t srvPlayersHoldingCountries() const;
    uint32_t srvConnectedPlayers() const;
public:

    /**
     * Self-play training or measurement with no window. See Game_Server.cpp.
     *
     * The same runAITraining/runAIEvaluation the game binary calls -- but
     * reachable from the SERVER binary, which links no renderer, so a week of
     * self-play can run on a headless box. Until this existed every AI mode
     * went through init(), and init() is where InitWindow() lives.
     */
    struct HeadlessAIOptions {
        std::string dataDir;
        bool train = true;
        int  maps = 0, turns = 0, countries = 0, difficulty = 2;
        unsigned seed = 0;
        int  workerId = -1, workerCount = 0;
        bool vsRandom = false, scenarios = false;
        std::string vsModel;
    };
    int runHeadlessAI(const HeadlessAIOptions& options);
    static constexpr int SPC_CID = 65533;
    static constexpr int UNC_CID = 65534;
    static constexpr int BLC_CID = 65535;

    // Hard ceiling on a single province's population. Per-turn growth is
    // multiplicative, so over the thousands of turns an AI training run plays
    // it compounds without bound — logs showed provinces at ~3e17, a few
    // orders of magnitude from overflowing long long and swamping every
    // pop-derived economy/AI signal. 10 billion is well above any plausible
    // in-game province and far below the point where the arithmetic degrades.
    static constexpr long long MAX_PROVINCE_POP = 10000000000LL;

    // Baseline population growth, in percent per turn, before research. The
    // world grew at 0.5%/turn for a long time as an undocumented side effect of
    // the default deportation policy, which reached only provinces that had a
    // minority and scaled with how many of them there were. This replaces it
    // with a rule that applies to every province once a turn, and it is set
    // lower because it now covers every province rather than a handful per
    // country. 0.25%/turn is ~2.7x over a 400-turn game before research.
    // This is the tuning knob for how fast the world (and, through
    // maxRecruit = pop/5, every army) grows.
    static constexpr float BASE_POP_GROWTH_PCT = 0.25f;

    friend class ScriptEngine;
    friend class AISystem;
    /**
     * The order-validation test, which drives mpApplyOrders directly.
     *
     * A friend rather than a public hook: that function is the boundary
     * between this world and bytes a stranger chose, and the one thing worth
     * testing about it is what it does with bytes no honest client would ever
     * send. Reaching it through the lobby would mean standing up a socket and
     * a session to test a pure function of its input.
     */
    friend struct OrderValidationTest;

    /**
     * The doctrine-rules test. A friend only so it can stand a world up: the
     * rules it checks are public, but loading a map without a window is not.
     */
    friend struct PolicyRules;

    Game();
    ~Game();

    bool init(int screenW, int screenH, const char* title);
    void run();
    void shutdown();

    // Public wrapper for loading a save file from command line
    void loadSaveAndStart(const std::string& savePath);

    /**
     * Straight into the tutorial, as the menu's "?" button does it.
     *
     * Public because the command line is the other way in: --tutorial. The
     * button is otherwise the only caller, which makes the one thing worth
     * checking -- that a tutorial world leaves no save behind -- reachable
     * only by hand.
     */
    void startTutorial();

    /**
     * Offer a piece of the interface as something the tutorial may point at.
     *
     * Called by whatever draws the element, every frame, with the rectangle
     * it just used. The register is cleared each frame for the same reason it
     * is filled there: the only code that knows where a button ended up is
     * the code that put it there, and a cached rectangle is a rectangle that
     * is wrong the first time the window is resized.
     *
     * Names are what scripts write, so they are part of the data format:
     * "tab.economy", "button.end_turn". Renaming one silently breaks a
     * script, so unresolved names are reported rather than ignored -- see
     * drawTutorialPointer.
     */
    void offerUiTarget(const std::string& name, Rectangle r);

    /**
     * Cap this run at `budget` (0..1) of the machine, without persisting it.
     *
     * Backs `--resource-limit`. The same value the F10 panel and the settings
     * slider drive, applied for the length of the process only — a limit typed
     * on a command line describes this invocation, not the player's preference,
     * and writing it into config.json would cap the next ordinary game too.
     */
    void setSessionResourceLimit(float budget);


    // Headless AI self-play training (`--train-ai`): generate a procedural
    // map, play N turns with every country AI-driven, then rotate to a fresh
    // map so the model never overfits one geography. Model persists to
    // data/ai/model.bin between maps and runs.
    void runAITraining(int numMaps, int turnsPerMap, int numCountries, unsigned int baseSeed);

    /**
     * Run this process as one worker of a parallel training pool.
     *
     * `id` of 0..count-1 selects this worker's own model file
     * (data/ai/model.w<id>.bin) and the peers it periodically averages toward.
     * Without this a second --train-ai process would train against the same
     * data/ai/model.bin as the first and the two would overwrite each other
     * every minute, which is worse than not running the second one at all.
     */
    void setAIWorker(int id, int count);

    // Headless AI MEASUREMENT (`--eval-ai`). Plays the trained model over a
    // fixed set of seeded maps without learning from them, and reports what it
    // actually did: how often maps resolve, how concentrated the world ends up,
    // how much war and diplomacy per country-turn, how many amphibious
    // operations reach a shore, how many calls to arms are answered.
    //
    // Training's reward sparklines cannot answer "did it get better", because
    // the reward function itself keeps changing and a rising line may only mean
    // the yardstick moved. This is the yardstick that does not move: the model
    // is loaded read-only, sampling comes from the difficulty setting rather
    // than the training exploration schedule, and the seeds are constants — so
    // two runs against two model files are directly comparable.
    // `vsRandom` splits every map's countries into two matched cohorts: half
    // driven by the model, half picking uniformly at random from the same
    // validity masks with the same reflexes and the same restraint constants.
    // The report then answers the only question with an absolute answer — does
    // the trained policy beat a coin flip, and by how much.
    // `opponentModel` names a model file to give that control cohort instead of
    // dice. Random is a FLOOR: it never improves, so once a model clears it the
    // ratio keeps climbing without saying anything about how well the AI plays.
    // A named opponent is a rung — beat this file, then pin a better one — and
    // it is how a target like "as good as an intermediate player" becomes
    // something a run can pass or fail rather than a matter of opinion.
    // False when the run produced no measurement at all — today that means an
    // `opponentModel` that would not load. The caller must exit non-zero on it:
    // the summary is skipped, so a silent success would be a missing report
    // that looks exactly like a quiet one.
    /**
     * Would `observerCid` be able to see that this stated reason is false?
     *
     * The believability rule, and the whole of it. A lie is safe exactly when
     * the other side cannot check it against what it already knows, and this
     * game already decides what is knowable: wars and borders are on the map,
     * war weariness and intent are not. So the filter is not a table of
     * plausible excuses somebody has to maintain -- it is a question asked of
     * the same state the observer can see anyway.
     *
     * Nothing is forbidden by this. Both the AI and the player may state a
     * reason this returns true for; it simply costs them, because the other
     * side can see it is not so. What it exists for is to let the AI CHOOSE
     * well -- an opponent whose lies fall apart the moment you look at the map
     * is not a liar, it is a bug.
     */
    bool refusalIsContradicted(int observerCid, int subjectCid, int reason) const;
    /**
     * The player, having refused `toIso`, tells them why — or does not.
     *
     * The other half of the same channel the AI uses, and the reason this is a
     * mechanic rather than the AI narrating at the player: every statement in
     * the game travels this way, in both directions, and neither side can say
     * anything the other could not. What the recipient does with it is up to
     * them; today it is recorded, and it is where credibility will attach.
     */
    void tellRefusal(const std::string& toIso, int statedReason);
    /**
     * Would `observerCid` be able to see that this stated war goal is false?
     *
     * The same question refusalIsContradicted asks, over the same public state:
     * claims, borders and the size of the map are all things anyone can look
     * at. A country announcing it is recovering land it holds no claim to has
     * said something the whole world can check.
     *
     * WAR_GOAL_CONQUEST is never contradicted, and that is the interesting
     * asymmetry: the honest goal is the one nobody can argue with. A country
     * that wants a pretext has to find one that happens to be true.
     */
    bool warGoalIsContradicted(int observerCid, int attackerCid,
                               int defenderCid, int goal) const;
    /** What `attackerIso` announced when it declared on `defenderIso`. */
    int statedWarGoal(const std::string& attackerIso,
                      const std::string& defenderIso) const;

    // ─── Credibility ────────────────────────────────────────────────────
    //
    // What `hearerIso` thinks `speakerIso`'s word is worth, from 0 to 1.
    //
    // PER PAIR, not one public reputation, and the asymmetry is the reason: the
    // evidence that breaks a claim is public, but the CLAIM is not. Only the
    // country a thing was said to knows it was said, so only that country can
    // put the two halves together. It also makes lying a decision with a shape
    // -- you can mislead an enemy and stay straight with an ally, and the cost
    // lands where you told the story.
    //
    // SOFT, never a gate. It leans on a diplomacy answer through the same logit
    // bias the NAP willingness and the call reluctance already use; no request
    // is ever refused because of it, and nothing is hidden or disabled. A
    // country nobody believes can still ask, and can still be told yes.
    float credibility(const std::string& speakerIso,
                      const std::string& hearerIso) const;
    /** Knock `speakerIso`'s word down with `hearerIso`. Clamped at zero. */
    void  loseCredibility(const std::string& speakerIso,
                          const std::string& hearerIso, float amount);
    /** Remember a claim conduct may yet disprove. See SpokenClaim. */
    void  recordSpokenClaim(const std::string& speakerIso,
                            const std::string& hearerIso, int kind,
                            const std::string& aboutIso = std::string());
    /**
     * Called when `speakerIso` declares war: anyone it recently told it was too
     * exhausted to fight has just watched it start one.
     */
    void  claimsBrokenByDeclaration(const std::string& speakerIso);
    /**
     * Called when a province changes hands: a war announced as recovering what
     * is ours, taking ground we never claimed, is the pretext coming apart.
     */
    void  claimsBrokenByConquest(int winnerCid, int loserCid, int provinceId);
    /** Age out claims nobody can reasonably be held to any longer, and forgive. */
    void  ageCredibility();
    /**
     * One statement, judged once, wherever it came from.
     *
     * Both sides route through here so neither gets a rule of its own: a claim
     * the hearer can already disprove costs immediately, and one only conduct
     * can disprove is written down and watched. The player's word is worth
     * exactly what an AI's is, and is spent the same way.
     */
    void  noteRefusalStatement(const std::string& speakerIso,
                               const std::string& hearerIso, int statedReason);
    void  noteWarGoalStatement(const std::string& attackerIso,
                               const std::string& defenderIso, int statedGoal);

    // `scenarios` measures on the maps data/STDmaps ships — the worlds a player
    // opens — instead of generated archetypes. Opt-in and never mixed with
    // generated maps in one run: a mean over both describes neither, and every
    // previously stored result was taken on generated worlds.
    bool runAIEvaluation(int numMaps, int turnsPerMap, unsigned int baseSeed,
                         int difficulty, bool vsRandom,
                         const std::string& opponentModel = std::string(),
                         bool scenarios = false);

    /**
     * The seat to play for an absolute benchmark run, as "map:ISO" -- e.g.
     * "1939:USA". See m_benchSeatIso.
     */
    /**
     * Start a PERSON on a benchmark seat: same map, same country and the same
     * frozen scripted opposition the model faces. See startBenchSeat.
     */
    void startBenchSeat(const std::string& spec, int untilTurn);

    /**
     * Play a benchmark seat BY HAND, one turn per call. See the definition.
     *
     * Stateless on purpose: it loads the save, applies the choices, resolves a
     * turn and exits, so a caller with no session of its own can play a whole
     * seat by invoking it repeatedly. `doList` is "e:1,w:2" -- module letter,
     * action index, from the menu the previous call printed.
     */
    bool runBenchAgent(const std::string& seatSpec, const std::string& pipePath,
                       unsigned int seed, int untilTurn);
    /** Scope a benchmark rush to the seat's neighbours. See m_benchRushNeighbours. */
    void setBenchRushNeighbours(int howMany) { m_benchRushNeighbours = howMany; }
    void setBenchSeat(const std::string& spec) {
        const size_t colon = spec.find(':');
        if (colon == std::string::npos) { m_benchSeatIso = spec; return; }
        m_benchSeatMap = spec.substr(0, colon);
        m_benchSeatIso = spec.substr(colon + 1);
    }

    // Unattended self-play on a REAL scenario (`--simulate`). Loads a shipped
    // .odmap through the ordinary menu pipeline, creates the .odsv the menu
    // would have created, plays `turns` turns with every country AI-driven,
    // and leaves the save behind with its full turn history intact.
    //
    // Two callers wanted the same thing for different reasons and this is the
    // overlap: --export-timelapse needs a save that HAS a history (a fresh
    // world has one turn and nothing to animate), and the per-platform smoke
    // test needs one command that proves a build can load a map, resolve turns
    // and write a save without a human driving it. Training cannot serve
    // either -- it generates its own maps and deliberately never writes a save.
    /// Measure a batch of strings per language; see --measure-text.
    bool measureTextJobs(const std::string& inPath, const std::string& outPath);

    bool runHeadlessSimulation(const std::string& mapPath, int turns,
                               const std::string& worldName);

    // Scripted screenshot tour (`--screenshots <dir> [save.odsv]`). Walks the
    // game through a fixed list of screens and writes a PNG of each.
    //
    // The alternative was a human with a screenshot key, and that is exactly
    // what makes documentation rot: the shots are taken once, the UI moves, and
    // nobody re-takes twelve pictures by hand. This can be re-run after any
    // change, which is the only reason the README's images can be trusted to
    // still be the game.
    void beginScreenshotTour(const std::string& outDir, const std::string& savePath);

    /**
     * --tutorial-walk: play every route of the tutorial, page by page, and
     * report every page that points at nothing, waits on a condition that
     * never comes true, or offers a choice that opens a script which is not
     * there. See Game_TutorialWalk.cpp. Needs a window: the pointer check asks
     * whether the game DREW the thing, which only a real frame can answer.
     */
    void beginTutorialWalk();
    /// How many problems the walk found. Non-zero fails the run.
    int tutorialWalkProblems() const { return (int)m_walkProblems.size(); }

private:
    // Advances the tour by one frame. Returns false when there is nothing left
    // to shoot, which is the signal for run() to exit.
    bool tickScreenshotTour();
    void applyLanguageForShot(const char* code);
    /// Advances the walk by one frame. False when every route has been walked.
    bool tickTutorialWalk();
    /// Drives the game into the state a page's `until=` is waiting for.
    void walkSatisfy(const std::string& cond);
    void walkTakeProvince(int pid);
    void walkSelect(int pid);
    int  walkProvinceOf(const std::string& iso) const;
    std::string walkDiploTarget() const;
    bool walkAtWarWith(const std::string& iso) const;
    void walkProblem(const std::string& what);
    /// Acts on a choice the moment it is made. See Game_Comms.cpp.
    bool consumePickedChoice();
    void walkPointerVerdict();
    void walkCheckPage(const dlg::Page& page);
    void walkCheckReachable(const dlg::Page& page);
    /// Every option on every menu, taken for real. False when it is finished.
    bool tickBranchDrill();
    /// The last-resort exit, exercised end to end. False when it is finished.
    bool tickEscapeDrill();
    void walkDismissPopup();
    bool walkScriptExists(const std::string& name) const;
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
        SCREEN_MODS,
        SCREEN_ACCOUNT,
        SCREEN_MULTIPLAYER
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
    /**
     * Why the last load failed, in words a player can act on.
     *
     * A failed load sends the player back to the main menu and, until this
     * existed, said nothing at all: they picked a scenario, the loading screen
     * flickered, and they were back where they started. That is reported as
     * "scenarios do not load", and it is indistinguishable from a broken
     * scenario list, a corrupt download or a missing data folder -- which need
     * completely different fixes. The reasons were written to LoadLog(), which
     * on a Windows GUI build goes nowhere at all.
     *
     * Cleared when a load starts, drawn on the menu while it is set.
     */
    std::string m_loadError;
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
    // One turn's changes, from a save or from a host. See the definition.
    void applyTurnDelta(const struct TurnDelta& delta);
    void rebuildOwnershipPixels();
    void startNewGame(const std::string& mapName);
    void startNewGameWithName(const std::string& mapName, const std::string& worldName);

    // Enter gameplay as a country: unlocks the techs its built level implies,
    // stamps the save, hands the map back to the player and sets SCREEN_PLAYING.
    // Shared by the country-select popup and Quick Start.
    void commitPlayerCountry(int countryId);

    // Quick Start: main menu straight to a turn, no scenario or country asked
    // for. See the block comment in Game_Menus.cpp.
    void startQuickStart();
    // Which country Quick Start hands over. Valid only once a map is loaded --
    // it reads m_playableCountryIds. Returns 0 if there is nothing playable.
    int pickQuickStartCountry() const;
    // Set by startQuickStart, read and cleared by LOAD_FINALIZE.
    bool m_quickStartPending = false;

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
    // Touch-only: the pause menu has no other entrance without an Escape key.
    void drawTouchMenuButton();
    void drawEconomy();
    void updateEconomy();
    void drawEconomyGlobal(int centerX, int startY);
    void drawEconomyLocal(int centerX, int startY);
    int drawBreakdownRow(int x, int y, int valX, const char* label, const char* value, Color col, bool highlight);
    void recordIncomeSnapshot();

#ifdef __EMSCRIPTEN__
    // Canvas backing store, CSS box and raylib's screen size, all set to the
    // browser viewport. Called at startup and whenever the window changes: if
    // any two of them disagree the browser scales one onto the other and every
    // mouse coordinate is wrong by that factor. See Game::init().
    void odFitCanvasToWindow();
    int m_odCanvasCheck = 0;   // frames until the next size re-check

    // The frame painted while an account request blocks the frame thread.
    // A STATIC MEMBER rather than a free function so it can still be handed
    // over as a plain function pointer while reading the player's accent
    // colour out of m_config. See NetWaitHook in net/HttpClient.h.
    static void odAccountWaitFrame(double elapsedMs);
#endif

    // The "Play as X?" popup on the country-select screen. Returned as one
    // layout rather than recomputed in both places: draw and update each had
    // their own copy of popW/popH/btnY, so a change to the box size moved the
    // buttons on screen without moving where a click counted.
    struct CountryConfirmLayout {
        Rectangle box{}, flag{}, yes{}, no{};
        float questionY = 0;
    };
    CountryConfirmLayout countryConfirmLayout() const;

    // Main menu
    void drawMainMenu();
    void updateMainMenu();

    // .odstate: the whole of the player's data/ in one file, and the way back
    // in. Every build has it. See OdState.h.
    //
    // Nothing here acts on a single click. Saving asks for a name first, and
    // loading asks which file and -- if the archive carries mods -- whether the
    // player really wants executable content put back. A one-click restore that
    // silently reinstates mods is the version of this feature not to build.
    enum OdStatePrompt {
        ODP_NONE = 0,
        ODP_SAVE_NAME,     // typing the filename to write
        ODP_PICK_FILE,     // desktop: choosing among exports/*.odstate
        ODP_MODS_WARNING,  // confirming an archive that contains mods
    };
    void openOdStateSave();
    void openOdStateLoad();
    void drawOdStatePrompt();
    void updateOdStatePrompt();
    void applyOdStateLoad(const std::string& path, bool modsAccepted);
    void setOdStateMsg(const std::string& msg, bool bad);

    OdStatePrompt m_odStatePrompt = ODP_NONE;
    std::string m_odStateName;                  // edited in ODP_SAVE_NAME
    std::vector<std::string> m_odStateFiles;    // listed in ODP_PICK_FILE
    int m_odStatePick = 0;
    std::string m_odStatePending;               // archive awaiting the mods answer
    int m_odStatePendingMods = 0;
    std::string m_odStateMsg;                   // shown under the menu buttons
    float m_odStateMsgTimer = 0.0f;             // seconds left; <= 0 means hidden
    bool m_odStateMsgBad = false;               // colours it as a failure
#ifdef __EMSCRIPTEN__
    // The browser hands a chosen file back asynchronously, so the menu asks
    // once a frame whether one has arrived.
    void pollOdStateImport();
#endif
    // The "!" beside the version in the main menu, and the panel it opens.
    // Returns the clickable rect so update and draw cannot disagree about
    // where the badge is.
    Rectangle updateBadgeRect() const;
    void drawUpdatePanel();
    bool updatePanelClick(Vector2 mouse);
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

    // --- account screen (src/Game_Account.cpp) --------------------------------
    void openAccountMenu();
    void updateAccountMenu();
    void drawAccountMenu();
    void drawAccountField(int x, int y, int w, int h);
    void drawAccountProviders(int centerX, int y, const struct AccountInfo& info);

    std::string m_accountNote;
    float m_accountNoteTimer = 0.0f;
    // The account id is revealed on request rather than always shown: it is
    // not a credential, but it is a stable identifier and this screen gets
    // streamed.
    bool m_accountShowId = false;

    std::string m_accountNickField;
    bool m_accountFieldFocused = false;
    // The stored token is checked once per launch. Retrying on every visit
    // would hammer the service when it is down, and the state already held is
    // the right answer in the meantime.
    bool m_accountRestoreTried = false;

    // --- multiplayer (src/Game_Multiplayer.cpp) ------------------------------
    //
    // One screen with four pages rather than four screens: the whole flow is
    // hub -> host/join -> lobby, and back always means "the previous page",
    // which a single screen expresses and four separate ones would not.
    enum class MpPage : uint8_t { Hub = 0, Join, HostSetup, Lobby };

    void openMultiplayerMenu();
    void updateMultiplayerMenu();
    void drawMultiplayerMenu();
    void drawMpHub(Vector2 mouse, bool click);
    void drawMpJoin(Vector2 mouse, bool click);
    void drawMpHostSetup(Vector2 mouse, bool click);
    void drawMpLobby(Vector2 mouse, bool click);
    /** The store the host has selected, as a kind rather than an index. */
    TurnStoreKind mpStoreKind() const;
    void mpStartHosting();
    /** Data directory probe for headless modes. See Game_Server.cpp. */
    bool srvResolveDataDir(const std::string& override);
    void mpOpenHost();

    // --- the lobby -> game bridge ---
    //
    // Loading a world is asynchronous and shared with singleplayer, so the
    // multiplayer paths set a PURPOSE before starting a load and pick it up in
    // mpOnWorldLoaded() when the loader finishes. Without it the loader would
    // drop a host into the singleplayer country-select screen, which is not
    // where either side of a network game belongs.
    enum class MpLoad : uint8_t { None = 0, HostOpen, JoinApply };
    MpLoad      m_mpLoad = MpLoad::None;
    int         m_mpMapIndex = 0;
    std::string m_mpMapId;                    // portable map identity, not a path
    /**
     * Encoded ModAttestation of the `both`-side mods a client must match.
     *
     * Set before mpOpenHost(), which hands it to NetHost::Config::requiredMods.
     * Empty means "no requirement", which is what an ordinary in-game host
     * still does. See ModAttest.h -- this is an integrity check, not an
     * anti-tamper one, and the header is emphatic about the difference.
     */
    std::string m_mpRequiredMods;

    /** Live dedicated-server state; null when no server is running. */
    std::unique_ptr<ServerRuntime> m_srv;
    std::vector<uint8_t> m_mpPendingSnapshot; // held while the world loads
    uint16_t    m_mpMyCountry = 0;

    /** Map id -> a path on THIS machine. False when the map is not installed. */
    bool mpResolveMap(const std::string& id, std::string& pathOut, std::string& nameOut);
    /** Tell joiners what this world contains. Host only. */
    void mpPublishCountries();
    /** Called by the loader when a multiplayer load finishes. */
    void mpOnWorldLoaded();
    /** Build the whole-world payload. Host only. */
    std::vector<uint8_t> mpBuildSnapshot();
    /** Load the world a snapshot describes. Client only. */
    void mpApplySnapshot(const std::vector<uint8_t>& payload);
    /** Enter the game as `countryId`. */
    void mpEnterGame(uint16_t countryId);

    // --- the turn loop ---
    //
    // In a network game the HOST resolves turns and nobody else does. A client
    // that ran processTurn() locally would produce its own answer to what
    // happened, and two machines would quietly disagree about the world. So a
    // client submits orders and waits for the delta the host actually produced.

    /** True while a network game is running and this machine is not the host. */
    bool mpIsClient() const;
    /** True while this machine is the authority for a running network game. */
    bool mpIsHost() const;

    /** This country's pending orders, for submission. */
    std::vector<uint8_t> mpSerializeOrders(int countryId) const;

    /**
     * Merge a peer's submitted orders into this world.
     *
     * Every order is checked against what `countryId` actually owns and dropped
     * otherwise. This is where "the server is authoritative" stops being a
     * design note: a client can ask for anything, and only orders over its own
     * provinces and ships survive.
     */
    void mpApplyOrders(int countryId, const std::vector<uint8_t>& payload);

    /** Drive the host's turn: collect, resolve when due, broadcast. */
    void mpHostTurnUpdate();
    /** Resolve now: apply everyone's orders, process, send the delta. */
    void mpResolveTurn();
    /** Apply a delta the host produced. Client only. */
    void mpApplyDelta(uint32_t turnNumber, const std::vector<uint8_t>& payload);
    /** Called instead of processTurn() when this machine is a client. */
    void mpSubmitTurn();

    class TurnRunner* m_mpTurns = nullptr;
    /** Set once this client's orders are in; cleared when a delta lands. */
    bool m_mpWaitingForTurn = false;

    // ------------------------------------------------------- long form ----
    //
    // A long-form game moves its turns through a store rather than over the
    // connection, because the host is expected to be OFFLINE between turns --
    // for days. Players connect straight to the host, so when it is away there
    // is no lobby to reach and the store is the only thing both sides share.

    /** Pump the store: publish what is due, collect what has arrived. */
    void mpStoreUpdate();
    /** Take one finished store request. */
    void mpHandleStoreResult(const struct TurnStoreResult& result);
    /** Point the runner at this session. Host and client both call it. */
    void mpConfigureStore();
    /** True when this game moves turns through a store at all. */
    bool mpLongForm() const;

    /** Publish a resolved turn. Host only; a no-op outside long-form. */
    void mpPublishTurn(uint32_t turnNumber, const std::vector<uint8_t>& packed);
    /** Seal and publish this client's orders. */
    bool mpPublishOrders(uint32_t turnNumber, const std::vector<uint8_t>& orders);
    /** Ask the store for anything this machine is waiting on. */
    void mpPollStore();

    /**
     * Remember a long-form game this machine JOINED, beside its save.
     *
     * Players connect straight to the host, so once the host closes there is no
     * lobby left to ask -- and the store details and key arrived only over that
     * connection. Without this, a player who joined on Monday and reopened the
     * game on Thursday would have a world, a country, and no way to submit
     * anything or to discover the turns that were played meanwhile.
     *
     * `<save>.odjoin`, and it holds the session key, so it is written with the
     * same care as the host's `.odkey`: never in a file anybody is encouraged
     * to pass around.
     */
    void mpSaveJoinedSession() const;
    /** Read it back. False when there is none, which is the normal case. */
    bool mpLoadJoinedSession(const std::string& savePath);
    static std::string mpJoinedSessionPath(const std::string& savePath);

    // ----------------------------------------------------------- manual ----
    //
    // No infrastructure at all: the game shows a block of text and takes one
    // back, and the players carry it between themselves however they like.
    // Everything else about a long-form turn is unchanged -- orders are still
    // sealed, a turn bundle is still the same bytes -- so this is a transport
    // swap and not a second set of rules.

    /** Put a resolved turn on screen for the host to copy out. */
    void mpManualOfferTurn(uint32_t turnNumber, const std::vector<uint8_t>& packed);
    /** Put this player's sealed orders on screen to copy out. */
    void mpManualOfferOrders(uint32_t turnNumber, const std::vector<uint8_t>& sealed);
    /** Read whatever is in the paste box and route it. Says what happened. */
    void mpManualApplyPasted();
    void mpDrawManualExchange(int screenW, int screenH);

    /** The block to copy out. Empty when there is nothing waiting. */
    std::string m_mpManualOut;
    /** What was pasted in, waiting to be applied. */
    std::string m_mpManualIn;
    /** Whether the exchange panel is showing. */
    bool m_mpManualOpen = false;

    /** Named apart from `m_mpStore`, which is the setup screen's picker index. */
    class TurnStoreRunner* m_mpStoreRunner = nullptr;

    /**
     * The key that seals orders. Host: minted or loaded beside the save.
     * Client: handed over by the host on the authenticated connection.
     */
    TurnSealKey   m_mpSealKey;
    TurnStoreKind m_mpStoreKind = TurnStoreKind::DurableObject;
    std::string   m_mpSessionCode;

    /**
     * This machine's own pseudonym on this server.
     *
     * Needed to address its orders and to seal them -- the psid is bound in as
     * associated data, so orders sealed under the wrong one open as nothing.
     */
    std::string m_mpMyPsid;

    /** Turn whose orders the host is currently collecting from the store. */
    uint32_t m_mpStorePollTurn = 0;
    /** Psids already asked for this turn, so a poll does not queue twice. */
    std::unordered_set<std::string> m_mpStoreAsked;
    /** Seconds on the clock at the last poll; the store is not free. */
    double m_mpStoreNextPoll = 0.0;
    void mpBeginJoin(const std::string& address, const std::string& code);
    void mpLeave();
    /** Tear multiplayer down at exit. Defined where the types are complete. */
    void mpShutdown();
    void mpDrainEvents();
    void mpNote(const std::string& text, bool error = false);

    MpPage      m_mpPage = MpPage::Hub;
    std::string m_mpNote;
    bool        m_mpNoteError = false;
    float       m_mpNoteTimer = 0.0f;

    // Which text box has the keyboard. -1 is none.
    int         m_mpFocus = -1;
    std::string m_mpAddressField;
    std::string m_mpCodeField;
    std::string m_mpNameField;
    std::string m_mpPortField;
    /**
     * Seconds per turn, as typed.
     *
     * A text box rather than a preset list: on a live server the right interval
     * is whatever that group actually plays at -- 45 seconds for a quick game,
     * 20 minutes for a slow one -- and a fixed set of choices is always missing
     * the one somebody wants. Empty or 0 means long-form (no timer at all).
     */
    std::string m_mpTurnField;

    int         m_mpSelected = -1;      // index into the server book
    bool        m_mpBindAll = false;
    bool        m_mpListed = false;
    int         m_mpMaxPlayers = 8;

    /**
     * Seconds per turn, read from the text box.
     *
     * Derived rather than cached, because a cached copy is only as fresh as the
     * last time the tab holding that box happened to be drawn -- and a host who
     * never opened it would run the game on a stale value.
     */
    int mpTurnSeconds() const;

    // --- what the host can configure ---
    int  m_mpAssignment = 1;    // NetAssignment: 0 host-assigns, 1 players pick
    int  m_mpLateJoin  = 1;     // NetLateJoin:  0 refuse, 1 spectate
    int  m_mpAbsent    = 0;     // NetAbsent:    0 AI plays, 1 idle
    int  m_mpStore     = 0;     // TurnStoreKind index; see mpStoreKind()
    bool m_mpAnonymous = false;
    bool m_mpDedicated = false;
    /** Which page of host settings is showing: 0 basics, 1 rules, 2 turns. */
    int  m_mpSetupTab = 0;

    /**
     * The join screen makes the player acknowledge that the host will see
     * their IP. Reset per join, never remembered: it is the one thing about
     * joining that cannot be undone afterwards.
     */
    bool m_mpIpWarningAccepted = false;

    /** Lobby has a Players tab showing who is human and who the AI plays. */
    bool m_mpPlayersTab = false;

    /** The country picker is open, covering the lobby. */
    bool m_mpPickingCountry = false;
    int  m_mpCountryScroll = 0;

    /**
     * Whether anyone outside this network can actually reach the game.
     *
     * A host cannot tell by looking. "It works on my machine" is exactly what a
     * LAN-only bind looks like from the host's chair, and the failure lands on
     * the players instead -- so the game checks rather than leaving them to
     * find out.
     */
    enum class MpReach : uint8_t { Unknown = 0, Testing, Reachable, Unreachable };
    MpReach     m_mpReach = MpReach::Unknown;
    std::string m_mpReachNote;
    float       m_mpReachTimer = 0.0f;
    class WebSocket* m_mpReachProbe = nullptr;

    /** What a host hands to players, as one block of text. */
    std::string mpInviteText() const;
    void mpBeginReachTest();
    void mpUpdateReachTest();

    /**
     * Start a tunnel automatically when hosting, if one is installed.
     *
     * On by default when a provider is available: the tunnel is the recommended
     * way to host, and a host who has already installed cloudflared has plainly
     * opted into it. It never installs or downloads anything -- see Tunnel.h.
     */
    /**
     * Continue a saved campaign instead of starting a new world.
     *
     * A network game played over weeks is the case the whole turn-delta design
     * was for, and it was unreachable until now: hosting could only ever begin
     * a fresh world from a map.
     */
    /** Start hosting was pressed with no turn timer; awaiting a second press. */
    /**
     * The player has agreed to the terms and privacy policy.
     *
     * Asked once, before the first sign-in, and remembered -- agreeing is a
     * decision about the service, not about this launch of the game.
     */
    bool m_accountAgreed = false;
    bool m_mpConfirmSlow = false;
    /** Start game was pressed while somebody was still choosing. */
    bool m_mpConfirmStart = false;
    /** Scroll offset for the lobby roster. */
    int  m_mpRosterScroll = 0;
    /** Console: scroll, and which row is armed for a kick/ban. */
    int  m_mpConsoleScroll = 0;
    std::string m_mpArmedBan;
    /** Which spectator and which free country the console is offering. */
    int m_mpSeatWho = 0;
    int m_mpSeatWhich = 0;
    /**
     * Everyone goes back to the lobby once this turn resolves.
     *
     * Set during a turn, acted on after it -- stopping mid-turn would throw
     * away orders people had already given.
     */
    bool m_mpReturnAfterTurn = false;
    bool m_mpResume = false;
    /** Search box for the map list, and scroll offsets for both lists. */
    std::string m_mpMapSearch;
    int m_mpMapScroll = 0;
    int m_mpSaveScroll = 0;
    /** Index of the save awaiting a confirmed delete, or -1. */
    int m_mpSaveDeleting = -1;
    int  m_mpSaveIndex = 0;
    std::vector<std::string> m_mpSavePaths;   // full paths, saves/multiplayer
    std::vector<std::string> m_mpSaveNames;   // what the host sees
    void mpRefreshSaves();
    /** Write the current seating beside the save, for the next session. */
    void mpSaveSeats();
    /**
     * The host declaring its own orders finished.
     *
     * The host plays too, and its orders are already in this world -- but the
     * lobby did not know that, so the host sat in missingSubmissions forever
     * and a game with no turn timer could never resolve at all.
     */
    void mpHostReady();
    /**
     * Each player's own deadline for this turn, host-side.
     *
     * Not one shared clock: somebody who joins mid-turn has not had the same
     * time as everyone else, and treating them as late the moment they arrive
     * is how a latecomer loses a turn they never got to play. Kept off the
     * wire -- the host is the only machine that decides anything from it.
     */
    std::unordered_map<uint16_t, long long> m_mpDeadlineMs;
    /** Milliseconds this player has left, or -1 when there is no timer. */
    long long mpDeadlineLeft(uint16_t peerId, long long nowMs) const;
    /**
     * When this turn runs out, on a CLIENT's own clock.
     *
     * The host sends how long is left, not a wall-clock time: the two machines
     * do not agree on what time it is, and a shared absolute instant would be
     * wrong by whatever their clocks differ by. Anchored on arrival instead.
     * 0 means no timer.
     */
    long long m_mpTurnEndsAtMs = 0;
    /** Take back "ready". Works for host and client alike. */
    void mpUnready();
    /** Whether this player has already declared for this turn. */
    bool mpAmReady() const;
    /** Resolve now, without waiting for the stragglers. Host only. */
    void mpForceResolve();
    /** Who the turn is waiting on, drawn above the Ready button. */
    void drawMpTurnPanel(int x, int bottomY);
    /** The host's server console: what this server is, and who is on it. */
    void drawMpHostConsole(int x, int top);

    bool m_mpUseTunnel = true;
    int  m_mpTunnelChoice = 0;      // index into the available providers
    class Tunnel* m_mpTunnel = nullptr;
    /** Fetches cloudflared on request. See the safeguards in Tunnel.h. */
    class TunnelInstaller* m_mpTunnelInstaller = nullptr;
    std::string mpToolsDir() const;

    class NetHost*    m_netHost = nullptr;
    class NetSession* m_netSession = nullptr;

    /**
     * Mod messages waiting to be read, across every mod.
     *
     * One queue rather than one per mod: a mod that is loaded but never calls
     * net_recv would otherwise own a queue nothing drains, and this way the
     * bound is on the game, not on however many mods are installed.
     */
    std::deque<NetModMsg> m_mpModInbox;
    class ServerBook* m_serverBook = nullptr;

    // Registering a server credential is one blocking HTTPS call, so it runs
    // off the render thread. Doing it inline froze the game for as long as the
    // request took -- on the click that is supposed to start the game.
    enum class MpRegister : uint8_t { Idle = 0, Working, Done, Failed };
    std::thread              m_mpRegisterThread;
    std::atomic<MpRegister>  m_mpRegisterState{MpRegister::Idle};
    std::string              m_mpRegisterResult;   // guarded by m_mpRegisterMutex
    std::string              m_mpRegisterError;
    std::mutex               m_mpRegisterMutex;
    bool                     m_mpHostAfterRegister = false;

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

    // ── Gearbox 1.1 backing ──────────────────────────────────────────────────
    //
    // Same rule as the block below: plain accessors, so the mod layer never
    // sees a game header and a mod never holds a pointer into game state.
    // Ship and army handles are indices into the live containers, checked on
    // every call, because a mod that cached a NavyShip* would be reading freed
    // memory the turn something sank.
    int         modShipCount() const;
    bool        modShipExists(int sid) const;
    int         modShipOwner(int sid) const;
    std::string modShipType(int sid) const;
    double      modShipLon(int sid) const;
    double      modShipLat(int sid) const;
    int         modShipHealth(int sid) const;
    int         modShipCrew(int sid) const;
    double      modShipRange(int sid) const;
    int         modArmyStackCount(int pid) const;
    int         modArmyStackOwner(int pid, int index) const;
    long long   modArmyStackSize(int pid, int index) const;
    long long   modCountryArmy(int cid) const;
    int         modProvinceFortification(int pid) const;
    int         modProvincePortLevel(int pid) const;
    bool        modOrderArmyMove(int fromPid, int toPid, int pct);
    bool        modOrderShipMove(int sid, double lon, double lat);
    bool        modOrderShipEngage(int sid, int targetSid);
    bool        modOrderShipBombard(int sid, int pid, const std::string& ammo);
    int         modResearchNodeCount() const;
    std::string modResearchNodeId(int index) const;
    std::string modResearchNodeName(int index) const;
    std::string modResearchNodeCategory(int index) const;
    int         modResearchNodeCost(int index) const;
    bool        modCountryHasResearched(int cid, const std::string& nodeId) const;
    double      modCountryResearchFunding(int cid) const;
    bool        modSetCountryResearchFunding(int cid, double value);
    double      modCountryCompassEcon(int cid) const;
    double      modCountryCompassSocial(int cid) const;
    double      modProvinceUnrest(int pid) const;
    int         modPolicyCount() const;
    std::string modPolicyId(int index) const;
    std::string modPolicyName(int index) const;
    bool        modCountryHasPolicy(int cid, const std::string& policyId) const;
    bool        modSetCountryPolicy(int cid, const std::string& policyId, bool on);

    // ── mapeditor (ABI 1.1) ──────────────────────────────────────────────────
    // Null anywhere but the editor screen. The gate every modEditor* call uses.
    class MapEditor* modEditorOrNull() const;
    const struct MapEditor::EditorProvinceData* modEditorProv(int pid) const;
    struct MapEditor::EditorProvinceData* modEditorProvMut(int pid);
    bool        modEditorActive() const;
    int         modEditorProvinceCount() const;
    int         modEditorProvinceAt(int index) const;
    long long   modEditorProvincePopulation(int pid) const;
    int         modEditorProvinceIndustryLevel(int pid) const;
    int         modEditorProvinceFortification(int pid) const;
    int         modEditorProvincePortLevel(int pid) const;
    double      modEditorProvinceResource(int pid, const std::string& which) const;
    double      modEditorProvinceCompassEcon(int pid) const;
    double      modEditorProvinceCompassSocial(int pid) const;
    bool        modEditorSetProvincePopulation(int pid, long long v);
    bool        modEditorSetProvinceIndustryLevel(int pid, int v);
    bool        modEditorSetProvinceFortification(int pid, int v);
    bool        modEditorSetProvincePortLevel(int pid, int v);
    bool        modEditorSetProvinceResource(int pid, const std::string& which, double v);
    bool        modEditorSetProvinceCompass(int pid, double econ, double social);
    std::string modEditorMapName() const;
    bool        modEditorSetMapName(const std::string& n);
    bool        modEditorSetAuthor(const std::string& a);
    bool        modEditorSetLicense(const std::string& l);

    // ── net (ABI 1.1) ────────────────────────────────────────────────────────
    /** Push the current network role into ModHostContext and ModManager. */
    void        syncModNetContext();
    bool        modNetIsMultiplayer() const;
    bool        modNetIsAuthoritative() const;
    int         modNetPeerAt(int index) const;
    std::string modNetPeerName(int index) const;
    int         modNetMaxMessageBytes() const;

    // ── neural (ABI 1.1) ─────────────────────────────────────────────────────
    int         modNeuralModuleCount() const;
    std::string modNeuralModuleName(int m) const;
    int         modNeuralActionCount(int m) const;
    std::string modNeuralActionName(int m, int a) const;
    bool        modNeuralCountryIsAI(int cid) const;
    long long   modNeuralUpdateCount() const;
    bool        modNeuralModelLoaded() const;
    int         modProvinceMinorityCount(int pid) const;
    std::string modProvinceMinorityName(int pid, int index) const;
    double      modProvinceMinorityShare(int pid, int index) const;
    double      modCountryIncomeGross(int cid) const;
    double      modCountryIncomeNet(int cid) const;
    double      modCountryArmyUpkeep(int cid) const;
    double      modCountryNavyUpkeep(int cid) const;
    bool        modCountryIsBankrupt(int cid) const;
    int         modProvinceIndustryLevel(int pid) const;
    std::string modProvinceIndustrySpecialization(int pid) const;
    double      modProvinceResource(int pid, const std::string& which) const;
    bool        modSetProvinceIndustryLevel(int pid, int level);
    bool        modProvinceIsCoastal(int pid) const;
    bool        modSeaRouteExists(double lon1, double lat1, double lon2, double lat2) const;
    bool        modPointIsLand(double lon, double lat) const;

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
    void        installModBridges();
    /** Move arrived mod messages into m_mpModInbox, for the Net module. */
    void        mpDrainModMessages();
    /** Free sounds a mod started; empty id means every mod's. */
    void        unloadModSounds(const std::string& modId = std::string());
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
    bool  m_updatePanel = false;      // the game-update panel is open
    bool  m_gameUpdateAsked = false;  // the check has been started this session
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

    // ─── Greater Diplomacy translation layer (Experimental) ───
    //
    // Three dialogs in sequence, and the order is the point: the warning is
    // never skipped, and nothing is written until after a destination has been
    // chosen. See Game_Gdtl.h and drawGdtlDialogs().
    enum class GdtlStage { None, Warning, Destination, Result };
    GdtlStage m_gdtlStage = GdtlStage::None;
    int m_gdtlMapIndex = -1;              // which map is being translated
    // Installations found by the last search. Empty and m_gdtlSearched false
    // means no search has been run -- the game has not looked at the disk.
    std::vector<std::string> m_gdtlFound;
    bool m_gdtlSearched = false;
    bool m_gdtlOk = false;
    std::string m_gdtlMessage;
    std::vector<std::string> m_gdtlNotes;
    int m_gdtlNotesScroll = 0;

    void drawGdtlDialogs();               // returns via m_gdtlStage
    void gdtlTranslateTo(const std::string& destDir);
    void gdtlDownloadInBrowser();
    void gdtlImportFromGd5();
    std::vector<Notification> m_notifications;
    void addNotification(const std::string& msg, Color color = WHITE, float duration = 6.0f);
    void updateNotifications();

    std::vector<PopupEntry> m_popupQueue;
    // Ceasefire popup: whether the itemised terms panel is expanded. Reset
    // whenever a popup is dismissed so the next one starts collapsed.
    bool m_popupShowTerms = false;
    /**
     * The reason the player will give if they reject the popup in front of them.
     *
     * Cycled with the button on the popup, reset when a popup is dismissed.
     * REFUSE_NONE — say nothing — is the default and the first option, because
     * declining to explain yourself is a move rather than an omission, and it
     * is the one a player who does not care about this system will use without
     * noticing it exists.
     *
     * The player picks from the WHOLE list, exactly as the AI does. Nothing is
     * hidden and nothing is greyed out: you may claim to be at war while at
     * peace, and the country you say it to can look at the map. See
     * Game::refusalIsContradicted, which is what the AI uses to avoid doing
     * that to itself.
     */
    int m_popupRefusalReason = REFUSE_NONE;
    /**
     * What the player will announce if they declare war on whoever's panel is
     * open. Cycled on the diplomacy panel; WAR_GOAL_NONE by default.
     *
     * Sticky rather than reset per target on purpose -- a player running a
     * campaign of "border security" wars should set it once, and one who never
     * touches it declares in silence forever, which is exactly what the game
     * did before this existed.
     */
    int m_declareWarGoal = WAR_GOAL_NONE;
    void pushPopup(PopupType type, const std::string& title, const std::string& message,
                    int countryId = 0, const std::string& action = "",
                    const std::string& sourceIso = "", const std::string& targetIso = "");
    void drawPopup();
    void updatePopup();

    // Plays the open sound for the popup that has just reached the FRONT of the
    // queue, which is the only one on screen. Queueing used to play it, so a
    // turn that produced several popups played them all at once and then showed
    // each one silently. See PopupEntry::id.
    void announceFrontPopup();
    unsigned long long m_popupNextId = 0;      // stamps each queued popup
    unsigned long long m_popupAnnouncedId = 0; // the one already chimed for

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

    /**
     * Where the pointer is, whoever is holding it.
     *
     * The single place the game asks -- which is why the controller's virtual
     * cursor is injected here rather than in nineteen screens. odPad::active()
     * is false the moment a real mouse moves, so a desktop player never notices
     * this exists.
     */
    Vector2 getMouse() const {
        // Touch first: on Android raylib's own mouse follows the finger, so
        // asking it would return the contact point rather than the cursor the
        // player is actually aiming with. See Touch.h.
        // Divided by the UI scale: the pointer arrives in PHYSICAL pixels and
        // every screen now hit-tests in the LOGICAL space odUi magnifies from.
        // Without this the cursor lands 1.5x away from whatever it is over.
        const float u = odUi::scale();
        if (odTouch::active()) { Vector2 c = odTouch::cursor(); return { c.x * m_dpiScale / u, c.y * m_dpiScale / u }; }
        if (odPad::active()) { Vector2 c = odPad::cursor(); return { c.x * m_dpiScale / u, c.y * m_dpiScale / u }; }
        return { GetMousePosition().x * m_dpiScale / u, GetMousePosition().y * m_dpiScale / u };
    }

    LandSeaMap m_landSea;
    ProvinceMap m_provinces;
    CountryMap m_countries;
    MapRenderer* m_renderer = nullptr;
    Texture2D m_politicalTex{};
    int m_screenW = 1600;
    int m_screenH = 900;
    float m_dpiScale = 1.0f;
    float m_uiScaleBase = 1.0f;         // what the PLATFORM insists on, before the player's factor
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
    bool m_draggingResourceSlider = false;
    bool m_waitingForKey = false;
    int m_rebindingAction = -1;
    int m_settingsScroll = 0;

    /**
     * Tells the audio layer where the game currently is, and ages the toast.
     * Called once per frame from run().
     */
    void updateMusic(float dt);

    /**
     * Reads the game state as a point in mood space (see Mood in Audio.h).
     *
     * Recomputed a few times a second rather than every frame: it walks every
     * province, and nothing it reads moves faster than a turn.
     */
    Mood currentMood();

    /**
     * How much map atmosphere the current view calls for, 0..1.
     *
     * Driven by the camera, not by the screen: fully zoomed out is 1, and it
     * falls to 0 as the view closes in, so a map examined province-by-province
     * sounds like the menus and the whole world sounds like a room. Menus
     * return 0 outright.
     */
    float mapAtmosphereIntensity() const;

    /**
     * The now-playing toast. Drawn by endFrame() so it lands on top of every
     * screen, including menus and the popup overlay.
     */
    void drawNowPlayingToast();
    /** Raises the toast for whatever is playing right now, if anything. */
    void showNowPlayingToast();
    /** EndDrawing, plus everything that must sit above all other drawing. */
    void endFrame();

    /** The controller's virtual cursor, drawn over everything endFrame() covers. */
    void drawPadCursor();

    TrackInfo m_toast;              // what the toast is announcing
    float m_toastTimer = 0.0f;      // seconds left; <= 0 means not shown
    Mood  m_mood;                   // cached, refreshed on m_moodStamp
    double m_moodStamp = -1.0;
    // Province count the player held when this country was first seen, which is
    // what "gaining" and "losing" are measured against. Keyed by country id so
    // loading a save or picking a new country re-baselines with no extra hook.
    int m_moodBaseline = -1;
    int m_moodBaselineCid = -1;
    // Turn the player was last at war. "Rebuilding" is peace shortly after a
    // war, which cannot be read from a snapshot -- peace looks identical either
    // side of it — so it has to be remembered.
    int m_moodLastWarTurn = -1;

    // Main-menu row the pointer was last on, so the hover sound fires once on
    // entering a row instead of once per frame it rests there. -1 = no row.
    int m_lastMenuHover = -1;

    // ─── Settings > Audio ────────────────────────
    //
    // The volume rows are dragged, not typed, so they need geometry that the
    // rest of the table-driven settings list has no concept of. There is one
    // draw path for settings (drawPauseMenu, which drawSettingsFromMenu reuses)
    // but two input paths -- the in-game one and the main-menu one -- and both
    // call updateVolumeSliders. Sharing it is what stops the two screens from
    // growing two subtly different volume controls.
    Rectangle sliderBarRect(int y, int centerX) const;

    // Every settings slider is this one. `steps` is 0 for a continuous value
    // (the volumes) and the number of stops for a stepped one (the frame cap):
    // that is the only difference between them, and it used to be the excuse
    // for two separate widgets with different geometry, different hit areas
    // and a thumb that did not sit on its own tick marks.
    void drawSliderWidget(Rectangle bar, float t, bool active, int steps) const;
    // Drives one slider for a frame and makes its noises. `t` is normalised and
    // updated in place; `owns` is the caller's drag flag. Returns true if the
    // value changed.
    bool sliderInteract(Rectangle bar, int steps, float& t, bool& owns);
    /**
     * Runs drag and hover for the three volume rows.
     *
     * Returns true when the pointer is on a slider, which tells the caller to
     * leave the click alone: without that the same press would drag the slider
     * AND "activate" the row under it.
     */
    bool updateVolumeSliders(int startY, int itemH, int centerX, int effScroll);
    /** Draws one volume row's bar. Called from the settings item-draw loop. */
    void drawVolumeSlider(int index, int y, int centerX, bool selected);
    /** Nudges a volume row by delta and applies it. Used by LEFT/RIGHT and R. */
    void adjustVolume(int index, float delta);

    // ─── Resource limiter ────────────────────────
    //
    // Not a settings row: it is driven from the runtime panel (F10 / Ctrl+L),
    // where the CPU graph is right next to the slider.
    /** Slider position (0..1) for a budget, and the inverse. */
    static float resourceBudgetSliderT(float budget);
    static float resourceBudgetFromSliderT(float t);
    /** Pushes the config value into the limiter and re-paces the frame cap. */
    void applyResourceBudget();
    // ─── Runtime resource panel (F10) ────────────
    //
    // The Settings row is fine for setting a policy before you start, but the
    // moment you actually want to throttle something is while it is running --
    // a training session you need to share the machine with, a big map that
    // turned the fans on. This is the same budget, adjustable in place, with a
    // graph of what the machine actually did so the setting can be judged
    // against a measurement rather than a guess.
    struct PerfSample {
        float cpuShare;   // process CPU time / wall time, 0..N cores
        float budget;     // the limit in force when this sample was taken
        float turnMs;     // last turn's processing time, 0 outside a game
    };
    bool m_showResourcePanel = false;
    std::deque<PerfSample> m_perfHistory;
    double m_perfLastWall = 0.0;   // wall clock at the last sample
    double m_perfLastCpu  = 0.0;   // process CPU seconds at the last sample
    float  m_lastTurnMs = 0.0f;
    // Rolling window the CPU-share throttle measures against. Short enough to
    // react within a couple of seconds of a slider move, long enough that a
    // single expensive turn does not swing it.
    static constexpr double BUDGET_WINDOW_SECONDS = 4.0;
    double m_budgetEpochWall = 0.0;   // 0 = window not started
    double m_budgetEpochCpu  = 0.0;
    bool m_draggingPanelSlider = false;
    /** Total CPU seconds this process has burned, or -1 where unsupported. */
    static double processCpuSeconds();
    /** Called once per frame; appends to m_perfHistory a few times a second. */
    void samplePerformance();
    // ~4 samples a second, 200 of them: a rolling 50 seconds, which is long
    // enough to see a limit change take effect and short enough to stay live.
    static constexpr double PERF_SAMPLE_SECONDS = 0.25;
    static constexpr size_t PERF_HISTORY = 200;
    Rectangle resourcePanelRect() const;
    /** Draws the panel. Safe to call with the panel hidden (does nothing). */
    void drawResourcePanel();
    /** Handles F10 and the panel's slider. Returns true if it ate the click. */
    bool updateResourcePanel();

    /**
     * Duty-cycles the turn loop to the configured budget.
     *
     * The simulation is single-threaded and runs as hard as it can, so capping
     * the frame rate does nothing for it -- during self-play it is the whole
     * CPU cost. Sleeping for a share of the work just done is what actually
     * hands the machine back. `workSeconds` is how long the turn took;
     * `maxSleepSeconds` bounds the pause so an interactive end-turn cannot look
     * like a hang (self-play raises it, since nothing is waiting on it).
     */
    void throttleForBudget(double workSeconds, double maxSleepSeconds = 1.0);

    int m_draggingVolume = -1;        // volume row being dragged, -1 when none
    double m_lastVolumePreview = 0.0; // rate-limits the click played while dragging
    double m_lastSliderTick = 0.0;    // shared by every slider, so a drag ticks once
    // Which button the pointer was over last frame, as a cheap hash of its
    // rect. Buttons are drawn immediate-mode with no identity of their own, so
    // this is what makes "the pointer arrived" distinguishable from "the
    // pointer is still here" -- without it every hovered button would scream
    // once a frame.
    int m_lastHoverBtn = 0;
    // Set immediately before a drawActBtn call to give that one button its own
    // click sound; the lambda consumes and clears it. Declaring war and asking
    // for an alliance are not generic clicks, and playing both the specific
    // sound and the generic one on top of each other just muddies them.
    const char* m_btnSfxOverride = nullptr;
    // The value the live drag last reported. Compared against instead of the
    // caller's stored value, which is rounded (volumes), clamped (research and
    // pacification) or snapped (the frame cap) -- so it almost never equalled
    // the raw mouse position and every held frame counted as a move.
    float m_sliderDragT = -1.0f;
    int  m_lastResearchHover = -1;      // edge-detects the node the pointer is on
    bool m_draggingResearchAlloc = false;
    bool m_draggingPacification = false;

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

    // ── the communication window ──
    // A speaker over a video link, coloured entirely from the player's accent
    // (see src/comms/Transmission.h). Opened lazily on first use: it needs a
    // GL context for its buffer and its filter, so it cannot be built in the
    // constructor. update() renders into its own target and therefore runs in
    // Game::update(), never inside a BeginDrawing block.
    /**
     * The link holds TWO people, not one.
     *
     * A conversation between Pr1nted and Mia was one window swapping between
     * them, which reads as two separate calls rather than as the two of them
     * talking to each other -- and the player loses track of who just said
     * what. Both stay on screen; whoever is speaking is lit, the other sits
     * there listening on a quieter signal.
     *
     * A speaker takes a free slot, keeps it while they are in the
     * conversation, and gives it up on act=tune_out. One speaker uses one
     * slot and looks exactly as it did before.
     */
    comms::Transmission m_comms;
    comms::Transmission m_comms2;
    std::string m_commsSlot[2];      ///< who is in each slot, empty if free
    int m_commsActive = 0;           ///< the slot that is talking
    /// Where each window is actually drawn, eased toward commsBounds(slot).
    Rectangle m_commsRect[2]{};
    bool m_commsRectOn[2]{false, false};
    comms::Transmission& commsAt(int slot) { return slot ? m_comms2 : m_comms; }
    const comms::Transmission& commsAt(int slot) const { return slot ? m_comms2 : m_comms; }
    /// Where slot `i` is drawn. One on the link gets the whole stage; two
    /// share it side by side.
    Rectangle commsBounds(int slot) const;
    int commsSlots() const;          ///< how many are on the link right now
    void commsHangUp();              ///< the speaker leaves; the listener stays
    bool m_commsOpen = false;
    bool m_commsBuilt = false;
    void toggleComms();
    Rectangle commsBounds() const;
    void updateComms(float dt);
    void drawComms();

    // ── the tutorial ──
    // A scripted conversation: the words in the textbox, the speaker in the
    // communication window. `m_tutorialPending` is set by the menu and read
    // once the world it asked for is actually on screen -- the same shape as
    // m_quickStartPending, because it rides on the same loader.
    dlg::Box m_dialog;

    // Characters a MAP brought with it, merged over the base cast while that
    // map is loaded and dropped when it unloads. Scoped that way on purpose:
    // a map's cast is the map's, and must not leak into the next one or into
    // the tutorial's.
    std::vector<std::string> m_mapCastNames;
    void loadMapCast();
    /// Builds the communication window if it is not built yet. Needs a GL context.
    bool ensureCommsBuilt();
    /// Runs a dialogue the MAP carries (dialog/<name>.oddlg inside the .odmap).
    bool beginMapDialogue(const std::string& name);
    bool m_dialogOpen = false;
    bool m_tutorialPending = false;

    /**
     * The tutorial's world is not the player's.
     *
     * It exists to be practised on and thrown away, so it must never appear
     * in Load World and must never be written to disk. That is one flag and
     * two consequences: no save file is created at load, and trySaveGame
     * refuses rather than quietly writing an "AutoSave" the player did not
     * ask for and will not recognise a week later.
     */
    bool m_tutorialMode = false;
    /// The opening conversation is running on the menu; the world loads when
    /// it ends. Separate from m_tutorialPending, which arms the LESSON.
    bool m_introRunning = false;
    /// Where the "Stop the tutorial" button was drawn, so the click handler
    /// can find it. Zeroed on any frame that does not draw it.
    Rectangle m_tutorialStopRect{0, 0, 0, 0};
    void stopTutorial();
    /// What the player picked in the intro ("basics" / "specifics"), kept so
    /// the lesson can open differently for someone who said they know this.
    std::string m_tutorialTrack;
    /**
     * A script to open when the current one ends.
     *
     * How the specialised tutorials come back. Each topic is its own .oddlg
     * -- there is no jump instruction in the format, and adding one to give
     * four topics a menu would be a branch table nobody else needs -- so a
     * topic simply runs to its end and this brings the menu back up.
     */
    std::string m_dialogReturnTo;
    std::string m_dialogScript;   ///< the .oddlg currently open, by name
    void startTutorialWorld();
    /**
     * The country Quick Start must hand the player, by ISO code.
     *
     * The tutorial names its country in the script -- Ashford, repeatedly --
     * so it cannot let a heuristic choose. pickQuickStartCountry falls back to
     * "largest by province count" on an unknown map, and the moment the map
     * was rebuilt with Verrick holding more ground than Ashford, the tutorial
     * put the player in charge of the country the lesson tells them to invade.
     *
     * Empty for an ordinary Quick Start, which should keep choosing.
     */
    std::string m_forcedStartIso;

    // Where each named piece of the interface was drawn, and on which frame.
    //
    // Stamped rather than cleared: offerUiTarget is called from drawing code,
    // so a "clear at the start of the frame" has to sit at exactly the right
    // point in the draw order and quietly breaks when a panel moves. Entries
    // simply go stale, and a panel that stopped drawing itself stops being
    // pointable a frame later without anybody having to remember to say so.
    struct UiTarget { Rectangle rect; uint64_t frame; };
    std::unordered_map<std::string, UiTarget> m_uiTargets;
    uint64_t m_uiFrame = 0;
    /// Names a script asked for that nothing offered, warned about once each.
    std::set<std::string> m_uiTargetsMissing;
    float m_pointerT = 0.0f;      ///< the pointer's own animation clock

    /// The element the current page points at, if the game drew it this frame.
    bool tutorialFocus(Rectangle& out, bool& round) const;
    void drawTutorialPointer();
    /**
     * True when the tutorial is holding the player's hands.
     *
     * While a gated page is up, every click outside the ring and outside the
     * textbox is swallowed. A tutorial that says "press End Turn" and then
     * lets the player press anything at all spends most of its life
     * recovering from wherever they went instead.
     */
    bool tutorialBlocksInput(Vector2 mouse) const;
    /// True while a gated page is up at all, regardless of where the mouse is.
    bool tutorialGateActive() const;
    /**
     * May the player end the turn right now?
     *
     * False while a gated page is up that is not pointing at the button. Half
     * the lesson is "give the orders, THEN end the turn", and a player who
     * ends it early resolves a turn the script has not described yet -- the
     * armies move, the AI answers, and Mia is still explaining what a
     * province is.
     */
    bool tutorialAllowsEndTurn() const;
    /**
     * Does the gate hold the MAP still, as well as the interface?
     *
     * Not always. Some pages point at the province panel -- recruiting,
     * declaring war -- and that panel is empty until the player clicks a
     * province. Freezing the map for those makes the page impossible to
     * satisfy: it says "select one of ours" and then refuses the click.
     */
    bool tutorialGateHoldsMap() const;
    /**
     * Has the lesson introduced the Process Turn button yet?
     *
     * Until it has, the button is dead and drawn dead. A turn resolved before
     * the tutorial has explained what a turn IS moves every army, answers
     * with the AI and changes the board underneath a script that is still on
     * "this is a province" -- and the player has no way of knowing they did
     * anything wrong.
     *
     * Set by act=unlock_turn, and immediately for someone who told Pr1nted
     * they already know the basics: they never see the page that unlocks it,
     * and locking them out of the game would be an odd reward for saying so.
     */
    bool m_tutorialTurnUnlocked = false;
    /// Answer a page's `until`. Unknown conditions are reported and treated
    /// as already met, so a typo stalls the tutorial visibly once rather than
    /// wedging the player on a page forever.
    bool tutorialConditionMet(const std::string& cond);
    /// Perform a page's `act`, once, when the page arrives.
    void tutorialAct(const std::string& act);
    int  m_dialogPageTurn = 0;    ///< the turn the current page arrived on
    int  m_dialogPage = -1;
    void beginDialogue(const std::string& script);
    void endDialogue();
    void updateDialogue(float dt);
    void drawDialogue();
    Rectangle dialogueBounds() const;
    void commsSpeaker(const std::string& speaker);
    /// What the speaker feels on this page: from its pose if it names one,
    /// otherwise read off the line itself.
    float emotionFor(const dlg::Page& page) const;
    // The cast: speaker name -> how they look. Read once from
    // data/comms/cast.json, so adding a character is a data change.
    void loadCommsCast();
    std::map<std::string, comms::Profile> m_cast;
    std::map<std::string, std::string> m_castVoice;   // speaker -> sfx name
    // How the plate reads: name, role, short tag. Keyed by the script's
    // formal name, which is an id and does not move.
    struct CastLabel { std::string display, role, tag; };
    std::map<std::string, CastLabel> m_castLabel;
    bool m_castLoaded = false;

    // The speaker's blip while the typewriter runs. Rate limited: one per
    // couple of letters, or a line of dialogue becomes a buzz.
    std::string m_speakerVoice = "narrator";
    float m_voiceCooldown = 0.0f;
    int   m_voiceLetters = 0;
    int   m_lastRevealed = 0;
    // What the window is currently showing, so the link only drops out when
    // one of them actually changes.
    std::string m_commsShowing;
    std::string m_commsPose;

    // Draw text with per-character font selection:
    // default raylib font for ASCII (32-126), m_gameFont for non-ASCII
    void drawHybridText(int x, int y, int fontSize, const char* text, Color color);
    /// Switch language, rebuild the atlas and remember the choice.
    bool applyLanguage(const std::string& code);

    // ─── The language picker (Game_Language.cpp) ───
    /// The national flag beside a language, rendered once and kept.
    Texture2D languageFlag(const char* iso);
    void unloadLanguageFlags();
    Rectangle languagePickerBounds() const;
    /// The picker's Back button; touch devices have no Escape key.
    Rectangle languageBackButton() const;
    Rectangle settingsLanguageArea() const;
    /// The list itself, drawn into whatever area the caller owns.
    void drawLanguageList(Rectangle area, bool withHeading);
    bool updateLanguageList(Rectangle area, bool withHeading);
    void drawLanguageDisclaimer(Rectangle area);
    /// The main menu's modal version of the same list.
    void drawLanguagePicker();
    void updateLanguagePicker();
    std::unordered_map<std::string, Texture2D> m_langFlags;
    bool m_languageOpen = false;   ///< the menu overlay is up
    /// Rebuild the text atlas for the active language. See Game.cpp.
    void reloadFonts();

    /// Push m_config.uiScale into odUi and re-read the logical canvas.
    ///
    /// The player's factor MULTIPLIES the platform's own minimum rather than
    /// replacing it, so a phone that needs 1.2x to be legible still gets it.
    /// m_screenW/H come from the SHADOWED GetScreenWidth, which reports the
    /// logical size, so they have to be re-read after the scale moves or every
    /// panel lays itself out against the old canvas.
    void applyUiScale();

    /// Build the resource / claims overlay the first time one is actually
    /// wanted. Each is a map-sized buffer and a map-sized texture -- 256 MB
    /// the pair at 8192x4096 -- and neither is needed until its view is
    /// opened. Building them during the load is what put an iPhone over its
    /// budget. Cheap and idempotent once built.
    void ensureResourceTexture();
    void ensureClaimsTexture();
    void ensurePopulationTexture();
    void ensureProvincePixels();

    /// The canvas is too narrow to spell things out.
    ///
    /// A phone held upright reports about 400 logical points across. The HUD
    /// was laid out against 1600: eight view tabs with words under them, a
    /// 360-point country panel and a 100-point sidebar, which on 400 points
    /// leaves the panel running underneath the sidebar and the tab labels
    /// smeared into one another. Below this width the same controls are drawn
    /// as icons alone and the panel is clamped, which is the difference
    /// between cramped and unusable. 700 is where the eight labels stop
    /// fitting; there is nothing else magic about it.
    bool compactHud() const { return m_screenW < 700; }

    /// How tall the row of view tabs along the bottom is.
    ///
    /// Seven places worked this out for themselves and all seven wrote 80.
    /// The moment the bar became shorter on a narrow screen, the date, the
    /// Process Turn button and two panel heights were all still reserving
    /// space for the old one -- so the button sat on top of the tabs it was
    /// supposed to sit above. One number, read by everyone who needs it.
    int bottomBarH() const { return compactHud() ? 44 : 80; }

    /// The top of the bottom-left stub row -- Process Turn, or Ready in a
    /// network game. The left-hand panels have to stop above it.
    ///
    /// They did not, and the arithmetic guaranteed it. drawCountryPanel() ran
    /// to m_screenH - bottomBarH() - 16; this row's bottom is
    /// m_screenH - bottomBarH() - 22. So on any window short enough that the
    /// panel was not capped at its 700 maximum, the panel covered the button
    /// entirely -- and not merely drew over it, because the panel hands its
    /// rect to the renderer as a hit-test region, so the clicks went to the
    /// panel and the button did nothing.
    ///
    /// A desktop window is tall enough for the 700 cap to hide it. A phone
    /// never is: 699 points tall, so the panel ran to the bottom every game.
    int bottomLeftStubTop() const {
        return m_screenH - bottomBarH() - 16 - 36 - 6;
    }

    /// Whether that row is on screen at all. The panels reserve space for it
    /// only when it is, so a screen without it keeps the height it had.
    bool bottomLeftStubVisible() const {
        return (!m_mapDate.empty() || m_playerCountryId == SPC_CID) &&
               m_turnState == TURN_NORMAL;
    }

    /// How far down a left-hand panel may run, given all of the above.
    int leftPanelBottom() const {
        const int barLimit = m_screenH - bottomBarH() - 16;
        return bottomLeftStubVisible() ? std::min(barLimit, bottomLeftStubTop() - 8)
                                       : barLimit;
    }

    /// Why the control under the cursor is not going to do anything.
    ///
    /// A greyed button that gives no reason makes the player think the game is
    /// broken. The diplomacy panel disables EVERY act the moment one request
    /// is pending, and the only explanation anywhere was a page in the
    /// tutorial -- "one request at a time to one country, that is not a bug
    /// you stepped on, it is a rule" -- which a player who skipped the
    /// tutorial never sees, and one who did has long forgotten.
    ///
    /// Set during the frame by whatever the cursor is over; drawn last so it
    /// sits above the panels, and cleared as it is drawn.
    std::string m_uiHint;
    void drawUiHint();

    // ─── FIND A COUNTRY ───────────────────────────────────────────────────
    //
    // Two hundred and fifty-two countries on a world map and no way to reach
    // one except by recognising its shape and panning to it. The doctrine and
    // keybind screens already have search boxes; the map, which needs it most,
    // had none.
    //
    // Ctrl+F or "/" rather than a rebindable action: the keybind rows are
    // addressed by index in several tables and a new action moves all of them.
    // Worth revisiting when that table is reworked.
    bool m_findOpen = false;
    std::string m_findQuery;
    int m_findIndex = 0;                  // which match is highlighted
    std::vector<int> m_findMatches;       // country ids, best first
    void updateCountryFinder();
    void drawCountryFinder();
    void rebuildFindMatches();
    int  largestProvinceOf(int countryId) const;
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

    // ── WHAT SPECIALISING A PROVINCE IS WORTH ───────────────────────
    //
    // It was worth nothing. Completing a specialization wrote the resource's
    // name into ProvinceIndustry and stopped there: `boost` was read in
    // exactly one place, the info line under the province panel, and
    // `resourceIncome` was loaded from the map and never recomputed by
    // anything. So the purchase -- one and a half industry levels, three turns
    // -- bought a label and a number that described a bonus nobody was paying.
    //
    // DERIVED, NOT STORED, and that is the part that matters. Writing the
    // boost into resourceIncome would compound every time a province was
    // re-specialised and would have to be unwound when it changed, so
    // resourceIncome stays the province's unspecialised base -- which is what
    // every existing save already holds -- and the bonus is applied on the way
    // out. Every reader goes through provinceResourceIncome for that reason.
    /** The current specialization's boost, in percent. 0 when there is none. */
    float specializationBoostPct(int pid) const;
    /** A province's resource income with its specialization applied. */
    float provinceResourceIncome(int pid) const;
    /**
     * The resource this province is best off specialising in, by the boost it
     * would actually earn -- nullptr when it has no deposits worth naming.
     */
    const char* bestSpecializationFor(int pid) const;
    /**
     * What specialising this province would cost, and in what.
     *
     * `resource` may be null or empty to mean "whatever is best here", which
     * is what the bulk brush's Optimal setting passes. No side effects: the
     * confirm panel totals a hundred of these before a penny is spent.
     */
    /** `countryId` below zero means the local player. See upgradeQuote. */
    bool specializationQuote(int pid, const char* resource,
                             float& cost, std::string& outResource,
                             int countryId = -1) const;
    /** Queue one province's specialization and pay for it. */
    bool queueSpecialization(int pid, const char* resource, int countryId = -1);
    mutable std::unordered_map<int, CountryIncomeSnapshot> m_countryIncomeCache;
    std::unordered_map<int, std::vector<CountryIncomeSnapshot>> m_incomeHistory;
    std::string m_mapDate;
    std::unordered_map<int, long long> m_provincePopulations;
    /**
     * Men a province can still be asked for: its population less whatever is
     * already on order there.
     *
     * Recruiting DEDUCTS population when the order resolves, so a province is
     * a finite pool that refills only through the population growth on ethnic
     * policy options (see popGrowthPerTurn). The subtraction of pending orders
     * is what stops one turn's worth of decisions from spending the same
     * people several times over -- with the per-module action cap lifted, a
     * country could otherwise place eight orders in a turn against a pool that
     * none of them had reduced yet, and raise 160% of a province.
     */
    long long availableManpower(int provinceId) const;
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

    // ─── Province ownership index ────────────────────────────────────────
    //
    // cid -> the province ids that country owns.
    //
    // Everything that used to ask "which provinces does country X hold?"
    // answered it by walking the ENTIRE province map and testing each one, and
    // several of those walks sat inside a per-country loop. refreshIncomeCache
    // did it twice per country, processRebellions once, and the AI's economy
    // and war executors once each per decision -- so a 50-country map paid for
    // a couple of hundred full-map scans every single turn. Measured on a
    // 30-country self-play map that was the largest single cost in the turn
    // loop, several times the neural net it was supposedly waiting on.
    //
    // Rebuilt once at the top of each turn and spliced on every conquest, so a
    // per-country pass is O(that country's holdings).
    std::unordered_map<int, std::vector<int>> m_countryProvinces;
    /** Full rebuild from current province ownership. O(provinces). */
    void rebuildCountryProvinceIndex();
    /**
     * One province changed hands: move it between the two lists.
     *
     * Call it beside every ownership write. Missing a call costs correctness
     * only until the next turn's rebuild, but a caller that hands back a
     * province the index still lists will see it skipped by the ownership
     * re-check every consumer does.
     */
    void reindexProvinceOwner(int pid, int oldOwner, int newOwner);
    /**
     * Move a province's pixels between the two owners' render lists.
     *
     * Was a copy-pasted lambda in each of the two movement resolvers, with a
     * comment in one pointing at the other. Ownership bookkeeping belongs
     * beside reindexProvinceOwner, which is the other half of the same write.
     */
    void transferCountryPixels(int pid, int newOwner, int oldOwner);
    /**
     * Province ids `cid` is believed to own -- a candidate set, not a
     * guarantee. Consumers re-check `prov.countryId` because a conquest
     * earlier in the same turn can leave a stale entry behind.
     */
    const std::vector<int>& provincesOf(int cid) const;
    std::unordered_map<int, int> m_provinceConquestTurn; // turn# when province was conquered (0 = not conquered)
    std::unordered_map<int, int> m_conqueredProvincePrevOwner; // previous owner of conquered province (for ongoing war debuff)
    std::vector<long long> m_provincePopArray;
    // Per-pixel lookups for fast population texture updates
    // uint16, NOT int. One entry per map pixel -- 33.6 million of them at
    // 8192x4096 -- so the width of this is 128 MB against 64 MB. Country ids
    // are bounded by BLC_CID = 65535, which is exactly the top of the range,
    // and REBEL_CID_MIN is 60000, so every id a pixel can hold fits.
    //
    // Callers still read it into int and compare against int; the only place
    // the narrowing matters is the write, and every writer is assigning an id
    // that came from the same bounded set.
    std::vector<uint16_t> m_pixelCountryArray;
    std::vector<std::vector<int>> m_countryPixels;
    std::unordered_map<int, std::vector<int>> m_provincePixels;
    std::vector<Color> m_populationPixelBuffer;
    std::vector<Color> m_politicalPixelBuffer;
    std::vector<uint8_t> m_gradientDist; // distance-to-border (0-255, capped at ~30)
    // Set by reindexProvinceOwner whenever a province changes hands; cleared
    // by rebuildGradientField(). Rebuilding is a full-raster BFS, so it runs
    // once per turn that actually moved territory rather than every frame.
    bool m_gradientDirty = false;
    void rebuildGradientField();
    void generatePopulationTexture(int countryId, int prevCountryId);
    void generatePoliticalTexture();
    void buildPopulationLookups();
    int m_lastPopCountryId = -1;

    std::unordered_map<std::string, std::unordered_map<std::string, CountryRelation>> m_relations;
    /** speaker -> hearer -> what the hearer thinks the speaker's word is worth. */
    std::unordered_map<std::string, std::unordered_map<std::string, float>> m_credibility;
    /** Claims still inside the window conduct can disprove them in. */
    std::vector<SpokenClaim> m_openClaims;
    /**
     * How many times somebody's word has been caught short, and how low it ever
     * got. Kept because the CURRENT credibility cannot answer the question the
     * eval needs answering: forgiveness runs every turn, so a run that caught
     * two liars early reports a serene 1.000 three hundred turns later and
     * looks exactly like a run where the checks never fired once.
     */
    long long m_credibilityHits = 0;
    float m_credibilityLow = 1.0f;
    long long m_realConquests = 0;   // see noteRealConquest
    // Naval routing diagnostics: how many ship moves were stopped by land
    // before covering any meaningful distance. See processNavyMovement.
    long long m_navMoves = 0, m_navBlocked = 0;
    // OF THE BLOCKED ONES, HOW MANY WERE ORDERED ONTO DRY LAND IN THE FIRST
    // PLACE. "Stopped dead by land" has two quite different causes -- a route
    // that had to round a headland and could not, and a destination that was
    // never at sea -- and only the second is the order's fault. The AI aims at
    // enemy PORT PROVINCE CENTRES, which are land pixels, so this separates
    // "the router is weak" from "the target was never reachable by anything".
    long long m_navDestOnLand = 0;
    // ...and how many were at sea but not in THIS ship's sea. The third case
    // is the honest one: water, same body, and the straight line between still
    // clips a headland, which is the router's job rather than the order's.
    long long m_navDestOtherSea = 0;
    // Landing orders the resolver threw away because the hull was not close
    // enough to the province CENTRE. The AI's own landing test measures to the
    // water beside the harbour, so the two can disagree about the same hull --
    // and when they do, an invasion that the AI believes it launched simply
    // never happens. Counted so the amphibious funnel has the missing stage.
    long long m_navLandingsOutOfRange = 0;
    long long m_navEngagements = 0, m_navSinkings = 0;
    long long m_navTransportsSunk = 0, m_navCrewDrowned = 0;
    void generateRelationsTexture(int countryId, int prevCountryId);
    int m_lastRelationsCountryId = -1;
    std::vector<Color> m_countryRelationColors;
    std::unordered_map<int, ProvinceResources> m_provinceResources;
    int m_activeResourceIdx = 0;   // 0=oil, 1=gold, 2=rubber, 3=gemstones, 4=metal
    // ONE resource layer in memory, not five.
    //
    // These are full-map RGBA buffers -- 8192x4096x4 is 128 MB each -- and all
    // five were generated at load and kept for the session: 640 MB to display
    // one of them. Only m_activeResourceIdx is ever on screen, and
    // generateResourceTextureFor() can rebuild any of them from
    // m_provinceResources in a single pass, so the other four were being stored
    // because nobody had priced them.
    //
    // This was the largest single item in the 1.2 GB a scenario load added to
    // the heap, which is why a phone could reach the menu and die on the map.
    // m_resourceBufferIdx says which resource the buffer currently holds, or
    // -1 for none; generateResourceTexture() refills it when the player
    // switches, which is the only moment it can become wrong.
    std::vector<Color> m_resourceBuffer;
    int m_resourceBufferIdx = -1;
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
    // The ship counterpart to flyToProvince. Cycling ships moved the selection
    // but left the camera where it was, so stepping through a navy scrolled a
    // list while the map sat still.
    void flyToShip(int shipIndex);

    // ─── Claims system ────────────────────────────
    std::unordered_map<std::string, std::vector<int>> m_claims;  // claimer ISO -> claimed province IDs
    std::unordered_map<int, std::vector<std::string>> m_claimsByProvince;  // province ID -> list of claimant ISOs
    bool m_showClaims = false;
    int m_lastClaimsCountryId = -1;
    std::vector<Color> m_claimsPixelBuffer;
    void generateClaimsTexture();
    void clearClaimsView();
    bool isCountryInvolvedInClaims(int countryId, int claimantCid);
    // m_claims and m_claimsByProvince are one fact stored twice, and every
    // caller used to open-code both halves. Sites that forgot the reverse index
    // left the claims panel, the unrest maths and the rebellion odds reading a
    // claim the claimant no longer had. Every write goes through these.
    void grantClaim(const std::string& claimantIso, int pid);
    void revokeClaim(const std::string& claimantIso, int pid);
    // Load-time repair: a claim on a province the claimant already owns.
    void dropSelfOwnedClaims();

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
    /**
     * The doctrine search box: what has been typed, and whether it has focus.
     *
     * Forty-five doctrines in five collapsible folders is more than a player
     * will scroll through to find the one they half-remember the name of.
     * Matching runs over the name, the description and the folder, so "upkeep"
     * finds the doctrines that touch it even though none is called that.
     */
    std::string m_policySearch;
    bool m_policySearchFocus = false;
    int m_analysisHotspotScroll = 0;
    int m_analysisMinorityScroll = 0;
    int m_analysisHotspotCount = 0;
    std::vector<std::pair<int, Rectangle>> m_analysisGoToButtons;

    std::vector<EthnicPolicyCategory> m_ethnicPolicyCategories;

    // ── Minority policy is a COUNTRY's policy, not the world's ──────────
    //
    // Both of these used to be keyed on the minority name alone. One table for
    // the whole map meant a single government's treatment of, say, the
    // Kortorians was the treatment every government gave them, and only the
    // player could edit it — so every AI country's rebellion risk (alignment
    // feeds straight into getProvinceRebellionChance) was being driven by a
    // screen the AI could not reach. Keying on the country makes minority
    // policy something each government owns, answers for, and can be judged on.
    //
    // countryId -> minority name -> one option index per category.
    std::unordered_map<int, std::unordered_map<std::string, std::vector<int>>> m_ethnicPolicies;
    // countryId -> minority name -> cumulative alignment drift, always within
    // +/-MINORITY_DRIFT_LIMIT. See addMinorityDrift for why the bound is on the
    // stored value rather than on the reader.
    std::unordered_map<int, std::unordered_map<std::string, float>> m_minorityAlignmentDrift;

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
    /**
     * Why this country cannot enact this doctrine, or "" if it can.
     *
     * canCountryEnactPolicy is this asking whether it found anything, so the
     * greyed-out button and the sentence under it can never disagree about the
     * reason -- which they did: the screen blamed conflicting doctrines for
     * every refusal, including the many that were really about the treasury.
     */
    std::string policyBlockReason(int countryId, const Policy& p) const;
    /// True when either doctrine names the other. Incompatibility is a property
    /// of the pair, so declaring it on one side is enough; see the definition.
    bool policiesConflict(const std::string& a, const std::string& b) const;
    /// Display names of every doctrine that conflicts with @p p, both directions.
    std::vector<std::string> conflictingPolicyNames(const Policy& p) const;
    void enactPolicy(int countryId, const std::string& policyId, int targetProvince = -1, const std::string& targetMinority = "");
    void cancelPolicy(int activePolicyIndex);
    void applyPolicyEffects(int countryId);
    void shiftCountryCompass(int countryId, float econDelta, float socDelta);
    /**
     * Rename and restyle countries whose government has moved far enough.
     * Once a turn, after policy effects have shifted the compass. See
     * PoliticalIdentity.h.
     */
    void updatePoliticalIdentities();
    float getCountryUnrest(int countryId) const;
    void drawPoliciesTab();
    void updatePoliciesTab();
    void drawPoliticalCompass(int x, int y, int size, int countryId, bool showPopAverage = true);
    void drawAnalysisTab();
    float getProvinceRebellionChance(int provinceId) const;
    float getProvinceRebellionChance(int provinceId, int countryId) const;
    /** How well `minorityName` is disposed toward `countryId`'s government, 0-100. */
    float getMinorityAlignment(int countryId, const std::string& minorityName) const;
    /**
     * Alignment change per turn implied by `countryId`'s current option set.
     *
     * The policy dial alone, so it is bounded by the option table and nothing
     * else -- which is what AISystem's trend bounds and validity mask rely on
     * to ask "is there anywhere left to move". Not what the player is shown:
     * for that see getMinorityAlignmentTrend.
     */
    float getMinorityPolicyRate(int countryId, const std::string& minorityName) const;
    /**
     * Everything that moves alignment per turn: the policy dial plus the
     * standing penalty for holding conquered ground in a live war. The one
     * definition, used both by the turn resolver that applies it and by the UI
     * that reports it, so the number shown is the number that happens.
     */
    float minorityDriftPerTurn(int countryId, const std::string& minorityName) const;
    /**
     * What alignment will actually change by next turn, which is
     * minorityDriftPerTurn clipped by the drift bound -- zero once a minority
     * is pinned at 0 or 100, because that is what the player will observe.
     */
    float getMinorityAlignmentTrend(int countryId, const std::string& minorityName) const;
    /** Add to a minority's stored drift, keeping it inside the bound. */
    void addMinorityDrift(int countryId, const std::string& minorityName, float delta);
    // Alignment is 50 + drift, clamped to 0..100, so drift beyond this bound
    // could not show up on the bar. Storing it anyway is what made repression
    // irreversible; see addMinorityDrift.
    static constexpr float MINORITY_DRIFT_LIMIT = 50.0f;
    /**
     * The option `countryId` has chosen for `minorityName` in category `ci`.
     *
     * Falls back to the category's default when the country has never touched
     * it, which is the same "or the default" dance four separate call sites
     * used to open-code — including one that got it subtly wrong by treating a
     * short option vector as "no entry" for every category rather than for the
     * missing ones.
     */
    int ethnicPolicyOption(int countryId, const std::string& minorityName, size_t ci) const;
    /** Set one category, creating a fully defaulted row for the country if needed. */
    void setEthnicPolicyOption(int countryId, const std::string& minorityName,
                               size_t ci, int option);
    /** Every category's default, in order. Used to seed a country's first row. */
    std::vector<int> defaultEthnicPolicyOptions() const;
    void initEthnicPolicyCategories();
    void drawEthnicTab();
    void updateEthnicTab();
    void applyEthnicPolicyEffects(int countryId);
    void growCountryPopulation(int countryId);

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

    // ─── War weariness ────────────────────────────
    //
    // Extra unrest, in rebellion-chance percentage points, carried by a country
    // that was dragged into somebody else's war. This is what an alliance
    // actually COSTS: before it, honouring one was free, so a policy-gradient
    // learner correctly concluded that alliances were worth nothing and never
    // signed any. Added to every province's rebellion chance and decayed a
    // little each turn, so the price is paid over the years that follow rather
    // than all at once.
    std::unordered_map<int, float> m_countryWarWeariness;
    // Per-pair call cooldown, keyed on (defender, ally). A late-game brawl
    // declares wars constantly, and without this every one of them re-asked
    // every ally: measured at ~4 calls a turn between seven countries, which is
    // both unplayable as a popup stream and meaningless as a decision.
    std::unordered_map<long long, int> m_callToArmsCooldown;
    static constexpr int CALL_TO_ARMS_COOLDOWN_TURNS = 30;
    /** One country answered a call to arms: charge it at home. */
    void addWarWeariness(int cid, float amount);
    /** Per-turn decay. Called once from processTurn. */
    void decayWarWeariness();
    float warWearinessOf(int cid) const {
        auto it = m_countryWarWeariness.find(cid);
        return it == m_countryWarWeariness.end() ? 0.0f : it->second;
    }
    /**
     * Ask every ally of `defenderIso` to join against `attackerIso`.
     *
     * A request, not a summons: allies (including the player) can refuse, and
     * refusing breaks the alliance instead of costing unrest. Accepting joins
     * the war and adds war weariness.
     */
    void issueCallsToArms(const std::string& attackerIso, const std::string& defenderIso);

    /** A country's name for player-facing text, falling back to its ISO code. */
    std::string diploDisplayName(const std::string& iso) const;

    /**
     * Asks one ALLY to join a war this country is already fighting.
     *
     * issueCallsToArms() only fires for a defender, at the instant war is
     * declared on them. Nothing could ask afterwards, and nothing could ask at
     * all for a war you started -- so an alliance was only ever worth anything
     * to whoever was attacked. This is the deliberate version: pick an ally,
     * pick the enemy, and let them decide.
     *
     * Returns false (and explains why) when the ask is not available.
     */
    bool requestAllyJoinWar(const std::string& allyIso, std::string& outWhy);
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

public:
    // Render a save's timelapse straight to a GIF with no window and no UI.
    //
    // renderHistoryFrame() is pure CPU -- it reads the province Image and fills
    // an RGBA buffer, with no Draw* calls -- so the only thing standing between
    // the export and a headless run was the progress overlay. Worth having:
    // it makes the export testable, scriptable for promo renders, and usable on
    // a machine with no display.
    /**
     * Force the timelapse credit off (or on) for this run only.
     *
     * Backs `--no-watermark`. Beats config.json rather than writing to it: a
     * script generating art wants a clean frame this once, not to change what
     * the player's own exports look like from then on -- the same reasoning as
     * --resource-limit.
     */
    void setTimelapseWatermark(bool on) { m_watermarkOverride = on ? 1 : 0; }

    bool exportTimelapseHeadless(const std::string& savePath,
                                 const std::string& outPath,
                                 int outW, int outH, int subFrames,
                                 HistoryView view = HV_POLITICAL);
private:
    // Set while exportTimelapseHeadless runs. Suppresses anything that needs a
    // GL context.
    bool m_headless = false;
    void renderHistoryFrame(const TurnSnapshot& a, const TurnSnapshot& b, float t,
                            int outW, int outH, std::vector<uint8_t>& rgba,
                            HistoryView view = HV_POLITICAL);
    /** The small credit burned into every exported timelapse frame.
     *  Config-gated (timelapseWatermark), on by default. */
    void drawTimelapseMark(std::vector<uint8_t>& rgba, int outW, int outH) const;
    // -1 unset (follow config.json), 0 forced off, 1 forced on.
    int m_watermarkOverride = -1;
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

    // ─── Screenshot tour state (--screenshots) ───
    bool m_shotTour = false;
    // Token from the startup integrity seal; see odseal / Game::init.
    unsigned long long m_localeSeal = 0;
    std::string m_shotDir;               // where the PNGs land
    std::string m_shotSave;              // save loaded for the in-game shots
    std::string m_shotBaseLang;          // the language the tour runs in
    int m_shotIndex = 0;                 // which shot in the list
    int m_shotFrame = 0;                 // frames spent settling on it
    int m_shotProvince = 0;              // the province the panel shots describe
    int m_shotForeignProvince = 0;       // one somebody else owns, for the diplomacy shots

    // ─── Tutorial route walk state (--tutorial-walk) ───
    bool m_walk = false;
    int  m_walkRoute = 0;                // which route in the list
    bool m_walkOpened = false;           // its script is open and being walked
    int  m_walkPage = -1;                // page being walked, -1 between routes
    int  m_walkFrames = 0;               // frames spent on it
    bool m_walkSatisfied = false;        // its condition has been driven once
    bool m_walkPointerSeen = false;      // the thing it points at was drawn
    std::string m_walkPointerName;       // ...which thing, for the report
    bool m_walkPointerTrap = false;      // ...on a page that also gates and waits
    bool m_walkJumped = false;           // a choice sent us to another script
    std::string m_walkExpect;            // ...and this is the one it named
    bool m_walkMapDirty = false;         // ownership moved; the picture is stale
    int  m_walkPages = 0;
    int  m_walkDrill = 0;                // phase of the escape-hatch drill
    struct BranchCase { int route; int page; int option; };
    std::vector<BranchCase> m_walkBranches;
    int  m_walkBranch = 0;
    int  m_walkBranchPhase = 0;
    std::string m_walkBranchKey, m_walkBranchLabel, m_walkBranchFrom, m_walkBranchExpect;
    std::string m_walkBranchNote;        // one log line per case, built as it goes
    bool m_walkBranchEnds = false;       // the opened script signs off rather than returning
    std::vector<std::string> m_walkProblems;
    /// One synthetic click, consumed by updateDialogue. The walk turns pages
    /// with this because raylib has no way to inject a real one.
    bool m_dialogAdvance = false;

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
    // `statedGoal` is what the attacker announces, and WAR_GOAL_NONE -- say
    // nothing -- is both the default and a perfectly ordinary choice. It is
    // recorded and shown; it changes nothing else. No declaration is refused,
    // delayed or made more expensive for want of one.
    void declareWar(const std::string& attackerIso, const std::string& defenderIso,
                    bool chainGuarantees = true, int statedGoal = WAR_GOAL_NONE);
    void applyWarKinPenalty(const std::string& attackerIso, const std::string& defenderIso);
    // A treaty binds both signatories, but scenario relations.json writes one
    // row per country and authors routinely fill in only one of them. Reads
    // that care about the treaty rather than about who recorded it go through
    // here; `flag` is one of CountryRelation's bools.
    bool hasRelation(const std::string& isoA, const std::string& isoB,
                     bool CountryRelation::*flag) const;
    // Inherent civil order subtracted from every province's rebellion chance —
    // makes stability the default and rebellion a grievance-driven exception.
    // Tuned so baseline provinces are stable but claim/war/minority hotspots
    // still revolt (see getProvinceRebellionChance).
    static constexpr float REBELLION_LOYALTY_FLOOR = 6.0f;
    // Answering a call to arms costs this many points of rebellion chance in
    // every province, on top of whatever the war itself stirs up. Set against
    // REBELLION_LOYALTY_FLOOR above: 7 points more than wipes out the baseline
    // loyalty a well-run country enjoys, so a country that keeps honouring
    // alliances it cannot afford starts shedding provinces.
    static constexpr float CALL_TO_ARMS_UNREST = 7.0f;

    /**
     * Turns a province cannot revolt again for after it has just revolted.
     *
     * WITHOUT THIS, PUTTING A REVOLT DOWN ACHIEVES NOTHING.
     *
     * A rebellion was a Bernoulli trial run fresh every turn, and nothing about
     * having just had one changed the odds of the next. So a province whose
     * unrest cleared the threshold would revolt, be reconquered, and revolt
     * again for as long as the grievance stood -- which is for ever, because
     * crushing a revolt does not move a compass, a minority or a treasury.
     *
     * Measured on a 250-turn run of the shipped 1939 scenario: 945 revolts
     * across 484 provinces, one province rising 25 separate times, 921
     * rebellion wars against 187 real ones, and rebel state ids past R1900. The
     * map does not fracture dramatically, it flickers.
     *
     * Forty turns is deliberately long. It is not "the garrison is still
     * there"; it is "this province rose, it was put down, and that is a thing
     * that happened rather than weather". A government that never addresses the
     * grievance still sees the province rise again -- just a handful of times
     * across a long game instead of every tenth turn.
     */
    static constexpr int REBELLION_COOLDOWN_TURNS = 40;

    /**
     * Ceilings on rebellion, so a world cannot dissolve faster than anyone
     * notices.
     *
     * There was no limit at all. A training run reached 24,030 living countries
     * by turn 14 and slowed to fifty seconds a turn, and nothing anywhere said
     * a word about it -- the cause was upstream (a compass sign), but the game
     * had no opinion on twenty-four thousand countries and simply tried to
     * simulate them. The rebel id band is 5,533 wide, so it had been wrapping
     * and reusing ids long before that.
     *
     * These are not balance numbers. They are the point past which something
     * has gone wrong somewhere else, chosen to sit far above any real game: a
     * 1641-province map has never come near 400 simultaneous rebel states, and
     * twelve new ones in a single turn is already a catastrophe.
     */
    static constexpr int MAX_LIVE_REBELS = 400;
    static constexpr int MAX_NEW_REBELS_PER_TURN = 12;
    /** Live rebel states, recounted once per turn; see processRebellions. */
    int m_rebelCensus = 0;
    int m_rebelCensusTurn = -1;
    int m_rebelsSpawnedThisTurn = 0;
    /** Says it once per session, not once per suppressed revolt. */
    bool m_rebelCeilingWarned = false;

    /**
     * Unrest added per turn a country spends bankrupt, before severity scaling.
     *
     * Sustained bankruptcy should reach WAR_WEARINESS_MAX in a handful of
     * turns: the point is that a country which cannot pay for itself and has
     * already sold its fleet is in real trouble, not mildly inconvenienced.
     * Scaled by how deep the shortfall is against income -- see
     * applyBankruptcyPenalties().
     */
    static constexpr float BANKRUPTCY_UNREST_PER_TURN = 2.5f;

    /**
     * Percentage points added to EVERY province's rebellion chance while the
     * country is bankrupt.
     *
     * The weariness above accumulates slowly and decays; this does not. It is a
     * flat, immediate, visible consequence of an empty treasury, and it is
     * deliberately large — twenty points against a loyalty floor and a
     * pacification budget that tops out at fifty is the difference between a
     * quiet country and one coming apart. Going broke should be the worst thing
     * that can happen to a government short of losing a war.
     *
     * It applies only while the treasury is actually empty, and the cascade in
     * applyBankruptcyPenalties() can always reach solvency — budgets, policies,
     * minority spending, ships and finally troops — so this is a state a
     * country can always get out of, not a spiral it cannot escape.
     */
    static constexpr float BANKRUPTCY_UNREST_PCT = 20.0f;
    /** Countries whose treasury emptied this turn. Cleared when solvent. */
    std::unordered_set<int> m_bankruptCountries;
    bool isBankrupt(int cid) const { return m_bankruptCountries.count(cid) > 0; }
    static constexpr float WAR_WEARINESS_MAX = 20.0f;
    // ~45 turns to work off a single call at full strength. Long enough that a
    // second call while the first is still hurting is a genuinely bad idea.
    static constexpr float WAR_WEARINESS_DECAY = 0.15f;
    static constexpr int REBEL_CID_MIN = 60000;
    void createRebelCountry(int rebelCid, int parentCid, const std::vector<int>& provinceIds);
    void processRebellions(int countryId);

    // ── Country AI (neural-net RL, see src/ai/) ──
    // Created lazily on the first processed turn; owns its model file.
    AISystem* m_ai = nullptr;
    // Model file this process trains, relative to the data directory. A pool
    // worker points somewhere of its own; everything else uses the shared one.
    std::string m_aiModelPath = "ai/model.bin";
    // Set from OD_EVAL_MODEL: evaluate this exact file rather than the shared
    // one, so a PBT ranking round can score a worker without disturbing it.
    std::string m_evalModelOverride;
    /**
     * THE SEAT, when running the absolute benchmark. See runAIEvaluation.
     *
     * An isoA3. Non-empty means: this ONE country is played by the model (or by
     * a person, in the game's own bench mode) and every other country in the
     * world is played by the frozen scripted rung. The score is then simply how
     * much of the world the seat ends up holding -- an absolute number that does
     * not depend on who it was measured against, which is the whole point.
     */
    std::string m_benchSeatIso;
    /** Which shipped scenario the seat is played on. See m_benchSeatIso. */
    std::string m_benchSeatMap;
    /**
     * Scope the rush to the seat's NEIGHBOURS rather than the whole world.
     *
     * See AISystem::s_exploitCids for why: a world where all 52 countries
     * attack without pause kills everybody equally and ranks nobody.
     */
    int m_benchRushNeighbours = 0;   // 0 = off, -1 = all neighbours, N = N largest
    /**
     * Turn the benchmark ends on when a PERSON is playing the seat, or 0.
     *
     * The model's half of this benchmark stops because the eval loop counted
     * the turns. A person's half has to stop at the same turn or the two
     * numbers are not the same measurement, so the game says so and reports the
     * score itself rather than trusting anybody to stop on time.
     */
    int m_benchPlayUntilTurn = 0;
    /** The score the seat finished on, once it has. Negative until then. */
    float m_benchScoreShare = -1.0f;
    int m_aiWorkerId = -1, m_aiWorkerCount = 0;
    // Self-play training mode: skips political-texture/label/delta work in
    // processTurn so turns run as fast as the simulation allows.
    bool m_aiTraining = false;
    // Rebellions that fired this turn, per country — cleared at processTurn
    // start. The AI reads it both as a feature and as a punishment signal.
    std::unordered_map<int, int> m_rebellionsThisTurnByCid;
    // pid -> turns before this province may revolt again. Set when it revolts,
    // counted down once a turn, entry erased at zero — so the map holds only
    // provinces actually cooling down, not one entry per province on the map.
    // Owner-independent on purpose: conquering a province that has just risen
    // does not hand the new owner a fresh revolt.
    std::unordered_map<int, int> m_provinceRebellionCooldown;
    /** Per-turn countdown for m_provinceRebellionCooldown. */
    void decayRebellionCooldowns();
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
    /** Sum of one modifier over `countryId`'s researched nodes (-1 = player). */
    float getTotalEffect(const std::string& effectField, int countryId = -1) const;

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

    // ── ONE DIPLOMATIC CHANNEL PER PAIR ─────────────────────────────
    //
    // A country says one thing to another country per turn. The player has
    // always been held to that -- the diplomacy panel greys out every other
    // button for a pair the moment one action is pending -- but the rule lived
    // in the panel, so it bound nobody else. The AI takes up to
    // ACTIONS_PER_MODULE_PER_TURN goes at each module, and its politics and war
    // modules queue independently, so in one turn it could propose an alliance
    // to a neighbour and declare war on it, and declare that same war three
    // times over: relations do not change until the queue resolves, so every
    // pick saw the same untouched world and made the same choice again.
    //
    // What came out the other side was a country the game could not describe.
    // Duplicate declarations mean one WAR_DECLARED popup per copy; an alliance
    // request answered in the same pass as the declaration that follows it
    // leaves a pair both allied and at war, which every reader of
    // m_relations then disagrees about.
    //
    // So the rule moves here, where the player, the AI and anything else that
    // ever queues diplomacy all have to pass through it.
    /** Is `sourceIso` already waiting on an answer from `targetIso`? */
    bool hasPendingDiplomacy(const std::string& sourceIso,
                             const std::string& targetIso) const;
    /** Has `sourceIso` already declared a war this turn that has yet to land? */
    bool hasPendingDeclaration(const std::string& sourceIso) const;
    /**
     * Queue one diplomatic action, or refuse it.
     *
     * Refused when the pair already has something in flight, and -- for a
     * declaration of war -- when this country has already declared one this
     * turn: a declaration is a singular act of state, and stacking several
     * also walked straight past AI_MAX_CONCURRENT_WARS, which counts wars
     * fought rather than wars announced.
     *
     * Returns false without queueing anything, so callers that spend something
     * on the attempt (a cooldown, a stat, money) can decline to spend it.
     */
    bool queueDiplomaticAction(PendingDiplomaticAction da);

    // ─── Bulk upgrading, by painting over the map ───────────────────────
    //
    // Queueing a hundred industry upgrades one province at a time is the
    // complaint this answers. The mode is a toggle in the toolbar strip above
    // the bottom bar -- the same strip the resource picker and the navy
    // filters live in -- and WHAT it paints comes from the view you are
    // already in: industry in the industry view, forts in defence, ports in
    // navy. One mechanism, three targets, and no new place to look.

    /** Which upgrade the current view paints. Null when the view has none. */
    const char* bulkPaintType() const;
    /** Human name for what the brush buys, for the button and the totals. */
    std::string bulkPaintLabel() const;

    /**
     * What one province's next upgrade would cost, and whether it may have one.
     *
     * NO SIDE EFFECTS, and that is the point: the confirm panel has to total a
     * hundred of these before a penny is spent. Affordability is deliberately
     * NOT checked here -- one province is affordable in isolation while the
     * selection as a whole is not, and it is the whole that is being decided.
     */
    /**
     * `countryId` below zero means the local player, which is what the UI
     * wants. The multiplayer host passes the SUBMITTING country instead, so
     * the same caps, research limits and prices apply to an order that arrived
     * over a socket as to one somebody clicked. See mpApplyOrders.
     */
    bool upgradeQuote(int provinceId, const char* type,
                      float& cost, int& nextLevel, int& turns,
                      int countryId = -1) const;

    /**
     * Queue one province's upgrade and pay for it.
     *
     * The SAME rules the province panel's own buttons use -- caps, research
     * limits, cost, whether something is already building -- because a bulk
     * path with its own copy of them is a bulk path that eventually disagrees
     * with the button next to it about what a thing costs.
     */
    bool queueUpgrade(int provinceId, const char* type, int countryId = -1);

    void updateBulkPaint();
    void drawBulkPaintStrip();

    // ── The toolbar row above the bottom bar ────────────────────────
    //
    // Its geometry was written out twice -- once to draw the bulk-upgrade
    // button and once to catch the click on it -- under a comment telling the
    // next person to keep the two in step by hand. Everything on the row now
    // asks these instead.
    int toolbarRowH() const { return 20 + 6 * 2; }
    int toolbarRowY() const { return (m_screenH - 80 - 16) - toolbarRowH() - 4; }
    /** First free x on the row: after the navy filters when the view has them. */
    int toolbarRowX() const {
        const int mainBarW = std::min(880, m_screenW - 32);
        const int mainBarX = m_screenW - mainBarW - 16;
        return mainBarX + 8 + ((m_activeViewTab == 6) ? (5 * 80 + 4 * 4 + 8) : 0);
    }
    /** What the row is offering right now, in draw order. */
    enum ToolbarId {
        TB_BULK_UPGRADE = 1, TB_BULK_SPECIALIZE, TB_BULK_PANMODE,
        TB_SPEC_OPTIMAL, TB_SPEC_RESOURCE,   // TB_SPEC_RESOURCE + i, i in [0,5)
        TB_DISBAND_ALL = 20, TB_SCRAP_ALL,
    };
    struct ToolbarButton { Rectangle rect; int id = 0; std::string label; bool on = false; };
    /** The row's buttons, positions and labels. Draw and click both read this. */
    void buildToolbarRow(std::vector<ToolbarButton>& out) const;
    /** Act on a click at `mouse`. True when the row consumed it. */
    bool handleToolbarRowClick(Vector2 mouse);
    /** Where the confirm panel sits -- it floats above however many rows show. */
    Rectangle bulkConfirmPanelRect() const;
    /** Is the panel spelling out which resource each province would get? */
    bool bulkSplitShown() const;
    bool handleBulkConfirmClick(Vector2 mouse);

    // ── Disbanding the whole army from the army view ────────────────
    //
    // The province panel has had "Disband All" for one province since forever.
    // A player winding an army down at the end of a war, or cutting an army
    // they cannot pay for, was clicking it province by province across a
    // hundred provinces -- and the austerity reflex the AI gets does exactly
    // this for itself in one step.
    //
    // Reversible until the turn resolves, like every other queued order, which
    // is what makes one click acceptable for something this large: the same
    // button cancels the lot.
    /** Provinces this country holds that have troops and no disband queued. */
    int disbandableProvinces(long long& troopsOut) const;
    /** Queue a full disband in every one of them. Returns how many. */
    int disbandAllArmies();
    /** Take back every queued disband. Returns how many were cancelled. */
    int cancelAllDisbands();
    // The navy's half of the same pair, in the navy view. A fleet is wound
    // down for the same reasons an army is -- an upkeep bill that outlived the
    // war it was built for -- and one hull at a time is the same chore.
    /** Own hulls with no scrap queued. */
    int scrappableShips() const;
    int scrapAllShips();
    int cancelAllScraps();
    /** The confirm/cancel panel, shown while anything is painted. */
    void drawBulkConfirmPanel();
    /** Total cost of the current selection, and how many of it is buildable. */
    void bulkSelectionTotals(float& cost, int& count) const;
    /** Push the selection into the build queue. Everything or nothing. */
    void commitBulkSelection();
    void clearBulkSelection();
    /** Tell the renderer what to light up. Called on every change. */
    void refreshBulkOverlay();

    // ── WHAT THE BRUSH PAINTS ───────────────────────────────────────
    //
    // The view still decides the UPGRADE (industry, forts, ports); this
    // decides whether the brush is buying upgrades at all. Specialisation is
    // the industry view's second brush, because it is an industry decision and
    // because doing a hundred of them one dropdown at a time is the same
    // complaint bulk upgrading answered.
    //
    // Two brushes rather than one with a mode, because they cost different
    // money and mean different things -- but only one can be down at a time:
    // both own the left mouse button.
    enum BulkTarget { BULK_UPGRADE = 0, BULK_SPECIALIZE = 1 };
    int m_bulkTarget = BULK_UPGRADE;
    /**
     * Which resource the specialisation brush paints.
     *
     * Empty means OPTIMAL: each province gets whatever pays best there, which
     * is the setting worth having -- the alternative is reading five numbers
     * off every province before deciding. A named resource is for when the
     * player wants a coherent industrial base rather than the best local
     * return.
     */
    std::string m_bulkSpecResource;
    /** The five a province may specialise in, in the panel's order. */
    static const char* const SPEC_RESOURCES[5];

    bool m_bulkPaint = false;
    /**
     * Pan instead of paint while the mode is on.
     *
     * The map editor's compromise, and for the same reason: a mode that owns
     * the left button owns panning too, and a player who cannot move the map
     * cannot reach the provinces they meant to paint. One toggle, same row.
     */
    bool m_bulkPanMode = false;

    /** Painted, costed, and not yet bought. */
    std::unordered_set<int> m_bulkSelection;
    /**
     * Provinces already touched during THIS press.
     *
     * A drag crosses the same province on many frames; without this a sweep
     * would toggle it on and off again as fast as the game draws.
     */
    std::unordered_set<int> m_bulkPaintStroke;
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
    /**
     * Answers for isProvinceCoastal, which the province panel asks every frame.
     *
     * The answer cannot change while a map is loaded -- it is a question about
     * the land/sea image, not about anything the player does -- but working it
     * out means walking every pixel of the province and flood-filling the water
     * around it. Cheap once, wasteful sixty times a second on a large province.
     *
     * Cleared with m_provincePixels when a map loads; see Game_Loading.cpp.
     */
    mutable std::unordered_map<int, bool> m_coastalCache;
    /**
     * Where a province's harbour is, as against where its middle is.
     *
     * The navy view drew the anchor at m_provinceCenters, and a province
     * centroid is not a harbour. For a province wrapped around a bay the
     * centroid is in the bay, so roughly thirty anchors per map floated in open
     * water with no land under them -- most visibly around the Baltic -- and a
     * handful landed inside the neighbouring country instead. It is the same
     * mistake the ship generator used to make, one layer up: a centroid is a
     * convenient point, not a place anything happens.
     *
     * Same lifetime and same reasoning as m_coastalCache: a question about the
     * land/sea image, which cannot change while a map is loaded, and expensive
     * enough to be worth not asking every frame.
     */
    mutable std::unordered_map<int, Vector2> m_portAnchorCache;
    Vector2 portAnchor(int pid) const;
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

    /**
     * Province armed by the army panel's Move button (-1 = not armed).
     *
     * The same order, reached by clicking instead of dragging. Dragging with a
     * key nobody was told about was the ONLY way to move an army: the panel
     * offered Recruit, Disband and Cancel Orders, so the one action the tab
     * exists for was the one with no button. A player who never found the
     * keybind concluded armies could not be moved at all.
     *
     * Armed, not modal: it survives exactly one click. That click is either the
     * destination or, anywhere else, thinking better of it. Escape, a second
     * press of the button and leaving the tab all back out too. Clicking the
     * source province only backs out -- unlike ending a DRAG there, it does not
     * wipe that province's orders; the panel has a button that says so.
     */
    int m_armyMovePickFrom = -1;

    /** Whether two provinces share a border, per the adjacency graph. */
    bool provincesAdjacent(int a, int b) const;

    /** Whether `to` is somewhere `from`'s army may legally be sent. */
    bool canArmyMoveTo(int fromPid, int toPid) const;

    /**
     * Add the move order, or remove it if that exact one already exists.
     *
     * Splits what is left rather than what there is: several orders may leave
     * one province, and their percentages are of the same army.
     */
    void queueArmyMove(int fromPid, int toPid);

    /** Take back every move order leaving this province. */
    void cancelArmyMovesFrom(int fromPid);
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
    /**
     * The negotiation screen is showing a TRADE, not a ceasefire.
     *
     * The two are the same screen because they are the same act: provinces,
     * money and claims moving both ways, agreed by both sides. A ceasefire is
     * that plus an end to a war, which is the only thing this flag changes --
     * the wording, the action queued, and whether applyCeasefireTerms() runs
     * its war-ending tail. Reusing CeasefireTerms and the same pending map
     * keeps one code path for the part that actually moves territory.
     */
    bool m_tradeMode = false;
    /** Set by the state loader when it restyles a country; consumed once the
     *  map archive is available and flags can actually be drawn. */
    bool m_identityFlagsDirty = false;
    std::string m_ceasefireTargetIso;   // ISO of country we're negotiating with
    // Which money slider the mouse grabbed, or -1. A slider that only tracks
    // while the cursor is inside its bar cannot be dragged to zero: the value
    // stops updating the moment the cursor crosses the left edge, which is the
    // exact gesture for emptying it. See drawMoneySlider().
    int m_ceasefireMoneyDrag = -1;      // 0 = we offer, 1 = we demand

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
    // Backstop sweep, run once at the end of every turn: send home any stack
    // standing in a country that is not its own and not an ally's, whatever
    // transition left it there. An assault is settled the turn it is made (see
    // resolveAssault), so ground held without owning it is always a leftover.
    void expelStrandedArmies();
    /**
     * Move the provinces, claims and money a set of terms describes.
     *
     * endsWar tells it whether this is a peace. A ceasefire passes true and
     * gets the withdrawal at the end -- nobody's troops may still be standing
     * on the other's soil once the war is over. A trade passes false: no war
     * ended, so the armies stay exactly where they are, and sending them home
     * would be a free retreat bought with a province.
     */
    void applyCeasefireTerms(const std::string& sourceIso, const std::string& targetIso, const CeasefireTerms& terms, bool alreadyDeducted = false, bool endsWar = true);

    void drawCeasefireScreen();
    // A thumbnail of the political map cropped to the land an offer touches,
    // with every province in the terms shaded by who ends up holding it. Used by
    // the incoming-offer popup, which otherwise showed only province numbers.
    // `cacheKey` identifies the offer (the popup's id): the shading is rastered
    // once per offer and reused for every frame the panel stays open.
    void drawCeasefireTermsMap(const CeasefireTerms& terms, unsigned long long cacheKey,
                               int x, int y, int w, int h);
    // Wheel-zoom and drag-pan for the map above. Handled from updatePopup(), so
    // it reads the same view rect the last frame drew.
    void updateCeasefireTermsMap(Rectangle slot);
    // The part of the province texture the terms map is currently showing, in
    // texture pixels, for a given on-screen slot. Fills the slot exactly (the
    // auto-fit crop is widened to the slot's aspect), so zoom and pan have no
    // letterbox to fight with.
    Rectangle ceasefireTermsMapView(Rectangle slot) const;
    // Shading for the map above, plus the crop it was built for. Sized to the
    // crop rather than the whole world -- a full-map buffer is tens of millions
    // of pixels, and this one is on screen at thumbnail size.
    std::vector<Color> m_popupTermsMapBuf;
    Texture2D m_popupTermsMapTex{};
    unsigned long long m_popupTermsMapKey = 0;   // popup id the cache belongs to
    bool m_popupTermsMapEmpty = false;           // key resolved to nothing drawable
    int m_popupTermsMapSrcX = 0, m_popupTermsMapSrcY = 0;
    int m_popupTermsMapSrcW = 0, m_popupTermsMapSrcH = 0;
    // View on top of that crop. The centre is absolute (texture pixels) rather
    // than an offset, because clamping the view to the map's edges has to be
    // able to stop the centre moving without leaving a stale offset behind.
    float m_popupTermsMapZoom = 1.0f;
    float m_popupTermsMapCx = 0.0f, m_popupTermsMapCy = 0.0f;
    bool m_popupTermsMapDragging = false;
    Vector2 m_popupTermsMapDragPrev{0, 0};
    void updateCeasefireScreen();

    // ─── Turn processing state ───
    enum TurnState {
        TURN_NORMAL,        // Playing normally
    } m_turnState = TURN_NORMAL;
    // Turned off by a map script with `set rules.rebellions false`. A
    // generated world can be built around a premise that revolts contradict,
    // and the tutorial already suppresses them the same way -- this gives a
    // mapmaker the switch the tutorial has.
    bool m_scriptRebellionsOff = false;

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
    /**
     * Walk a stack home when a treaty leaves it standing somewhere it has no
     * standing to be.
     *
     * A ceasefire or a broken alliance can end with one country's army sitting
     * inside another's borders with neither a war nor an alliance between them
     * -- a position no order the game accepts can produce, and one nothing else
     * cleans up, so the stack sits there indefinitely as a garrison nobody
     * agreed to. This marches it to the nearest province its owner holds, by
     * the province graph rather than by map distance, because armies walk.
     *
     * Rebels are exempt both ways: rebel-held land is a war zone whatever the
     * relations table says, and a rebel stack inside a country IS the revolt.
     */
    void repatriateStrandedArmies();
    void processUpgrades();
    // Refloat a hull that is sitting on land. False if none was found nearby.
    bool nudgeShipToWater(NavyShip& s);

    // ── Sea routing ──
    //
    // A COARSE MAP OF WHERE WATER CONNECTS TO WHAT. The land raster is
    // 8192x4096, far too fine to search per ship per turn, but ocean topology
    // is a large-scale fact: whether the Mediterranean reaches the Atlantic
    // does not depend on 16-pixel detail. This downsamples hard, keeps one real
    // water pixel per navigable cell so every waypoint is guaranteed to be at
    // sea, and labels connected components so "can this fleet even get there"
    // is an O(1) question.
    //
    // Built once per map load; ocean shape does not change during a game.
    struct NavGrid {
        int w = 0, h = 0;                 // cells
        int cell = 0;                     // raster pixels per cell
        std::vector<uint8_t> navigable;   // 1 = has water
        std::vector<int32_t> px, py;      // a real water pixel inside the cell
        std::vector<int32_t> component;   // -1 = land
        bool ready() const { return w > 0 && h > 0; }
    };
    /** One hull has left m_ships: drop every order that named it and shift
     *  the indices of every order that named a later one. See the definition. */
    void forgetShipOrders(int removedIdx);
    /**
     * WHAT THIS COUNTRY WILL BE EARNING IN `turns` TURNS, given only what it
     * has ALREADY committed to.
     *
     * computeCountryIncome answers for right now, and right now is the wrong
     * question for any decision that takes more than one turn to pay off. A
     * factory ordered eight turns ago is not in this turn's income and will be
     * in the income of the turn it lands; a carrier under construction costs
     * nothing yet and 25 a turn for ever afterwards. A player reads both off
     * the build queue without thinking about it. The AI had no way to.
     *
     * Deterministic and cheap: it walks the pending queues, not the map, and
     * asks nothing about what anybody might decide next. Population growth,
     * conquest, war and every other source of change are deliberately absent --
     * this is "what have I already bought", not a forecast.
     */
    CountryIncomeSnapshot projectIncome(int countryId, int turns) const;
    void buildNavGrid();
    // Nearest navigable cell index to a raster pixel, or -1.
    static int navCellNear(const NavGrid& g, int px, int py);
    // Is there a sea route between these two points at all?
    bool navReachable(double lon1, double lat1, double lon2, double lat2) const;
    // Is the straight segment between these two points all water? Used to skip
    // ahead along a route only where the shortcut is genuinely sailable.
    bool navLineClear(double lon1, double lat1, double lon2, double lat2) const;
    // Waypoints from->to, in lon/lat, each guaranteed to be water. Empty if
    // unreachable. The first element is the next place to steer for.
    bool navRoute(double fromLon, double fromLat, double toLon, double toLat,
                  std::vector<std::pair<double, double>>& out) const;
    /**
     * WHERE A FLEET ACTUALLY SITS WHEN IT VISITS A HARBOUR.
     *
     * A port's province centre is a land pixel -- centres are anchored inside
     * the province, which is the whole point of them -- so ordering a hull to
     * one is ordering it ashore. The resolver then clamps the move at the last
     * water on the line, the hull ends up pressed against the beach, and next
     * turn the same order is issued down the same blocked line. Measured on
     * the shipped 1914 map: 38% of all moves that went nowhere had been aimed
     * at dry land in the first place.
     *
     * This answers with the nav grid's own water pixel nearest that province --
     * a point the router has already proven is at sea and in a real body of
     * water. False when the province has no navigable water anywhere near it.
     */
    bool portApproach(int provinceId, double& lon, double& lat) const;
    /**
     * WHICH SEA a province's harbour sits on, as a nav-grid component id, or -1.
     *
     * navReachable answers for a PAIR of points. This answers for one province,
     * so a caller that needs "can any of my harbours reach any of theirs" can
     * intersect two small sets of body ids instead of running a pairwise test
     * over every combination of ports on the map.
     */
    int seaBodyOfPort(int provinceId) const;
    NavGrid m_nav;
    // How far this hull may move or shoot in one turn, in map pixels, and the
    // same figure in degrees for the lon/lat resolvers. THE one definition:
    // the player's range circle, the AI's step and every resolver read it, so
    // no side can quietly get a different rule from another.
    float shipMaxRangePx(const NavyShip& s) const;
    double shipMaxRangeDeg(const NavyShip& s) const;
    // ── WHOSE ORDERS THE MAP SHOWS ──────────────────────────────────
    //
    // Every queued order used to be drawn for every country: the sky-blue line
    // of an AI fleet's next move, the green + over a province some foreign
    // power was recruiting in, "Disbanding..." over another country's
    // garrison. A player could read the AI's whole turn off the map before
    // taking their own -- and the army move arrows are interactive, so they
    // could drag the percentage on somebody else's attack.
    //
    // A spectator is the deliberate exception: nobody is playing, so there is
    // nothing to keep from them, and watching what the AI intends is the point
    // of the mode.
    bool provinceIsPlayers(int pid) const;
    bool shipIsPlayers(int shipIndex) const;

    // Are these two countries at war? Resolvers need this to refuse a shot at
    // somebody nobody declared on; the UI already refused to aim it.
    bool atWarCids(int a, int b) const;
    // The same question about an alliance. Reads BOTH rows -- a scenario's
    // relations.json routinely fills in only one, and the two copies of this
    // that used to live as lambdas in the movement code read one row each.
    bool alliedCids(int a, int b) const;

    // ── ONE ASSAULT, ONE PLACE ──────────────────────────────────────
    //
    // A province was taken in two places that had drifted apart: the land
    // move in processArmyMovement and the landing in processShipDisembarks.
    // Both fought the FIRST hostile stack they found and ignored the rest, so
    // a province held by two enemies changed hands while one of their armies
    // was still standing on it -- the "enemy troops on my land that never took
    // it" the player sees -- and the defence was counted short into the
    // bargain. They also disagreed: a repulsed landing left a tenth of the
    // invaders squatting inside the defender's province forever, a repulsed
    // land attack left nobody, and a landing on an ALLY's coast annexed it.
    //
    // One function now, so an assault is the same event however the troops
    // arrived. `survivors` comes back with what is left of the attacking
    // force, already placed on the ground it ended up holding.
    bool resolveAssault(int attackerCid, int pid, int attackers, int& survivors);
    // Ownership plus every book that follows from it: conquest counters, the
    // pixel and index maps, minority drift, the loser's new claim and the
    // winner's spent one.
    void captureProvince(int newOwner, int pid, bool contested);
    // May `cid` take this province by standing on it? Own and allied ground
    // never changes hands, unclaimed land is colonised, and everything else
    // needs a war -- a rule that lived only in the player's move validator, so
    // a mod or a modified multiplayer client could annex a neutral by walking
    // into it.
    bool mayTakeProvince(int cid, int pid) const;
    // Add troops to a province, merging into that country's stack if it
    // already has one. Two stacks with the same owner in one province is a
    // state the movement code cannot read: it moves a percentage of the FIRST
    // one and leaves the other standing.
    void addTroopsTo(int pid, int cid, int count);
    // Province transfers between REAL countries (rebels excluded), counted for
    // the trainer's stagnation detector. Rebel churn is deliberately not
    // counted: a province flipping between a rebel and its parent every turn
    // is not strategic progress, and treating it as such is what kept dead
    // maps running for thousands of turns.
    void noteRealConquest(int newOwner, int prevOwner);
    long long realConquests() const { return m_realConquests; }
    void resetRealConquests() { m_realConquests = 0; }
    void processEconomy(int countryId);

    /**
     * The cascade that runs when a turn ends with the treasury short.
     *
     * Budgets, then ships, then troops, then unrest for whatever is still
     * unpaid. The order is by what each saves per thing lost, not by what
     * hurts least -- the reasoning, with the numbers, is on the definition.
     */
    void applyBankruptcyPenalties(int countryId, float shortfall,
                                  const CountryIncomeSnapshot& cs);
    void processPopulation();
    std::string saveStateJson();
    void loadStateJson(const std::string& json);
};
