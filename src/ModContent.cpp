#include "ModContent.h"

#include <algorithm>
#include <unordered_set>

namespace odcontent {
namespace {

constexpr size_t kMaxId = 64;
/// A definition is JSON a mod wrote. Bounded so a save cannot be grown without
/// limit by a mod that keeps adding larger ones.
constexpr size_t kMaxJson = 16 * 1024;

const char* const kNames[] = {
    "doctrine", "research", "troop", "artillery", "district_law",
};

}  // namespace

const char* kindName(Kind k) {
    const size_t i = (size_t)k;
    return i < (size_t)Kind::Count_ ? kNames[i] : "?";
}

Kind kindFromName(const std::string& name) {
    for (size_t i = 0; i < (size_t)Kind::Count_; ++i)
        if (name == kNames[i]) return (Kind)i;
    return Kind::Count_;
}

bool validContentId(const std::string& id) {
    if (id.empty() || id.size() > kMaxId) return false;
    int colons = 0;
    for (unsigned char c : id) {
        if (c == ':') { ++colons; continue; }
        const bool okChar = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!okChar) return false;
    }
    // One colon at most, so "com.example:thing" style namespacing works and
    // "a:b:c" does not -- an id with two separators has no single obvious
    // split, and something downstream would eventually pick the wrong one.
    if (colons > 1) return false;
    // Not leading or trailing: ":x" and "x:" are an author's typo, and both
    // would compare equal to nothing in the data file.
    return id.front() != ':' && id.back() != ':';
}

bool Registry::add(Kind kind, const std::string& modId, const std::string& id,
                   const std::string& json, Mode mode, bool aiVisible) {
    if (modId.empty() || kind >= Kind::Count_) return false;
    if (!validContentId(id)) return false;
    if (json.empty() || json.size() > kMaxJson) return false;

    for (Entry& e : m_entries) {
        if (e.kind != kind || e.id != id) continue;
        // IDS ARE GLOBAL WITHIN A CATALOGUE, unlike country fields. A country
        // holds a doctrine BY ID and a save records it that way, so two
        // meanings for one id would make a save ambiguous -- and which meaning
        // applied would depend on load order.
        if (e.modId != modId) return false;
        e.json = json;
        e.mode = mode;
        e.aiVisible = aiVisible;
        e.ownerPresent = true;      // declaring it proves the owner is back
        return true;
    }

    Entry e;
    e.kind = kind; e.modId = modId; e.id = id; e.json = json;
    e.mode = mode; e.aiVisible = aiVisible; e.ownerPresent = true;
    m_entries.push_back(std::move(e));
    return true;
}

bool Registry::remove(Kind kind, const std::string& modId, const std::string& id) {
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->kind != kind || it->id != id) continue;
        if (it->modId != modId) return false;    // not yours
        m_entries.erase(it);
        return true;
    }
    return false;
}

std::string Registry::ownerOf(Kind kind, const std::string& id) const {
    for (const Entry& e : m_entries)
        if (e.kind == kind && e.id == id) return e.modId;
    return {};
}

std::vector<Entry> Registry::ofKind(Kind kind) const {
    std::vector<Entry> out;
    for (const Entry& e : m_entries) if (e.kind == kind) out.push_back(e);
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.id < b.id; });
    return out;
}

std::vector<Entry> Registry::ofMod(const std::string& modId) const {
    std::vector<Entry> out;
    for (const Entry& e : m_entries) if (e.modId == modId) out.push_back(e);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.id < b.id;
    });
    return out;
}

size_t Registry::countOf(Kind kind, const std::string& modId) const {
    size_t n = 0;
    for (const Entry& e : m_entries)
        if (e.kind == kind && e.modId == modId) ++n;
    return n;
}

void Registry::removeAllOf(const std::string& modId) {
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&](const Entry& e) { return e.modId == modId; }),
                    m_entries.end());
}

void Registry::clear() { m_entries.clear(); }

void Registry::setLoadedMods(const std::vector<std::string>& modIds) {
    const std::unordered_set<std::string> live(modIds.begin(), modIds.end());
    for (Entry& e : m_entries) e.ownerPresent = live.count(e.modId) != 0;
}

void Registry::clearHollow() {
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [](const Entry& e) { return e.mode == Mode::Hollow; }),
                    m_entries.end());
}

bool Registry::persistsAnything(const std::string& modId) const {
    for (const Entry& e : m_entries)
        if (e.modId == modId && e.mode == Mode::Persist) return true;
    return false;
}

std::vector<Entry> Registry::toSave() const {
    std::vector<Entry> out;
    for (const Entry& e : m_entries) {
        if (e.mode != Mode::Persist) continue;
        // Written even when the owner is gone, the same rule country fields
        // follow: a save that recorded a country holding a mod's doctrine must
        // keep being able to say what that doctrine WAS, or uninstalling the
        // mod turns a campaign into a file full of unreadable ids.
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.id < b.id;
    });
    return out;
}

void Registry::fromSave(const std::vector<Entry>& in) {
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [](const Entry& e) { return e.mode == Mode::Persist; }),
                    m_entries.end());
    for (const Entry& e : in) {
        if (e.modId.empty() || !validContentId(e.id)) continue;
        if (e.kind >= Kind::Count_) continue;
        Entry copy = e;
        copy.mode = Mode::Persist;
        // Inert until a mod declares it or setLoadedMods says the owner is
        // here. A definition read back from a save must not start affecting
        // the game before anything has confirmed its mod is running.
        copy.ownerPresent = false;
        m_entries.push_back(std::move(copy));
    }
}

}  // namespace odcontent
