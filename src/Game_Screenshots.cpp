#include "Game.h"
#include "Game_Gdtl.h"
#include "GameInternals.h"
#include "Audio.h"
#include "MapEditor.h"
#include "mods/ModManager.h"

#include <algorithm>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// ─── Scripted screenshot tour ────────────────────────────
// `OpenDoctrines --screenshots <dir> [save.odsv]`
//
// Walks a fixed list of screens, waits for each to settle, and writes a PNG.
// Documentation images are the first thing to go stale in a project that keeps
// moving, and they go stale silently -- a screenshot does not fail to compile.
// The only fix that holds is being able to retake all of them with one command.
//
// HOW IT HOOKS IN
//
// One call at the bottom of Game::run(). The tour does not own a loop, does not
// draw, and does not know what any screen looks like: it sets the same state
// the menus set, lets the real frame happen, and captures the result. So a
// screen that changes is photographed as it now is, and a screen that is broken
// photographs as broken rather than as whatever the tour imagined.
//
// SETTLING
//
// Shots are taken several frames after the state is set, never on the same
// frame. Backgrounds scroll, panels animate open, and the map's border texture
// is built on first draw -- capturing immediately catches a screen mid-assembly.

namespace {

struct Shot {
    const char* name;      // file stem; the PNG is <dir>/<name>.png
    int settleFrames;      // frames to let it settle before capturing
    bool needsWorld;       // requires the save to have been loaded
};

// Order matters: everything before the first needsWorld shot is photographed
// while no world is loaded, which is also the cheapest time to photograph it.
const Shot SHOTS[] = {
    {"main-menu",     30, false},
    // The opening conversation, where it actually plays: on the menu,
    // with no world under it.
    {"menu-intro",   150, false},
    // The sign-off: on the menu, Pr1nted back on the link.
    {"outro",        340, false},
    // The sign-off reached the way it is reached in play: through
    // act=to_menu, which unloads the world under the open dialogue.
    {"outro-live",   340, false},
    // The language picker, over the menu it is opened from.
    {"language",      20, false},
    // ...and the same list where a player already in a game finds it.
    {"language-settings", 20, false},
    // The menu in a language that is not English, which is the only way to see
    // whether the table, the atlas and the layout actually work together.
    {"menu-de",       20, false},
    {"menu-ja",       20, false},
    {"menu-uk",       20, false},
    {"menu-hi",       20, false},
    {"menu-ar",       20, false},
    {"menu-ko",       20, false},
    {"menu-bg",       20, false},
    {"menu-tr",       20, false},
    {"menu-ur",       20, false},
    {"language-uk",   20, false},
    {"mods",          20, false},
    {"multiplayer",   20, false},
    {"map-editor",    45, false},
    // The translation layer, in a build that has it with the option switched
    // on -- see the skip in tickScreenshotTour. Before the world shots because
    // they need no world, and loading one costs seconds.
    {"gdtl-info",        20, false},
    {"gdtl-warning",     20, false},
    {"gdtl-destination", 20, false},
    {"gdtl-result",      20, false},
    // The other half of the feature: the card that reads a map back IN,
    // which lives on the Custom tab and so is not in any shot above.
    {"gdtl-import",      20, false},

    // The tutorial world, to prove its own flags and ships load. It is a
    // different map from the tour's, so it is loaded on its own.
    {"tutorial-world", 60, false},
    {"world-map",     45, true},
    // The same map with the names written the way another language writes
    // them: the proof that a generated name is transliterated rather than left
    // in Latin among Cyrillic.
    {"world-map-uk",  45, true},
    {"world-map-ja",  45, true},
    {"province",      20, true},
    {"policies",      20, true},
    {"economy",       20, true},
    {"research",      20, true},
    // The comms window over the map. Photographed like everything else here:
    // by setting the state the F9 key sets and letting the real frame happen,
    // so a broken filter photographs as broken.
    {"comms",         40, true},
    // The link coming up and going down. Both are a fifth of a second, so the
    // settle counts ARE the shot: three frames in the tube is still opening,
    // and forty-one is after the static has peaked and the picture has begun
    // to fold back into the line it came out of.
    {"comms-arrive",   6, true},
    {"comms-leave",   41, true},
    {"tutorial",      110, true},
    // Further in, where the script hands over to a different speaker: the one
    // moment that proves the cast and the dropout between them.
    {"tutorial-swap", 135, true},
    {"tutorial-unknown", 110, true},
    // The intro script, for the markup: it is the only page that
    // carries an action, a strike and an accent run at once.
    {"intro-markup", 150, true},
    // The choice page, waiting for a pick.
    {"intro-choice", 260, true},
    // Two people on the link at once, which is what a conversation is.
    {"intro-two",    260, true},
    // The specialised topic menu, and the speaker plate.
    {"specifics",    260, true},
    // The diplomacy topic, mid-walkthrough.
    {"diplomacy",    260, true},
    // The relations panel on a foreign country: the act buttons at both
    // widths, and the "Cancel <act>" form that is the longest label they hold.
    {"diplo-acts",    60, true},
    {"diplo-pending", 60, true},
    // The country finder, with a query typed into it: the list, the highlight
    // and the country names in the player's own script.
    {"find-country",  60, true},
    // The main menu on a phone-shaped canvas.
    {"menu-portrait", 40, false},
    {"world-portrait", 60, true},
    // Every "I know the basics" topic, opened for real: the shot is the proof
    // that the script parses, the speaker resolves and the first page draws.
    {"topic-ships",    120, true},
    {"topic-research", 120, true},
    {"topic-economy",  120, true},
    {"topic-unrest",   120, true},
    // The way out of the tutorial, where somebody looking for one goes.
    {"tutorial-stop", 150, true},
    // The tutorial pointer: a ringed tab, and the input gate on.
    {"pointer-rect", 150, true},
    {"pointer-round", 150, true},
    // The same ring after the window has changed size, which is where
    // it used to drift away from what it points at.
    {"pointer-resized", 200, true},
    // A small target, and a deliberately wrong name.
    {"pointer-small", 150, true},
    {"pointer-miss",  150, true},
    // The lesson page that names four keybinds by lookup.
    {"tutorial-keys", 300, true},
    // A page held open by its `until`: the waiting mark, not the ▼.
    {"tutorial-waiting", 300, true},
    // The turn button before the lesson has introduced it.
    {"turn-locked",   200, true},
};
const int SHOT_COUNT = (int)(sizeof(SHOTS) / sizeof(SHOTS[0]));

// A shot that only exists when the feature does.
bool isGdtlShot(const char* name) {
    return std::string(name).rfind("gdtl-", 0) == 0;
}

}  // namespace

