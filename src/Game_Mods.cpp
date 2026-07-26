// Mod menu, mod panel rendering, and the bridge from the game world to the
// GameState.Read capability.
//
// The menu is the only place a mod is ever loaded (docs/modding.md, "Lifecycle
// rules"), so everything that can start or stop a mod lives in this file.

#include "Game.h"
#include "GameInternals.h"
#include "mods/ModManager.h"
#include "mods/ModHost.h"

#include <algorithm>
#include "ai/AISystem.h"
#include <cstdio>
#include <cstring>

// ----------------------------------------------------- world access -------

namespace {

// Bridges the game's containers to the mod-facing getters. Deliberately narrow:
// a mod sees exactly these fields and nothing else about the world.
class GameModAccess : public ModGameAccess {
public:
    explicit GameModAccess(Game* g) : m_game(g) {}

    uint32_t turnNumber() override { return (uint32_t)m_game->modTurnNumber(); }
    uint32_t countryCount() override { return (uint32_t)m_game->modCountryIds().size(); }

    uint32_t countryAt(uint32_t index) override {
        const auto& ids = m_game->modCountryIds();
        if (index >= ids.size()) return 0xFFFFFFFFu;
        return (uint32_t)ids[index];
    }
    bool countryExists(uint32_t cid) override { return m_game->modCountryExists((int)cid); }
    std::string countryName(uint32_t cid) override { return m_game->modCountryName((int)cid); }
    double countryTreasury(uint32_t cid) override { return m_game->modCountryTreasury((int)cid); }
    uint32_t countryProvinceCount(uint32_t cid) override {
        return (uint32_t)m_game->modCountryProvinceCount((int)cid);
    }
    long long provincePopulation(uint32_t pid) override {
        return m_game->modProvincePopulation((int)pid);
    }
    uint32_t provinceOwner(uint32_t pid) override {
        int o = m_game->modProvinceOwner((int)pid);
        return o <= 0 ? 0xFFFFFFFFu : (uint32_t)o;
    }

private:
    Game* m_game;
    uint32_t mapWidth() override  { return (uint32_t)m_game->modMapWidth(); }
    uint32_t mapHeight() override { return (uint32_t)m_game->modMapHeight(); }
    uint32_t provinceCount() override {
        return (uint32_t)m_game->modProvinceIds().size();
    }
    uint32_t provinceAt(uint32_t index) override {
        const auto& ids = m_game->modProvinceIds();
        return index < ids.size() ? (uint32_t)ids[index] : 0xFFFFFFFFu;
    }
    bool provinceExists(uint32_t pid) override {
        return m_game->modProvinceExists((int)pid);
    }
    std::string provinceName(uint32_t pid) override {
        return m_game->modProvinceName((int)pid);
    }
    double provinceCenterX(uint32_t pid) override {
        return m_game->modProvinceCenterX((int)pid);
    }
    double provinceCenterY(uint32_t pid) override {
        return m_game->modProvinceCenterY((int)pid);
    }
    bool provinceIsLand(uint32_t pid) override {
        return m_game->modProvinceIsLand((int)pid);
    }
    uint32_t provinceNeighborCount(uint32_t pid) override {
        return (uint32_t)m_game->modProvinceNeighborCount((int)pid);
    }
    uint32_t provinceNeighborAt(uint32_t pid, uint32_t index) override {
        int n = m_game->modProvinceNeighborAt((int)pid, (int)index);
        return n < 0 ? 0xFFFFFFFFu : (uint32_t)n;
    }
    bool atWar(uint32_t a, uint32_t b) override {
        return m_game->modAtWar((int)a, (int)b);
    }
    bool allied(uint32_t a, uint32_t b) override {
        return m_game->modAllied((int)a, (int)b);
    }
    bool nonAggression(uint32_t a, uint32_t b) override {
        return m_game->modNonAggression((int)a, (int)b);
    }
    bool guaranteed(uint32_t a, uint32_t b) override {
        return m_game->modGuaranteed((int)a, (int)b);
    }
    bool proposeWar(uint32_t a, uint32_t b) override {
        return m_game->modProposeWar((int)a, (int)b);
    }
    bool setCountryTreasury(uint32_t cid, double v) override {
        return m_game->modSetCountryTreasury((int)cid, v);
    }
    bool addCountryTreasury(uint32_t cid, double d) override {
        return m_game->modAddCountryTreasury((int)cid, d);
    }
    bool setProvinceOwner(uint32_t pid, uint32_t cid) override {
        return m_game->modSetProvinceOwner((int)pid, (int)cid);
    }
    bool setProvincePopulation(uint32_t pid, long long v) override {
        return m_game->modSetProvincePopulation((int)pid, v);
    }
    uint32_t neuralFeatureCount() override {
        return (uint32_t)m_game->modNeuralFeatureCount();
    }
    uint32_t neuralFeatures(uint32_t cid, float* out, uint32_t cap) override {
        return (uint32_t)m_game->modNeuralFeatures((int)cid, out, (int)cap);
    }
    uint32_t neuralRewardCount() override {
        return (uint32_t)m_game->modNeuralRewardCount();
    }
    double neuralRewardMean(uint32_t i) override {
        return m_game->modNeuralRewardMean((int)i);
    }
};

GameModAccess* g_access = nullptr;

// macOS-only native picker, mirroring the one in Game_Browsers.cpp (which is
// static to that translation unit). Everywhere else, drag-and-drop is the path
// -- and on web it is the only one.
std::string pickOdmodFile() {
#ifdef __APPLE__
    std::string script =
        "osascript -e 'POSIX path of (choose file with prompt \"Select a .odmod file\" "
        "of type {\"odmod\"})' 2>/dev/null";
    FILE* pipe = popen(script.c_str(), "r");
    if (!pipe) return "";
    char buf[1024];
    std::string out;
    while (fgets(buf, sizeof buf, pipe)) out += buf;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#else
    return "";
#endif
}

Color colorFromRGBA(uint32_t rgba) {
    return Color{(unsigned char)((rgba >> 24) & 0xFF), (unsigned char)((rgba >> 16) & 0xFF),
                 (unsigned char)((rgba >> 8) & 0xFF), (unsigned char)(rgba & 0xFF)};
}

}  // namespace

