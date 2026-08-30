// ─────────────────────────────────────────────────────────────────────────────
// --tutorial-walk: play every route of the tutorial, page by page.
//
// The screenshot tour photographs the FIRST page of each script, which proves
// the file parses and the speaker resolves and nothing more. Everything that
// actually goes wrong in a tutorial goes wrong further in: a page that points
// at a name nothing offers, a page held open by a condition the player cannot
// reach from where the script has left them, a choice that names a script that
// is not there. None of that is visible in a picture of page one.
//
// So this walks them. For every route, for every page:
//
//   * the pointer must resolve -- the named element has to have been drawn,
//     this frame, while that page is up. tutorialFocus() answers exactly that
//     question, and is the same call the ring and the input gate use.
//   * the condition must be reachable -- the walk DRIVES the state the page is
//     waiting for (opens the panel, picks the province, files the request) and
//     then asks tutorialConditionMet, which is the same call the box uses. A
//     page whose condition never comes true inside its budget is a page that
//     can hang a player forever, and it is reported as one.
//   * a choice must name something real -- every script: and world: key has to
//     be a script that exists.
//
// Driving the state rather than performing the player's clicks is deliberate:
// this is a check that the SCRIPT is walkable, not a robot that plays the game.
// A page that waits on `open:economy` is satisfied by opening the economy, and
// what is being tested is that opening it is enough to move the lesson on.
//
// Exit code is the number of problems, so it fails a build. See tickTutorialWalk.
// ─────────────────────────────────────────────────────────────────────────────

#include "Game.h"
#include "GameInternals.h"
#include "ai/AISystem.h"
#include "Keybinds.h"
#include "UiScale.h"
#include "dialog/DialogScript.h"
#include "dialog/DialogBox.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// WHICH LANGUAGE THE WALK READS.
//
// It read en/ and nothing else, which meant every check below -- pointers that
// resolve, conditions that can be met, pages that cannot deadlock -- was only
// ever run against the English. A translation is the same machinery wrapped in
// different prose, and it is exactly as capable of losing a `::` line or a
// choice target. OD_WALK_LANG=uk walks the Ukrainian instead; unset, nothing
// changes.
static std::string walkLanguage() {
    if (const char* e = std::getenv("OD_WALK_LANG")) {
        if (*e) return e;
    }
    return "en";
}


namespace {

// Every route a player can be on, in the order they meet them. `inWorld` is
// where the script is played from: the two ends of the tutorial happen on the
// menu with no world under them, and everything between happens on the map.
struct WalkRoute {
    const char* script;
    bool inWorld;
};
const WalkRoute ROUTES[] = {
    {"intro",         false},
    {"tutorial",      true},
    {"specifics",     true},
    {"tut_ships",     true},
    {"tut_research",  true},
    {"tut_economy",   true},
    {"tut_unrest",    true},
    {"tut_diplomacy", true},
    {"outro",         false},
};
const int ROUTE_COUNT = (int)(sizeof(ROUTES) / sizeof(ROUTES[0]));

// markup_demo.oddlg and tutorial_pointer.oddlg are deliberately absent. They
// are not routes: they are the fixtures the screenshot tour photographs, and
// tutorial_pointer exists precisely to point at a name nothing offers, which
// is the thing this file reports as a fault. Walking them would fail on
// purpose, every time.

// How long a page gets. Generous: a page waits for its condition, and some of
// them wait for a turn to resolve. Long enough that a slow one is not called
// broken, short enough that a hung one does not hang the walk.
// A condition is driven a few frames in and answered immediately after, so
// this is the point at which one is not coming true rather than a wait.
constexpr int PAGE_BUDGET_FRAMES = 180;

// Frames to let a page settle before its condition is driven, so the pointer
// check sees the screen the player would have seen on arriving.
constexpr int SETTLE_FRAMES = 3;

}  // namespace

void Game::beginTutorialWalk() {
    m_walk = true;
    m_walkRoute = 0;
    m_walkOpened = false;
    m_currentScreen = SCREEN_MENU;
    m_inSettings = false;
    // As fast as the machine will go. The walk is hundreds of pages and each
    // one costs a handful of frames; at the display's rate that is minutes of
    // waiting for a swap chain nobody is looking at.
    ClearWindowState(FLAG_VSYNC_HINT);
    SetTargetFPS(0);
    // Printed because the pointer lives in two coordinate spaces and the
    // difference between them is invisible until a click misses: raylib
    // reports physical pixels, the interface is laid out in logical ones.
    printf("[WALK] %d routes (dpi scale %.2f, ui scale %.2f)\n",
           ROUTE_COUNT, m_dpiScale, odUi::scale());
}

void Game::walkProblem(const std::string& what) {
    const char* script = (m_walkRoute < ROUTE_COUNT) ? ROUTES[m_walkRoute].script : "?";
    char line[512];
    snprintf(line, sizeof(line), "%s page %d: %s", script, m_walkPage, what.c_str());
    m_walkProblems.push_back(line);
    printf("[WALK]   FAIL %s\n", line);
    fflush(stdout);
}

// ─── driving the state a page is waiting for ────────────────────────────────

void Game::walkTakeProvince(int pid) {
    Province* pp = m_provinces.getProvinceById(pid);
    if (!pp || pp->countryId == m_playerCountryId) return;
    const int from = pp->countryId;
    pp->countryId = m_playerCountryId;
    if ((size_t)pid < m_provinceCountryLookup.size())
        m_provinceCountryLookup[pid] = m_playerCountryId;
    reindexProvinceOwner(pid, from, m_playerCountryId);
    m_walkMapDirty = true;
}

