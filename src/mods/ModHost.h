#pragma once

// Host side of the Gearbox capability modules: the functions a mod can import.
//
// Every function here is reachable only if the mod declared the matching module
// in MANIFEST.json and the user granted it (see ModRuntime::instantiate, which
// refuses a mod importing anything ungranted). Adding a function to the table
// in ModHost.cpp is therefore a security decision, not a convenience one.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Game;

// What the Core `env` import reports. Set by ModManager; the mod sees a
// snapshot taken when it asks, not a live pointer.
struct ModHostContext {
    Game*    game = nullptr;
    bool     headless = false;      // --train-ai: no renderer, UI must no-op
    uint32_t screenW = 0;
    uint32_t screenH = 0;
};

extern ModHostContext g_modHost;

// How the GameState.Read capability reaches the world.
//
// An interface rather than a direct Game dependency, so this layer stays free
// of raylib and the game headers -- which keeps the host functions testable
// against a fake, and keeps a mod's view of the world an explicit, auditable
// list rather than "whatever Game happens to expose".
//
// Handles are country and province ids. They are documented to modders as
// opaque and valid only for the duration of a hook.
struct ModGameAccess {
    virtual ~ModGameAccess() = default;
    virtual uint32_t turnNumber() = 0;
    virtual uint32_t countryCount() = 0;
    virtual uint32_t countryAt(uint32_t index) = 0;   // GEARBOX_INVALID if none
    virtual bool     countryExists(uint32_t cid) = 0;
    virtual std::string countryName(uint32_t cid) = 0;
    virtual double   countryTreasury(uint32_t cid) = 0;
    virtual uint32_t countryProvinceCount(uint32_t cid) = 0;
    virtual long long provincePopulation(uint32_t pid) = 0;
    virtual uint32_t provinceOwner(uint32_t pid) = 0;

    // Map capability. Geometry only; every one of these is a lookup into data
    // the game already computed at load.
    virtual uint32_t mapWidth() = 0;
    virtual uint32_t mapHeight() = 0;
    virtual uint32_t provinceCount() = 0;
    virtual uint32_t provinceAt(uint32_t index) = 0;   // GEARBOX_INVALID if none
    virtual bool     provinceExists(uint32_t pid) = 0;
    virtual std::string provinceName(uint32_t pid) = 0;
    virtual double   provinceCenterX(uint32_t pid) = 0;
    virtual double   provinceCenterY(uint32_t pid) = 0;
    virtual bool     provinceIsLand(uint32_t pid) = 0;
    virtual uint32_t provinceNeighborCount(uint32_t pid) = 0;
    virtual uint32_t provinceNeighborAt(uint32_t pid, uint32_t index) = 0;

    // Diplomacy capability. Reads are cheap flag lookups; proposeWar is the one
    // mutating call and the game may refuse it.
    virtual bool atWar(uint32_t a, uint32_t b) = 0;
    virtual bool allied(uint32_t a, uint32_t b) = 0;
    virtual bool nonAggression(uint32_t a, uint32_t b) = 0;
    virtual bool guaranteed(uint32_t a, uint32_t b) = 0;
    virtual bool proposeWar(uint32_t attacker, uint32_t defender) = 0;

    // GameState.Write. Each validates and returns false rather than trapping.
    virtual bool setCountryTreasury(uint32_t cid, double value) = 0;
    virtual bool addCountryTreasury(uint32_t cid, double delta) = 0;
    virtual bool setProvinceOwner(uint32_t pid, uint32_t cid) = 0;
    virtual bool setProvincePopulation(uint32_t pid, long long value) = 0;

    // Neural. Observe only -- there is no write path here by design.
    virtual uint32_t neuralFeatureCount() = 0;
    virtual uint32_t neuralFeatures(uint32_t cid, float* out, uint32_t cap) = 0;
    virtual uint32_t neuralRewardCount() = 0;
    virtual double   neuralRewardMean(uint32_t index) = 0;
};

// Null whenever no world is loaded. Every GameState.Read native must cope with
// that: a mod's panel can be open on the main menu.
extern ModGameAccess* g_modGame;