// -------------------------------------------------- world accessors -------

int Game::modTurnNumber() const { return m_turnNumber; }

const std::vector<int>& Game::modCountryIds() const {
    // Rebuilt when the turn changes; a mod walking every country would
    // otherwise pay for a full map rebuild on each call.
    if (m_modCountryIdsTurn != m_turnNumber || m_modCountryIds.empty()) {
        m_modCountryIds.clear();
        for (const auto& kv : m_countries.getAll()) {
            if (kv.first <= 0 || kv.first >= REBEL_CID_MIN) continue;
            m_modCountryIds.push_back(kv.first);
        }
        std::sort(m_modCountryIds.begin(), m_modCountryIds.end());
        m_modCountryIdsTurn = m_turnNumber;
        m_modProvCounts.clear();
    }
    return m_modCountryIds;
}

bool Game::modCountryExists(int cid) const {
    return m_countries.getCountry(cid) != nullptr;
}

std::string Game::modCountryName(int cid) const {
    const Country* c = m_countries.getCountry(cid);
    return c ? c->name : std::string();
}

double Game::modCountryTreasury(int cid) const {
    const Country* c = m_countries.getCountry(cid);
    return c ? c->treasury : 0.0;
}

int Game::modCountryProvinceCount(int cid) const {
    if (m_modProvCounts.empty()) {
        for (size_t pid = 0; pid < m_provinceCountryLookup.size(); ++pid) {
            int owner = m_provinceCountryLookup[pid];
            if (owner > 0) m_modProvCounts[owner]++;
        }
    }
    auto it = m_modProvCounts.find(cid);
    return it == m_modProvCounts.end() ? 0 : it->second;
}

long long Game::modProvincePopulation(int pid) const {
    auto it = m_provincePopulations.find(pid);
    return it == m_provincePopulations.end() ? 0 : it->second;
}

int Game::modProvinceOwner(int pid) const {
    if (pid < 0 || (size_t)pid >= m_provinceCountryLookup.size()) return 0;
    return m_provinceCountryLookup[pid];
}

// ---------------------------------------------------------------- Map -----
//
// Geometry only. Adjacency and centres are computed once at load, so every one
// of these is a lookup rather than a scan -- which matters because a mod may
// call them from a per-frame draw hook.

int Game::modMapWidth() const  { return m_provinces.getWidth(); }
int Game::modMapHeight() const { return m_provinces.getHeight(); }

const std::vector<int>& Game::modProvinceIds() const {
    // m_provinces is an unordered_map, so its iteration order is not stable
    // across runs. A mod indexing provinces needs an order that is, hence the
    // sorted cache. Provinces do not appear or vanish mid-game, so unlike the
    // country list this is built once.
    if (m_modProvinceIds.empty()) {
        for (const auto& kv : m_provinces.getAllProvinces())
            m_modProvinceIds.push_back(kv.first);
        std::sort(m_modProvinceIds.begin(), m_modProvinceIds.end());
    }
    return m_modProvinceIds;
}

bool Game::modProvinceExists(int pid) const {
    return m_provinces.getAllProvinces().count(pid) != 0;
}

std::string Game::modProvinceName(int pid) const {
    const auto& all = m_provinces.getAllProvinces();
    auto it = all.find(pid);
    return it == all.end() ? std::string() : it->second.name;
}

float Game::modProvinceCenterX(int pid) const {
    auto it = m_provinceCenters.find(pid);
    return it == m_provinceCenters.end() ? 0.0f : it->second.x;
}

float Game::modProvinceCenterY(int pid) const {
    auto it = m_provinceCenters.find(pid);
    return it == m_provinceCenters.end() ? 0.0f : it->second.y;
}

bool Game::modProvinceIsLand(int pid) const {
    // Sampled at the province centre. A province is land or sea as a whole in
    // this game, so one sample is the answer rather than an approximation of it.
    auto it = m_provinceCenters.find(pid);
    if (it == m_provinceCenters.end()) return false;
    return m_landSea.isLand((int)it->second.x, (int)it->second.y);
}

int Game::modProvinceNeighborCount(int pid) const {
    auto it = m_provinceNeighbors.find(pid);
    return it == m_provinceNeighbors.end() ? 0 : (int)it->second.size();
}

int Game::modProvinceNeighborAt(int pid, int index) const {
    auto it = m_provinceNeighbors.find(pid);
    if (it == m_provinceNeighbors.end()) return -1;
    if (index < 0 || (size_t)index >= it->second.size()) return -1;
    return it->second[index];
}

// --------------------------------------------------------- Diplomacy -----
//
// m_relations is keyed by isoA3 and the relation is stored on both sides, so
// each read resolves two ids to codes and looks at one direction.

const std::string* Game::modIsoFor(int cid) const {
    const Country* c = m_countries.getCountry(cid);
    return c ? &c->isoA3 : nullptr;
}