// The map click a player would have made. The panel is drawn from the
// RENDERER's selection -- the Game-side field is a copy update() keeps in step
// -- so this sets the one that decides whether the panel exists at all.
void Game::walkSelect(int pid) {
    if (pid <= 0 || !m_renderer) return;
    m_renderer->setSelectedProvince(pid);
    m_lastSelectedProvince = pid;
}

/**
 * The country a diplomacy page is about.
 *
 * The scripts say "Verrick again" and mean one particular neighbour all the
 * way through; the condition only records that SOMETHING was filed. This is
 * the same choice a player following the words would make -- the first
 * foreign country there is -- and it is deliberately the same answer every
 * time, because asking twice about two different countries would prove
 * nothing about a panel that allows one request per country.
 */
std::string Game::walkDiploTarget() const {
    const Country* me = m_countries.getCountry(m_playerCountryId);
    if (!me) return "";

    // WHOEVER'S PANEL IS OPEN. The page before said "click a province
    // belonging to Verrick", and satisfying that left Verrick selected -- so
    // the country being talked about is simply the one on the screen.
    //
    // Taking the first foreign country instead picked Kestrel, who we are at
    // war with from the first turn, and a country you are at war with offers
    // one entry and it is not a pact. The walk was filing a request the panel
    // would never have shown.
    if (m_renderer) {
        const int pid = m_renderer->getSelectedProvinceId();
        auto it = m_provinces.getAllProvinces().find(pid);
        if (it != m_provinces.getAllProvinces().end()) {
            const int owner = it->second.countryId;
            if (owner > 0 && owner != m_playerCountryId && owner < REBEL_CID_MIN) {
                const Country* c = m_countries.getCountry(owner);
                if (c) return c->isoA3;
            }
        }
    }
    // Otherwise somebody we are at peace with, which is who a pact is for.
    std::string anyForeign;
    for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
        (void)pid;
        if (pr.countryId <= 0 || pr.countryId == m_playerCountryId) continue;
        if (pr.countryId >= REBEL_CID_MIN) continue;
        const Country* c = m_countries.getCountry(pr.countryId);
        if (!c) continue;
        if (anyForeign.empty()) anyForeign = c->isoA3;
        if (!walkAtWarWith(c->isoA3)) return c->isoA3;
    }
    return anyForeign;
}

bool Game::walkAtWarWith(const std::string& iso) const {
    const Country* me = m_countries.getCountry(m_playerCountryId);
    if (!me) return false;
    auto side = [&](const std::string& a, const std::string& b) {
        auto r = m_relations.find(a);
        if (r == m_relations.end()) return false;
        auto st = r->second.find(b);
        return st != r->second.end() && st->second.war;
    };
    return side(me->isoA3, iso) || side(iso, me->isoA3);
}

/// Any province flying that flag, or 0.
int Game::walkProvinceOf(const std::string& iso) const {
    for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
        const Country* c = m_countries.getCountry(pr.countryId);
        if (c && c->isoA3 == iso) return pid;
    }
    return 0;
}

