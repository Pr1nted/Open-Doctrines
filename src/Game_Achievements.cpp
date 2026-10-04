// The game's half of achievements: what it counts, when, and the screen.
//
// The rules about what an achievement IS live in src/achievements/; this file
// only feeds them. Two kinds of feeding:
//
//   ACTIONS the player takes (fire a mortar, save, send a letter) are counted
//   where they happen, through achNote(). One line at each call site.
//
//   STATE is measured once a turn by achTurnSnapshot(), and changes in it are
//   found by comparing with the previous turn's snapshot rather than by hooking
//   every place that state can change. Alliances, peace, conquests and
//   eliminations each have several writers -- treaties, AI requests, the
//   ceasefire terms, a multiplayer client applying the host's delta -- and a
//   diff sees all of them, including the ones added next month.

#include "Game.h"
#include "GameInternals.h"
#include "Audio.h"
#include "achievements/Achievements.h"
#include "achievements/AchievementCatalog.gen.h"
#include "i18n/Text.h"
#include "net/AccountClient.h"
#include "platform/SteamBridge.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

bool Game::achievementsLive() const {
    // A person, playing. Every automated mode presses the same buttons a
    // person does, so each has to be excluded by name; the list is the one
    // processTurn uses for "somebody is watching", plus the modes it does not
    // need to know about.
    if (m_aiTraining || m_walk || !m_shotDir.empty() || m_headless || m_shotTour || m_ojhFps ||
        m_llmLetter || m_tutorialMode || serverRunning() || chatPlaysActive())
        return false;
    if (m_benchPlayUntilTurn > 0 || m_agentUntilTurn > 0) return false;
    return odach::Tracker::get().initialised();
}

void Game::achNote(const char* stat, double n) {
    if (!achievementsLive()) return;
    odach::Tracker::get().add(stat, n);
    m_achDirty = true;
}

void Game::achNoteSet(const char* stat, const std::string& member) {
    if (!achievementsLive()) return;
    odach::Tracker::get().addToSet(stat, member);
    m_achDirty = true;
}

void Game::achInit() {
    const unsigned long long seal = tableSeal();
    odach::Tracker::get().init(m_dataDir, GAME_VERSION, seal);
    odsteam::init();
}

void Game::achWorldStarted(bool fromTurnZero) {
    odach::Tracker::get().resetWorld();
    m_achSnap = AchSnapshot{};
    m_achDeclaredOn.clear();
    m_achWarStreak = 0;
    m_achWorldFromZero = fromTurnZero;
    m_achEverAtWar = false;
}

void Game::achPlayerChose(int countryId) {
    if (!achievementsLive()) return;
    if (countryId == SPC_CID) return;
    const Country* c = m_countries.getCountry(countryId);
    if (!c) return;
    achNote("games_started");
    achNoteSet("distinct_countries", c->isoA3.empty() ? c->name : c->isoA3);

    // Size relative to the map, at the moment of choosing.
    std::unordered_map<int, int> count;
    for (auto& [pid, p] : m_provinces.getAllProvinces())
        if (p.countryId > 0 && p.countryId < REBEL_CID_MIN) count[p.countryId]++;
    int mine = count[countryId], biggest = 0;
    for (auto& [cid, n] : count)
        if (cid != UNC_CID && cid != BLC_CID && cid != SPC_CID) biggest = std::max(biggest, n);
    if (mine == 1) achNote("started_tiny");
    if (mine > 0 && mine == biggest) achNote("started_biggest");
    achWorldStarted(m_turnNumber <= 1);
}