bool Game::modRelationFlag(int a, int b, int which) const {
    const std::string* ia = modIsoFor(a);
    const std::string* ib = modIsoFor(b);
    if (!ia || !ib || ia->empty() || ib->empty() || *ia == *ib) return false;
    auto row = m_relations.find(*ia);
    if (row == m_relations.end()) return false;
    auto cell = row->second.find(*ib);
    if (cell == row->second.end()) return false;
    switch (which) {
        case 0: return cell->second.war;
        case 1: return cell->second.alliance;
        case 2: return cell->second.nonAggression;
        case 3: return cell->second.guarantee;
    }
    return false;
}

bool Game::modAtWar(int a, int b) const          { return modRelationFlag(a, b, 0); }
bool Game::modAllied(int a, int b) const         { return modRelationFlag(a, b, 1); }
bool Game::modNonAggression(int a, int b) const  { return modRelationFlag(a, b, 2); }
bool Game::modGuaranteed(int a, int b) const     { return modRelationFlag(a, b, 3); }

bool Game::modProposeWar(int attacker, int defender) {
    const std::string* ia = modIsoFor(attacker);
    const std::string* ib = modIsoFor(defender);
    if (!ia || !ib || ia->empty() || ib->empty()) return false;
    if (*ia == *ib) return false;                 // no civil war by this route
    if (modAtWar(attacker, defender)) return false;   // already at war

    // declareWar owns the consequences -- guarantee chains, kin penalties, the
    // lot. Reimplementing any of that here would let a mod produce a state the
    // game itself could never reach.
    declareWar(*ia, *ib, true);
    return true;
}

// ----------------------------------------------------- GameState.Write ----
//
// The only capability that can change the world, so every entry point here
// validates first and refuses rather than half-applying. Values are clamped to
// a range the game's own economy stays inside; a mod handing us an infinity
// would otherwise poison every later calculation.

namespace {
// Treasuries are doubles but the economy is not meaningful past this, and
// letting NaN or infinity in would silently corrupt every downstream sum.
constexpr double kTreasuryLimit = 1e12;
// A province holding more than this is not a state the game's economy or
// rebellion maths stays sane in; refuse rather than store it.
constexpr long long kMaxProvincePopulation = 100000000000LL;   // 100 billion
bool finiteAndSane(double v) {
    return v == v && v > -kTreasuryLimit && v < kTreasuryLimit;
}
}  // namespace

bool Game::modSetCountryTreasury(int cid, double value) {
    if (!finiteAndSane(value)) return false;
    Country* c = m_countries.getCountry(cid);
    if (!c) return false;
    c->treasury = value;
    return true;
}

bool Game::modAddCountryTreasury(int cid, double delta) {
    if (!finiteAndSane(delta)) return false;
    Country* c = m_countries.getCountry(cid);
    if (!c) return false;
    double next = c->treasury + delta;
    if (!finiteAndSane(next)) return false;
    c->treasury = next;
    return true;
}

bool Game::modSetProvincePopulation(int pid, long long value) {
    // Population lives in TWO structures: m_provincePopulations (the map the
    // rest of the game reads) and m_provincePopArray (a dense mirror built at
    // load for the population texture and the hot loops in Game_Loading).
    // Writing one and not the other makes them disagree silently, and the
    // symptom surfaces turns later as a province whose population depends on
    // which code path asked. Both are updated here, together.
    if (value < 0) return false;
    if (value > kMaxProvincePopulation) return false;
    if (m_provincePopulations.find(pid) == m_provincePopulations.end())
        return false;                        // unknown province

    m_provincePopulations[pid] = value;
    if (pid >= 0 && (size_t)pid < m_provincePopArray.size())
        m_provincePopArray[pid] = value;
    return true;
}

bool Game::modSetProvinceOwner(int pid, int toCid) {
    const Province* pp = m_provinces.getProvinceById(pid);
    if (!pp) return false;
    if (!m_countries.getCountry(toCid)) return false;
    int fromCid = pp->countryId;
    if (fromCid == toCid) return false;          // nothing to do

    // The shared implementation, so a mod's transfer and a ceasefire's are the
    // same operation and cannot drift apart.
    transferProvinceOwnership(pid, fromCid, toCid);

    // Every existing caller also disbands the previous owner's troops left in
    // the province; a mod-driven transfer that skipped it would leave an army
    // sitting inside territory its country no longer owns.
    auto it = m_provinceArmies.find(pid);
    if (it != m_provinceArmies.end()) {
        it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
            [fromCid](const ArmyUnit& u) { return u.countryId == fromCid; }),
            it->second.end());
    }
    return true;
}

// -------------------------------------------------------------- Neural ----
//
// OBSERVE ONLY, deliberately. The capability is documented as being able to
// corrupt a trained model, and data/ai/model.bin represents hours of training,
// so v1 exposes no path that can write to the model, the optimiser state, or
// the reward history. A mod can see what the AI sees; it cannot change it.
//
// If an override is ever added it should be a separate capability, not a
// widening of this one, so that granting "let this mod read the AI" never
// silently becomes "let this mod retrain it".

int Game::modNeuralFeatureCount() const {
    return AISystem::FEATURE_COUNT;
}

int Game::modNeuralFeatures(int cid, float* out, int cap) const {
    if (!m_ai) return 0;
    if (!modCountryExists(cid)) return 0;
    std::vector<float> f;
    m_ai->modObserveFeatures(cid, f);
    int n = (int)f.size();
    if (out && cap > 0) {
        int take = n < cap ? n : cap;
        for (int i = 0; i < take; i++) out[i] = f[(size_t)i];
    }
    return n;                       // full length, per two-call sizing
}

int Game::modNeuralRewardCount() const {
    return m_ai ? (int)AISystem::MOD_COUNT : 0;
}

double Game::modNeuralRewardMean(int index) const {
    if (!m_ai || index < 0 || index >= (int)AISystem::MOD_COUNT) return 0.0;
    return (double)m_ai->rewardMeans()[index];
}