void Game::walkSatisfy(const std::string& cond) {
    const size_t colon = cond.find(':');
    const std::string what = cond.substr(0, colon);
    const std::string arg  = (colon == std::string::npos) ? "" : cond.substr(colon + 1);
    const Country* me = m_countries.getCountry(m_playerCountryId);

    if (what == "open" || what == "closed") {
        const bool want = (what == "open");
        // The sidebar id goes with the panel: a panel open with the sidebar
        // pointing somewhere else is a state the game cannot be in, and the
        // next page's pointer would be asking about a tab that is not lit.
        if (arg == "economy")  { m_inEconomy  = want; if (want) m_activeSidebarTab = 2; }
        if (arg == "research") { m_inResearch = want; if (want) m_activeSidebarTab = 4; }
        if (arg == "politics") { m_inPolitics = want; if (want) m_activeSidebarTab = 1; }
        if (arg == "claims")   { m_inClaims   = want; if (want) m_activeSidebarTab = 3; }
        if (arg == "any" && !want) m_inEconomy = m_inResearch = m_inPolitics = m_inClaims = false;
        if (!want) m_activeSidebarTab = 0;
        return;
    }
    if (what == "view") {
        static const char* kViews[] = {"population", "industry", "defence", "relations",
                                       "army", "navy", "resources", "names"};
        for (int i = 0; i < 8; ++i)
            if (arg == kViews[i]) { m_activeViewTab = i + 1; return; }
        return;
    }
    if (what == "war") {
        if (!me) return;
        // Selected first, then declared. A player declares war from a
        // province panel, so the panel is up by the time the war exists --
        // and the next page usually points at it. Setting the relation alone
        // would leave the walk in a state play never passes through, and
        // would report the page that points at that panel as broken.
        walkSelect(walkProvinceOf(arg));
        m_relations[me->isoA3][arg].war = true;
        m_relations[arg][me->isoA3].war = true;
        return;
    }
    if (what == "filed") {
        if (!me) return;
        const std::string target = walkDiploTarget();
        if (target.empty()) return;
        walkSelect(walkProvinceOf(target));      // filed from their panel
        PendingDiplomaticAction da;
        da.sourceIso = me->isoA3;
        da.targetIso = target;
        da.action = arg;
        m_pendingDiplomaticActions.push_back(da);
        return;
    }
    if (what == "selected") {
        if (!m_renderer) return;
        int pick = 0;
        for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
            const int owner = pr.countryId;
            bool ok = false;
            if (arg == "any")     ok = owner > 0;
            if (arg == "own")     ok = (owner == m_playerCountryId);
            if (arg == "foreign") ok = (owner > 0 && owner != m_playerCountryId);
            if (arg == "atpeace") {
                ok = false;
                if (owner > 0 && owner != m_playerCountryId && me) {
                    const Country* them = m_countries.getCountry(owner);
                    if (them) {
                        auto side = [&](const std::string& a, const std::string& b) {
                            auto r = m_relations.find(a);
                            if (r == m_relations.end()) return false;
                            auto st = r->second.find(b);
                            return st != r->second.end() && st->second.war;
                        };
                        ok = !side(me->isoA3, them->isoA3) && !side(them->isoA3, me->isoA3);
                    }
                }
            }
            if (ok) { pick = pid; break; }
        }
        walkSelect(pick);
        return;
    }
    if (what == "recruiting") {
        int own = 0;
        for (const auto& [pid, pr] : m_provinces.getAllProvinces())
            if (pr.countryId == m_playerCountryId) { own = pid; break; }
        if (own <= 0) return;
        walkSelect(own);                          // recruited from its panel
        PendingRecruitment r;
        r.provinceId = own;
        r.count = 1;
        m_pendingRecruitments.push_back(r);
        return;
    }
    if (what == "province") {
        walkTakeProvince(atoi(arg.c_str()));
        return;
    }
    if (what == "destroyed") {
        std::vector<int> theirs;
        for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
            const Country* c = m_countries.getCountry(pr.countryId);
            if (c && c->isoA3 == arg) theirs.push_back(pid);
        }
        for (int pid : theirs) walkTakeProvince(pid);
        return;
    }
    if (what == "rebels") {
        if (arg != "none") return;
        std::vector<int> rebels;
        for (const auto& [pid, pr] : m_provinces.getAllProvinces())
            if (pr.countryId >= REBEL_CID_MIN && pr.countryId < SPC_CID) rebels.push_back(pid);
        for (int pid : rebels) walkTakeProvince(pid);
        return;
    }
    if (what == "turns") {
        // A REAL TURN, not a bigger number.
        //
        // This used to just move m_turnNumber, which satisfies the condition
        // and nothing else -- and half of what a lesson waits for on the far
        // side of a turn is a turn's WORK: a diplomatic request answered, the
        // recruits arriving, the boat leaving the coast. Faking the counter
        // walked straight past every page that depends on any of that, which
        // is the deadlock this whole mode exists to find.
        const int n = std::max(1, atoi(arg.c_str()));
        for (int i = 0; i < n; ++i) processTurn();
        return;
    }
    // Anything else is a condition the runtime does not know either -- it says
    // so on stderr and treats it as met, so the walk reports it here rather
    // than passing the page silently.
    walkProblem("condition \"" + cond + "\" is not one the game knows");
}

// ─── the walk itself ────────────────────────────────────────────────────────

// ─── every branch, and the way back ─────────────────────────────────────────

/**
 * EVERY OPTION ON EVERY CHOICE PAGE, TAKEN.
 *
 * The route walk above takes ONE answer per menu -- whichever keeps it inside
 * the script it is walking -- so five of the topic menu's six options were only
 * ever checked for "the file it names exists". That is not the same as the
 * option working, as the topic menu itself proved: for a while every one of
 * them silently did nothing.
 *
 * And a topic is supposed to COME BACK. `script:` records a return address and
 * endDialogue reopens it, which is the loop the menu's own words promise --
 * "ask again when you are done". Walking each topic from a fresh world never
 * went near it.
 *
 * So this takes every option in turn, from a world set up the way that option
 * would be met, and follows it: the target has to open, and a topic run to its
 * end has to put the player back in the menu they came from.
 */