// ─── MEASURING, FOR tools/i18n_fit.py ─────────────────────────────────────
//
// Grouped by language and switched once per group: setLanguage rebuilds the
// glyph atlas, which is the expensive part, and doing it per line turned a
// two-second job into minutes.
bool Game::measureTextJobs(const std::string& inPath, const std::string& outPath) {
    std::ifstream in(inPath);
    if (!in) {
        fprintf(stderr, "[MEASURE] cannot read %s\n", inPath.c_str());
        return false;
    }
    struct Job { std::string lang; int size; std::string text; };
    std::vector<Job> jobs;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const size_t a = line.find('\t');
        if (a == std::string::npos) continue;
        const size_t b = line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        jobs.push_back({line.substr(0, a),
                        atoi(line.substr(a + 1, b - a - 1).c_str()),
                        line.substr(b + 1)});
    }
    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const Job& x, const Job& y) { return x.lang < y.lang; });

    std::ofstream out(outPath);
    if (!out) {
        fprintf(stderr, "[MEASURE] cannot write %s\n", outPath.c_str());
        return false;
    }
    std::string current;
    for (const Job& j : jobs) {
        if (j.lang != current) {
            applyLanguageForShot(j.lang.c_str());
            current = j.lang;
        }
        // MeasureText is shadowed to odText::measureText, which is the same
        // call every widget makes -- so this is the width the button sees,
        // not a reconstruction of it.
        out << j.lang << '\t' << j.size << '\t'
            << MeasureText(j.text.c_str(), j.size) << '\t' << j.text << '\n';
    }
    printf("[MEASURE] %zu string(s) measured\n", jobs.size());
    return true;
}