// ------------------------------------------------------------- setup ------

void Game::initModSystem() {
    if (!g_access) g_access = new GameModAccess(this);
    g_modGame = g_access;
    g_modHost.game = this;
    g_modHost.headless = m_aiTraining;
    g_modHost.screenW = (uint32_t)m_screenW;
    g_modHost.screenH = (uint32_t)m_screenH;

    // Headless training never opens the mod menu, which is the only load path,
    // so it never scans for mods either.
    if (m_aiTraining) return;
    ModManager::get().init(m_dataDir + "mods", m_dataDir + "mods.json");
}

void Game::clearModThumbnails() {
    for (auto& kv : m_modThumbs)
        if (kv.second.id) UnloadTexture(kv.second);
    m_modThumbs.clear();
}

// ------------------------------------------------------- mod panels -------

void Game::drawModPanels() {
    if (m_aiTraining) return;
    ModUI& ui = ModUI::get();
    if (ui.panels().empty()) return;

    g_modHost.screenW = (uint32_t)m_screenW;
    g_modHost.screenH = (uint32_t)m_screenH;

    // Panels live in a column down the right edge. The host owns the geometry;
    // the mod only ever sees its own width and height.
    const float panelW = 300.0f;
    float x = (float)m_screenW - panelW - 16.0f;
    float y = 110.0f;

    Vector2 mouse = getMouse();
    bool clicked = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    ui.clearCommands();
    for (auto& p : ui.panels()) {
        if (!p.visible) continue;
        float h = (float)std::max(60u, p.minH);
        if (y + h > (float)m_screenH - 16.0f) { p.visible = false; continue; }
        p.x = x; p.y = y; p.w = panelW; p.h = h;

        p.mouseInside = mouse.x >= x && mouse.x < x + panelW &&
                        mouse.y >= y && mouse.y < y + h;
        p.mouseX = mouse.x - x;
        p.mouseY = mouse.y - y;
        p.clickPending = p.mouseInside && clicked;

        y += h + 10.0f;
    }

    // Mods emit their draw commands here.
    ModManager::get().drawPanels();

    Color accent = hexToColor(m_config.accentColor);
    for (auto& p : ui.panels()) {
        if (!p.visible) continue;
        Rectangle r{p.x, p.y, p.w, p.h};
        DrawRectangleRounded(r, 0.06f, 8, Color{15, 15, 25, 230});
        DrawRectangleRoundedLines(r, 0.06f, 8, Color{60, 60, 90, 200});
        DrawText(p.title.c_str(), (int)p.x + 10, (int)p.y + 6, 14, accent);

        // Everything the mod drew is clipped to its own rectangle. Coordinates
        // are translated here, so a negative or huge coordinate from a mod
        // cannot land outside this box.
        int cy = (int)p.y + 26;
        BeginScissorMode((int)p.x + 2, cy, (int)p.w - 4, (int)(p.h - 28));
        for (const auto& c : p.cmds) {
            float cx = p.x + (float)c.x;
            float cyy = (float)cy + (float)c.y;
            if (c.kind == ModDrawCmd::Rect) {
                DrawRectangle((int)cx, (int)cyy, c.w, c.h, colorFromRGBA(c.rgba));
            } else if (c.kind == ModDrawCmd::Text) {
                drawHybridText((int)cx, (int)cyy, 14, c.text.c_str(), colorFromRGBA(c.rgba));
            } else {
                Rectangle b{cx, cyy, (float)c.w, (float)c.h};
                DrawRectangleRounded(b, 0.2f, 6,
                                     c.hovered ? Color{70, 80, 110, 255}
                                               : Color{40, 45, 65, 255});
                DrawRectangleRoundedLines(b, 0.2f, 6, Color{90, 95, 125, 255});
                int tw = MeasureText(c.text.c_str(), 13);
                drawHybridText((int)(cx + (c.w - tw) / 2), (int)(cyy + (c.h - 13) / 2),
                               13, c.text.c_str(), WHITE);
            }
        }
        EndScissorMode();
    }
}

// --------------------------------------------------------- mod menu ------