bool Game::tickBranchDrill() {
    // The cases, found once: every choice page in every route, one entry per
    // option. Discovered rather than listed, so an option added to a script is
    // covered by having been written.
    if (m_walkBranches.empty()) {
        for (int r = 0; r < ROUTE_COUNT; ++r) {
            dlg::Box probe;
            if (!probe.open(m_dataDir + "dialog", ROUTES[r].script, walkLanguage())) continue;
            for (int i = 0; i < probe.pageCount(); ++i) {
                probe.jumpTo(i);
                const dlg::Page* pg = probe.currentPage();
                if (!pg || pg->choices.empty()) continue;
                for (int c = 0; c < (int)pg->choices.size(); ++c)
                    m_walkBranches.push_back({r, i, c});
            }
        }
        printf("\n[WALK] === every option on every menu (%d)\n", (int)m_walkBranches.size());
        fflush(stdout);
    }
    if (m_walkBranch >= (int)m_walkBranches.size()) return false;

    const BranchCase& bc = m_walkBranches[m_walkBranch];
    const WalkRoute& route = ROUTES[bc.route];

    auto nextCase = [&]() { ++m_walkBranch; m_walkBranchPhase = 0; };

    switch (m_walkBranchPhase) {
    case 0: {
        if (m_dialogOpen) endDialogue();
        m_dialogReturnTo.clear();
        m_introRunning = false;
        if (route.inWorld) {
            m_tutorialMode = true;
            startNewGameWithName(m_dataDir + "STDmaps/tutorial.odmap", "Walk");
            m_loadingShouldCreateSave = false;
            m_quickStartPending = true;
            m_forcedStartIso = "ASH";
            while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
                if (WindowShouldClose()) return false;
                updateLoading();
            }
            hideLoadingScreen();
            m_currentScreen = SCREEN_PLAYING;
            m_tutorialMode = true;
            m_tutorialTurnUnlocked = true;
        } else {
            m_currentScreen = SCREEN_MENU;
        }
        // The intro's fork is only a fork while the intro is running: that is
        // the flag endDialogue reads to decide where to go next, and the flag
        // the answer is remembered under.
        m_introRunning = (std::string(route.script) == "intro");
        m_tutorialTrack.clear();
        beginDialogue(route.script);
        if (!m_dialogOpen) { walkProblem("script would not open for the branch drill"); nextCase(); return true; }
        m_dialog.jumpTo(bc.page);
        m_dialogPage = m_dialog.pageIndex();
        ++m_walkBranchPhase;
        m_walkFrames = 0;
        return true;
    }
    case 1: {
        // Let the page finish asking before answering it. The walk runs with
        // no frame cap, so a page typing itself out at its own speed would
        // take tens of thousands of frames -- a click is what a player uses to
        // skip the typewriter, and one is exactly what this is.
        if (!m_dialog.pageComplete()) m_dialogAdvance = true;
        if (!m_dialog.awaitingChoice()) {
            if (++m_walkFrames > 2000) {
                walkProblem(std::string(route.script) + " page " + std::to_string(bc.page) +
                            " never finished asking its question");
                nextCase();
            }
            return true;
        }
        const dlg::Page* pg = m_dialog.currentPage();
        if (!pg || bc.option >= (int)pg->choices.size()) { nextCase(); return true; }
        m_walkBranchKey = pg->choices[bc.option].key;
        m_walkBranchLabel = pg->choices[bc.option].label;
        m_walkBranchFrom = m_dialogScript;
        for (int guard = 0; guard < (int)pg->choices.size() + 1; ++guard) {
            const int at = m_dialog.selectionIndex();
            if (at < 0 || at == bc.option) break;
            m_dialog.moveSelection(at < bc.option ? 1 : -1);
        }
        m_dialog.commitChoice();
        ++m_walkBranchPhase;
        m_walkFrames = 0;
        return true;
    }
    case 2: {
        // What the answer did.
        const std::string key = m_walkBranchKey;
        m_walkBranchNote = "[WALK]   " + m_walkBranchFrom + ": \"" + m_walkBranchLabel + "\"";
        if (key.rfind("script:", 0) == 0 && key != "script:done") {
            const std::string target = key.substr(7);
            if (!m_dialogOpen || m_dialogScript != target) {
                walkProblem("\"" + m_walkBranchLabel + "\" should open \"" + target +
                            "\" and opened \"" + (m_dialogOpen ? m_dialogScript : "nothing") + "\"");
                nextCase();
                return true;
            }
            m_walkBranchNote += " -> " + target;
            // Now run it to the end and see what it does there. The page
            // index is deliberately NOT written back: updateDialogue runs a
            // page's act when the index CHANGES, and suppressing that was how
            // an earlier version of this drill got the sign-off wrong -- it
            // never ran act=end_tutorial, so the outro handed back to the
            // topic menu and the drill called that correct.
            m_dialog.jumpTo(m_dialog.pageCount() - 1);
            const dlg::Page* last = m_dialog.currentPage();
            m_walkBranchEnds = (last && last->act == "end_tutorial");
            ++m_walkBranchPhase;
            m_walkFrames = 0;
            return true;
        }
        if (key.rfind("world:", 0) == 0) {
            const std::string target = key.substr(6);
            while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
                if (WindowShouldClose()) return false;
                updateLoading();
            }
            hideLoadingScreen();
            if (!m_renderer) {
                walkProblem("\"" + m_walkBranchLabel + "\" did not bring the world back");
                nextCase();
                return true;
            }
            m_currentScreen = SCREEN_PLAYING;
            m_walkBranchExpect = target;
            m_walkBranchPhase = 4;      // wait for the world's script to open
            m_walkFrames = 0;
            m_walkBranchNote += " -> the world, then " + target;
            return true;
        }
        // An answer that stays in the script: it must have turned the page,
        // and the intro's fork must have been remembered.
        if (!m_dialogOpen || m_dialog.pageIndex() <= bc.page) {
            walkProblem("\"" + m_walkBranchLabel + "\" did not move the conversation on");
        } else if (std::string(route.script) == "intro" && m_tutorialTrack != key) {
            walkProblem("the fork \"" + m_walkBranchLabel + "\" was answered \"" + key +
                        "\" and remembered as \"" + m_tutorialTrack + "\"");
        } else {
            m_walkBranchNote += " -> page " + std::to_string(m_dialog.pageIndex());
            if (!m_tutorialTrack.empty()) m_walkBranchNote += " (track: " + m_tutorialTrack + ")";
            printf("%s\n", m_walkBranchNote.c_str());
            fflush(stdout);
        }
        m_introRunning = false;         // do not let endDialogue start a world
        nextCase();
        return true;
    }
    case 3: {
        // The topic is on its last page: close it, and the menu should come
        // back on its own.
        if (m_dialogOpen && m_dialogScript != m_walkBranchFrom) {
            m_dialogAdvance = true;
            if (++m_walkFrames > 900) {
                walkProblem("\"" + m_walkBranchLabel + "\" opened a topic that would not end");
                nextCase();
            }
            return true;
        }
        // A topic comes back to the menu it was opened from; the sign-off
        // does not, because it ends the tutorial. Which of the two is right is
        // read off the script's own last page rather than assumed.
        if (m_walkBranchEnds) {
            if (m_dialogOpen)
                walkProblem("the sign-off handed back to \"" + m_dialogScript +
                            "\" instead of ending the tutorial");
            else if (m_tutorialMode)
                walkProblem("the sign-off ended without leaving tutorial mode");
            else
                printf("%s -> and the tutorial ends\n", m_walkBranchNote.c_str());
        } else if (!m_dialogOpen) {
            walkProblem("\"" + m_walkBranchLabel + "\" ended without returning to " +
                        m_walkBranchFrom);
        } else if (m_dialogScript != m_walkBranchFrom) {
            walkProblem("\"" + m_walkBranchLabel + "\" ended in \"" + m_dialogScript +
                        "\" rather than back in " + m_walkBranchFrom);
        } else {
            printf("%s -> back to %s\n", m_walkBranchNote.c_str(), m_dialogScript.c_str());
        }
        fflush(stdout);
        nextCase();
        return true;
    }
    case 4: {
        // The world is up; the script it asked for should follow.
        if (m_dialogOpen && m_dialogScript == m_walkBranchExpect) {
            printf("%s -> opened\n", m_walkBranchNote.c_str());
            fflush(stdout);
            nextCase();
            return true;
        }
        if (++m_walkFrames > 600) {
            walkProblem("\"" + m_walkBranchLabel + "\" reloaded the world but \"" +
                        m_walkBranchExpect + "\" never opened");
            nextCase();
        }
        return true;
    }
    default: break;
    }
    nextCase();
    return true;
}