void Game::beginScreenshotTour(const std::string& outDir, const std::string& savePath) {
    m_shotTour  = true;
    // THE LANGUAGE THE TOUR IS BEING RUN IN.
    //
    // The menu-<code> shots change it and used to leave it changed, so every
    // shot after the last of them was photographed in whatever language came
    // last in the array rather than in the configured one -- the whole
    // in-world half of the set, silently, including the pictures that ship.
    // It also made the OD_I18N_FIT sweep useless: five languages were asked
    // for and the same one answered.
    m_shotBaseLang = od::i18n::language();
    m_shotDir   = outDir;
    m_shotSave  = savePath;
    m_shotIndex = 0;
    m_shotFrame = 0;
    if (!m_shotDir.empty() && m_shotDir.back() == '/') m_shotDir.pop_back();
    // Same reason as the GIF export in Game_History.cpp: "mkdir -p" is a POSIX
    // command that cmd.exe does not have, and it spawns a shell to do what one
    // library call does on every platform.
    {
        std::error_code ec;
        std::filesystem::create_directories(m_shotDir, ec);
        if (ec)
            fprintf(stderr, "[SHOT] could not create %s: %s\n",
                    m_shotDir.c_str(), ec.message().c_str());
    }

    // The tour starts on the main menu, never on the splash: the splash is a
    // timed fade, so shooting it means racing it.
    m_currentScreen = SCREEN_MENU;
    m_inSettings = false;
    printf("[SHOT] %d screens -> %s\n", SHOT_COUNT, m_shotDir.c_str());
}

// Everything the world shots share: the save loaded, on the map, with a
// province worth looking at already selected. Run once, before the first of
// them, because loading a save costs seconds and the shots differ only in
// which panel is open over it.
//
// Returns false if the world could not be loaded, which fails the tour rather
// than quietly producing four pictures of an empty map.
static bool g_worldReady = false;

// setLanguage + the redraws applyLanguage does, WITHOUT saving the config: the
// tour must not leave the player in Japanese because it photographed a map.
void Game::applyLanguageForShot(const char* code) {
    od::i18n::setLanguage(code, m_dataDir);
    reloadFonts();
    if (m_renderer && !m_countryLabels.empty()) {
        computeCountryLabels();
        m_renderer->setCountryLabels(&m_countryLabels);
    }
}