void Game::drawModsMenu() {
    drawMenuBackground(true);
    Color accent = hexToColor(m_config.accentColor);
    ModManager& mm = ModManager::get();
    auto& mods = mm.mods();
    Vector2 mouse = getMouse();

    const char* title = "Mods";
    DrawText(title, m_screenW / 2 - MeasureText(title, 48) / 2, 40, 48, accent);

    // Runtime line: says plainly when this build cannot run mods at all.
    {
        std::string line = std::string("Runtime: ") + ModRuntime::get().backendName();
        Color c = ModRuntime::get().available() ? Color{150, 150, 165, 255}
                                                : Color{220, 120, 100, 255};
        if (!ModRuntime::get().available())
            line += "  —  mods cannot run in this build";
        DrawText(line.c_str(), m_screenW / 2 - MeasureText(line.c_str(), 16) / 2, 96, 16, c);
    }

    const int startY = 130;
    const int itemH = 92;
    const int bottomBar = 70;
    int maxVisible = std::max(1, (m_screenH - startY - bottomBar) / itemH);
    int maxScroll = std::max(0, (int)mods.size() - maxVisible);
    m_modScroll = std::clamp(m_modScroll, 0, maxScroll);

    if (mods.empty()) {
        const char* none = "No mods installed";
        DrawText(none, m_screenW / 2 - MeasureText(none, 22) / 2, startY + 40, 22,
                 Color{140, 140, 150, 255});
        const char* hint = "Drag a .odmod file onto this window, or use Add from computer";
        DrawText(hint, m_screenW / 2 - MeasureText(hint, 15) / 2, startY + 76, 15,
                 Color{110, 110, 120, 255});
    }

    for (int i = 0; i < (int)mods.size(); ++i) {
        int y = startY + (i - m_modScroll) * itemH;
        if (y + itemH < 0 || y > m_screenH - bottomBar) continue;
        ModEntry& e = mods[i];

        bool selected = (i == m_modIndex);
        Rectangle row{40.0f, (float)y, (float)m_screenW - 80.0f, (float)itemH - 8};
        bool hovered = CheckCollisionPointRec(mouse, row);

        Color bg{255, 255, 255, 8};
        if (e.state == ModState::PendingReload) bg = ColorAlpha(accent, 0.16f);
        else if (e.state == ModState::Failed)   bg = Color{120, 40, 40, 60};
        else if (selected)                      bg = Color{60, 70, 100, 60};
        else if (hovered)                       bg = Color{255, 255, 255, 18};
        DrawRectangleRounded(row, 0.08f, 8, bg);
        if (e.state == ModState::PendingReload)
            DrawRectangleRoundedLines(row, 0.08f, 8, accent);

        // Thumbnail, decoded lazily and cached until the list is rescanned.
        float tx = row.x + 10, ty = row.y + 10;
        float thumb = (float)itemH - 28;
        if (!e.thumbnailPng.empty()) {
            auto it = m_modThumbs.find(e.id);
            if (it == m_modThumbs.end()) {
                Image img = LoadImageFromMemory(".png", e.thumbnailPng.data(),
                                                (int)e.thumbnailPng.size());
                Texture2D tex{};
                if (img.data) { tex = LoadTextureFromImage(img); UnloadImage(img); }
                it = m_modThumbs.emplace(e.id, tex).first;
            }
            if (it->second.id)
                DrawTexturePro(it->second,
                               Rectangle{0, 0, (float)it->second.width, (float)it->second.height},
                               Rectangle{tx, ty, thumb, thumb}, Vector2{0, 0}, 0.0f, WHITE);
            else
                DrawRectangleRounded({tx, ty, thumb, thumb}, 0.1f, 6, Color{40, 40, 55, 255});
        } else {
            DrawRectangleRounded({tx, ty, thumb, thumb}, 0.1f, 6, Color{40, 40, 55, 255});
        }

        float textX = tx + thumb + 14;
        std::string name = e.manifestValid ? e.manifest.name : e.fileName;
        drawHybridText((int)textX, (int)row.y + 10, 22, name.c_str(),
                       e.state == ModState::Failed ? Color{230, 150, 140, 255} : WHITE);

        std::string sub;
        if (e.manifestValid) {
            sub = "v" + e.manifest.version;
            if (!e.manifest.authors.empty()) sub += "  ·  " + e.manifest.authors[0];
            sub += "  ·  " + modModuleMaskToString(e.grants);
        } else {
            sub = e.fileName;
        }
        drawHybridText((int)textX, (int)row.y + 38, 13, sub.c_str(), Color{155, 155, 168, 255});

        std::string status = modStateName(e.state);
        if (e.state == ModState::PendingReload)
            status = "Needs reload to take effect";
        if (!e.diagnostic.empty()) status += " — " + e.diagnostic;
        else if (!e.warnings.empty()) status += " — " + e.warnings[0];
        Color sc = Color{130, 200, 140, 255};
        if (e.state == ModState::PendingReload) sc = accent;
        else if (e.state == ModState::Failed)   sc = Color{225, 110, 100, 255};
        else if (e.state == ModState::Disabled) sc = Color{130, 130, 140, 255};
        if (status.size() > 96) status.resize(96);
        drawHybridText((int)textX, (int)row.y + 58, 13, status.c_str(), sc);

        // Right-anchored controls, laid out right to left.
        const int btn = 30;
        int bx = (int)(row.x + row.width) - 12 - btn;
        int by = (int)row.y + (itemH - 8 - btn) / 2;
        auto drawBtn = [&](const char* glyph, Color tint) {
            Rectangle b{(float)bx, (float)by, (float)btn, (float)btn};
            bool bh = CheckCollisionPointRec(mouse, b);
            DrawRectangleRounded(b, 0.25f, 6, bh ? Color{70, 75, 100, 255}
                                                 : Color{35, 38, 55, 220});
            int gw = MeasureText(glyph, 15);
            DrawText(glyph, bx + (btn - gw) / 2, by + (btn - 15) / 2, 15, tint);
            bx -= btn + 8;
        };
        drawBtn("X", Color{225, 120, 110, 255});      // delete
        drawBtn("R", Color{200, 200, 210, 255});      // reload this mod
        drawBtn("A", Color{200, 200, 210, 255});      // advanced
        // Enable toggle
        {
            Rectangle b{(float)bx - 34, (float)by, 64.0f, (float)btn};
            bool bh = CheckCollisionPointRec(mouse, b);
            bool on = e.enabled;
            DrawRectangleRounded(b, 0.35f, 8,
                                 on ? ColorAlpha(accent, bh ? 0.9f : 0.7f)
                                    : (bh ? Color{70, 75, 100, 255} : Color{35, 38, 55, 220}));
            const char* t = on ? "ON" : "OFF";
            int tw = MeasureText(t, 14);
            DrawText(t, (int)b.x + (int)(b.width - tw) / 2, (int)b.y + 8, 14,
                     on ? Color{20, 20, 25, 255} : Color{190, 190, 200, 255});
        }
    }

    // Bottom bar
    int by = m_screenH - 52;
    auto bar = [&](const char* label, int x, int w, Color tint) {
        Rectangle b{(float)x, (float)by, (float)w, 36.0f};
        bool bh = CheckCollisionPointRec(mouse, b);
        DrawRectangleRounded(b, 0.25f, 8, bh ? Color{70, 75, 100, 255} : Color{30, 33, 48, 220});
        DrawRectangleRoundedLines(b, 0.25f, 8, Color{70, 70, 100, 180});
        int tw = MeasureText(label, 16);
        DrawText(label, x + (w - tw) / 2, by + 10, 16, tint);
    };
    bar("Add from computer", 40, 200, WHITE);
    bar("Reload modloader", 250, 190, WHITE);
    bar("Back", m_screenW - 140, 100, WHITE);

    if (m_modFeedbackTimer > 0.0f) {
        int tw = MeasureText(m_modFeedback.c_str(), 16);
        DrawText(m_modFeedback.c_str(), m_screenW / 2 - tw / 2, by - 28, 16,
                 ColorAlpha(accent, std::min(1.0f, m_modFeedbackTimer)));
    }

    if (m_modAdvancedFor >= 0) drawModAdvanced();
    if (m_modDeleteFor >= 0)   drawModDeleteConfirm();
    if (m_modAiWarnFor >= 0)   drawModAiWarning();
    if (m_modReloading)        drawModReloadingOverlay();
}