/**
 * THE WAY OUT, PROVED.
 *
 * Everything else here checks that a page can be got past. This checks the
 * other thing a player needs, which is that a page they CANNOT get past is
 * survivable: Escape reaches the pause menu from a gated page, the settings
 * screen offers "Stop the tutorial", and pressing it actually ends the thing.
 *
 * It matters most exactly when the rest of this file has missed something.
 * Both halves have been broken before -- Escape used to close the dialogue
 * and leave the player standing in a practice world with no lesson and no way
 * on -- and neither is visible in a screenshot.
 *
 * Returns false when the drill is finished.
 */
bool Game::tickEscapeDrill() {
    switch (m_walkDrill) {
    case 0: {
        printf("\n[WALK] === the way out (Escape, then Stop the tutorial)\n");
        if (m_dialogOpen) endDialogue();
        m_tutorialMode = true;
        startNewGameWithName(m_dataDir + "STDmaps/tutorial.odmap", "Walk");
        m_loadingShouldCreateSave = false;
        m_quickStartPending = true;
        m_forcedStartIso = "ASH";
        while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
            if (WindowShouldClose()) return false;
            updateLoading();
        }
        hideLoadingScreen();
        m_currentScreen = SCREEN_PLAYING;
        m_tutorialMode = true;
        beginDialogue("tutorial");
        // The first page that shuts the screen. Found rather than counted:
        // the lesson is edited often and a page number would rot.
        int gated = -1;
        for (int i = 0; i < m_dialog.pageCount() && gated < 0; ++i) {
            m_dialog.jumpTo(i);
            const dlg::Page* pg = m_dialog.currentPage();
            if (pg && pg->gate && !pg->pointAt.empty()) gated = i;
        }
        if (gated < 0) { walkProblem("the lesson gates nothing, so this drill proves nothing"); break; }
        m_dialog.jumpTo(gated);
        m_dialogPage = m_dialog.pageIndex();
        printf("[WALK]   trapped on page %d\n", gated);
        ++m_walkDrill;
        return true;
    }
    case 1: {
        // The gate really is shut: a click in the middle of the map does
        // nothing. If it were open the rest of this would prove nothing.
        if (!tutorialBlocksInput({m_screenW * 0.5f, m_screenH * 0.32f}))
            printf("[WALK]   (the gate is open on this page; the drill still runs)\n");
        // What Escape does, from Game_Update.cpp.
        m_paused = true;
        m_inSettings = true;
        ++m_walkDrill;
        return true;      // a frame, so the settings screen draws itself
    }
    case 2: {
        if (m_tutorialStopRect.width <= 0.0f) {
            walkProblem("the pause menu offers no way to stop the tutorial");
            m_walkDrill = 4;
            return true;
        }
        printf("[WALK]   \"Stop the tutorial\" is on screen\n");
        // And what clicking it does.
        stopTutorial();
        ++m_walkDrill;
        return true;
    }
    case 3: {
        if (m_tutorialMode)  walkProblem("stopping the tutorial left tutorial mode on");
        if (m_dialogOpen)    walkProblem("stopping the tutorial left the lesson open");
        if (m_currentScreen == SCREEN_PLAYING)
            walkProblem("stopping the tutorial left the player in the practice world");
        if (!m_tutorialMode && !m_dialogOpen && m_currentScreen != SCREEN_PLAYING)
            printf("[WALK]   out: tutorial off, lesson closed, back on the menu\n");
        ++m_walkDrill;
        return true;
    }
    default: break;
    }
    return false;
}