void Game::achTurnSnapshot(bool multiplayerTurn) {
    if (!achievementsLive()) return;
    auto& tr = odach::Tracker::get();

    if (m_playerCountryId == SPC_CID) {
        tr.add("spectator_turns", 1);
        tr.evaluate();
        return;
    }
    if (m_playerCountryId <= 0) return;
    const Country* me = m_countries.getCountry(m_playerCountryId);
    if (!me) return;
    const std::string myIso = me->isoA3;

    tr.add("turns_ended", 1);
    if (multiplayerTurn) tr.add("mp_turns", 1);

    // ---- measured now ----
    AchSnapshot now;
    now.valid = true;
    int worldOwned = 0, maxInd = 0, maxFort = 0, maxPort = 0;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId > 0 && p.countryId != UNC_CID && p.countryId != BLC_CID) ++worldOwned;
        if (p.countryId != m_playerCountryId) continue;
        now.provinces.insert(pid);
        auto ind = m_provinceIndustry.find(pid);
        if (ind != m_provinceIndustry.end()) {
            maxInd = std::max(maxInd, ind->second.level);
            maxFort = std::max(maxFort, ind->second.fortification);
        }
        auto port = m_provincePorts.find(pid);
        if (port != m_provincePorts.end()) maxPort = std::max(maxPort, port->second.level);
    }
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID || cid == SPC_CID) continue;
        if (!m_eliminatedCids.count(cid)) now.alive.insert(cid);
    }
    auto rel = m_relations.find(myIso);
    if (rel != m_relations.end()) {
        for (auto& [other, r] : rel->second) {
            if (other == myIso) continue;
            const int ocid = cidForIso(other);
            if (ocid <= 0 || m_eliminatedCids.count(ocid)) continue;
            if (r.war) now.wars.insert(other);
            if (r.alliance) now.allies.insert(other);
            if (r.nonAggression) now.naps.insert(other);
            if (r.guarantee) now.guarantees.insert(other);
        }
    }

    const double treasury = me->treasury;
    int ships = 0;
    for (const auto& s : m_ships) if (s.countryId == m_playerCountryId) ++ships;
    auto researched = m_countryResearched.find(m_playerCountryId);

    tr.set("provinces", (double)now.provinces.size());
    tr.set("world_share_pct", worldOwned ? 100.0 * (double)now.provinces.size() / worldOwned : 0);
    tr.set("population", (double)countryPopulation(m_playerCountryId));
    tr.set("treasury", treasury);
    tr.set("in_debt", treasury < 0 ? 1 : 0);
    tr.set("exact_change", treasury >= 0 && treasury < 1 ? 1 : 0);
    tr.set("wars_active", (double)now.wars.size());
    tr.set("allies", (double)now.allies.size());
    tr.set("research_known", researched == m_countryResearched.end() ? 0 : (double)researched->second.size());
    tr.set("ships", ships);
    tr.set("troops", (double)countryTroops(m_playerCountryId));
    tr.set("max_industry", maxInd);
    tr.set("max_fort", maxFort);
    tr.set("max_port", maxPort);
    tr.set("world_turn", m_turnNumber);
    tr.set("few_countries_left", (now.alive.size() < 10 && now.alive.count(m_playerCountryId)) ? 1 : 0);
    {
        auto cc = m_countryCompass.find(m_playerCountryId);
        if (cc != m_countryCompass.end()) {
            const double e = cc->second.economic, so = cc->second.social;
            tr.set("compass_extreme", (std::fabs(e) >= 90 || std::fabs(so) >= 90) ? 1 : 0);
            if (m_turnNumber == 50 && std::fabs(e) <= 5 && std::fabs(so) <= 5) tr.add("compass_centre_t50", 1);
        }
    }
    if (treasury < 0) tr.add("bankrupt_turns", 1);
    {
        auto rb = m_rebellionsThisTurnByCid.find(m_playerCountryId);
        if (rb != m_rebellionsThisTurnByCid.end() && rb->second > 0) tr.add("rebellions_suffered", rb->second);
    }

    // ---- streaks within this world ----
    if (!now.wars.empty()) { m_achEverAtWar = true; ++m_achWarStreak; }
    else m_achWarStreak = 0;
    tr.set("war_streak", m_achWarStreak);
    tr.set("peaceful_turns", (m_achWorldFromZero && !m_achEverAtWar) ? m_turnNumber : 0);

    // ---- what changed since last turn ----
    if (m_achSnap.valid) {
        int gained = 0, lost = 0;
        for (int pid : now.provinces) if (!m_achSnap.provinces.count(pid)) ++gained;
        for (int pid : m_achSnap.provinces) if (!now.provinces.count(pid)) ++lost;
        // A turn that moves hundreds of provinces at once is not a conquest
        // but a reload or a scenario swap under the same tracker; ignore it
        // rather than award "Cartographers Hate This One Trick" for a load.
        if (gained + lost < 400) {
            tr.add("provinces_conquered", gained);
            tr.add("provinces_lost", lost);
        }
        for (const auto& w : now.wars)
            if (!m_achSnap.wars.count(w) && !m_achDeclaredOn.count(w)) tr.add("wars_received", 1);
        for (const auto& w : m_achSnap.wars)
            if (!now.wars.count(w)) {
                const int ocid = cidForIso(w);
                if (ocid > 0 && now.alive.count(ocid)) tr.add("peace_made", 1);
            }
        for (const auto& a : now.allies) if (!m_achSnap.allies.count(a)) tr.add("alliances_formed", 1);
        for (const auto& a : now.naps) if (!m_achSnap.naps.count(a)) tr.add("naps_signed", 1);
        for (const auto& a : now.guarantees) if (!m_achSnap.guarantees.count(a)) tr.add("guarantees_given", 1);
        for (int cid : m_achSnap.alive) {
            if (now.alive.count(cid)) continue;
            if (cid == m_playerCountryId) { tr.add("player_eliminated", 1); continue; }
            const Country* c = m_countries.getCountry(cid);
            if (c && m_achSnap.wars.count(c->isoA3)) tr.add("enemies_eliminated", 1);
        }
    }
    m_achDeclaredOn.clear();
    m_achSnap = std::move(now);

    tr.evaluate();
    m_achDirty = false;
}