bool Game::tickScreenshotTour() {
    if (m_shotIndex >= SHOT_COUNT) {
        printf("[SHOT] done\n");
        return false;
    }
    const Shot& shot = SHOTS[m_shotIndex];

    // OD_SHOT_ONLY=a,b photographs just those screens. A development aid for
    // working on one of them: the tour is eighty pictures and several minutes,
    // and most of that is loading the same save over and over. Shots that lean
    // on a world a previous shot loaded will not stand up on their own, so
    // this is for looking at one screen, not for producing the set.
    if (const char* only = std::getenv("OD_SHOT_ONLY")) {
        const std::string list = std::string(",") + only + ",";
        if (list.find(std::string(",") + shot.name + ",") == std::string::npos) {
            ++m_shotIndex;
            m_shotFrame = 0;
            return true;
        }
    }

    // Skip whole screens the build does not have rather than photographing a
    // browser with no button on it.
    if (isGdtlShot(shot.name) && !(m_config.gdtl && Gdtl::available())) {
        ++m_shotIndex;
        m_shotFrame = 0;
        return true;
    }

    // ── frame 0: put the game on the screen this shot wants ──
    if (m_shotFrame == 0) {
        // Back to the tour's own language unless this shot is one of the ones
        // whose whole point is a different one. See m_shotBaseLang.
        const bool ownsLanguage = (shot.name == std::string("language-uk")) ||
                                  (std::string(shot.name).rfind("menu-", 0) == 0 &&
                                   std::string(shot.name).size() == 7);
        if (!ownsLanguage && od::i18n::language() != m_shotBaseLang)
            applyLanguageForShot(m_shotBaseLang.c_str());
        // A world shot means the MAP is on screen, every time -- not merely
        // the first time one is asked for.
        //
        // SCREEN_PLAYING used to be set only inside the load below, so a shot
        // that changed the screen (the menu, the map editor, the tutorial
        // sign-off) left it changed and every world shot after it quietly
        // photographed whatever that was. pointer-small came back as a
        // picture of the map editor's front page.
        if (shot.needsWorld && g_worldReady) {
            m_currentScreen = SCREEN_PLAYING;
            m_paused = false;
            m_inSettings = false;
        }
        if (shot.needsWorld && !g_worldReady) {
            if (m_shotSave.empty()) {
                fprintf(stderr, "[SHOT] %s needs a save and none was given\n", shot.name);
                return false;
            }
            printf("[SHOT] loading %s\n", m_shotSave.c_str());
            startLoadedGame(m_shotSave);
            // Same hand-cranking as the simulation: the loader normally runs a
            // step per frame from run(), and we are inside that frame already.
            while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
                if (WindowShouldClose()) return false;
                updateLoading();
            }
            if (m_loadingFailed) {
                fprintf(stderr, "[SHOT] could not load %s\n", m_shotSave.c_str());
                return false;
            }
            hideLoadingScreen();
            m_currentScreen = SCREEN_PLAYING;

            // Play as the country holding the most ground, so the panels have
            // real numbers in them. A spectator (country 0) renders the same
            // map with every player-facing panel empty, which is a picture of
            // the UI not working.
            {
                std::vector<int> byCountry;
                for (int owner : m_provinceCountryLookup) {
                    if (owner <= 0 || owner >= REBEL_CID_MIN) continue;
                    if ((int)byCountry.size() <= owner) byCountry.resize(owner + 1, 0);
                    byCountry[owner]++;
                }
                int best = 0, bestN = 0;
                for (int cid = 1; cid < (int)byCountry.size(); ++cid)
                    if (byCountry[cid] > bestN) { bestN = byCountry[cid]; best = cid; }
                m_playerCountryId = best;
                printf("[SHOT] playing as country %d (%d provinces)\n", best, bestN);

                // The most populous province we own: the province panel is
                // mostly numbers, and an empty tundra tile shows none of them.
                int pick = 0;
                long bestPop = -1;
                for (auto& [pid, pop] : m_provincePopulations) {
                    if (pid <= 0 || (size_t)pid >= m_provinceCountryLookup.size()) continue;
                    if (m_provinceCountryLookup[pid] != best) continue;
                    if ((long)pop > bestPop) { bestPop = (long)pop; pick = pid; }
                }
                m_shotProvince = pick;

                // A province somebody ELSE owns, for the diplomacy shots.
                //
                // The act buttons only exist for a country that is not yours,
                // and they lay out two to a row -- so a target offering one
                // action photographs a full-width button and a target offering
                // five photographs the HALF-width one, which is the narrow box
                // the long labels actually overflow. The tour had only ever
                // seen the wide one, which is why sweeping five languages for
                // overflow found nothing: the tight geometry was never drawn.
                int fpick = 0; long fbestPop = -1;
                for (auto& [pid, pop] : m_provincePopulations) {
                    if (pid <= 0 || (size_t)pid >= m_provinceCountryLookup.size()) continue;
                    const int owner = m_provinceCountryLookup[pid];
                    if (owner <= 0 || owner == best || owner >= REBEL_CID_MIN) continue;
                    if ((long)pop > fbestPop) { fbestPop = (long)pop; fpick = pid; }
                }
                m_shotForeignProvince = fpick;
            }
            g_worldReady = true;
        }

        // Every shot starts from a clean slate, so an overlay left open by the
        // previous one cannot end up in this one's picture.
        m_inResearch = m_inEconomy = m_inPolitics = m_inClaims = false;
        if (std::string(shot.name) != "find-country") m_findOpen = false;
        m_activeSidebarTab = 0;
        m_inSettings = false;

        // Which province the panels talk about. Cleared for the map shot,
        // because that one is meant to show the map and nothing over it.
        //
        // The selection lives on the RENDERER: update() copies it into
        // m_lastSelectedProvince every frame, so setting the Game-side field
        // alone is undone before anything is drawn.
        if (shot.needsWorld) {
            const std::string sn = shot.name;
            const int pid = (sn == "world-map") ? 0
                          : (sn.rfind("diplo-", 0) == 0) ? m_shotForeignProvince
                          : m_shotProvince;
            if (m_renderer) m_renderer->setSelectedProvince(pid);
            m_lastSelectedProvince = pid;
            if (pid > 0) buildCountryProvinceList(pid);
        }

        const std::string name = shot.name;
        if (name != "pointer-resized" && name != "menu-portrait" &&
            name != "world-portrait" &&
            (GetScreenWidth() != 1600 || GetScreenHeight() != 900))
            SetWindowSize(1600, 900);   // undo the resize shot, for everyone after it
        if (name == "tutorial-world") {
            // The tour photographs one save; this shot needs a different map
            // entirely, so it loads it the way the menu button does and then
            // hand-cranks the loader, exactly as the world shots do.
            m_tutorialMode = true;
            startNewGameWithName(m_dataDir + "STDmaps/tutorial.odmap", "TutorialShot");
            m_loadingShouldCreateSave = false;
            m_quickStartPending = true;
            m_forcedStartIso = "ASH";    // exactly as startTutorialWorld does
            while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
                if (WindowShouldClose()) return false;
                updateLoading();
            }
            hideLoadingScreen();
            m_currentScreen = SCREEN_PLAYING;
            m_activeViewTab = 0;
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            if (m_dialogOpen) endDialogue();
            // The same framing the lesson opens on.
            m_tutorialPending = true;
            updateDialogue(0.016f);
            if (m_dialogOpen) endDialogue();
        } else if (name == "main-menu") {
            m_currentScreen = SCREEN_MENU;
        } else if (name == "menu-intro") {
            m_currentScreen = SCREEN_MENU;
            m_inSettings = false;
            // beginDialogue, not startTutorial: the tour must not go on to
            // load the tutorial world when the script ends.
            // Page 0 deliberately: the first face on the link must be
            // Pr1nted's, and that is the thing worth photographing.
            beginDialogue("intro");
        } else if (name == "language") {
            m_currentScreen = SCREEN_MENU;
            m_inSettings = false;
            m_languageOpen = true;
        } else if (name == "language-settings") {
            m_currentScreen = SCREEN_MENU;
            m_inSettings = true;
            m_settingsTab = LANGUAGE_TAB;
            m_settingsIndex = 0;
            m_settingsScroll = 0;
        } else if (name == "world-map-uk" || name == "world-map-ja") {
            applyLanguageForShot(name == "world-map-uk" ? "uk" : "ja");
            m_activeViewTab = 8;   // Country Names: the view these labels are for
            if (m_renderer) m_renderer->setSelectedProvince(0);
        } else if (name == "language-uk") {
            m_currentScreen = SCREEN_MENU;
            m_inSettings = false;
            applyLanguageForShot("uk");
            m_languageOpen = true;
        } else if (name.rfind("menu-", 0) == 0 && name.size() == 7) {
            // menu-<code>: the main menu in one language. Every script the game
            // can draw has a shot here, because the scripts that need shaping
            // fail SILENTLY -- Devanagari without a shaper draws the halant as
            // a visible mark, which looks like a font choice rather than a bug
            // unless somebody who reads it looks, or unless there is a picture.
            m_currentScreen = SCREEN_MENU;
            m_inSettings = false;
            m_languageOpen = false;
            // setLanguage + reloadFonts rather than applyLanguage: the tour
            // must not write the player's config on its way past.
            applyLanguageForShot(name.substr(5).c_str());
        } else if (name == "mods") {
            m_modIndex = m_modScroll = 0;
            m_modAdvancedFor = m_modDeleteFor = m_modAiWarnFor = -1;
            ModManager::get().rescan();
            m_currentScreen = SCREEN_MODS;
        } else if (name == "multiplayer") {
            openMultiplayerMenu();
        } else if (name == "map-editor") {
            if (!m_mapEditor) {
                // Loads synchronously on this thread, exactly as the menu does.
                Audio::BlockingCall quiet;
                m_mapEditor = new MapEditor();
                m_mapEditor->init(m_screenW, m_screenH, m_dataDir);
            }
            m_currentScreen = SCREEN_MAP_EDITOR;
        } else if (name == "gdtl-info") {
            // The map browser, with the info panel open on the first world --
            // which is where the Translate button lives.
            loadMapEntries();
            m_mapTabIndex = 0;
            m_currentScreen = SCREEN_MAP_SELECT;
            m_gdtlStage = GdtlStage::None;
            m_showMapInfoPopup = true;
            m_mapInfoIndex = 0;
        } else if (name == "gdtl-warning") {
            m_showMapInfoPopup = false;
            m_mapInfoIndex = -1;
            m_gdtlMapIndex = 0;
            m_gdtlStage = GdtlStage::Warning;
        } else if (name == "gdtl-destination") {
            m_gdtlStage = GdtlStage::Destination;
        } else if (name == "gdtl-import") {
            // The custom worlds tab, where the import card is the last entry.
            loadMapEntries();
            m_gdtlStage = GdtlStage::None;
            m_showMapInfoPopup = false;
            m_mapInfoIndex = -1;
            m_mapTabIndex = 1;
            // The card is the last entry, so on a machine with custom maps
            // already in it the tab opens above the thing being photographed.
            // The draw clamps this to the real maximum, so asking for far more
            // scroll than exists is how you say "the bottom" from here.
            m_mapScroll = 9999;
            m_currentScreen = SCREEN_MAP_SELECT;
        } else if (name == "gdtl-result") {
            // Not a mock-up: this runs the conversion the button runs, through
            // the same method, and photographs whatever it actually reported.
            const std::string out = m_shotDir + "/gdtl-translated-map";
            std::filesystem::remove_all(out);
            gdtlTranslateTo(out);
            printf("[SHOT] gdtl: ok=%d notes=%zu %s\n", (int)m_gdtlOk, m_gdtlNotes.size(),
                   m_gdtlMessage.c_str());
        } else if (name == "menu-portrait") {
            // A phone held upright. The menu had never been measured against a
            // canvas narrower than it is tall, and the title ran off both
            // edges; this is the shot that would have caught it.
            m_currentScreen = SCREEN_MENU;
            m_inSettings = false;
            m_languageOpen = false;
            SetWindowSize(402, 874);          // iPhone 16 Pro, logical points
        } else if (name == "world-portrait") {
            // The map and its panels on the same phone canvas. The menu is a
            // column and survives being narrow; this is the screen that has a
            // province panel down the left, four tabs down the right and eight
            // view tabs along the bottom, all at fixed pixel offsets.
            m_activeSidebarTab = 0;
            m_activeViewTab = 0;
            SetWindowSize(402, 874);
        } else if (name == "find-country") {
            if (m_dialogOpen) endDialogue();
            m_activeSidebarTab = 0;
            m_activeViewTab = 0;
            m_findOpen = true;
            m_findQuery = "ind";        // enough to show ranking, short enough to type
            m_findIndex = 0;
            rebuildFindMatches();
        } else if (name == "diplo-acts") {
            // The relations panel on somebody else's country: the act buttons,
            // laid out two to a row. The tutorial box from the diplomacy shot
            // before this one covers exactly the corner they sit in.
            if (m_dialogOpen) endDialogue();
            m_activeSidebarTab = 0;
            m_activeViewTab = 4;
            m_pendingDiplomaticActions.clear();
        } else if (name == "diplo-pending") {
            if (m_dialogOpen) endDialogue();
            // The same panel with a request already sent, which is the form
            // that overflows: the label becomes "Cancel <the whole act name>"
            // and it is the longest string the button ever holds.
            m_activeSidebarTab = 0;
            m_activeViewTab = 4;
            m_pendingDiplomaticActions.clear();
            const Country* pc = m_countries.getCountry(m_playerCountryId);
            const int tcid = (m_shotForeignProvince > 0 &&
                              (size_t)m_shotForeignProvince < m_provinceCountryLookup.size())
                                 ? m_provinceCountryLookup[m_shotForeignProvince] : 0;
            const Country* tc = m_countries.getCountry(tcid);
            if (pc && tc) {
                PendingDiplomaticAction pda;
                pda.sourceIso = pc->isoA3;
                pda.targetIso = tc->isoA3;
                pda.action    = "request_alliance";
                m_pendingDiplomaticActions.push_back(pda);
            }
        } else if (name == "world-map") {
            m_activeViewTab = 0;          // no panel: this shot is the map itself
        } else if (name == "province") {
            m_activeViewTab = 2;          // industry: the busiest of the tabs
        } else if (name == "policies") {
            m_activeSidebarTab = 1;
            m_inPolitics = true;
            // With every folder collapsed this photographs five headers and
            // nothing else -- the doctrines, their gains and their costs are
            // the whole subject of the screen. Opened here rather than left to
            // whatever the last session happened to leave expanded, so the shot
            // is the same every time it is retaken.
            m_openFolders.insert("Left");
            m_openFolders.insert("Right");
            m_policyScroll = 0;
        } else if (name == "economy") {
            m_activeSidebarTab = 2;
            m_inEconomy = true;
        } else if (name == "research") {
            m_activeSidebarTab = 4;
            m_inResearch = true;
        } else if (name == "intro-markup") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            // Unconditionally: a shot earlier in the tour leaves the
            // tutorial dialogue open, and "if not already open" then quietly
            // photographs THAT script instead of this one.
            beginDialogue("intro");
            m_dialog.jumpTo(2);          // "never ran a country before?"
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "turn-locked") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_activeViewTab = 0;
            m_tutorialMode = true;
            m_tutorialTurnUnlocked = false;
            beginDialogue("tutorial");
            m_dialog.jumpTo(1);
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "tutorial-waiting") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;   // so open:economy is FALSE
            m_activeViewTab = 0;
            beginDialogue("tutorial");
            m_dialog.jumpTo(8);          // "open the economy" -- waits for the tab
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "tutorial-keys") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_activeViewTab = 0;
            beginDialogue("tutorial");
            m_dialog.jumpTo(9);          // artillery and the ship orders
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "comms-arrive") {
            // Straight from beginDialogue, which is what tunes a speaker in.
            if (m_dialogOpen) endDialogue();
            m_comms.tuneOut();
            m_comms2.tuneOut();
            for (int i = 0; i < 200; ++i) { m_comms.update(0.016f); m_comms2.update(0.016f); }
            beginDialogue("tutorial");
        } else if (name == "comms-leave") {
            // And the way one leaves: the same call act=tune_out makes.
            if (!m_dialogOpen) beginDialogue("tutorial");
            for (int i = 0; i < 120; ++i) { m_comms.update(0.016f); m_comms2.update(0.016f); }
            commsHangUp();
        } else if (name == "outro-live") {
            // THE HANDOVER, exactly as play performs it -- ON A REAL WORLD.
            //
            // The first version of this shot never loaded one, set the screen
            // to PLAYING by hand and then called update(): it crashed on a
            // renderer that had never existed, which looks exactly like the
            // reported crash and proves nothing about it. So the world is
            // loaded here the way the tutorial button loads it, and the
            // lesson's last page is allowed to hand over on its own.
            m_tutorialMode = true;
            startNewGameWithName(m_dataDir + "STDmaps/tutorial.odmap", "OutroShot");
            m_loadingShouldCreateSave = false;
            m_quickStartPending = true;
            m_forcedStartIso = "ASH";
            while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
                if (WindowShouldClose()) return false;
                updateLoading();
            }
            hideLoadingScreen();
            m_currentScreen = SCREEN_PLAYING;
            m_activeViewTab = 0;
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            if (m_dialogOpen) endDialogue();
            m_tutorialTurnUnlocked = true;

            beginDialogue("tutorial");
            m_dialog.jumpTo(m_dialog.pageCount() - 1);   // act=then:outro
            updateDialogue(1.0f / 60.0f);                // the page change runs it
            endDialogue();                               // hands to the outro
            update(1.0f / 60.0f);                        // first full frame of it
            m_dialog.jumpTo(2);                          // Pr1nted asks
            // Let him finish asking: the box only takes an answer once the
            // page has finished typing itself out, so a single frame here
            // means commitChoice does nothing and the shot proves nothing.
            for (int i = 0; i < 600 && !m_dialog.awaitingChoice(); ++i)
                updateDialogue(1.0f / 60.0f);

            // AND TAKE THE ANSWER THAT CRASHED.
            //
            // "Go back down and cover something specific" is world:specifics:
            // it unloads the world and starts loading another one, from inside
            // updateDialogue, from inside update() -- and everything below
            // that call is the playing-screen update, which then ran against a
            // renderer that had just been deleted. That is the reported crash,
            // and this is the frame it happened on.
            m_dialog.commitChoice();
            update(1.0f / 60.0f);

            // Then let the world it asked for come up, exactly as run() does.
            while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
                if (WindowShouldClose()) return false;
                updateLoading();
            }
            hideLoadingScreen();
            // The world must actually be back. Forcing SCREEN_PLAYING with no
            // renderer is how the first version of this shot manufactured a
            // crash of its own and sent the hunt after the wrong bug.
            if (!m_renderer) {
                fprintf(stderr, "[SHOT] outro-live: the world never came back\n");
                return false;
            }
            m_currentScreen = SCREEN_PLAYING;
            for (int i = 0; i < 4; ++i) update(1.0f / 60.0f);
        } else if (name == "outro") {
            m_currentScreen = SCREEN_MENU;
            m_inSettings = false;
            beginDialogue("outro");
            m_dialog.jumpTo(2);          // Pr1nted asks
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "tutorial-stop") {
            // The button only exists during a tutorial, so the shot has to be
            // in one. Not startTutorial: that would load the tutorial world
            // out from under the tour. The pause and the settings are set
            // AFTER the reset above, which is what clears them for everyone
            // else.
            m_tutorialMode = true;
            m_paused = true;
            m_inSettings = true;
            m_settingsTab = 0;
            m_settingsScroll = 0;
        } else if (name.rfind("topic-", 0) == 0) {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_activeViewTab = 0;
            beginDialogue("tut_" + name.substr(6));
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "diplomacy") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_activeViewTab = 4;         // relations, which the page asks for
            beginDialogue("tut_diplomacy");
            m_dialog.jumpTo(6);          // "all of it is greyed"
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "specifics") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_activeViewTab = 0;
            beginDialogue("specifics");
            m_dialogPage = m_dialog.pageIndex();
            // The box grows per option, so the shot is also the check that
            // five of them still fit under the question.
        } else if (name == "pointer-resized") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_activeViewTab = 0;
            SetWindowSize(1280, 720);
            beginDialogue("tutorial_pointer");
            m_dialog.jumpTo(0);          // the ringed, gated economy tab
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "pointer-rect" || name == "pointer-round" ||
                   name == "pointer-small" || name == "pointer-miss") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_activeViewTab = 0;
            beginDialogue("tutorial_pointer");
            m_dialog.jumpTo(name == "pointer-rect"  ? 0
                          : name == "pointer-round" ? 1
                          : name == "pointer-small" ? 2 : 6);
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "intro-two") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            beginDialogue("intro");
            m_dialog.jumpTo(5);          // Pr1nted, before Mia answers
            commsSpeaker("Pr1nted");
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "intro-choice") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            beginDialogue("intro");
            m_dialog.jumpTo(11);         // "tell us now," + the two options
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "tutorial-unknown") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            beginDialogue("markup_demo");
            m_dialog.jumpTo(6);          // the Cryptographer's intercept
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "tutorial-swap") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            beginDialogue("markup_demo");
            m_dialog.jumpTo(4);          // the Signals Officer's page
            if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
            m_dialogPage = m_dialog.pageIndex();
        } else if (name == "tutorial") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_activeViewTab = 0;
            beginDialogue("tutorial");
        } else if (name == "comms") {
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_activeViewTab = 0;
            if (!m_commsOpen) toggleComms();
        }
    }

    // A shot that needs something to happen PARTWAY through its settle. The
    // two-speaker window is the case: driving both page changes in frame 0
    // tests a sequence the game never performs, because in play there are
    // seconds of frames between one person speaking and the next.
    if (std::string(shot.name) == "pointer-resized" && m_shotFrame == 120) {
        // Resize AGAIN part-way through, so the capture is of a window that
        // has just moved rather than one that settled minutes ago.
        SetWindowSize(1500, 820);
    }
    if (std::string(shot.name) == "intro-two" && m_shotFrame == 90) {
        m_dialog.jumpTo(6);          // Mia answers; both are now on the link
        if (const dlg::Page* pg = m_dialog.currentPage()) commsSpeaker(pg->speaker);
        m_dialogPage = m_dialog.pageIndex();
    }

    // ── settle, then capture ──
    if (++m_shotFrame < shot.settleFrames) return true;


    // TakeScreenshot throws the directory away -- it calls GetFileName() on
    // whatever it is handed and writes the result into the working directory.
    // Passing a full path therefore silently drops nine PNGs into the repo
    // root and reports success, so the move has to happen here.
    const std::string file = std::string(shot.name) + ".png";
    const std::string path = m_shotDir + "/" + file;
    TakeScreenshot(file.c_str());
    if (rename(file.c_str(), path.c_str()) != 0) {
        fprintf(stderr, "[SHOT] captured %s but could not move it to %s\n",
                file.c_str(), path.c_str());
        return false;
    }
    printf("[SHOT] %s\n", path.c_str());
    fflush(stdout);

    m_shotIndex++;
    m_shotFrame = 0;
    return m_shotIndex < SHOT_COUNT;
}