// A line a mod wrote via gearbox_log, kept for the mod menu's log view.
struct ModLogLine {
    std::string modId;
    std::string text;
    int level = 1;
};

const std::vector<ModLogLine>& modHostLog();
void modHostLogClear();

// ------------------------------------------------------------------- UI ----
//
// A mod never touches the renderer. It appends commands to a per-panel list and
// the game draws them, clipped to a rectangle the host chose. That keeps this
// file free of raylib (so it is testable headlessly), and means "a mod cannot
// draw outside its panel" is enforced by the host owning the transform rather
// than by trusting coordinates.

struct ModDrawCmd {
    enum Kind : uint8_t { Rect, Text, Button } kind = Rect;
    int32_t  x = 0, y = 0, w = 0, h = 0;   // panel-relative
    uint32_t rgba = 0xFFFFFFFFu;
    bool     hovered = false;              // Button only
    std::string text;                      // Text/Button only
};

struct ModPanel {
    uint32_t    id = 0;
    std::string ownerId;                   // manifest id of the owning mod
    std::string title;
    uint32_t    minW = 0, minH = 0;
    bool        visible = true;

    // Screen rect, assigned by the game before it calls the mod's draw hook.
    float x = 0, y = 0, w = 0, h = 0;

    // Input for this frame, in panel-relative coordinates. mouseInside is false
    // when the cursor is elsewhere, so a mod cannot detect the pointer outside
    // its own panel.
    bool  mouseInside = false;
    float mouseX = 0, mouseY = 0;
    bool  clickPending = false;            // consumed by the first button hit

    std::vector<ModDrawCmd> cmds;
};

// Persistent key-value storage, namespaced per mod id.
//
// One file per mod under <modsDir>/storage/, named from a sanitised mod id, so
// a mod can only ever see its own keys -- the namespacing is the filesystem
// layout, not a prefix a bug could escape. Values are arbitrary bytes.
//
// Held in memory and flushed at safe points (turn boundaries, unload,
// shutdown) rather than written through on every set: a mod may call set from
// a draw hook, which runs every frame.
//
// This is the first capability that lets a mod consume disk, so it is quota'd.
// The limits are host-derived and not declarable in the manifest -- a mod
// author should not be choosing how much of the player's disk to take.
class ModStorage {
public:
    static ModStorage& get();

    // 64 KiB per mod is generous for the settings and small state this is for,
    // and far too small to be interesting as a place to hide a payload.
    static constexpr size_t kMaxKeys        = 256;
    static constexpr size_t kMaxKeyBytes    = 128;
    static constexpr size_t kMaxValueBytes  = 16 * 1024;
    static constexpr size_t kMaxTotalBytes  = 64 * 1024;

    void setDir(const std::string& dir);         // <modsDir>/storage/

    // Absent and empty are different: a zero-length value is a real value.
    bool get(const std::string& modId, const std::string& key,
             std::string& out);
    // False when a quota would be exceeded; `err` says which.
    bool set(const std::string& modId, const std::string& key,
             const std::string& value, std::string& err);
    bool remove(const std::string& modId, const std::string& key);

    void flush();                                // writes dirty mods only
    void forget(const std::string& modId);       // drop cache, keep the file
    void clear();                                // tests

private:
    struct Store {
        std::map<std::string, std::string> kv;
        size_t bytes = 0;
        bool loaded = false;
        bool dirty  = false;
    };
    Store& storeFor(const std::string& modId);
    std::string pathFor(const std::string& modId) const;

    std::string m_dir;
    std::map<std::string, Store> m_stores;
};

class ModUI {
public:
    static ModUI& get();

    // Returns 0 (an invalid handle) if the mod is at its panel limit.
    uint32_t registerPanel(const std::string& ownerId, const std::string& title,
                           uint32_t minW, uint32_t minH);
    ModPanel* find(uint32_t id);
    std::vector<ModPanel>& panels() { return m_panels; }

    void clearCommands();                       // start of a frame
    void removePanelsOf(const std::string& modId);
    void clear();

    static constexpr size_t kMaxPanelsPerMod = 8;
    static constexpr size_t kMaxCmdsPerPanel = 4096;

private:
    std::vector<ModPanel> m_panels;
    uint32_t m_nextId = 1;
};