void Game::achNoteWarDeclared(const std::string& targetIso) {
    if (!achievementsLive()) return;
    achNote("wars_declared");
    if (m_turnNumber <= 1) achNote("war_on_turn_one");
    m_achDeclaredOn.insert(targetIso);
}

void Game::achNoteArtillery(const std::string& ammo, int targetPid) {
    if (!achievementsLive()) return;
    static const char* kinds[] = {"mortar", "light", "heavy", "napalm", "carpet", "chemical", "nuclear", "biological"};
    for (const char* k : kinds) {
        if (ammo != k) continue;
        achNote((std::string("fired_") + k).c_str());
        achNoteSet("artillery_kinds", k);
    }
    const Province* p = m_provinces.getProvinceById(targetPid);
    if (p && p->countryId == m_playerCountryId) achNote("shelled_own");
}

void Game::achFrame() {
    auto& tr = odach::Tracker::get();
    if (!tr.initialised()) return;
    // NOTHING for an automated run. init() finishes before --screenshots,
    // --train-ai and friends switch their modes on, so anything counted at
    // init would have been counted for them too -- and a tour then earned and
    // wrote achievements from a counter file on disk.
    if (!achievementsLive()) return;
    // PLAY TIME, like Steam's: time the game is open with a person at it.
    // Kept in progress.json as a counter, so the launcher can show it however
    // the game was started; flushed once a minute rather than every frame.
    m_achPlaySeconds += GetFrameTime();
    if (m_achPlaySeconds >= 60.0f) {
        tr.add("seconds_played", m_achPlaySeconds);
        m_achPlaySeconds = 0;
        m_achDirty = true;
    }
    if (!m_achSessionNoted) {
        m_achSessionNoted = true;
        tr.addToSet("languages_used", od::i18n::current().code);
        // Started by the launcher: it sets this so the game can say thank you.
        if (const char* l = std::getenv("OD_LAUNCHER"); l && std::strcmp(l, "unifico") == 0) tr.add("launcher_launches", 1);
        m_achDirty = true;
    }
    if (m_achDirty) {
        tr.evaluate();
        m_achDirty = false;
    }
    tr.update();
    if (!m_achToastActive) {
        odach::Toast t;
        if (tr.popToast(t)) {
            m_achToast = t;
            m_achToastActive = true;
            m_achToastT = 0;
            Audio::get().playSfx(t.verified ? "click_light" : "deal_accepted");
        }
    }
}

// --------------------------------------------------------------------- toast

void Game::drawAchievementToast() {
    if (!m_achToastActive) return;
    m_achToastT += GetFrameTime();
    const float life = m_achToast.verified ? 3.5f : 5.5f;
    if (m_achToastT > life) { m_achToastActive = false; return; }
    const odach::Def* def = nullptr;
    for (int i = 0; i < odach::kCatalogCount; ++i)
        if (m_achToast.id == odach::kCatalog[i].id) def = &odach::kCatalog[i];
    if (!def) { m_achToastActive = false; return; }

    // Slides in from the top, holds, slides out. Over everything, on every
    // screen: an achievement earned in the background of a menu still counts.
    const float in = std::min(1.0f, m_achToastT / 0.35f);
    const float out = std::min(1.0f, (life - m_achToastT) / 0.35f);
    const float k = std::min(in, out);
    const float ease = 1.0f - (1.0f - k) * (1.0f - k);
    const int w = 420, h = 76;
    const int x = m_screenW / 2 - w / 2;
    const int y = (int)(-h + (h + 18) * ease);
    const Color gold{201, 162, 39, 255};
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.18f, 8, Color{12, 15, 22, 235});
    DrawRectangleRoundedLinesEx({(float)x, (float)y, (float)w, (float)h}, 0.18f, 8, 2, gold);
    achDrawIcon(def->icon, true, x + 10, y + 10, 56);
    const char* head = m_achToast.verified ? T("Achievement confirmed") : T("Achievement earned");
    DrawText(head, x + 78, y + 12, 16, gold);
    DrawText(T(def->name), x + 78, y + 34, 22, RAYWHITE);
}