void Game::drawModAdvanced() {
    auto& mods = ModManager::get().mods();
    if (m_modAdvancedFor >= (int)mods.size()) { m_modAdvancedFor = -1; return; }
    ModEntry& e = mods[m_modAdvancedFor];
    Color accent = hexToColor(m_config.accentColor);
    Vector2 mouse = getMouse();

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 170});
    int w = 520, h = 460;
    int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.05f, 10,
                         Color{20, 20, 32, 245});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.05f, 10,
                              Color{70, 70, 100, 220});

    std::string t = "Permissions — " + (e.manifestValid ? e.manifest.name : e.fileName);
    drawHybridText(x + 24, y + 20, 22, t.c_str(), accent);
    DrawText("Only what the mod requested can be granted.", x + 24, y + 50, 13,
             Color{150, 150, 160, 255});

    static const uint32_t kBits[] = {
        MODULE_CORE, MODULE_GAMESTATE_READ, MODULE_GAMESTATE_WRITE, MODULE_GAMEPROCESS,
        MODULE_NEURAL, MODULE_UI, MODULE_MAP, MODULE_DIPLOMACY, MODULE_ASSETS, MODULE_STORAGE};

    int ry = y + 80;
    for (uint32_t bit : kBits) {
        bool requested = (e.manifest.modules & bit) != 0;
        bool granted = (e.grants & bit) != 0 || bit == MODULE_CORE;
        std::string label = modModuleMaskToString(bit);

        Color labelCol = requested ? WHITE : Color{95, 95, 105, 255};
        drawHybridText(x + 28, ry + 6, 16, label.c_str(), labelCol);

        Rectangle b{(float)(x + w - 110), (float)ry, 72.0f, 26.0f};
        bool locked = !requested || bit == MODULE_CORE;
        bool bh = !locked && CheckCollisionPointRec(mouse, b);
        const char* st = locked ? (requested ? "ALWAYS" : "—") : (granted ? "ON" : "OFF");
        Color bc = locked ? Color{40, 42, 55, 200}
                          : (granted ? ColorAlpha(accent, bh ? 0.9f : 0.7f)
                                     : (bh ? Color{70, 75, 100, 255} : Color{35, 38, 55, 220}));
        DrawRectangleRounded(b, 0.35f, 8, bc);
        int tw = MeasureText(st, 13);
        DrawText(st, (int)b.x + (int)(b.width - tw) / 2, (int)b.y + 7, 13,
                 (granted && !locked) ? Color{20, 20, 25, 255} : Color{180, 180, 190, 255});
        ry += 34;
    }

    const char* note = "Changing permissions requires a reload.";
    DrawText(note, x + 24, y + h - 66, 13, Color{150, 150, 160, 255});

    Rectangle close{(float)(x + w - 120), (float)(y + h - 46), 96.0f, 32.0f};
    bool ch = CheckCollisionPointRec(mouse, close);
    DrawRectangleRounded(close, 0.25f, 8, ch ? Color{70, 75, 100, 255} : Color{35, 38, 55, 220});
    DrawText("Close", (int)close.x + 24, (int)close.y + 8, 16, WHITE);
}

void Game::drawModDeleteConfirm() {
    auto& mods = ModManager::get().mods();
    if (m_modDeleteFor >= (int)mods.size()) { m_modDeleteFor = -1; return; }
    ModEntry& e = mods[m_modDeleteFor];
    Vector2 mouse = getMouse();

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 170});
    int w = 460, h = 180;
    int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.06f, 10,
                         Color{20, 20, 32, 245});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.06f, 10,
                              Color{70, 70, 100, 220});
    DrawText("Delete this mod?", x + 24, y + 22, 22, WHITE);
    drawHybridText(x + 24, y + 58, 15,
                   (e.manifestValid ? e.manifest.name : e.fileName).c_str(),
                   Color{190, 190, 200, 255});
    DrawText("The .odmod file is removed from disk.", x + 24, y + 82, 13,
             Color{150, 150, 160, 255});

    Rectangle del{(float)(x + w - 220), (float)(y + h - 52), 96.0f, 34.0f};
    Rectangle can{(float)(x + w - 114), (float)(y + h - 52), 96.0f, 34.0f};
    DrawRectangleRounded(del, 0.25f, 8,
                         CheckCollisionPointRec(mouse, del) ? Color{200, 60, 60, 255}
                                                            : Color{150, 45, 45, 230});
    DrawRectangleRounded(can, 0.25f, 8,
                         CheckCollisionPointRec(mouse, can) ? Color{70, 75, 100, 255}
                                                            : Color{35, 38, 55, 220});
    DrawText("Delete", (int)del.x + 20, (int)del.y + 9, 16, WHITE);
    DrawText("Cancel", (int)can.x + 20, (int)can.y + 9, 16, WHITE);
}

