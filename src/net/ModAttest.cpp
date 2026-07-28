#include "ModAttest.h"

#include "../util/Sha256.h"

#include <algorithm>
#include <cctype>

namespace {

bool validHexDigest(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

// Everything here parses text another machine sent. A field long enough to be
// a denial of service is refused before it is stored, not after.
constexpr size_t kMaxIdLen      = 128;
constexpr size_t kMaxVersionLen = 32;
constexpr size_t kMaxEntries    = 512;
constexpr size_t kMaxTextBytes  = 128 * 1024;

}  // namespace

// ---------------------------------------------------------------- entry ----

std::string ModAttestEntry::toString() const {
    return id + "@" + version + "#" + sha256;
}

bool ModAttestEntry::parse(const std::string& text, ModAttestEntry& out) {
    // Split on the LAST '#' and the last '@' before it, so an id containing
    // either character cannot shift the field boundaries.
    const size_t hash = text.rfind('#');
    if (hash == std::string::npos) return false;
    const size_t at = text.rfind('@', hash);
    if (at == std::string::npos || at == 0) return false;

    ModAttestEntry e;
    e.id      = text.substr(0, at);
    e.version = text.substr(at + 1, hash - at - 1);
    e.sha256  = text.substr(hash + 1);

    if (e.id.empty() || e.id.size() > kMaxIdLen) return false;
    if (e.version.empty() || e.version.size() > kMaxVersionLen) return false;
    if (!validHexDigest(e.sha256)) return false;

    // The wire form carries only the shared set, so anything decoded from it
    // is Both by definition. A client claiming a side would be claiming
    // something the server has no reason to believe.
    e.side = ModSide::Both;
    out = e;
    return true;
}

// ---------------------------------------------------------- attestation ----

void ModAttestation::sort() {
    std::sort(entries.begin(), entries.end(),
              [](const ModAttestEntry& a, const ModAttestEntry& b) { return a.id < b.id; });
}

std::vector<ModAttestEntry> ModAttestation::shared() const {
    std::vector<ModAttestEntry> out;
    for (const auto& e : entries)
        if (e.side == ModSide::Both) out.push_back(e);
    std::sort(out.begin(), out.end(),
              [](const ModAttestEntry& a, const ModAttestEntry& b) { return a.id < b.id; });
    return out;
}

std::string ModAttestation::digest() const {
    std::string canonical;
    for (const auto& e : shared()) {
        canonical += e.toString();
        canonical += '\n';
    }
    return ::sha256Hex(canonical);
}

// ------------------------------------------------------------- compare ----

namespace {

const ModAttestEntry* find(const std::vector<ModAttestEntry>& v, const std::string& id) {
    for (const auto& e : v)
        if (e.id == id) return &e;
    return nullptr;
}

}  // namespace

ModAttestResult modAttestCompare(const std::vector<ModAttestEntry>& required,
                                 const ModAttestation& offered,
                                 ModExtraPolicy extras) {
    ModAttestResult result;
    const std::vector<ModAttestEntry> have = offered.shared();

    for (const auto& want : required) {
        const ModAttestEntry* got = find(have, want.id);
        if (!got) {
            result.problems.push_back({
                ModAttestVerdict::Missing, want.id,
                "This server needs \"" + want.id + "\" " + want.version +
                ", which you do not have installed.",
            });
            continue;
        }
        // Version first: it is the difference a player can act on, and saying
        // "the bytes differ" when the real answer is "you have 1.2, they run
        // 1.3" sends them looking for a corruption that is not there.
        if (got->version != want.version) {
            result.problems.push_back({
                ModAttestVerdict::VersionDiffers, want.id,
                "This server runs \"" + want.id + "\" " + want.version +
                " and you have " + got->version + ".",
            });
            continue;
        }
        if (got->sha256 != want.sha256) {
            result.problems.push_back({
                ModAttestVerdict::BytesDiffer, want.id,
                "Your copy of \"" + want.id + "\" " + want.version +
                " is not the same file as the server's. Re-download it.",
            });
        }
    }

    if (extras == ModExtraPolicy::Refuse) {
        for (const auto& got : have) {
            if (find(required, got.id)) continue;
            result.problems.push_back({
                ModAttestVerdict::Extra, got.id,
                "\"" + got.id + "\" changes how turns are played and this "
                "server does not have it. Turn it off to join.",
            });
        }
    }

    result.ok = result.problems.empty();
    return result;
}

std::string ModAttestResult::summary() const {
    if (problems.empty()) return {};
    if (problems.size() == 1) return problems[0].detail;
    return problems[0].detail + " (and " + std::to_string(problems.size() - 1) +
           " other mod " + (problems.size() == 2 ? "problem" : "problems") + ")";
}

// ---------------------------------------------------------------- wire ----

std::string modAttestEncode(const ModAttestation& a) {
    std::string out;
    for (const auto& e : a.shared()) {
        out += e.toString();
        out += '\n';
    }
    return out;
}

bool modAttestDecode(const std::string& text, ModAttestation& out) {
    out.entries.clear();
    if (text.size() > kMaxTextBytes) return false;

    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;

        if (line.empty()) {
            if (end == text.size()) break;
            continue;
        }
        if (out.entries.size() >= kMaxEntries) return false;

        ModAttestEntry e;
        // One bad line fails the whole message. A partially understood mod
        // list is worse than a refused one: it would let a client drop an
        // entry by malforming it.
        if (!ModAttestEntry::parse(line, e)) return false;
        out.entries.push_back(e);
    }
    out.sort();
    return true;
}