void Game::achDrawIcon(int tile, bool unlocked, int x, int y, int size) {
    if (!m_achAtlasLoaded) {
        m_achAtlasLoaded = true;
        const std::string path = m_dataDir + "icons/achievements.png";
        if (FileExists(path.c_str())) {
            m_achAtlas = LoadTexture(path.c_str());
            SetTextureFilter(m_achAtlas, TEXTURE_FILTER_BILINEAR);
        }
    }
    const float t = (float)odach::kIconTilePx;
    if (m_achAtlas.id == 0) {
        DrawCircle(x + size / 2, y + size / 2, size / 2.0f, unlocked ? Color{201, 162, 39, 255} : DARKGRAY);
        return;
    }
    DrawTexturePro(m_achAtlas, {tile * t, unlocked ? 0.0f : t, t, t},
                   {(float)x, (float)y, (float)size, (float)size}, {0, 0}, 0, WHITE);
}

// -------------------------------------------------------------------- screen

namespace {
const char* categoryLabel(const char* cat) {
    // The same words tools/i18n_extract.py offers translators, by design.
    if (!std::strcmp(cat, "meta")) return T("Head of State");
    if (!std::strcmp(cat, "war")) return T("Warfare");
    if (!std::strcmp(cat, "artillery")) return T("Artillery");
    if (!std::strcmp(cat, "diplomacy")) return T("Diplomacy");
    if (!std::strcmp(cat, "economy")) return T("Economy");
    if (!std::strcmp(cat, "empire")) return T("Empire");
    if (!std::strcmp(cat, "science")) return T("Science");
    if (!std::strcmp(cat, "society")) return T("Society");
    if (!std::strcmp(cat, "navy")) return T("Navy");
    if (!std::strcmp(cat, "social")) return T("Multiplayer");
    if (!std::strcmp(cat, "creator")) return T("Creator");
    return cat;
}
}  // namespace

void Game::openAchievements(ScreenState back) {
    m_achBack = back;
    m_currentScreen = SCREEN_ACHIEVEMENTS;
    m_achScroll = 0;
    if (achievementsLive()) {
        odach::Tracker::get().add("achievements_viewed", 1);
        odach::Tracker::get().evaluate();
        odach::Tracker::get().syncSoon();
    }
}

void Game::updateAchievements() {
    if (isMouseOverConsole()) return;
    const Vector2 mouse = getMouse();
    const Rectangle xBtn = {(float)(m_screenW - 52), 16, 36, 36};
    if (IsKeyPressed(KEY_ESCAPE) ||
        (CheckCollisionPointRec(mouse, xBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
         m_pressScreen == SCREEN_ACHIEVEMENTS)) {
        m_currentScreen = m_achBack;
        return;
    }
    // Category filter chips.
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && m_pressScreen == SCREEN_ACHIEVEMENTS) {
        for (size_t i = 0; i < m_achChipRects.size(); ++i)
            if (CheckCollisionPointRec(mouse, m_achChipRects[i])) {
                m_achFilter = (int)i;
                m_achScroll = 0;
            }
    }
    m_achScroll -= GetMouseWheelMove() * 60.0f;
    if (IsKeyDown(KEY_DOWN)) m_achScroll += 600 * GetFrameTime();
    if (IsKeyDown(KEY_UP)) m_achScroll -= 600 * GetFrameTime();
    m_achScroll = std::max(0.0f, std::min(m_achScroll, m_achScrollMax));
}