bool Game::tickTutorialWalk() {
    if (m_walkRoute >= ROUTE_COUNT) {
        if (tickBranchDrill()) return true;
        if (tickEscapeDrill()) return true;
        printf("\n[WALK] %d pages walked, %d problem(s)\n", m_walkPages, (int)m_walkProblems.size());
        for (const std::string& p : m_walkProblems) printf("[WALK]   %s\n", p.c_str());
        fflush(stdout);
        return false;
    }
    const WalkRoute& route = ROUTES[m_walkRoute];

    // OD_WALK_ONLY=a,b walks just those routes. Every route reloads the world,
    // so the whole set is minutes; working on one lesson should not be.
    if (const char* only = std::getenv("OD_WALK_ONLY")) {
        const std::string list = std::string(",") + only + ",";
        if (list.find(std::string(",") + route.script + ",") == std::string::npos) {
            ++m_walkRoute;
            m_walkOpened = false;
            return true;
        }
    }

    // ── open the route ──
    if (!m_walkOpened) {
        if (m_dialogOpen) endDialogue();
        if (route.inWorld) {
            // A fresh world per route. The beginner lesson takes ground, files
            // wars and stages a rebellion; a topic walked after it would start
            // from a map the script was never written against.
            m_tutorialMode = true;
            startNewGameWithName(m_dataDir + "STDmaps/tutorial.odmap", "Walk");
            m_loadingShouldCreateSave = false;
            m_quickStartPending = true;
            m_forcedStartIso = "ASH";
            while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
                if (WindowShouldClose()) return false;
                updateLoading();
            }
            hideLoadingScreen();
            if (!m_renderer) {
                walkProblem("the tutorial world would not load");
                ++m_walkRoute;
                return true;
            }
            m_currentScreen = SCREEN_PLAYING;
            AISystem::s_tutorialAI = true;
            m_tutorialMode = true;
            m_activeViewTab = 0;
            m_activeSidebarTab = 0;
            m_inResearch = m_inEconomy = m_inPolitics = m_inClaims = false;
            // THE TURN LOCK IS LEFT EXACTLY AS PLAY LEAVES IT.
            //
            // It was unlocked for every route, which is comfortable and wrong:
            // the beginner lesson unlocks it itself, half way through, with
            // act=unlock_turn -- so unlocking it here hid the entire class of
            // deadlock where an earlier page waits for something only a
            // resolved turn can bring about. A player on the specifics track
            // never meets that page, and updateDialogue unlocks it for them
            // on the way in; this is the same rule.
            m_tutorialTurnUnlocked = (std::string(route.script) != "tutorial");
        } else {
            m_currentScreen = SCREEN_MENU;
            m_paused = false;
            m_inSettings = false;
        }
        printf("\n[WALK] === %s (%s)\n", route.script, route.inWorld ? "in the world" : "on the menu");
        fflush(stdout);
        beginDialogue(route.script);
        if (!m_dialogOpen) {
            walkProblem("script would not open");
            ++m_walkRoute;
            return true;
        }
        m_walkOpened = true;
        m_walkPage = -1;
        m_walkJumped = false;
        return true;      // one frame, so the page gets drawn and offers its targets
    }

    // ── the route ended ──
    if (!m_dialogOpen || m_walkJumped || m_dialogScript != route.script) {
        walkPointerVerdict();
        // The answer was supposed to open something. Did it? Asked before the
        // page is forgotten, so the report names the page it was answered on.
        if (!m_walkExpect.empty()) {
            if (!m_dialogOpen)
                walkProblem("the answer that opens \"" + m_walkExpect +
                            "\" ended the conversation instead");
            else if (m_dialogScript != m_walkExpect)
                walkProblem("the answer that opens \"" + m_walkExpect +
                            "\" opened \"" + m_dialogScript + "\"");
            else
                printf("[WALK]   -> opened %s\n", m_walkExpect.c_str());
            m_walkExpect.clear();
        }
        m_walkPage = -1;
        printf("[WALK]   %s: done\n", route.script);
        if (m_dialogOpen) endDialogue();
        ++m_walkRoute;
        m_walkOpened = false;
        return true;
    }

    const dlg::Page* page = m_dialog.currentPage();
    if (!page) { ++m_walkRoute; m_walkOpened = false; return true; }

    // ── a new page ──
    if (m_dialog.pageIndex() != m_walkPage) {
        walkPointerVerdict();
        m_walkPage = m_dialog.pageIndex();
        m_walkPointerName = page->pointAt;
        // A gated page that is also waiting turns a missing pointer from a
        // cosmetic fault into a locked door: see walkCheckPage.
        m_walkPointerTrap = page->gate && !page->until.empty();
        m_walkFrames = 0;
        m_walkSatisfied = false;
        m_walkPointerSeen = page->pointAt.empty();
        ++m_walkPages;
        {
            std::string note;
            if (!page->pointAt.empty()) note += " ->" + page->pointAt;
            if (!page->until.empty())   note += " until=" + page->until;
            if (!page->act.empty())     note += " act=" + page->act;
            if (!page->choices.empty())
                note += " (" + std::to_string(page->choices.size()) + " choices)";
            printf("[WALK]   page %d @%s%s\n", m_walkPage,
                   page->speaker.empty() ? "-" : page->speaker.c_str(), note.c_str());
            fflush(stdout);
        }
        walkCheckPage(*page);
        if (m_walkMapDirty) {
            // Ownership was moved by hand to satisfy a condition; the picture
            // of the map is built from that data and does not know yet.
            rebuildOwnershipPixels();
            rebuildFlags();
            if (m_renderer) m_renderer->setCountryFlags(&m_countryFlags);
            m_walkMapDirty = false;
        }
    }
    ++m_walkFrames;

    // The pointer, asked the way the ring asks it. Checked over the whole life
    // of the page rather than on one frame: a panel that opens on the page's
    // own condition is drawn a frame or two after the page arrives.
    if (!m_walkPointerSeen) {
        Rectangle r{};
        bool round = false;
        if (tutorialFocus(r, round) && r.width > 0.0f && r.height > 0.0f)
            m_walkPointerSeen = true;
    }

    // The condition. Driven once, a few frames in, then waited on.
    // Asked on the settling frame, while the screen is still exactly what the
    // player would be looking at -- before the walk itself touches anything.
    if (m_walkFrames == SETTLE_FRAMES) walkCheckReachable(*page);

    if (!page->until.empty()) {
        if (!m_walkSatisfied && m_walkFrames > SETTLE_FRAMES) {
            walkSatisfy(page->until);
            m_walkSatisfied = true;
        }
        if (!tutorialConditionMet(page->until)) {
            if (m_walkFrames < PAGE_BUDGET_FRAMES) return true;
            walkProblem("waits on \"" + page->until + "\", which never came true");
            // Past it by hand, so the rest of the route is still walked.
            m_dialog.jumpTo(m_walkPage + 1);
            return true;
        }
    }

    // A choice. Every key that names a script has to name one that exists.
    if (m_dialog.awaitingChoice()) {
        const std::vector<dlg::Choice>& cs = page->choices;
        int take = -1;
        for (int i = 0; i < (int)cs.size(); ++i) {
            const std::string& k = cs[i].key;
            const bool jumps = (k.rfind("script:", 0) == 0 && k != "script:done") ||
                               k.rfind("world:", 0) == 0;
            if (jumps) {
                const std::string target = k.substr(k.find(':') + 1);
                if (!walkScriptExists(target))
                    walkProblem("choice \"" + cs[i].label + "\" opens \"" + target +
                                "\", which is not a script");
            }
            // Prefer an answer that stays in this script, so the rest of it is
            // walked here rather than left to whichever route jumps back.
            if (take < 0 && !jumps) take = i;
        }
        if (take < 0) {
            // Every option leaves this script, so one of them is taken FOR
            // REAL and the jump is checked next tick. Following it matters:
            // the topic menu is a single page, so taking an option closes the
            // box, and for a while that meant the answer was thrown away
            // before anything read it -- clicking a topic did nothing, and a
            // walk that only checked the target file EXISTS said the menu was
            // fine. See consumePickedChoice.
            take = 0;
            m_walkJumped = true;
            m_walkExpect = cs[take].key.substr(cs[take].key.find(':') + 1);
            if (cs[take].key.rfind("world:", 0) == 0)
                m_walkExpect.clear();   // that one reloads the world; the
                                        // outro-live screenshot covers it
        }
        printf("[WALK]   page %d: %d choice(s), taking \"%s\"\n",
               m_walkPage, (int)cs.size(), cs[take].label.c_str());
        for (int guard = 0; guard < (int)cs.size() + 1; ++guard) {
            const int at = m_dialog.selectionIndex();
            if (at < 0 || at == take) break;
            m_dialog.moveSelection(at < take ? 1 : -1);
        }
        m_dialog.commitChoice();
        return true;
    }

    // Nothing left to wait for: turn the page. One synthetic click per frame --
    // the first completes the typewriter, the second turns it, exactly as two
    // real clicks would.
    if (m_walkFrames > PAGE_BUDGET_FRAMES) {
        walkProblem("would not turn");
        m_dialog.jumpTo(m_walkPage + 1);
        return true;
    }
    m_dialogAdvance = true;

    return true;
}

