#pragma once

// Owns the installed mod list, their states, and their lifetimes.
//
// The one rule everything else follows: a mod is instantiated only by an
// explicit reload, and reload is only ever triggered from the mod menu. There
// is no path from a save, a map, a CLI flag, or another mod to a running
// instance -- see docs/modding.md, "Lifecycle rules".

#include "ModPackage.h"
#include "ModRuntime.h"
#include "../net/ModAttest.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class ModState {
    Disabled,        // present, not running
    Active,          // instantiated, hooks live
    PendingReload,   // enabled or replaced while a game was running
    Failed,          // refused: bad archive, link error, limit, or a trap
};

const char* modStateName(ModState s);

struct ModEntry {
    std::string path;            // absolute path to the .odmod
    std::string fileName;
    std::string id;              // manifest id, or the filename if unreadable
    ModManifest manifest;
    bool        manifestValid = false;

    ModState    state = ModState::Disabled;
    bool        enabled = false; // the user's intent, persisted
    uint32_t    grants = 0;      // granted capabilities, persisted

    std::string diagnostic;      // why it failed, or why it needs a reload
    std::vector<std::string> warnings;

    // Conflicts the user has chosen to live with, as the OTHER mod's id. Kept
    // per-mod rather than as a global pair list so the user can override on one
    // side, the other, or both -- overriding on either side is enough to let the
    // pair run, which is what "make these two work anyway" means in practice.
    std::vector<std::string> conflictOverrides;
    bool overridesConflictWith(const std::string& otherId) const {
        for (const auto& o : conflictOverrides) if (o == otherId) return true;
        return false;
    }
    std::vector<uint8_t> thumbnailPng;   // raw bytes; the renderer decodes it

    long long   fileMTime = 0;   // to notice the file being replaced on disk

    std::unique_ptr<ModPackage>  package;
    std::unique_ptr<ModInstance> instance;
};

class ModManager {
public:
    static ModManager& get();

    // `modsDir` is created if missing. Safe to call more than once.
    void init(const std::string& modsDir, const std::string& stateFile);
    const std::string& modsDir() const { return m_modsDir; }

    // Re-reads the directory, preserving enabled/grant state by mod id and
    // marking a running mod PendingReload if its file changed underneath.
    void rescan();

    // Copies an external .odmod into the mods directory. Validates it first, so
    // a broken archive is reported before anything is written.
    bool importFile(const std::string& srcPath, std::string& err);
    bool removeMod(size_t index, std::string& err);

    void setEnabled(size_t index, bool on);

    // Dependency and conflict resolution. Both answer for the CURRENT set of
    // installed and enabled mods, so neither can live in ModPackage, which only
    // ever sees one archive.
    //
    // Returns empty when the mod may run; otherwise a sentence naming what is
    // missing or what it clashes with.
    std::string unmetDependency(const ModEntry& e) const;
    std::string blockingConflict(const ModEntry& e) const;

    // True when some enabled mod declares it bridges these two.
    bool bridged(const std::string& a, const std::string& b) const;

    // Toggle "run these two anyway" on one side of a pair.
    void setConflictOverride(size_t index, const std::string& otherId, bool on);
    void setGrant(size_t index, uint32_t moduleBit, bool on);

    // Tears down every instance and starts the enabled set again. This is the
    // only thing that ever creates an instance.
    void reloadAll();
    void reloadOne(size_t index);

    // The game sets this so the manager knows whether activation must be
    // deferred. Enabling a mod mid-game yields PendingReload, never Active.
    void setInGame(bool v) { m_inGame = v; }
    bool inGame() const { return m_inGame; }

    // Which side of a multiplayer game this process is, which decides what a
    // mod's declared `side` means for it. Must be set BEFORE reloadAll(),
    // because grants are fixed at instantiation: a mod cannot have a
    // capability taken away underneath it, so joining a game is a reload.
    //
    // `authoritative` is true on a host (dedicated or playing) and false on a
    // client. Outside multiplayer both are ignored.
    void setNetContext(bool multiplayer, bool authoritative) {
        m_multiplayer = multiplayer;
        m_authoritative = authoritative;
    }
    bool multiplayer() const { return m_multiplayer; }
    bool authoritative() const { return m_authoritative; }

    // What this install offers the other end: every enabled mod that loaded,
    // with its digest. See src/net/ModAttest.h for what this does and does not
    // prove.
    std::vector<ModAttestEntry> attestation() const;

    // A reload asked for during turn processing is queued until the turn ends.
    void requestReload() { m_reloadQueued = true; }
    bool reloadQueued() const { return m_reloadQueued; }
    void serviceQueuedReload();

    // --- the AI-learning interlock -------------------------------------------
    // Mods and in-game AI learning are never both live; see docs/modding.md.
    bool anyEnabled() const;
    bool anyActive() const;

    // --- turn hooks -----------------------------------------------------------
    void preTurn(int turn);
    void postTurn(int turn);

    // --- per-frame UI ---------------------------------------------------------
    // Calls mod_draw_panel on every visible panel. The caller has already put
    // each panel's rect and input state into ModUI.
    void drawPanels();

    std::vector<ModEntry>& mods() { return m_mods; }
    const std::vector<ModEntry>& mods() const { return m_mods; }

    void save() const;
    void load();

    // Shuts every mod down. Called at exit and before a modloader reload.
    void unloadAll();

private:
    ModManager() = default;
    void activate(ModEntry& e);
    void deactivate(ModEntry& e);
    void fail(ModEntry& e, const std::string& why);

    std::vector<ModEntry> m_mods;
    std::string m_modsDir;
    std::string m_stateFile;
    bool m_inGame = false;
    bool m_reloadQueued = false;
    bool m_inited = false;
    bool m_multiplayer = false;
    bool m_authoritative = true;
};
