#include "ModManager.h"
#include "ModHost.h"
#include "../net/ModAttest.h"

#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

#ifndef _WIN32
#include <dirent.h>
#else
#include <windows.h>
#endif

using json = nlohmann::json;

const char* modStateName(ModState s) {
    switch (s) {
        case ModState::Disabled:      return "Disabled";
        case ModState::Active:        return "Active";
        case ModState::PendingReload: return "Needs reload";
        case ModState::Failed:        return "Failed";
    }
    return "?";
}

ModManager& ModManager::get() {
    static ModManager m;
    return m;
}

namespace {

long long fileMTime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (long long)st.st_mtime;
}

bool endsWith(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() > n && s.compare(s.size() - n, n, suf) == 0;
}

void ensureDir(const std::string& d) {
#ifdef _WIN32
    CreateDirectoryA(d.c_str(), nullptr);
#else
    mkdir(d.c_str(), 0755);
#endif
}

bool copyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return out.good();
}

}  // namespace

void ModManager::init(const std::string& modsDir, const std::string& stateFile) {
    m_modsDir = modsDir;
    m_stateFile = stateFile;
    if (!m_modsDir.empty() && m_modsDir.back() != '/') m_modsDir += '/';
    ensureDir(m_modsDir);

    // Per-mod key-value stores live beside the mods themselves. Created here so
    // ModStorage never has to make a directory from inside a mod's call.
    const std::string storageDir = m_modsDir + "storage/";
    ensureDir(storageDir);
    ModStorage::get().setDir(storageDir);
    m_inited = true;
    rescan();
}

// -------------------------------------------------------------- scanning ---

