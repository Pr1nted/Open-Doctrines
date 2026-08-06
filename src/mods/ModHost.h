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


#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * How the game lends its session to mods, without lending them the wire.
 *
 * Installed when a network game exists and cleared when it ends. Every hook
 * being null is the ordinary singleplayer state, and the Net imports answer
 * "not a network game" -- which is a documented answer, not a failure.
 *
 * Note what is NOT here: no way to read or write orders, deltas, tickets or
 * chat. A mod message is its own thing, stamped with the sending mod's id by
 * the host, and it can never be mistaken for a turn.
 */
/**
 * What the UI module needs from the game that the mod host cannot do itself.
 *
 * The host deliberately does not link raylib -- the mod tests build without a
 * window, and that is worth keeping -- so measuring a string and reading or
 * setting the theme colour have to be lent in, exactly as the net and audio
 * bridges are.
 */
struct ModUiBridge {
    /** Width in pixels of `text` at `size`, in the font the game will draw. */
    std::function<uint32_t(const std::string&, int)> measureText;
    std::function<uint32_t()>     themeAccent;      // 0x00RRGGBB
    std::function<void(uint32_t)> setThemeAccent;
};

struct ModNetBridge {
    /** (modId, peer or -1 for everyone, payload) -> was it sent */
    std::function<bool(const std::string&, int32_t, const std::vector<uint8_t>&)> send;
    /** (modId, out payload, out sender) -> was there one */
    std::function<bool(const std::string&, std::vector<uint8_t>&, int32_t&)> recv;
    std::function<uint32_t()> peerCount;
    std::function<uint32_t()> selfPeer;
    std::function<bool()>     isHost;
};

/**
 * How the game lends its speakers to mods.
 *
 * The mod host does not link raylib -- the mod tests build it on its own, and
 * that is worth keeping -- so it reads the bytes out of the mod's package and
 * hands them over. Everything about decoding and mixing stays in the game.
 */
struct ModAudioBridge {
    /** (modId, extension, bytes, volume 0..1) -> handle, or 0 */
    std::function<uint32_t(const std::string&, const std::string&,
                           const std::vector<uint8_t>&, float)> play;
    std::function<void(const std::string&, uint32_t)>        stop;
    std::function<void(const std::string&, uint32_t, float)> setVolume;
    std::function<bool(const std::string&, uint32_t)>        isPlaying;
    /** Everything this mod started, because it is going away. */
    std::function<void(const std::string&)>                  stopAll;
};

/**
 * Silence a mod that is being unloaded.
 *
 * Called from the same place panels are removed: a mod that stops must not
 * leave a sound running with nobody able to stop it.
 */
void modReleaseAudio(const std::string& modId);

void modSetAudioBridge(const ModAudioBridge& bridge);

/** Hand the mod host a session, or take it away with a default-constructed one. */
void modSetNetBridge(const ModNetBridge& bridge);
void modSetUiBridge(const ModUiBridge& bridge);

class Game;

// Which side of a multiplayer game this process is, from a mod's point of
// view. Reported to mods through gearbox_env_t.net_role, and the thing a mod
// checks when it needs to know whether it is looking at the authoritative
// state or at a copy the server sent.
//
// Standalone is 0 deliberately: it is what an older mod, reading a byte it was
// told was reserved, already saw -- and singleplayer is the correct answer for
// one of those.
enum class ModNetRole : uint8_t {
    Standalone = 0,   // singleplayer; this process is both sides
    Client     = 1,   // a server elsewhere is authoritative
    Server     = 2,   // dedicated host: authoritative, nobody plays here
    HostPlayer = 3,   // authoritative AND playing
};

// What the Core `env` import reports. Set by ModManager; the mod sees a
// snapshot taken when it asks, not a live pointer.
struct ModHostContext {
    Game*      game = nullptr;
    bool       headless = false;      // --train-ai: no renderer, UI must no-op
    uint32_t   screenW = 0;
    uint32_t   screenH = 0;
    ModNetRole netRole = ModNetRole::Standalone;
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