// The verdict on the page just left: it named something, and over its whole
// life on the screen the game never drew it. In play that is a ring around
// nothing and, on a gated page, a lesson the player cannot click their way out
// of -- so it is a failure, not a warning.
void Game::walkPointerVerdict() {
    if (m_walkPage < 0 || m_walkPointerSeen || m_walkPointerName.empty()) return;
    std::string what = "points at \"" + m_walkPointerName + "\", which nothing drew";
    if (m_walkPointerTrap)
        what += " -- and it gates the screen while it waits, so with nothing to "
                "allow back the player can click nothing at all";
    walkProblem(what);
}

// Everything about a page that can be judged from the page itself: who says it,
// what it names, and whether the gate it puts up can be opened.
void Game::walkCheckPage(const dlg::Page& page) {
    // The speaker has to be in the cast, or the plate is blank and the window
    // shows whoever was on the link last.
    if (!page.speaker.empty()) {
        loadCommsCast();
        if (m_castLabel.find(page.speaker) == m_castLabel.end())
            walkProblem("speaker \"" + page.speaker + "\" is not in the cast");
    }

    // {key=x} names an action to look up. The resolver prints "?" for one it
    // does not know, which in play is a sentence telling the player to press
    // the question mark key.
    for (const dlg::Span& sp : page.spans) {
        if (sp.keyAction.empty()) continue;
        bool known = false;
        for (int i = 0; i < ACTION_COUNT; ++i)
            if (sp.keyAction == ACTION_IDS[i]) { known = true; break; }
        if (!known)
            walkProblem("{key=" + sp.keyAction + "} is not an action");
    }

    // An act nothing implements is logged at runtime and otherwise does
    // nothing at all, so the page it was written on quietly does not happen.
    if (!page.act.empty()) {
        static const char* kActs[] = {"rebellion", "to_menu", "end_tutorial",
                                      "unlock_turn", "tune_out"};
        bool known = page.act.rfind("then:", 0) == 0;
        for (const char* a : kActs) if (page.act == a) known = true;
        if (!known) walkProblem("act \"" + page.act + "\" is not one the game runs");
    }

    // A GATE WITH NOTHING TO POINT AT IS A LOCKED DOOR.
    //
    // tutorialBlocksInput gates everything but the textbox, and then allows
    // back the one rectangle the page points at -- so a gated page that names
    // nothing allows nothing. Harmless if the page turns on a click, and a
    // softlock if it is also waiting for something the player now cannot do.
    if (page.gate && page.pointAt.empty() && !page.until.empty())
        walkProblem("gates the screen, points at nothing, and waits on \"" +
                    page.until + "\" -- nothing the player can click will end it");
}

// ─── can the player actually get out of this page? ──────────────────────────

