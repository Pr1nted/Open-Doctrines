#include "Game.h"
#include <ctime>
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
    // ...and the editor with a real map open, on the Districts tools.
    {"editor-districts", 90, false},
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
    // ── THE CLAIMS TABS, IN A LANGUAGE THAT DOES NOT FIT ──
    //
    // These sat on a fixed 140 px pitch and overlapped in every language whose
    // words are longer than English's, which is most of them. Nobody saw it
    // because the claims screen had no shot at all -- so it gets one, and it
    // gets one in Ukrainian, where the two long labels are what break it.
    {"claims-uk",     25, true},
    // The Districts tab: an inline map painted by district, the budget they
    // divide, and the shares. Photographed because it is a new screen and
    // because every layout fault this session was found in a screenshot.
    {"districts",     25, true},
    // The country profile, opened on the player's own country so the published
    // figures are togglable and the flag strip has something in it.
    {"country-profile", 25, true},
    // A FOREIGN country's profile, with mail on: the one that carries "Write
    // to them". The shot above is the player's own, where it must not appear.
    {"profile-mail",  25, true},
    {"world-map-ja",  45, true},
    {"province",      20, true},
    // The army view: the garrison list, whose stacks are sharing the ground,
    // and the buttons that act on them. It is the screen unit types are FOR,
    // and it was built without anybody being able to look at it -- so it is
    // photographed every time these are retaken.
    {"army",          20, true},
    // The recruit picker, which only appears once a country has researched a
    // formation -- so the shot grants them, because "invisible until you have
    // the tech" is exactly the thing that would otherwise never be photographed.
    {"army-kinds",    20, true},
    // The same province with MECHANISED selected. Two shots that differ only in
    // the picked kind are the proof that the manpower rule is live: the same
    // people raise a quarter as many of them, and cost more money doing it.
    {"army-mech",     20, true},
    // The army research tab, which is where the Formations branch lives. Its
    // first draft was drawn straight through the navy column; nothing but a
    // picture of it would have said so.
    {"research-army", 25, true},
    // The Orders option, which hangs off Process Turn -- lit, with something to
    // show -- on a desktop canvas and on a phone. Both, because the request was
    // for a control that works on a phone AND does not look odd on a computer,
    // and neither half of that can be checked by reasoning about it.
    {"orders-desktop", 30, true},
    {"orders-portrait", 30, true},
    // The between-turns phase itself: banner, the one button, and a map with
    // everybody's orders on it. A phase that waits for a person is a hang for
    // anything that does not have one, so the tour sets the state directly
    // rather than processing a turn to reach it.
    {"orders-phase",  30, true},
    {"orders-zoom",   20, true},
    {"policies",      20, true},
    {"economy",       20, true},
    {"economy-local", 20, true},
    {"research",      20, true},
    {"research-portrait", 20, true},
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
    // The report form, with a report half written in it and the diagnostics
    // box ticked -- the layout is all hand-computed, and the chip row wraps.
    {"feedback",      60, true},
    // The same form showing the diagnostics in full, which is the promise the
    // feature makes and the one thing worth checking by eye.
    {"feedback-diag", 60, true},
    // The rating prompt, in its corner.
    {"rating",        60, true},
    // The post: a correspondence with letters both ways, one still unsent.
    {"mail",          60, true},
    {"mail-list",     60, true},
    // The mail settings, and the dialog for reporting a letter.
    {"mail-settings", 60, true},
    // The setup screen as a player with NO module actually reaches it: through
    // Experimental > AI Correspondents, with nothing configured and no Mail
    // button anywhere on screen. This is the state the feature was unreachable
    // in, so it is the state worth photographing.
    {"llm-setup",    120, true},
    // The SIDEBAR with a model configured, which is where the Mail button has
    // to appear. It did not: availability was recomputed only when the "use a
    // language model" checkbox was toggled, so a player who pulled a model --
    // never touching that box again -- got no button at all.
    {"mail-button",   60, true},
    // THE SEQUENCE THAT BROKE IT: open the setup from the settings menu, close
    // it, then press Mail. The setup flag used to survive the Close and the
    // post opened on the runner installer.
    {"mail-after-setup", 60, true},
    // The picker with its filter, and the caret sitting where the text ends.
    {"mail-pick",     60, true},
    // A room with three countries in it, opened by its owner: the member strip
    // and the remove controls only they can see.
    {"mail-group",    60, true},
    {"mail-caret",    60, true},
    {"mail-report",   60, true},
    // The developer queue, with one report opened for a decision.
    {"dev-reports",   40, false},
    {"dev-lookup",    40, false},
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
    // Tells Config::save to refuse for the rest of this process. The tour
    // mutates the live config for each shot, and a save from anywhere would
    // make those permanent -- see the note in Config::save.
