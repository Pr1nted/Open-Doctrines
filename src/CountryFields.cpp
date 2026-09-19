#include "CountryFields.h"

#include <algorithm>
#include <unordered_set>

namespace odcountry {
namespace {

/// Long enough for a sentence, short enough that a save cannot be padded out
/// with field names. The same ceiling odmoddir puts on a listing's name.
constexpr size_t kMaxName = 64;

}  // namespace

bool validFieldName(const std::string& name) {
    // A field name ends up in a save, in the debug view, and in a key. So:
    // printable ASCII only, no separators, and bounded.
    //
    // The NUL matters more than it looks: the store keys on
    // modId + '\0' + name, so a name containing one could be crafted to
    // collide with another mod's field. Refusing it here is the only place
    // that check belongs.
    if (name.empty() || name.size() > kMaxName) return false;
    for (unsigned char c : name) {
        if (c < 0x21 || c > 0x7E) return false;   // no space, no control, no NUL
        if (c == '\\' || c == '"' || c == '/') return false;
    }
    return true;
}

std::string Store::key(const std::string& modId, const std::string& name) {
    return modId + '\0' + name;
}

const Store::Entry* Store::find(const std::string& modId, const std::string& name) const {
    auto it = m_fields.find(key(modId, name));
    return it == m_fields.end() ? nullptr : &it->second;
}
Store::Entry* Store::find(const std::string& modId, const std::string& name) {
    auto it = m_fields.find(key(modId, name));
    return it == m_fields.end() ? nullptr : &it->second;
}

bool Store::add(const std::string& modId, const std::string& name,
                Mode mode, Type type) {
    if (modId.empty() || !validFieldName(name)) return false;

    auto it = m_fields.find(key(modId, name));
    if (it != m_fields.end()) {
        Entry& e = it->second;
        // Redeclaring identically is not an error -- it is what a hollow field
        // does on every single load, and making that fail would mean every mod
        // writing "have I already declared this" bookkeeping.
        if (e.field.type != type) return false;
        e.field.mode = mode;
        // Declaring it is what proves the owner is back after a load without
        // the mod. Its values, held all along, become live again here.
        e.field.ownerPresent = true;
        return true;
    }

    Entry e;
    e.field.modId = modId;
    e.field.name = name;
    e.field.mode = mode;
    e.field.type = type;
    e.field.ownerPresent = true;
    m_fields[key(modId, name)] = std::move(e);
    return true;
}

bool Store::remove(const std::string& modId, const std::string& name) {
    return m_fields.erase(key(modId, name)) > 0;
}

bool Store::has(const std::string& modId, const std::string& name) const {
    const Entry* e = find(modId, name);
    return e != nullptr && e->field.ownerPresent;
}

bool Store::setNumber(const std::string& modId, const std::string& name,
                      int countryId, double v) {
    Entry* e = find(modId, name);
    // A field whose owner is absent is INERT: its values are kept for when the
    // mod comes back, and nothing writes them meanwhile.
    if (!e || !e->field.ownerPresent || e->field.type != Type::Number) return false;
    e->numbers[countryId] = v;
    return true;
}

double Store::number(const std::string& modId, const std::string& name,
                     int countryId, double fallback) const {
    const Entry* e = find(modId, name);
    if (!e || !e->field.ownerPresent || e->field.type != Type::Number) return fallback;
    auto it = e->numbers.find(countryId);
    return it == e->numbers.end() ? fallback : it->second;
}

bool Store::setText(const std::string& modId, const std::string& name,
                    int countryId, const std::string& v) {
    Entry* e = find(modId, name);
    if (!e || !e->field.ownerPresent || e->field.type != Type::Text) return false;
    e->texts[countryId] = v;
    return true;
}

std::string Store::text(const std::string& modId, const std::string& name,
                        int countryId) const {
    const Entry* e = find(modId, name);
    if (!e || !e->field.ownerPresent || e->field.type != Type::Text) return {};
    auto it = e->texts.find(countryId);
    return it == e->texts.end() ? std::string() : it->second;
}

std::vector<Field> Store::fieldsOf(const std::string& modId) const {
    std::vector<Field> out;
    for (const auto& [k, e] : m_fields)
        if (e.field.modId == modId) out.push_back(e.field);
    std::sort(out.begin(), out.end(),
              [](const Field& a, const Field& b) { return a.name < b.name; });
    return out;
}

std::vector<Field> Store::all() const {
    std::vector<Field> out;
    for (const auto& [k, e] : m_fields) out.push_back(e.field);
    // Sorted so a save of the same world comes out byte-identical twice, and
    // so a diagnostic list does not reshuffle between runs.
    std::sort(out.begin(), out.end(), [](const Field& a, const Field& b) {
        return a.modId != b.modId ? a.modId < b.modId : a.name < b.name;
    });
    return out;
}

void Store::setLoadedMods(const std::vector<std::string>& modIds) {
    const std::unordered_set<std::string> live(modIds.begin(), modIds.end());
    for (auto& [k, e] : m_fields)
        e.field.ownerPresent = live.count(e.field.modId) != 0;
}

void Store::clearHollow() {
    for (auto it = m_fields.begin(); it != m_fields.end(); ) {
        if (it->second.field.mode == Mode::Hollow) it = m_fields.erase(it);
        else ++it;
    }
}

void Store::clear() { m_fields.clear(); }

bool Store::anyPersisted() const {
    for (const auto& [k, e] : m_fields)
        if (e.field.mode == Mode::Persist) return true;
    return false;
}

bool Store::persistsAnything(const std::string& modId) const {
    for (const auto& [k, e] : m_fields)
        if (e.field.modId == modId && e.field.mode == Mode::Persist) return true;
    return false;
}

std::vector<Store::Saved> Store::toSave() const {
    std::vector<Saved> out;
    for (const Field& f : all()) {
        if (f.mode != Mode::Persist) continue;
        const Entry* e = find(f.modId, f.name);
        if (!e) continue;
        Saved s;
        s.field = f;
        if (f.type == Type::Number)
            for (const auto& [cid, v] : e->numbers) s.values.push_back({cid, v, {}});
        else
            for (const auto& [cid, v] : e->texts) s.values.push_back({cid, 0.0, v});
        // WRITTEN EVEN WHEN THE OWNER IS ABSENT. That is what makes removing a
        // mod reversible: uninstall it, play on, reinstall, and the numbers are
        // still here. Dropping them would destroy a player's data the first
        // time they disabled a mod to see whether it was the problem.
        out.push_back(std::move(s));
    }
    return out;
}

void Store::fromSave(const std::vector<Saved>& in) {
    // Only the persisted ones are replaced; hollow fields belong to whatever
    // has already declared them this session.
    for (auto it = m_fields.begin(); it != m_fields.end(); ) {
        if (it->second.field.mode == Mode::Persist) it = m_fields.erase(it);
        else ++it;
    }
    for (const Saved& s : in) {
        if (s.field.modId.empty() || !validFieldName(s.field.name)) continue;
        Entry e;
        e.field = s.field;
        e.field.mode = Mode::Persist;
        // Assume absent until a mod declares it. setLoadedMods and add() are
        // what turn it back on, so a save loaded without its mod cannot have
        // its values quietly rewritten by anything.
        e.field.ownerPresent = false;
        for (const Value& v : s.values) {
            if (s.field.type == Type::Number) e.numbers[v.countryId] = v.number;
            else                               e.texts[v.countryId] = v.text;
        }
        m_fields[key(s.field.modId, s.field.name)] = std::move(e);
    }
}

}  // namespace odcountry