    // ── Military.Read ────────────────────────────────────────────────────────
    //
    // Handles are INDICES INTO A SNAPSHOT taken at the start of the call, not
    // pointers. A ship sunk between two calls makes shipExists() false rather
    // than reading freed memory, which is the whole reason a mod cannot be
    // handed a NavyShip*.
    virtual uint32_t shipCount() = 0;
    virtual uint32_t shipAt(uint32_t index) = 0;              // GEARBOX_INVALID if none
    virtual bool     shipExists(uint32_t sid) = 0;
    virtual uint32_t shipOwner(uint32_t sid) = 0;
    virtual std::string shipType(uint32_t sid) = 0;           // "boat", "destroyer", ...
    virtual double   shipLon(uint32_t sid) = 0;
    virtual double   shipLat(uint32_t sid) = 0;
    virtual int32_t  shipHealth(uint32_t sid) = 0;            // 0-100
    virtual int32_t  shipCrew(uint32_t sid) = 0;              // troops aboard; 0 for warships
    virtual double   shipRange(uint32_t sid) = 0;             // this turn, in map pixels
    // Armies are per province and per owner: a besieged province holds stacks
    // belonging to more than one country.
    virtual uint32_t armyStackCount(uint32_t pid) = 0;
    virtual uint32_t armyStackOwner(uint32_t pid, uint32_t index) = 0;
    virtual long long armyStackSize(uint32_t pid, uint32_t index) = 0;
    virtual long long countryArmy(uint32_t cid) = 0;
    virtual int32_t  provinceFortification(uint32_t pid) = 0;
    virtual int32_t  provincePortLevel(uint32_t pid) = 0;

    // ── Military.Write ───────────────────────────────────────────────────────
    //
    // EVERY ONE OF THESE GOES THROUGH THE SAME RESOLVER THE PLAYER AND THE AI
    // USE. The adjacency test, the range clamp, the at-war check and the
    // percentage bounds live in Game_TurnLogic precisely because that is where
    // all three order sources converge; a mod path that wrote orders directly
    // would reintroduce the trust hole the multiplayer host had. These queue an
    // order and return false if it would be refused, so a mod cannot do
    // anything a player sitting at the same keyboard could not.
    virtual bool orderArmyMove(uint32_t fromPid, uint32_t toPid, uint32_t pct) = 0;
    virtual bool orderShipMove(uint32_t sid, double lon, double lat) = 0;
    virtual bool orderShipEngage(uint32_t sid, uint32_t targetSid) = 0;
    virtual bool orderShipBombard(uint32_t sid, uint32_t pid, const std::string& ammo) = 0;

    // ── Research.Read / Write ────────────────────────────────────────────────
    virtual uint32_t researchNodeCount() = 0;
    virtual std::string researchNodeId(uint32_t index) = 0;
    virtual std::string researchNodeName(uint32_t index) = 0;
    virtual std::string researchNodeCategory(uint32_t index) = 0;
    virtual int32_t  researchNodeCost(uint32_t index) = 0;
    virtual bool     countryHasResearched(uint32_t cid, const std::string& nodeId) = 0;
    virtual double   countryResearchFunding(uint32_t cid) = 0;
    virtual bool     setCountryResearchFunding(uint32_t cid, double value) = 0;

    // ── Politics.Read / Write ────────────────────────────────────────────────
    virtual double   countryCompassEcon(uint32_t cid) = 0;    // left(-) .. right(+)
    virtual double   countryCompassSocial(uint32_t cid) = 0;  // lib(-) .. auth(+)
    virtual double   provinceUnrest(uint32_t pid) = 0;
    virtual uint32_t policyCount() = 0;
    virtual std::string policyId(uint32_t index) = 0;
    virtual std::string policyName(uint32_t index) = 0;
    virtual bool     countryHasPolicy(uint32_t cid, const std::string& policyId) = 0;
    virtual bool     setCountryPolicy(uint32_t cid, const std::string& policyId, bool on) = 0;
    virtual uint32_t provinceMinorityCount(uint32_t pid) = 0;
    virtual std::string provinceMinorityName(uint32_t pid, uint32_t index) = 0;
    virtual double   provinceMinorityShare(uint32_t pid, uint32_t index) = 0;

