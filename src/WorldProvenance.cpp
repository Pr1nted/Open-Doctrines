#include "WorldProvenance.h"

#include <algorithm>
#include <unordered_map>

namespace odprov {

Mismatch compare(const Provenance& saved,
                 const std::vector<ModRecord>& running,
                 const std::string& runningGameVersion) {
    Mismatch out;

    // A save written before this record existed says nothing about what it
    // was made with, and the honest answer is to report nothing rather than
    // to report that every running mod is "new". Absence of evidence.
    if (!saved.present) return out;

    if (!saved.gameVersion.empty() && saved.gameVersion != runningGameVersion)
        out.savedGameVersion = saved.gameVersion;

    std::unordered_map<std::string, const ModRecord*> live;
    for (const ModRecord& m : running) live[m.id] = &m;

    for (const ModRecord& was : saved.mods) {
        auto it = live.find(was.id);
        if (it == live.end()) {
            // The distinction the mod itself chose. Persisted content is in
            // the save with nothing left to interpret it; hollow content was
            // never written down, so its absence costs only the mod.
            (was.persisted ? out.missingPersisted : out.missingHollow)
                .push_back(was.id);
            continue;
        }
        if (!was.version.empty() && !it->second->version.empty() &&
            was.version != it->second->version)
            out.versionChanged.push_back(was.id + ": " + was.version +
                                         " -> " + it->second->version);
    }

    // Sorted so the same world always reports the same way. An unordered_map
    // walk would order these by hash, which differs between builds -- and a
    // warning that lists three mods in a different order each load reads as
    // three different warnings.
    std::sort(out.missingPersisted.begin(), out.missingPersisted.end());
    std::sort(out.missingHollow.begin(), out.missingHollow.end());
    std::sort(out.versionChanged.begin(), out.versionChanged.end());

    // A mod that is running and was NOT in the save is deliberately not
    // reported. Adding a mod to a world is the ordinary way to use one, and
    // warning about it would make the warning mean nothing.
    return out;
}

namespace {

std::string list(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += (i + 1 == v.size()) ? " and " : ", ";
        s += v[i];
    }
    return s;
}

}  // namespace

std::vector<std::string> describe(const Mismatch& m) {
    std::vector<std::string> out;

    // WORST FIRST. A player who reads one line should read the one that can
    // cost them their world.
    if (!m.missingPersisted.empty()) {
        out.push_back("This world was built with " + list(m.missingPersisted) +
                      ", which added things that are saved inside it. Without "
                      "them those things cannot be read back, and playing on "
                      "will lose them permanently.");
    }
    if (!m.missingHollow.empty()) {
        out.push_back("It was also last played with " + list(m.missingHollow) +
                      ", which is not installed. Nothing in the save depends "
                      "on it, so the world will load normally -- it just will "
                      "not do what that mod did.");
    }
    if (!m.versionChanged.empty()) {
        out.push_back("A different version is installed now: " +
                      list(m.versionChanged) + ".");
    }
    if (!m.savedGameVersion.empty()) {
        out.push_back("It was last loaded by version " + m.savedGameVersion + ".");
    }
    return out;
}

}  // namespace odprov