void ModManager::rescan() {
    if (!m_inited) return;

    // Remember what the user had decided, keyed by mod id, so a rescan never
    // silently re-grants or disables anything.
    struct Prev { bool enabled; uint32_t grants; ModState state; };
    std::vector<std::pair<std::string, Prev>> prev;
    for (auto& e : m_mods)
        prev.push_back({e.id, {e.enabled, e.grants, e.state}});

    std::vector<std::string> files;
#ifndef _WIN32
    if (DIR* d = opendir(m_modsDir.c_str())) {
        while (dirent* de = readdir(d)) {
            std::string n = de->d_name;
            if (endsWith(n, ".odmod")) files.push_back(n);
        }
        closedir(d);
    }
#else
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((m_modsDir + "*.odmod").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { files.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif
    std::sort(files.begin(), files.end());

    // Keep the live instances of mods whose file has not changed: a rescan
    // should not restart a running mod.
    std::vector<ModEntry> kept;
    kept.swap(m_mods);

    for (const std::string& fn : files) {
        std::string full = m_modsDir + fn;
        long long mtime = fileMTime(full);

        ModEntry e;
        e.path = full;
        e.fileName = fn;
        e.fileMTime = mtime;

        auto old = std::find_if(kept.begin(), kept.end(),
                                [&](const ModEntry& k) { return k.path == full; });

        auto pkg = std::make_unique<ModPackage>();
        ModLoadResult r = pkg->open(full);
        if (r != ModLoadResult::Ok) {
            e.id = fn;
            e.state = ModState::Failed;
            e.diagnostic = pkg->diagnostic();
            e.warnings = pkg->warnings();
            m_mods.push_back(std::move(e));
            continue;
        }

        e.manifestValid = true;
        e.manifest = pkg->manifest();
        e.id = e.manifest.id;

        // The id is the identity: it keys the persisted enable/grant state, the
        // pinned signing key, and the Storage namespace. Two files claiming the
        // same one would double-instantiate a mod and have them share all
        // three, so the second is refused by name rather than silently winning.
        bool dup = false;
        for (const auto& seen : m_mods) {
            if (!seen.manifestValid || seen.id != e.id) continue;
            e.state = ModState::Failed;
            e.diagnostic = "duplicate mod id, already provided by " + seen.fileName;
            e.manifestValid = false;
            dup = true;
            break;
        }
        if (dup) { m_mods.push_back(std::move(e)); continue; }
        e.warnings = pkg->warnings();
        e.thumbnailPng = pkg->thumbnail();
        e.grants = e.manifest.modules;      // default: everything it asked for
        e.package = std::move(pkg);

        // Carry over the user's decisions.
        for (const auto& p : prev) {
            if (p.first != e.id) continue;
            e.enabled = p.second.enabled;
            // Never widen a grant on rescan; a new version asking for more
            // capabilities gets only what it also asks for and had before.
            e.grants = p.second.grants & e.manifest.modules;
            break;
        }

        if (old != kept.end() && old->instance && old->fileMTime == mtime) {
            e.instance = std::move(old->instance);
            e.package = std::move(old->package);   // the instance points into it
            e.state = ModState::Active;
        } else if (old != kept.end() && old->instance && old->fileMTime != mtime) {
            // Replaced on disk while running: the old code keeps running until
            // the user reloads, which is exactly what PendingReload means.
            e.instance = std::move(old->instance);
            e.package = std::move(old->package);
            e.state = ModState::PendingReload;
            e.diagnostic = "the file changed on disk";
        } else if (e.enabled) {
            e.state = m_inGame ? ModState::PendingReload : ModState::Disabled;
            if (m_inGame) e.diagnostic = "enabled while a game is running";
        }

        m_mods.push_back(std::move(e));
    }

    load();      // re-apply persisted enable/grant state for newly seen mods
}

std::vector<ModAttestEntry> ModManager::attestation() const {
    std::vector<ModAttestEntry> out;
    for (const auto& e : m_mods) {
        // Only what is actually running. A mod sitting on disk disabled is not
        // part of this game and claiming it would be a lie in the honest
        // direction, which is still a lie.
        if (e.state != ModState::Active || !e.package || !e.manifestValid) continue;
        out.push_back(ModAttestEntry{
            e.manifest.id, e.manifest.version, e.package->sha256(), e.manifest.side,
        });
    }
    std::sort(out.begin(), out.end(),
              [](const ModAttestEntry& a, const ModAttestEntry& b) { return a.id < b.id; });
    return out;
}

// ------------------------------------------------------------- lifecycle ---

void ModManager::activate(ModEntry& e) {
    if (!e.package || !e.manifestValid) {
        fail(e, e.diagnostic.empty() ? "the archive could not be read" : e.diagnostic);
        return;
    }
    // Refuse before instantiating, not after: a mod whose dependency is missing
    // should never get as far as running mod_load, or it will fail somewhere
    // deep inside itself with a message that blames the wrong thing.
    if (std::string why = unmetDependency(e); !why.empty()) {
        fail(e, why);
        return;
    }
    if (std::string why = blockingConflict(e); !why.empty()) {
        // Recoverable by a user decision, so the message says so. The override
        // is offered in the mod menu against this entry.
        fail(e, why + " — enable anyway from this mod's entry if you accept it");
        return;
    }

    // A server-side mod has no business running on a client. Not a failure --
    // the player did nothing wrong and there is nothing to fix -- so it stays
    // Disabled with a diagnostic that explains rather than accuses.
    if (m_multiplayer && !m_authoritative && e.manifest.side == ModSide::Server) {
        e.state = ModState::Disabled;
        e.diagnostic = "server-side mod; it runs on the host, not here";
        return;
    }

    std::string err;
    // Core is never revocable, so it is always in the effective grant set.
    uint32_t grants = (e.grants & e.manifest.modules) | MODULE_CORE;

    // A client-side mod cannot write game state or run turn hooks while a
    // server owns the world. Applied here, as a mask over the same word the
    // Advanced panel edits, so there is exactly one definition of what a mod
    // may touch rather than a second check somewhere downstream.
    grants &= modSideGrantMask(e.manifest.side, m_multiplayer);

    e.instance = ModRuntime::get().instantiate(*e.package, grants, err);
    if (!e.instance) { fail(e, err); return; }

    uint32_t ret = 0;
    if (!e.instance->callExport("mod_load", nullptr, 0, &ret, err)) {
        e.instance.reset();
        fail(e, err);
        return;
    }
    if (ret != 0) {
        e.instance.reset();
        fail(e, "mod_load refused the load with code " + std::to_string(ret));
        return;
    }
    e.state = ModState::Active;
    e.diagnostic.clear();
}

// -------------------------------------------- dependencies and conflicts ---

namespace {

// Semver comparison, numeric per component. A prerelease suffix is ignored
// rather than ordered: getting -rc ordering subtly wrong is worse than not
// claiming to support it, and no mod manifest here uses one.
int cmpSemver(const std::string& a, const std::string& b) {
    auto part = [](const std::string& s, int idx) -> long {
        size_t start = 0;
        for (int i = 0; i < idx; i++) {
            size_t d = s.find('.', start);
            if (d == std::string::npos) return 0;
            start = d + 1;
        }
        return strtol(s.c_str() + start, nullptr, 10);
    };
    std::string ca = a.substr(0, a.find_first_of("-+"));
    std::string cb = b.substr(0, b.find_first_of("-+"));
    for (int i = 0; i < 3; i++) {
        long x = part(ca, i), y = part(cb, i);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// A range is a space-separated list of comparators, all of which must hold:
//   ">=2.0.0 <3.0.0"      "1.4.2"      ">=1.0.0"
// An empty range accepts anything, which is what a dependency with no version
// means.
bool versionSatisfies(const std::string& have, const std::string& range) {
    if (range.empty()) return true;
    size_t i = 0;
    while (i < range.size()) {
        while (i < range.size() && range[i] == ' ') i++;
        if (i >= range.size()) break;
        size_t j = range.find(' ', i);
        std::string term = range.substr(i, j == std::string::npos ? j : j - i);
        i = (j == std::string::npos) ? range.size() : j + 1;
        if (term.empty()) continue;

        std::string op = "==", ver = term;
        for (const char* cand : {">=", "<=", "==", ">", "<", "="}) {
            size_t n = strlen(cand);
            if (term.size() > n && term.compare(0, n, cand) == 0) {
                op = cand; ver = term.substr(n); break;
            }
        }
        int c = cmpSemver(have, ver);
        bool ok = (op == ">=") ? c >= 0 : (op == "<=") ? c <= 0
                : (op == ">")  ? c >  0 : (op == "<")  ? c <  0
                : c == 0;
        if (!ok) return false;
    }
    return true;
}

}  // namespace

std::string ModManager::unmetDependency(const ModEntry& e) const {
    for (const auto& dep : e.manifest.dependencies) {
        const ModEntry* found = nullptr;
        for (const auto& other : m_mods)
            if (other.manifestValid && other.id == dep.id) { found = &other; break; }

        if (!found) {
            if (dep.optional) continue;
            return "needs " + dep.id +
                   (dep.version.empty() ? "" : " " + dep.version) +
                   ", which is not installed";
        }
        if (!versionSatisfies(found->manifest.version, dep.version)) {
            if (dep.optional) continue;
            return "needs " + dep.id + " " + dep.version + ", but " +
                   found->manifest.version + " is installed";
        }
        // Present but switched off is its own message: "not installed" would
        // send the user looking for a download they already have.
        if (!found->enabled) {
            if (dep.optional) continue;
            return "needs " + dep.id + ", which is installed but not enabled";
        }
    }
    return "";
}

bool ModManager::bridged(const std::string& a, const std::string& b) const {
    for (const auto& m : m_mods) {
        if (!m.enabled || !m.manifestValid) continue;
        for (const auto& br : m.manifest.bridges) {
            if ((br.a == a && br.b == b) || (br.a == b && br.b == a)) return true;
        }
    }
    return false;
}

std::string ModManager::blockingConflict(const ModEntry& e) const {
    for (const auto& other : m_mods) {
        if (!other.enabled || !other.manifestValid || other.id == e.id) continue;

        // Declared in either direction -- a conflict is mutual even when only
        // one author knew about it.
        const ModConflict* decl = nullptr;
        for (const auto& c : e.manifest.conflicts)
            if (c.id == other.id) { decl = &c; break; }
        if (!decl)
            for (const auto& c : other.manifest.conflicts)
                if (c.id == e.id) { decl = &c; break; }
        if (!decl) continue;

        // A bridge mod exists precisely to make this pair work; so does an
        // explicit override on either side.
        if (bridged(e.id, other.id)) continue;
        if (e.overridesConflictWith(other.id)) continue;
        if (other.overridesConflictWith(e.id)) continue;

        return "conflicts with " + other.manifest.name + ": " + decl->reason;
    }
    return "";
}

void ModManager::setConflictOverride(size_t index, const std::string& otherId,
                                     bool on) {
    if (index >= m_mods.size()) return;
    auto& v = m_mods[index].conflictOverrides;
    auto it = std::find(v.begin(), v.end(), otherId);
    if (on && it == v.end())      v.push_back(otherId);
    else if (!on && it != v.end()) v.erase(it);
    save();
}

void ModManager::deactivate(ModEntry& e) {
    if (e.instance) {
        std::string err;
        if (e.instance->hasExport("mod_unload"))
            e.instance->callExport("mod_unload", nullptr, 0, nullptr, err);
        ModUI::get().removePanelsOf(e.id);
        e.instance.reset();
    }
    if (e.state == ModState::Active) e.state = ModState::Disabled;
}

void ModManager::fail(ModEntry& e, const std::string& why) {
    ModUI::get().removePanelsOf(e.id);
    e.instance.reset();
    e.state = ModState::Failed;
    e.diagnostic = why;
    printf("[MODS] %s failed: %s\n", e.id.c_str(), why.c_str());
}

void ModManager::reloadAll() {
    m_reloadQueued = false;
    for (auto& e : m_mods) deactivate(e);
    ModUI::get().clear();

    for (auto& e : m_mods) {
        if (!e.enabled) {
            if (e.state != ModState::Failed) e.state = ModState::Disabled;
            continue;
        }
        activate(e);
    }
}

void ModManager::reloadOne(size_t index) {
    if (index >= m_mods.size()) return;
    ModEntry& e = m_mods[index];
    deactivate(e);
    if (!e.enabled) { e.state = ModState::Disabled; return; }

    // Re-read from disk: reloading a single mod is how a modder picks up a
    // rebuild, so the file is expected to have changed.
    auto pkg = std::make_unique<ModPackage>();
    if (pkg->open(e.path) != ModLoadResult::Ok) {
        e.warnings = pkg->warnings();
        fail(e, pkg->diagnostic());
        return;
    }
    e.manifest = pkg->manifest();
    e.manifestValid = true;
    e.warnings = pkg->warnings();
    e.thumbnailPng = pkg->thumbnail();
    e.grants &= e.manifest.modules;
    e.fileMTime = fileMTime(e.path);
    e.package = std::move(pkg);
    activate(e);
}

void ModManager::serviceQueuedReload() {
    if (!m_reloadQueued) return;
    reloadAll();
}

void ModManager::unloadAll() {
    for (auto& e : m_mods) deactivate(e);
    ModUI::get().clear();
}

// ----------------------------------------------------------------- state ---

void ModManager::setEnabled(size_t index, bool on) {
    if (index >= m_mods.size()) return;
    ModEntry& e = m_mods[index];
    if (e.enabled == on) return;
    e.enabled = on;

    if (!on) {
        deactivate(e);
        e.state = ModState::Disabled;
        e.diagnostic.clear();
    } else if (m_inGame) {
        // Never join a game already in progress: turn order and determinism
        // would depend on when the user happened to click.
        e.state = ModState::PendingReload;
        e.diagnostic = "enabled while a game is running";
    } else {
        activate(e);
    }
    save();
}

void ModManager::setGrant(size_t index, uint32_t moduleBit, bool on) {
    if (index >= m_mods.size()) return;
    ModEntry& e = m_mods[index];
    if (moduleBit == MODULE_CORE) return;            // not revocable
    if (!(e.manifest.modules & moduleBit)) return;   // never grant what it did not ask for

    uint32_t before = e.grants;
    if (on) e.grants |= moduleBit;
    else    e.grants &= ~moduleBit;

    // Write implies Read: revoking Read must revoke Write too, or the mod would
    // instantiate holding a capability the user thought they had taken away.
    if (!on && moduleBit == MODULE_GAMESTATE_READ) e.grants &= ~MODULE_GAMESTATE_WRITE;
    if (on && moduleBit == MODULE_GAMESTATE_WRITE) e.grants |= MODULE_GAMESTATE_READ;

    if (e.grants == before) return;

    if (e.state == ModState::Active) {
        e.state = ModState::PendingReload;
        e.diagnostic = "permissions changed";
    }
    save();
}

bool ModManager::anyEnabled() const {
    for (const auto& e : m_mods) if (e.enabled) return true;
    return false;
}

bool ModManager::anyActive() const {
    for (const auto& e : m_mods) if (e.state == ModState::Active) return true;
    return false;
}

// ------------------------------------------------------------ import/rm ----

bool ModManager::importFile(const std::string& srcPath, std::string& err) {
    if (!m_inited) { err = "mod system is not initialised"; return false; }

    // Validate before copying: a broken archive should be reported, not stored.
    ModPackage probe;
    ModLoadResult r = probe.open(srcPath);
    if (r != ModLoadResult::Ok) { err = probe.diagnostic(); return false; }

    std::string base = probe.manifest().id + ".odmod";
    std::string dest = m_modsDir + base;

    if (!copyFile(srcPath, dest)) {
        err = "could not copy the mod into " + m_modsDir;
        return false;
    }
    rescan();
    return true;
}

bool ModManager::removeMod(size_t index, std::string& err) {
    if (index >= m_mods.size()) { err = "no such mod"; return false; }
    ModEntry& e = m_mods[index];
    deactivate(e);
    e.package.reset();

    if (::remove(e.path.c_str()) != 0) {
        err = "could not delete " + e.fileName;
        return false;
    }
    m_mods.erase(m_mods.begin() + (long)index);
    save();
    return true;
}

// ------------------------------------------------------------ turn hooks ---

void ModManager::preTurn(int turn) {
    // A turn boundary is where a mod's state is coherent, so it is where the
    // key-value stores are written. Cheap when nothing is dirty.
    ModStorage::get().flush();
    // Conflicts are judged within a turn: two mods writing the same thing on
    // alternating turns are taking turns, not fighting.
    ModConflicts::get().beginTurn(turn);
    if (m_mods.empty()) return;
    uint32_t args[1] = {(uint32_t)turn};
    for (auto& e : m_mods) {
        if (e.state != ModState::Active || !e.instance) continue;
        if (!(e.instance->granted() & MODULE_GAMEPROCESS)) continue;
        if (!e.instance->hasExport("mod_pre_turn")) continue;
        std::string err;
        if (!e.instance->callExport("mod_pre_turn", args, 1, nullptr, err))
            fail(e, err);
    }
}

void ModManager::postTurn(int turn) {
    if (m_mods.empty()) return;
    uint32_t args[1] = {(uint32_t)turn};
    for (auto& e : m_mods) {
        if (e.state != ModState::Active || !e.instance) continue;
        if (!(e.instance->granted() & MODULE_GAMEPROCESS)) continue;
        if (!e.instance->hasExport("mod_post_turn")) continue;
        std::string err;
        if (!e.instance->callExport("mod_post_turn", args, 1, nullptr, err))
            fail(e, err);
    }
    // A reload asked for mid-turn happens here, between turns, never inside one.
    serviceQueuedReload();
}

void ModManager::drawPanels() {
    if (g_modHost.headless) return;

    // Snapshot the handles first. A mod that traps mid-draw is failed, and
    // failing it removes its panels -- which would invalidate an iterator over
    // the very vector we are walking.
    std::vector<uint32_t> ids;
    for (auto& p : ModUI::get().panels())
        if (p.visible) ids.push_back(p.id);

    for (uint32_t id : ids) {
        ModPanel* p = ModUI::get().find(id);
        if (!p) continue;                 // removed by an earlier failure

        ModEntry* owner = nullptr;
        for (auto& e : m_mods)
            if (e.id == p->ownerId && e.state == ModState::Active) { owner = &e; break; }
        if (!owner || !owner->instance) continue;
        if (!owner->instance->hasExport("mod_draw_panel")) continue;

        uint32_t args[3] = {p->id, (uint32_t)p->w, (uint32_t)p->h};
        std::string err;
        if (!owner->instance->callExport("mod_draw_panel", args, 3, nullptr, err))
            fail(*owner, err);
    }
}

// ----------------------------------------------------------- persistence ---

void ModManager::save() const {
#ifndef __EMSCRIPTEN__
    if (m_stateFile.empty()) return;
    json j;
    j["schema"] = 1;
    json arr = json::array();
    for (const auto& e : m_mods) {
        if (!e.manifestValid) continue;
        json m;
        m["id"] = e.id;
        m["enabled"] = e.enabled;
        m["grants"] = e.grants;
        // "Run these two anyway" is a user decision like enabling is, so it
        // survives a restart the same way.
        if (!e.conflictOverrides.empty())
            m["conflictOverrides"] = e.conflictOverrides;
        arr.push_back(std::move(m));
    }
    j["mods"] = std::move(arr);

    std::ofstream f(m_stateFile, std::ios::trunc);
    if (f) f << j.dump(2) << "\n";
#endif
}

void ModManager::load() {
#ifndef __EMSCRIPTEN__
    if (m_stateFile.empty()) return;
    std::ifstream f(m_stateFile);
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    auto it = j.find("mods");
    if (it == j.end() || !it->is_array()) return;

    for (const auto& m : *it) {
        if (!m.is_object()) continue;
        auto idIt = m.find("id");
        if (idIt == m.end() || !idIt->is_string()) continue;
        std::string id = idIt->get<std::string>();

        for (auto& e : m_mods) {
            if (e.id != id || !e.manifestValid) continue;
            auto en = m.find("enabled");
            if (en != m.end() && en->is_boolean()) e.enabled = en->get<bool>();
            auto gr = m.find("grants");
            if (gr != m.end() && gr->is_number_unsigned())
                // Clamp to what the manifest actually requests: a stale or
                // hand-edited file must not grant a capability the mod did not
                // ask for and the user never saw.
                e.grants = gr->get<uint32_t>() & e.manifest.modules;
            auto co = m.find("conflictOverrides");
            e.conflictOverrides.clear();
            if (co != m.end() && co->is_array())
                for (const auto& o : *co)
                    if (o.is_string()) e.conflictOverrides.push_back(o.get<std::string>());
            break;
        }
    }
#endif
}