    // ── Economy.Read / Write ─────────────────────────────────────────────────
    virtual double   countryIncomeGross(uint32_t cid) = 0;
    virtual double   countryIncomeNet(uint32_t cid) = 0;
    virtual double   countryArmyUpkeep(uint32_t cid) = 0;
    virtual double   countryNavyUpkeep(uint32_t cid) = 0;
    virtual bool     countryIsBankrupt(uint32_t cid) = 0;
    virtual int32_t  provinceIndustryLevel(uint32_t pid) = 0;
    virtual std::string provinceIndustrySpecialization(uint32_t pid) = 0;
    virtual double   provinceResource(uint32_t pid, const std::string& which) = 0;
    virtual bool     setProvinceIndustryLevel(uint32_t pid, int32_t level) = 0;

    // ── Map, beyond the geometry the 1.0 module already exposes ──────────────
    virtual bool     provinceIsCoastal(uint32_t pid) = 0;
    // The sea-route query the navy itself uses. Answers "could a fleet get from
    // here to there at all", which is the question a naval mod actually has and
    // cannot compute from neighbours because those are LAND adjacency.
    virtual bool     seaRouteExists(double lon1, double lat1, double lon2, double lat2) = 0;
    virtual bool     pointIsLand(double lon, double lat) = 0;

    // ── mapeditor (ABI 1.1) ──────────────────────────────────────────────────
    // editorActive() is the gate: every other call here returns a neutral value
    // when the map editor is not the screen the player is on, so a mod cannot
    // reach into an editor project from inside a running game, or into a game
    // from inside the editor.
    virtual bool     editorActive() = 0;
    virtual uint32_t editorProvinceCount() = 0;
    virtual uint32_t editorProvinceAt(uint32_t index) = 0;
    virtual long long editorProvincePopulation(uint32_t pid) = 0;
    virtual int32_t  editorProvinceIndustryLevel(uint32_t pid) = 0;
    virtual int32_t  editorProvinceFortification(uint32_t pid) = 0;
    virtual int32_t  editorProvincePortLevel(uint32_t pid) = 0;
    virtual double   editorProvinceResource(uint32_t pid, const std::string& which) = 0;
    virtual double   editorProvinceCompassEcon(uint32_t pid) = 0;
    virtual double   editorProvinceCompassSocial(uint32_t pid) = 0;
    virtual bool     editorSetProvincePopulation(uint32_t pid, long long v) = 0;
    virtual bool     editorSetProvinceIndustryLevel(uint32_t pid, int32_t v) = 0;
    virtual bool     editorSetProvinceFortification(uint32_t pid, int32_t v) = 0;
    virtual bool     editorSetProvincePortLevel(uint32_t pid, int32_t v) = 0;
    virtual bool     editorSetProvinceResource(uint32_t pid, const std::string& which, double v) = 0;
    virtual bool     editorSetProvinceCompass(uint32_t pid, double econ, double social) = 0;
    virtual std::string editorMapName() = 0;
    virtual bool     editorSetMapName(const std::string& n) = 0;
    virtual bool     editorSetAuthor(const std::string& a) = 0;
    virtual bool     editorSetLicense(const std::string& l) = 0;

    // ── net (ABI 1.1) ────────────────────────────────────────────────────────
    // Alongside the send/recv the 1.0 module already has. Deliberately NOT
    // "am I in multiplayer" or "am I the authority": gearbox_is_multiplayer and
    // gearbox_is_server already answer both from the Core env, and a second way
    // to ask the same question is worse than none. Those two were broken --
    // ModHostContext::netRole was never assigned, so they always said
    // standalone -- and the fix was to set the field, not to route around it.
    virtual uint32_t netPeerAt(uint32_t index) = 0;
    virtual std::string netPeerName(uint32_t index) = 0;
    virtual uint32_t netMaxMessageBytes() = 0;