void Game::drawModAiWarning() {
    Vector2 mouse = getMouse();
    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 180});
    int w = 560, h = 230;
    int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.06f, 10,
                         Color{26, 20, 20, 248});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.06f, 10,
                              Color{170, 90, 80, 220});

    DrawText("AI Learning is on", x + 24, y + 22, 24, Color{240, 170, 150, 255});
    DrawText("Mods can change what the AI sees and how turns resolve.", x + 24, y + 62, 15,
             Color{205, 205, 215, 255});
    DrawText("Training with a mod loaded silently corrupts the model in", x + 24, y + 84, 15,
             Color{205, 205, 215, 255});
    std::string f = "data/ai/model.bin — and the damage is invisible until it is done.";
    drawHybridText(x + 24, y + 106, 15, f.c_str(), Color{205, 205, 215, 255});

    Rectangle off{(float)(x + 24), (float)(y + h - 58), 250.0f, 36.0f};
    Rectangle can{(float)(x + w - 130), (float)(y + h - 58), 106.0f, 36.0f};
    DrawRectangleRounded(off, 0.25f, 8,
                         CheckCollisionPointRec(mouse, off) ? Color{70, 90, 70, 255}
                                                            : Color{40, 60, 45, 230});
    DrawRectangleRounded(can, 0.25f, 8,
                         CheckCollisionPointRec(mouse, can) ? Color{70, 75, 100, 255}
                                                            : Color{35, 38, 55, 220});
    DrawText("Turn off AI Learning and enable", (int)off.x + 14, (int)off.y + 10, 15, WHITE);
    DrawText("Cancel", (int)can.x + 24, (int)can.y + 10, 16, WHITE);
}

void Game::drawModReloadingOverlay() {
    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 200});
    const char* t = "Reloading mods...";
    DrawText(t, m_screenW / 2 - MeasureText(t, 28) / 2, m_screenH / 2 - 14, 28,
             hexToColor(m_config.accentColor));
}

// ------------------------------------------------------- mod menu input ---