void Game::drawAchievements() {
    drawMenuBackground();
    DrawRectangle(0, 0, m_screenW, m_screenH, Color{8, 10, 15, 200});
    const Color gold{201, 162, 39, 255};
    const Color ink{232, 228, 218, 255};
    const Color muted{154, 160, 174, 255};
    const Color faint{102, 109, 124, 255};
    auto& tr = odach::Tracker::get();
    const auto rows = tr.entries();

    int granted = 0, earned = 0;
    for (auto& e : rows) { granted += e.granted; earned += e.earned; }

    const int pad = 40;
    int y = 28;
    DrawText(T("Achievements"), pad, y, 40, ink);
    y += 50;
    DrawText(TextFormat(T("%d of %d confirmed"), granted, odach::kCatalogCount), pad, y, 20, gold);
    if (earned > granted) {
        const int tw = MeasureText(TextFormat(T("%d of %d confirmed"), granted, odach::kCatalogCount), 20);
        DrawText(TextFormat(T("  ·  %d earned, waiting for the account service"), earned - granted),
                 pad + tw, y, 20, muted);
    }
    y += 28;
    // Progress bar across the page.
    DrawRectangle(pad, y, m_screenW - pad * 2, 6, Color{35, 41, 54, 255});
    DrawRectangle(pad, y, (int)((m_screenW - pad * 2) * (double)granted / odach::kCatalogCount), 6, gold);
    y += 18;

    std::string code = tr.syncStatus();
    if (!odach::Tracker::keysBaked()) code = "no_keys";
    const char* status = nullptr;
    if (code == "sign_in") status = T("Sign in (Account) to have achievements confirmed.");
    else if (code == "no_keys") status = T("This build cannot confirm achievements.");
    else if (code == "not_issuing") status = T("The account service is not issuing achievements.");
    else if (code == "unreachable") status = T("Achievements will be confirmed when the service is reachable.");
    else if (code.rfind("waiting:", 0) == 0)
        status = TextFormat(T("%d waiting to be confirmed by the account service."), atoi(code.c_str() + 8));
    if (status) DrawText(status, pad, y, 16, faint);
    y += 26;

    // ── CLOSEST TO UNLOCKING ──
    // The three counted achievements the player is furthest along, so the
    // screen answers "what next?" before it answers "what is there?". Hidden
    // ones stay hidden, and yes/no achievements have no "nearly".
    {
        std::vector<const odach::Entry*> near;
        for (auto& e : rows)
            if (!e.earned && !e.def->hidden && e.def->gte > 1 && e.progress > 0) near.push_back(&e);
        std::sort(near.begin(), near.end(), [](const odach::Entry* a, const odach::Entry* b) {
            return a->progress / a->def->gte > b->progress / b->def->gte;
        });
        if (near.size() > 3) near.resize(3);
        if (!near.empty()) {
            DrawText(T("Closest to unlocking"), pad, y, 18, gold);
            y += 28;
            const int gap = 14;
            const int cw = (m_screenW - pad * 2 - gap * 2) / 3;
            for (size_t i = 0; i < near.size(); ++i) {
                const odach::Entry& e = *near[i];
                const int cx = pad + (int)i * (cw + gap);
                const Rectangle card{(float)cx, (float)y, (float)cw, 70};
                DrawRectangleRounded(card, 0.15f, 8, Color{22, 32, 46, 235});
                DrawRectangleRoundedLinesEx(card, 0.15f, 8, 1, Color{140, 115, 32, 255});
                achDrawIcon(e.def->icon, false, cx + 10, y + 10, 50);
                const auto nm = wrapText(T(e.def->name), 17, cw - 80);
                DrawText(nm.empty() ? "" : nm[0].c_str(), cx + 70, y + 10, 17, ink);
                const double p = std::min(1.0, e.progress / e.def->gte);
                const int bw = cw - 84;
                DrawRectangle(cx + 70, y + 38, bw, 6, Color{35, 41, 54, 255});
                DrawRectangle(cx + 70, y + 38, (int)(bw * p), 6, gold);
                DrawText(TextFormat(T("%.0f of %.0f  ·  %d%%"), std::min(e.progress, e.def->gte), e.def->gte, (int)(p * 100)),
                         cx + 70, y + 49, 14, muted);
            }
            y += 84;
        }
    }

    // Category chips: All, then each category.
    m_achChipRects.clear();
    int cx = pad;
    const Vector2 mouse = getMouse();
    for (int i = -1; i < odach::kCategoryCount; ++i) {
        const char* label = i < 0 ? T("All") : categoryLabel(odach::kCategories[i]);
        const int w = MeasureText(label, 18) + 24;
        if (cx + w > m_screenW - pad) { cx = pad; y += 34; }
        const Rectangle r{(float)cx, (float)y, (float)w, 28};
        const bool on = m_achFilter == i + 1;
        const bool hov = CheckCollisionPointRec(mouse, r);
        DrawRectangleRounded(r, 0.5f, 8, on ? gold : (hov ? Color{51, 59, 75, 255} : Color{20, 24, 34, 255}));
        DrawText(label, cx + 12, y + 5, 18, on ? Color{12, 15, 22, 255} : ink);
        m_achChipRects.push_back(r);
        cx += w + 8;
    }
    y += 44;

    // The grid. Two columns when there is room for them.
    const int top = y;
    const int cols = m_screenW >= 1100 ? 2 : 1;
    const int gap = 14;
    const int cardW = (m_screenW - pad * 2 - gap * (cols - 1)) / cols;
    const int cardH = 92;
    BeginScissorMode(0, top, m_screenW, m_screenH - top);
    int idx = 0;
    for (const auto& e : rows) {
        if (m_achFilter > 0 && std::strcmp(e.def->cat, odach::kCategories[m_achFilter - 1]) != 0) continue;
        const int col = idx % cols, row = idx / cols;
        ++idx;
        const int x = pad + col * (cardW + gap);
        const int cy = top + row * (cardH + gap) - (int)m_achScroll;
        if (cy > m_screenH || cy + cardH < top) continue;
        const bool lit = e.granted;
        const Rectangle card{(float)x, (float)cy, (float)cardW, (float)cardH};
        DrawRectangleRounded(card, 0.12f, 8, lit ? Color{22, 32, 46, 240} : Color{14, 17, 25, 230});
        DrawRectangleRoundedLinesEx(card, 0.12f, 8, 1,
                                    lit ? gold : (e.earned ? Color{140, 115, 32, 255} : Color{35, 41, 54, 255}));
        achDrawIcon(e.def->icon, lit, x + 14, cy + 14, 64);

        const bool secret = e.def->hidden && !e.earned;
        const char* name = secret ? T("Hidden achievement") : T(e.def->name);
        DrawText(name, x + 92, cy + 12, 20, lit ? ink : muted);
        const std::string desc = secret ? T("Keep playing. Or keep doing something you probably shouldn't.")
                                        : T(e.def->desc);
        const auto lines = wrapText(desc, 16, cardW - 104);
        for (size_t i = 0; i < lines.size() && i < 2; ++i)
            DrawText(lines[i].c_str(), x + 92, cy + 38 + (int)i * 19, 16, lit ? muted : faint);

        // Right-hand state: a date, "waiting", or a progress bar.
        const char* state = nullptr;
        Color sc = faint;
        if (lit) { state = e.modded ? T("Confirmed (modified rules)") : T("Confirmed"); sc = gold; }
        else if (e.earned) { state = T("Waiting for confirmation"); sc = Color{180, 150, 60, 255}; }
        if (state) {
            const int sw = MeasureText(state, 14);
            DrawText(state, x + cardW - sw - 14, cy + cardH - 22, 14, sc);
        } else if (!secret && e.def->gte > 1) {
            const double p = std::min(1.0, e.progress / e.def->gte);
            const int bw = 140;
            DrawRectangle(x + cardW - bw - 14, cy + cardH - 16, bw, 5, Color{35, 41, 54, 255});
            DrawRectangle(x + cardW - bw - 14, cy + cardH - 16, (int)(bw * p), 5, Color{140, 115, 32, 255});
            const char* nums = TextFormat("%.0f / %.0f", std::min(e.progress, e.def->gte), e.def->gte);
            DrawText(nums, x + cardW - bw - 14 - MeasureText(nums, 14) - 8, cy + cardH - 21, 14, faint);
        }
    }
    EndScissorMode();
    const int rowsTotal = (idx + cols - 1) / cols;
    m_achScrollMax = std::max(0.0f, (float)(rowsTotal * (cardH + gap) - (m_screenH - top) + 20));

    // Close (X).
    const Rectangle xBtn = {(float)(m_screenW - 52), 16, 36, 36};
    const bool xHov = CheckCollisionPointRec(mouse, xBtn);
    DrawRectangleRounded(xBtn, 0.2f, 8, xHov ? Color{255, 64, 64, 32} : BLANK);
    const int xc = (int)xBtn.x + 18, yc = (int)xBtn.y + 18;
    DrawLineEx({(float)xc - 8, (float)yc - 8}, {(float)xc + 8, (float)yc + 8}, 2.5f, xHov ? RAYWHITE : LIGHTGRAY);
    DrawLineEx({(float)xc + 8, (float)yc - 8}, {(float)xc - 8, (float)yc + 8}, 2.5f, xHov ? RAYWHITE : LIGHTGRAY);
}