#if defined(_WIN32)
    _putenv_s("OD_SHOT_TOUR", "1");
#else
    setenv("OD_SHOT_TOUR", "1", 1);
#endif
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
            name != "world-portrait" && name != "orders-portrait" &&
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
        } else if (name == "profile-mail") {
            m_config.llmEnabled  = true;
            m_config.llmEndpoint = "http://127.0.0.1:11434/v1";
            m_config.llmModel    = "llama3.1:8b";
            m_config.mailPolicy  = (int)mail::Policy::Everyone;
            m_llmAvailable = true;
            m_mailOpen = false;
            m_inCountryProfile = true;
            m_profileScroll = 0;
            m_profileCountryId = 0;
            for (const auto& [cid2, c2] : m_countries.getAll()) {
                (void)c2;
                if (cid2 > 0 && cid2 != m_playerCountryId && cid2 < REBEL_CID_MIN) {
                    m_profileCountryId = cid2; break;
                }
            }
        } else if (name == "country-profile") {
            m_inCountryProfile = true;
            m_profileCountryId = m_playerCountryId;
            m_profileScroll = 0;
            // A history to show: two eras, so the strip is not a single flag.
            if (Country* pc2 = m_countries.getCountry(m_playerCountryId)) {
                if (pc2->flagHistory.empty()) {
                    pc2->flagHistory.push_back({0, pc2->flagActual, pc2->flagCensored});
                    pc2->flagHistory.push_back({14, pc2->flagActual, pc2->flagCensored});
                }
                pc2->foundedTurn = std::max(0, m_turnNumber - 38);
            }
            m_countryDisclosure[m_playerCountryId] =
                DISCLOSE_EXPENSES | DISCLOSE_TREASURY | DISCLOSE_DISTRICT_LAWS |
                DISCLOSE_DOCTRINES;
            m_treasuryLastTurn[m_playerCountryId] = 4210.0;
            // Divided, and governing the halves differently -- the profile
            // shows the division to anybody, and the LAW only because this
            // country publishes it. A shot of one undivided district with no
            // law would prove neither.
            {
                ensureDefaultDistrict(m_playerCountryId);
                auto& ds2 = m_districts[m_playerCountryId];
                if (ds2.size() == 1 && ds2[0].provinces.size() >= 4) {
                    auto all = ds2[0].provinces;
                    const size_t half = all.size() / 2;
                    ds2[0].provinces.assign(all.begin(), all.begin() + half);
                    ds2[0].sharePct = 65;
                    District d2;
                    d2.id = 2;
                    // Through uniqueDistrictName, like the real one: the two
                    // halves of one country often suggest the same name, and
                    // the first shot of this had two districts both called
                    // "Central Prefecture".
                    // GROUND FIRST. Both the suggested name and the fallback
                    // that picks a direction instead of a numeral are computed
                    // FROM the provinces, so naming a district before giving it
                    // any produced "British Empire II" where the real path
                    // produces "Eastern British Empire".
                    d2.provinces.assign(all.begin() + half, all.end());
                    d2.name = uniqueDistrictName(
                        m_playerCountryId,
                        suggestDistrictName(m_playerCountryId, d2.provinces),
                        -1, d2.provinces);
                    d2.sharePct = 35;
                    d2.r = 120; d2.g = 180; d2.b = 140;
                    ds2.push_back(std::move(d2));
                }
                if (!m_districtLaws.empty() && ds2.size() >= 2) {
                    ds2[0].policies.push_back(m_districtLaws.front().id);
                    if (m_districtLaws.size() > 2)
                        ds2[1].policies.push_back(m_districtLaws[2].id);
                }
            }
            // Twelve turns of history, or every graph on the screen says "not
            // enough turns yet" and the shot proves only that the boxes exist.
            {
                auto& h = m_incomeHistory[m_playerCountryId];
                if (h.size() < 8) {
                    auto base = computeCountryIncome(m_playerCountryId);
                    base.population = countryPopulation(m_playerCountryId);
                    h.clear();
                    for (int k = 0; k < 12; ++k) {
                        auto v = base;
                        const float t = 0.72f + 0.03f * k;
                        v.total      *= t;
                        v.expenses   *= 0.85f + 0.02f * k;
                        v.net         = v.total - v.expenses;
                        v.population  = (long long)(base.population * (0.90 + 0.01 * k));
                        h.push_back(v);
                    }
                }
            }
        } else if (name == "districts") {
            m_activeSidebarTab = 1;
            m_inPolitics = true;
            m_policyTab = 5;
            // Three districts, so the shares, the colours and the map all have
            // something to show. Carved off the front of what the country owns.
            ensureDefaultDistrict(m_playerCountryId);
            auto& ds = m_districts[m_playerCountryId];
            if (ds.size() == 1 && ds[0].provinces.size() >= 9) {
                auto all = ds[0].provinces;
                ds[0].provinces.assign(all.begin(), all.begin() + all.size() / 3);
                for (int k = 1; k <= 2; ++k) {
                    District d;
                    d.id = k + 1;
                    const Color c = ColorFromHSV((float)((d.id * 67) % 360), 0.55f, 0.85f);
                    d.r = c.r; d.g = c.g; d.b = c.b;
                    const size_t a = all.size() * k / 3, b = all.size() * (k + 1) / 3;
                    d.provinces.assign(all.begin() + a, all.begin() + b);
                    // Named the way the paint handler names one: after the
                    // ground, once it has some. Hard-coding "District 2" here
                    // photographed a state the game never produces.
                    d.name = uniqueDistrictName(
                        m_playerCountryId,
                        suggestDistrictName(m_playerCountryId, d.provinces), -1, d.provinces);
                    ds.push_back(std::move(d));
                }
                splitDistrictSharesEqually(m_playerCountryId);
                m_districtOverlayDirty = true;
            }
        } else if (name == "claims-uk") {
            applyLanguageForShot("uk");
            m_activeSidebarTab = 3;
            m_inClaims = true;
            m_claimsTab = 0;
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
        } else if (name == "editor-districts") {
            if (!m_mapEditor) {
                Audio::BlockingCall quiet;
                m_mapEditor = new MapEditor();
                m_mapEditor->init(m_screenW, m_screenH, m_dataDir);
            }
            {
                Audio::BlockingCall quiet;
                // 1914 rather than the modern map: fewer, larger countries, so
                // the two districts are legible at the tour's zoom.
                if (!m_mapEditor->shotSeedDistricts(m_dataDir + "STDmaps/1914.odmap"))
                    printf("  (editor-districts: map would not load, shot skipped)\n");
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
        } else if (name == "research-army") {
            m_activeSidebarTab = 4;
            m_inResearch = true;
            m_researchTab = 2;              // Army; see catKeys[] in Game_Research.cpp
            m_researchZoom = 0.75f;
            m_researchCamX = 0; m_researchCamY = 40;
        } else if (name == "army-kinds" || name == "army-mech") {
            m_activeViewTab = 5;
            // Grant the three formation technologies to the player so the
            // picker has something to offer.
            //
            // m_countryResearched, NOT Country::research -- there are two
            // stores and hasResearched() reads the first. Granting the second
            // photographed a panel with no picker on it, which is the shot
            // doing its job: an invisible feature and a broken one look
            // identical from the outside.
            for (const char* id : {"militia_levy", "assault_doctrine", "mechanisation"})
                m_countryResearched[m_playerCountryId].insert(id);
            int best = -1; long long bestPop = -1;
            for (const auto& [pid, pop] : m_provincePopulations) {
                const Province* p = m_provinces.getProvinceById(pid);
                if (!p || p->countryId != m_playerCountryId) continue;
                if (pop > bestPop) { bestPop = pop; best = pid; }
            }
            if (best > 0 && m_renderer) m_renderer->setSelectedProvince(best);
            m_recruitType = (name == "army-mech") ? TROOP_MECHANISED : TROOP_LINE;
        } else if (name == "orders-desktop" || name == "orders-portrait" ||
                   name == "orders-phase") {
            // The strip is greyed until a turn has resolved, and a loaded save
            // has no order log (it is per-turn display state, not saved). So
            // put a plausible turn in it: the option lit, the overlay drawn,
            // and the arrows over real provinces.
            m_activeSidebarTab = 0;
            m_activeViewTab = 5;
            m_turnOrderLog.clear();
            m_turnOrderLogTurn = m_turnNumber;
            {
                // ── ORDERS BETWEEN PROVINCES THAT TOUCH ──
                //
                // This used to pair mine[k] with mine[k+1] -- two entries of a
                // hash map, in whatever order it happened to iterate. Every
                // photograph it produced was therefore a spray of arrows
                // between random continents, Norway to Australia and Peru to
                // Siberia, which is not a thing the game can draw and not a
                // thing anyone could check. A fixture that fabricates nonsense
                // cannot catch a regression in what it photographs: the arrows
                // were unreadable, so nobody could see whether they were right.
                //
                // So each order now runs to a province that ACTUALLY ADJOINS
                // its source, and carries the detail a real order carries.
                std::vector<int> mine;
                for (const auto& [pid, units] : m_provinceArmies) {
                    const Province* p = m_provinces.getProvinceById(pid);
                    if (p && p->countryId > 0) mine.push_back(pid);
                    if (mine.size() > 400) break;
                }
                std::sort(mine.begin(), mine.end());   // and in a stable order
                int made = 0;
                const int pcts[] = {25, 50, 75, 100};
                for (size_t k = 0; k < mine.size() && made < 22; ++k) {
                    const Province* p = m_provinces.getProvinceById(mine[k]);
                    if (!p) continue;
                    int dst = -1;
                    for (size_t j = 0; j < mine.size() && dst < 0; ++j)
                        if (j != k && provincesAdjacent(mine[k], mine[j])) dst = mine[j];
                    if (dst < 0) continue;
                    TurnOrderMark m;
                    m.kind = (made % 3 == 0) ? TurnOrderMark::Kind::Artillery
                                             : TurnOrderMark::Kind::ArmyMove;
                    m.countryId = p->countryId;
                    m.fromProvince = mine[k];
                    m.toProvince = dst;
                    m.detail = (m.kind == TurnOrderMark::Kind::ArmyMove)
                                   ? std::to_string(pcts[made % 4]) + "%"
                                   : "he";
                    m_turnOrderLog.push_back(std::move(m));
                    ++made;
                }
                // A carrier working over a coast, so the shot covers the one
                // kind of attack that starts at a hull rather than a province.
                for (size_t k = 0; k < m_ships.size() && k < 3; ++k) {
                    int dst = -1;
                    for (int pid : mine) {
                        auto c = m_provinceCenters.find(pid);
                        if (c == m_provinceCenters.end()) continue;
                        dst = pid; break;
                    }
                    if (dst < 0) break;
                    TurnOrderMark m;
                    m.kind = TurnOrderMark::Kind::NavalBombard;
                    m.countryId = m_ships[k].countryId;
                    m.fromLon = m_ships[k].lon; m.fromLat = m_ships[k].lat;
                    m.fromProvince = -1;
                    m.toProvince = dst;
                    m.detail = "heavy";
                    m_turnOrderLog.push_back(std::move(m));
                }

                // And the two kinds that stand still, so the shot covers them.
                const size_t stride = std::max<size_t>(1, mine.size() / 7);
                for (size_t k = 0, done = 0; k < mine.size() && done < 6; k += stride, ++done) {
                    const Province* p = m_provinces.getProvinceById(mine[k]);
                    if (!p) continue;
                    TurnOrderMark m;
                    m.countryId = p->countryId;
                    m.fromProvince = m.toProvince = mine[k];
                    if (done % 2 == 0) {
                        m.kind = TurnOrderMark::Kind::Recruit;
                        m.detail = "12.0k Line Infantry";
                    } else {
                        m.kind = TurnOrderMark::Kind::Build;
                        m.detail = "industry";
                    }
                    m_turnOrderLog.push_back(std::move(m));
                }
            }
            if (name == "orders-portrait") SetWindowSize(402, 874);
            // ONLY THE ORDERS SHOTS. Setting this for every shot in the
            // block put the Viewing Orders banner across the economy screen.
            m_turnState = TURN_VIEWING_ORDERS;
            // ── AND ONE OF THEM IS ZOOMED IN ──
            //
            // The standing orders -- a levy raised, a works going up -- are
            // deliberately not drawn at world zoom, so the world-zoom shot
            // proves only that the clutter is gone. It cannot show the cues
            // are RIGHT, which is the half that can silently break. This one
            // flies to a province that has one and photographs it.
            if (name == "orders-zoom" && m_renderer && !m_turnOrderLog.empty()) {
                int pid = -1;
                for (const auto& m2 : m_turnOrderLog)
                    if (m2.kind == TurnOrderMark::Kind::Recruit) { pid = m2.fromProvince; break; }
                auto cit = m_provinceCenters.find(pid);
                if (cit != m_provinceCenters.end())
                    m_renderer->flyTo(cit->second.x, cit->second.y,
                                      m_renderer->getMinZoom() * 6.0f, 1000.0f);
            }
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
        } else if (name == "army") {
            // Tab 5 is the army view, and the shot wants a province with troops
            // in it -- the most populous one we own is the tour's selection and
            // is very likely garrisoned, but pick the biggest garrison outright
            // rather than hope.
            m_activeViewTab = 5;
            int best = -1; long long bestMen = -1;
            for (const auto& [pid, units] : m_provinceArmies) {
                const Province* p = m_provinces.getProvinceById(pid);
                if (!p || p->countryId != m_playerCountryId) continue;
                long long n = 0;
                for (const auto& u : units) n += u.count;
                if (n > bestMen) { bestMen = n; best = pid; }
            }
            if (best > 0 && m_renderer) m_renderer->setSelectedProvince(best);
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
            m_turnState = TURN_NORMAL;
        } else if (name == "economy-local") {
            // The half with the country's own books in it -- the breakdown, the
            // two pies and the three graphs. The tour only ever photographed
            // the global league tables, so every layout fault on this side
            // (a legend drawn through the pies, the second pie drawn through
            // the first one's legend) went unseen for as long as it existed.
            m_activeSidebarTab = 2;
            m_inEconomy = true;
            m_economyTab = 1;   // Local
            m_turnState = TURN_NORMAL;
            // Two turns of history, so the graphs have something to draw.
            auto& h = m_incomeHistory[m_playerCountryId];
            if (h.size() < 6) {
                auto cs = computeCountryIncome(m_playerCountryId);
                cs.nationalValue = countryNationalValue(m_playerCountryId);
                cs.population    = countryPopulation(m_playerCountryId);
                for (int k = (int)h.size(); k < 6; ++k) {
                    auto v = cs;
                    v.total          *= 0.80f + 0.04f * k;
                    v.expenses       *= 0.90f + 0.02f * k;
                    v.net             = v.total - v.expenses;
                    v.nationalValue  *= 0.70f + 0.06f * k;
                    h.push_back(v);
                }
            }
        } else if (name == "research" || name == "research-portrait") {
            m_activeSidebarTab = 4;
            m_inResearch = true;
            // The phone canvas, where the cards cannot sit beside the economy
            // slider and drop to a row of their own. Photographed because the
            // alternative -- the fit loop skipping all three -- puts the whole
            // research system out of reach with nothing on screen saying so.
            if (name == "research-portrait") SetWindowSize(402, 874);
            // THREE LIVE CARDS, so the shot covers the controls and not three
            // locked placeholders. Whether this country really earns three
            // groups depends on the save; the panel is what is being
            // photographed, so the cards are put in the state they are drawn in.
            for (int g = 0; g < RESEARCH_GROUPS_MAX; ++g) {
                auto& grp = m_researchGroups[g];
                grp.sharePct = (g == 2) ? 34 : 33;
                grp.autoAdvance = (g == 1);
                for (size_t i = 0; i < m_researchNodes.size(); ++i) {
                    const auto& n = m_researchNodes[i];
                    if (n.researched || n.inProgress) continue;
                    if (!isNodeAvailableFor(n, m_playerCountryId)) continue;
                    bool taken = false;
                    for (int k = 0; k < g; ++k) taken |= (m_researchGroups[k].activeNode == (int)i);
                    if (taken) continue;
                    grp.activeNode = (int)i;
                    m_researchNodes[i].inProgress = true;
                    m_researchNodes[i].invested = m_researchNodes[i].cost / (g + 3);
                    break;
                }
            }
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
        } else if (name == "feedback" || name == "feedback-diag") {
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_paused = false;
            // NOT opened here. This runs after endFrame(), so a form opened now
            // has never had a frame drawn behind it and would photograph on
            // black -- which is not what a player sees. Opened at frame 20
            // below, once the world has drawn and been captured.
            m_feedbackKind = feedback::Kind::Bug;
            m_feedbackCategory = feedback::Category::Scripting;
            m_feedbackTitle = "Districts panel draws over the minimap";
            m_feedbackBody = "At 1280x720 the districts list overlaps the minimap\n"
                             "in the bottom right. It is fine at 1600x900.";
            m_feedbackAttach = true;
            m_feedbackPreview = (name == "feedback-diag");
            m_feedbackPreviewScroll = 0;
        } else if (name == "mail-group") {
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_config.llmEnabled  = true;
            m_config.llmEndpoint = "http://127.0.0.1:11434/v1";
            m_config.llmModel    = "llama3.1:8b";
            m_config.mailPolicy  = (int)mail::Policy::Everyone;
            m_llmAvailable = true;
            m_mail.clear();
            m_mailGroups.clear();
            std::vector<int> picks;
            for (const auto& [cid2, c2] : m_countries.getAll()) {
                (void)c2;
                if (cid2 > 0 && cid2 != m_playerCountryId && cid2 < REBEL_CID_MIN) {
                    picks.push_back(cid2);
                    if (picks.size() >= 3) break;
                }
            }
            const int gid = createMailGroup(m_playerCountryId, "The Entente", picks);
            mailbox(m_playerCountryId).writeToGroup(m_playerCountryId, gid,
                "Shall we settle the Balkan question together?", m_turnNumber);
            mailbox(m_playerCountryId).deliver(m_turnNumber);
            // Delivered into the members' boxes the way a turn would. NOT
            // runAdvisors(): that opens network requests, and a screenshot tour
            // that waits on a language model is a tour that hangs on a machine
            // with no runner.
            deliverMail();
            openMail();
            m_mailGroupThread = gid;
            m_mailThread = 0;
        } else if (name == "mail-pick" || name == "mail-caret") {
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_config.llmEnabled  = true;
            m_config.llmEndpoint = "http://127.0.0.1:11434/v1";
            m_config.llmModel    = "llama3.1:8b";
            m_config.mailPolicy  = (int)mail::Policy::Everyone;
            m_llmAvailable = true;
            m_mail.clear();
            mailbox(m_playerCountryId);
            openMail();
            if (name == "mail-pick") {
                m_mailPicking = true;
                m_mailPickerQuery = "ge";      // a filter with something to show
            } else {
                // The caret must sit at the END of the typed line, not at the
                // start of the line below it.
                m_mailPicking = false;
                m_mailThread = 0;
                for (const auto& [cid, c] : m_countries.getAll()) {
                    (void)c;
                    if (cid > 0 && cid != m_playerCountryId && cid < REBEL_CID_MIN) {
                        m_mailThread = cid; break;
                    }
                }
                // A SHORT pending letter: the case where the footer and the
                // edit/discard controls used to be drawn on top of each other.
                mailbox(m_playerCountryId).write(m_playerCountryId, m_mailThread,
                                                 "hello???", m_turnNumber);
                m_mailDraft.clear();
                m_mailComposeFocus = true;
            }
        } else if (name == "mail-after-setup") {
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_config.llmEnabled  = true;
            m_config.llmEndpoint = "http://127.0.0.1:11434/v1";
            m_config.llmModel    = "llama3.1:8b";
            m_config.mailPolicy  = (int)mail::Policy::Everyone;
            m_llmAvailable = true;
            mailbox(m_playerCountryId);          // a box to land in
            openLlmSetup();                      // 1. the setup, from settings
            closeMail();                         // 2. Close
            openMail();                          // 3. press Mail
        } else if (name == "mail-button") {
            // Exactly the state a player is in after pulling a model: enabled,
            // a real endpoint, a real model name, and the checkbox untouched
            // since. The SIDEBAR is the subject, so every panel must be shut --
            // an earlier shot leaves Mail open, and this one then photographed
            // that instead, byte-identical to the shot before it.
            m_mailOpen = false;
            m_mailSettingsOpen = false;
            m_hostReportsOpen = false;
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_config.llmEnabled  = true;
            m_config.llmEndpoint = "http://127.0.0.1:11434/v1";
            m_config.llmModel    = "llama3.1:8b";
            m_config.mailPolicy  = (int)mail::Policy::Everyone;
            m_config.llmModel    = "llama3.1:8b";
        } else if (name == "llm-setup") {
            // DELIBERATELY LEAVES m_llmAvailable false and llmEnabled off.
            // Going through openLlmSetup() is the whole point: openMail()
            // refuses in this state, and an earlier version of this row called
            // it and silently rendered nothing.
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_mail.clear();
            // Ticked, but nothing installed and no endpoint: the state a player
            // is in the moment they go looking for the installer. The install
            // button lives behind this checkbox, so an unticked shot would
            // photograph the one screen that does NOT answer "where do I
            // install it".
            m_config.llmEnabled = true;
            // NOT cleared here any more. The endpoint and model come from the
            // real loaded config, so this shot also proves the legacy-default
            // migration and the install-implies-address fill, rather than
            // photographing a state the harness manufactured.
            // Players-only in a single-player game: the combination that
            // produces no Mail button at all and used to say nothing about it.
            m_config.mailPolicy = (int)mail::Policy::PlayersOnly;
            m_config.llmEndpoint = "http://127.0.0.1:11434/v1";
            m_config.llmModel    = "llama3.1:8b";
            openLlmSetup();
        } else if (name == "mail" || name == "mail-list" ||
                   name == "mail-settings" || name == "mail-report") {
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_llmAvailable = true;            // so the button and a bot exist
            m_config.mailPolicy = (int)mail::Policy::Everyone;
            m_mail.clear();
            {
                // A conversation worth photographing: something sent, an answer
                // from a machine correspondent, and one still on the desk.
                mail::Box& mine = mailbox(m_playerCountryId);
                const int other = (m_playerCountryId == 5) ? 1 : 5;
                mine.write(m_playerCountryId, other,
                           "Your fleet movements in the Channel are noted. Are we to "
                           "understand these as exercises?", m_turnNumber - 2);
                mine.deliver(m_turnNumber - 1);
                mail::Message reply;
                reply.id = 9001;
                reply.fromCountry = other;
                reply.toCountry = m_playerCountryId;
                reply.body = "Exercises, nothing more. We would sooner discuss the "
                             "tariff question, which troubles us far more than your "
                             "coastline does.";
                reply.writtenTurn = m_turnNumber - 1;
                reply.deliverTurn = m_turnNumber;
                reply.status = mail::Status::Delivered;
                reply.author = mail::Author::Bot;
                mine.receive(reply);
                mine.write(m_playerCountryId, other,
                           "Then let us talk tariffs. I will send terms next turn.",
                           m_turnNumber);
                m_mailThread = (name == "mail-list") ? 0 : other;
            }
            m_mailOpen = true;
            m_mailScroll = m_mailListScroll = 0;
            m_mailSettingsOpen = (name == "mail-settings");
            if (name == "mail-settings") m_config.llmEnabled = true;
            if (name == "mail-report") {
                // Photographed on the advisor's letter, which is the one a
                // player would actually be reporting.
                m_mailSettingsOpen = false;
                m_reportOpen = true;
                m_reportMessageId = 9001;
                m_reportCountry = m_mailThread;
                m_reportReason = 1;
                m_reportNote = "They kept at it after being asked to stop.";
                m_reportWithContext = true;
            }
        } else if (name == "dev-reports" || name == "dev-lookup") {
            // Rows stood up directly: the real screen fetches them from the
            // account service, which a screenshot run has no account for.
            m_currentScreen = SCREEN_MENU;
            m_devReports.clear();
            DevReport a;
            a.id = "r1"; a.accused = "Troublemaker"; a.reporter = "Kaiserin";
            a.reason = "harassment"; a.server = "Vlad's evening game";
            a.note = "They kept at it after being asked to stop.";
            a.message = "you are worthless and should quit";
            a.context = {"I would rather not discuss the border tonight.",
                         "You never want to discuss anything.",
                         "That is not fair. We spoke about it last turn.",
                         "And you lied about it last turn.",
                         "I did not lie. My fleet moved for the exercises I announced.",
                         "Nobody believes that.",
                         "Please stop.",
                         "Or what? You will write me another letter?"};
            a.status = "open";
            m_devReports.push_back(a);
            DevReport b;
            b.id = "r2"; b.accused = "Someone"; b.reporter = "Anna";
            b.reason = "spam"; b.message = "join my server join my server join my";
            b.status = "actioned"; b.outcome = "timeout 7d";
            m_devReports.push_back(b);
            m_devReportsOpen = true;
            m_devReportSelected = 0;
            if (name == "dev-lookup") {
                m_devTab = ReportTab::Lookup;
                m_devReportSelected = -1;
                m_devLookupText = "TestTroublemaker";
                DevProfile p;
                p.valid = true;
                p.id = "acct-test-troublemaker";
                p.nickname = "TestTroublemaker";
                p.linkedCount = 1;
                p.banned = true;
                p.bannedUntil = (long long)time(nullptr) + 36 * 3600;
                p.banReason = "Conduct towards other players.";
                p.against = { m_devReports[0] };
                DevReport filed = m_devReports[1];
                filed.reason = "spam";
                p.filed = { filed };
                m_devProfile = p;
            }
        } else if (name == "rating") {
            m_inResearch = m_inEconomy = m_inPolitics = false;
            m_feedbackOpen = false;
            m_ratingPromptOpen = true;
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
    if ((std::string(shot.name) == "feedback" || std::string(shot.name) == "feedback-diag") &&
        m_shotFrame == 20) {
        // Now: the world is on screen and endFrame has a picture of it.
        // Set directly rather than through openFeedbackForm(), which refuses
        // when the build has no reporting endpoint -- the shot is of the form,
        // not of the refusal.
        m_feedbackOpen = true;
        m_feedbackSwallowClick = true;
        m_feedbackDiag = feedbackDiagnostics();
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