void Game::updateModsMenu() {
    ModManager& mm = ModManager::get();
    // A world being loaded is what makes activation deferred: joining a game
    // already in progress would make turn order depend on when the user
    // happened to click.
    mm.setInGame(m_renderer != nullptr && m_provinces.getImage().data != nullptr);

    // mod_load runs from this screen, and it is the first thing a mod calls
    // gearbox_env from, so the reported screen size has to be current here --
    // initModSystem runs before the window size is known.
    g_modHost.screenW = (uint32_t)m_screenW;
    g_modHost.screenH = (uint32_t)m_screenH;

    auto& mods = mm.mods();
    Vector2 mouse = getMouse();
    bool click = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    if (m_modFeedbackTimer > 0.0f) m_modFeedbackTimer -= GetFrameTime();

    // A reload draws one frame of its overlay before doing the work, so the
    // user sees the loading screen rather than a frozen frame.
    if (m_modReloading) {
        if (m_modReloadFrames++ > 0) {
            mm.reloadAll();
            clearModThumbnails();
            m_modReloading = false;
            m_modFeedback = "Modloader reloaded";
            m_modFeedbackTimer = 2.0f;
        }
        return;
    }

    // Drag-and-drop is the portable import path, and the only one on web.
    if (IsFileDropped()) {
        FilePathList dropped = LoadDroppedFiles();
        for (unsigned i = 0; i < dropped.count; ++i) {
            std::string p = dropped.paths[i];
            if (p.size() < 7 || p.compare(p.size() - 6, 6, ".odmod") != 0) continue;
            std::string err;
            if (mm.importFile(p, err)) {
                clearModThumbnails();
                m_modFeedback = "Added " + p.substr(p.find_last_of('/') + 1);
            } else {
                m_modFeedback = "Rejected: " + err;
            }
            m_modFeedbackTimer = 4.0f;
        }
        UnloadDroppedFiles(dropped);
    }

    // --- modal overlays swallow everything beneath them -----------------------
    if (m_modAiWarnFor >= 0) {
        int w = 560, h = 230;
        int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
        Rectangle off{(float)(x + 24), (float)(y + h - 58), 250.0f, 36.0f};
        Rectangle can{(float)(x + w - 130), (float)(y + h - 58), 106.0f, 36.0f};
        if (click && CheckCollisionPointRec(mouse, off)) {
            m_config.aiLearning = false;
            m_config.save(m_configPath);
            mm.setEnabled((size_t)m_modAiWarnFor, true);
            m_modAiWarnFor = -1;
        } else if ((click && CheckCollisionPointRec(mouse, can)) || IsKeyPressed(KEY_ESCAPE)) {
            m_modAiWarnFor = -1;
        }
        return;
    }

    if (m_modDeleteFor >= 0) {
        int w = 460, h = 180;
        int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
        Rectangle del{(float)(x + w - 220), (float)(y + h - 52), 96.0f, 34.0f};
        Rectangle can{(float)(x + w - 114), (float)(y + h - 52), 96.0f, 34.0f};
        if (click && CheckCollisionPointRec(mouse, del)) {
            std::string err;
            if (!mm.removeMod((size_t)m_modDeleteFor, err)) {
                m_modFeedback = err;
                m_modFeedbackTimer = 3.0f;
            } else {
                clearModThumbnails();
            }
            m_modDeleteFor = -1;
            m_modIndex = std::clamp(m_modIndex, 0, std::max(0, (int)mods.size() - 1));
        } else if ((click && CheckCollisionPointRec(mouse, can)) || IsKeyPressed(KEY_ESCAPE)) {
            m_modDeleteFor = -1;
        }
        return;
    }

    if (m_modAdvancedFor >= 0) {
        if (m_modAdvancedFor >= (int)mods.size()) { m_modAdvancedFor = -1; return; }
        ModEntry& e = mods[m_modAdvancedFor];
        int w = 520, h = 460;
        int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;

        static const uint32_t kBits[] = {
            MODULE_CORE, MODULE_GAMESTATE_READ, MODULE_GAMESTATE_WRITE, MODULE_GAMEPROCESS,
            MODULE_NEURAL, MODULE_UI, MODULE_MAP, MODULE_DIPLOMACY, MODULE_ASSETS,
            MODULE_STORAGE};
        int ry = y + 80;
        for (uint32_t bit : kBits) {
            Rectangle b{(float)(x + w - 110), (float)ry, 72.0f, 26.0f};
            bool locked = !(e.manifest.modules & bit) || bit == MODULE_CORE;
            if (!locked && click && CheckCollisionPointRec(mouse, b))
                mm.setGrant((size_t)m_modAdvancedFor, bit, !(e.grants & bit));
            ry += 34;
        }
        Rectangle close{(float)(x + w - 120), (float)(y + h - 46), 96.0f, 32.0f};
        if ((click && CheckCollisionPointRec(mouse, close)) || IsKeyPressed(KEY_ESCAPE))
            m_modAdvancedFor = -1;
        return;
    }

    // --- list navigation ------------------------------------------------------
    const int startY = 130, itemH = 92, bottomBar = 70;
    int maxVisible = std::max(1, (m_screenH - startY - bottomBar) / itemH);
    int maxScroll = std::max(0, (int)mods.size() - maxVisible);

    if (IsKeyPressed(KEY_ESCAPE)) { m_currentScreen = SCREEN_MENU; return; }
    if (IsKeyPressed(KEY_DOWN) && !mods.empty())
        m_modIndex = std::min(m_modIndex + 1, (int)mods.size() - 1);
    if (IsKeyPressed(KEY_UP)) m_modIndex = std::max(m_modIndex - 1, 0);
    if (m_modIndex < m_modScroll) m_modScroll = m_modIndex;
    if (m_modIndex >= m_modScroll + maxVisible) m_modScroll = m_modIndex - maxVisible + 1;
    m_modScroll = std::clamp(m_modScroll - (int)GetMouseWheelMove(), 0, maxScroll);

    // --- bottom bar -----------------------------------------------------------
    int by = m_screenH - 52;
    Rectangle addB{40.0f, (float)by, 200.0f, 36.0f};
    Rectangle relB{250.0f, (float)by, 190.0f, 36.0f};
    Rectangle backB{(float)(m_screenW - 140), (float)by, 100.0f, 36.0f};

    if (click && CheckCollisionPointRec(mouse, backB)) { m_currentScreen = SCREEN_MENU; return; }
    if (click && CheckCollisionPointRec(mouse, relB)) {
        m_modReloading = true;
        m_modReloadFrames = 0;
        return;
    }
    if (click && CheckCollisionPointRec(mouse, addB)) {
        std::string p = pickOdmodFile();
        if (p.empty()) {
            m_modFeedback = "Drag a .odmod file onto the window to add it";
            m_modFeedbackTimer = 3.0f;
        } else {
            std::string err;
            if (mm.importFile(p, err)) {
                clearModThumbnails();
                m_modFeedback = "Added " + p.substr(p.find_last_of('/') + 1);
            } else {
                m_modFeedback = "Rejected: " + err;
            }
            m_modFeedbackTimer = 4.0f;
        }
        return;
    }

    // --- rows -----------------------------------------------------------------
    for (int i = 0; i < (int)mods.size(); ++i) {
        int y = startY + (i - m_modScroll) * itemH;
        if (y + itemH < 0 || y > m_screenH - bottomBar) continue;
        Rectangle row{40.0f, (float)y, (float)m_screenW - 80.0f, (float)itemH - 8};

        const int btn = 30;
        int bx = (int)(row.x + row.width) - 12 - btn;
        int bY = (int)row.y + (itemH - 8 - btn) / 2;
        Rectangle delB{(float)bx, (float)bY, (float)btn, (float)btn};
        Rectangle rldB{(float)(bx - btn - 8), (float)bY, (float)btn, (float)btn};
        Rectangle advB{(float)(bx - 2 * (btn + 8)), (float)bY, (float)btn, (float)btn};
        Rectangle togB{(float)(bx - 3 * (btn + 8) - 34), (float)bY, 64.0f, (float)btn};

        if (!click) continue;
        if (CheckCollisionPointRec(mouse, delB))  { m_modDeleteFor = i; return; }
        if (CheckCollisionPointRec(mouse, advB))  { m_modAdvancedFor = i; return; }
        if (CheckCollisionPointRec(mouse, rldB))  {
            mm.reloadOne((size_t)i);
            clearModThumbnails();
            m_modFeedback = "Reloaded";
            m_modFeedbackTimer = 2.0f;
            return;
        }
        if (CheckCollisionPointRec(mouse, togB)) {
            ModEntry& e = mods[i];
            // The interlock: never let a mod and AI learning both be live.
            if (!e.enabled && m_config.aiLearning) { m_modAiWarnFor = i; return; }
            mm.setEnabled((size_t)i, !e.enabled);
            if (mods[i].state == ModState::PendingReload) {
                m_modFeedback = "Enabled — reload to apply while a game is running";
                m_modFeedbackTimer = 4.0f;
            }
            return;
        }
        if (CheckCollisionPointRec(mouse, row)) { m_modIndex = i; return; }
    }
}