namespace {
// The control a condition is asking the player to use. "map" is the map
// itself; empty means no single control -- a turn's worth of orders, or
// something the script does on its own.
std::string controlFor(const std::string& cond) {
    const size_t colon = cond.find(':');
    const std::string what = cond.substr(0, colon);
    const std::string arg  = (colon == std::string::npos) ? "" : cond.substr(colon + 1);
    if (what == "open")      return "tab." + arg;
    if (what == "closed")    return "panel.close";
    if (what == "view")      return "view." + arg;
    if (what == "selected")  return "map";
    if (what == "filed" || what == "recruiting" || what == "war") return "panel.province";
    if (what == "turns")     return "button.end_turn";
    return "";
}

// True when nothing but a resolved turn can make it come true.
bool needsATurn(const std::string& cond) {
    const std::string what = cond.substr(0, cond.find(':'));
    return what == "turns" || what == "destroyed" || what == "province" ||
           what == "rebels" || what == "war";
}
}  // namespace

/**
 * THE DEADLOCK CHECK.
 *
 * A page that waits is a page the player has to get out of, and there are only
 * two ways it can be impossible. The gate can block the very control the
 * condition is asking them to use -- it allows back exactly one rectangle, and
 * if that is not the one with the button in it, the player can see what to do
 * and cannot do it. Or the condition can need a turn to resolve while the turn
 * button is still locked, which is the same trap the turn lock was rewritten
 * once already to avoid.
 *
 * Both are asked HERE, of the live screen, with the same calls the game itself
 * uses to decide what a click may touch -- not of a model of them.
 */
void Game::walkCheckReachable(const dlg::Page& page) {
    if (page.until.empty()) return;

    if (needsATurn(page.until) && !tutorialAllowsEndTurn())
        walkProblem("waits on \"" + page.until + "\", which nothing but a resolved turn "
                    "can bring about, while Process Turn is still locked");

    // A REQUEST CAN BE GREYED OUT BY AN EARLIER ONE.
    //
    // The panel allows one pending request per country: while one is waiting
    // to be answered, every other entry for that country is dead. A page that
    // asks for a second request without a turn in between is asking for
    // something the interface will not let the player do.
    if (page.until.rfind("filed:", 0) == 0) {
        const Country* me = m_countries.getCountry(m_playerCountryId);
        const std::string them = walkDiploTarget();
        const std::string action = page.until.substr(6);
        // IS THE ENTRY EVEN IN THE LIST? What the province panel offers
        // depends entirely on what the two countries already are to each
        // other: at war it collapses to Request Ceasefire, and at peace there
        // is no ceasefire to ask for. A page waiting for an entry that this
        // relationship does not have is a page nobody can leave.
        if (me && !them.empty()) {
            const bool war = walkAtWarWith(them);
            const bool ceasefire = (action == "request_ceasefire");
            if (war && !ceasefire)
                walkProblem("waits for \"" + action + "\" with " + them +
                            ", who we are at war with -- their panel offers "
                            "Request Ceasefire and nothing else");
            if (!war && ceasefire)
                walkProblem("waits for a ceasefire with " + them +
                            ", who we are not at war with -- the entry is not there");
        }
        if (me && !them.empty()) {
            for (const auto& da : m_pendingDiplomaticActions) {
                if (da.sourceIso != me->isoA3 || da.targetIso != them) continue;
                walkProblem("waits for \"" + page.until + "\" while a \"" + da.action +
                            "\" is still pending with " + them + " -- the panel allows one "
                            "request per country and greys the rest, so there is nothing "
                            "to click");
                break;
            }
        }
    }

    const std::string need = controlFor(page.until);
    if (need.empty()) return;

    if (need == "map") {
        if (tutorialGateHoldsMap())
            walkProblem("waits for a province to be picked while the gate is holding "
                        "the map, so there is nothing left to pick it with");
        return;
    }

    auto it = m_uiTargets.find(need);
    if (it == m_uiTargets.end() || it->second.frame + 1 < m_uiFrame) {
        walkProblem("waits on \"" + page.until + "\", which is answered from \"" + need +
                    "\" -- and nothing is drawing that");
        return;
    }
    // EVERY part of it, not just the middle. The textbox is always live, so a
    // control that overlaps it is reachable through the overlap even while the
    // gate is shut -- the sidebar tabs sit half behind the box and are clicked
    // through it every game. A deadlock is when there is nowhere on the
    // control a click can land, so the whole rectangle has to be shut.
    const Rectangle r = it->second.rect;
    bool anyReachable = false;
    for (int gy = 0; gy <= 2 && !anyReachable; ++gy)
        for (int gx = 0; gx <= 2 && !anyReachable; ++gx) {
            const Vector2 pt{r.x + r.width * (0.1f + 0.4f * gx),
                             r.y + r.height * (0.1f + 0.4f * gy)};
            if (!tutorialBlocksInput(pt)) anyReachable = true;
        }
    if (!anyReachable)
        walkProblem("gates the screen away from \"" + need + "\", which is the only way "
                    "to satisfy \"" + page.until + "\" -- there is nowhere on it a click "
                    "can land");
}

// A popup is modal: run() draws it and starts the next frame without updating
// anything, the dialogue included. In play that is right -- the player reads it
// and clicks. The walk has no hands, so it takes the same answer clicking OK
// would, and says which popup it answered: a lesson that raises one is worth
// knowing about, because the lesson is frozen behind it until it is gone.
void Game::walkDismissPopup() {
    if (m_popupQueue.empty()) return;
    printf("[WALK]   popup \"%s\" -- dismissed\n", m_popupQueue.front().title.c_str());
    fflush(stdout);
    m_popupQueue.erase(m_popupQueue.begin());
}

bool Game::walkScriptExists(const std::string& name) const {
    const std::string path = m_dataDir + "dialog/" + walkLanguage() + "/" + name + ".oddlg";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