    // ── neural (ABI 1.1): the decision space, so a mod can name what it sees ──
    virtual uint32_t neuralModuleCount() = 0;
    virtual std::string neuralModuleName(uint32_t m) = 0;
    virtual uint32_t neuralActionCount(uint32_t m) = 0;
    virtual std::string neuralActionName(uint32_t m, uint32_t a) = 0;
    virtual bool     neuralCountryIsAI(uint32_t cid) = 0;
    virtual long long neuralUpdateCount() = 0;
    virtual bool     neuralModelLoaded() = 0;

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
    // Line, Circle and Image were added in ABI 1.1. A total conversion that can
    // only draw rectangles and 14pt text is not a reskin, it is a debug overlay;
    // Image in particular is what lets a mod put its OWN art on screen, which is
    // the whole point of the reskin surface.
    enum Kind : uint8_t { Rect, Text, Button, Line, Circle, Image } kind = Rect;
    int32_t  x = 0, y = 0, w = 0, h = 0;   // panel-relative
    int32_t  x2 = 0, y2 = 0;               // Line only: the far end
    float    thickness = 1.0f;             // Line only
    float    radius = 0.0f;                // Circle only
    int32_t  fontSize = 14;                // Text only
    uint32_t rgba = 0xFFFFFFFFu;           // Image: a tint, 0xFFFFFFFF for none
    bool     hovered = false;              // Button only
    std::string text;                      // Text/Button; Image: the asset name
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

// Conflict detection, by observation rather than declaration.
//
// WHY THIS IS NOT A MANIFEST FIELD
//
// A modder cannot list every mod they will ever be incompatible with -- they do
// not know what will be written next year. And declaring the same capability is
// not a conflict: two mods that both hold GameState.Write but touch different
// countries never collide, while two that both adjust one country's treasury
// do. Capability overlap is a bad proxy for the thing we actually care about.
//
// What we actually care about is observable. Every mutation a mod makes goes
// through a host native, so the host sees all of them. Record what each mod
// wrote and a conflict is a fact rather than a guess: two mods set the SAME
// target to DIFFERENT values within one turn.
//
// Two mods writing the same value is not reported. They agree; nothing is
// fighting, and telling the user otherwise would train them to ignore this.
class ModConflicts {
public:
    static ModConflicts& get();

    // Called from the write natives. `target` identifies the thing written --
    // "country:11:treasury", "province:3:owner" -- and `value` is its stringified
    // new value, which is what makes "both wrote it, differently" decidable.
    void recordWrite(const std::string& modId, const std::string& target,
                     const std::string& value);

    struct Clash {
        std::string modA, modB;
        std::string target;
        std::string valueA, valueB;
        int         turn = 0;
        uint32_t    seen = 1;      // how many turns this pair has clashed here
    };

    // Everything observed so far, newest first. The UI shows these against the
    // mods involved; nothing is suppressed here, because suppression is a
    // decision about a PAIR (an override or a bridge) and belongs to the
    // manager, which knows what else is installed.
    const std::vector<Clash>& clashes() const { return m_clashes; }
    bool anyFor(const std::string& modId) const;

    void beginTurn(int turn);      // clears the per-turn write log
    void forget(const std::string& modId);
    void clear();

private:
    int m_turn = 0;
    // target -> (mod -> its LATEST value this turn).
    //
    // Latest, not every write: a mod that sets a treasury to 500 and then
    // corrects it to 600 has only ever meant 600. Comparing a second mod
    // against the superseded 500 reported agreement as a conflict.
    std::map<std::string, std::map<std::string, std::string>> m_thisTurn;
    std::vector<Clash> m_clashes;

    static constexpr size_t kMaxClashes = 256;
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
