#include "HostBook.h"

#include "HttpClient.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

/** Bounded so a corrupt or hostile file cannot make us allocate freely. */
constexpr size_t kMaxSeats = 64;

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // Control characters are escaped rather than written raw --
                // a name arrives from another machine and must not be able to
                // produce a file that reads back as something else.
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

std::string HostBook::encode() const {
    std::ostringstream o;
    o << "{\n  \"mapId\": \"" << jsonEscape(mapId) << "\",\n"
      << "  \"turnNumber\": " << turnNumber << ",\n"
      << "  \"seats\": [";
    size_t written = 0;
    for (const SeatRecord& s : seats) {
        if (s.psid.empty() || s.countryId == 0) continue;   // nothing to hold
        if (written >= kMaxSeats) break;
        o << (written ? ",\n" : "\n")
          << "    {\"psid\": \"" << jsonEscape(s.psid)
          << "\", \"name\": \"" << jsonEscape(s.name)
          << "\", \"countryId\": " << s.countryId << "}";
        written++;
    }
    o << (written ? "\n  ]" : "]");

    o << ",\n  \"bans\": [";
    size_t nb = 0;
    for (const std::string& b : bans) {
        if (b.empty() || nb >= kMaxSeats) continue;
        o << (nb ? ", " : "") << "\"" << jsonEscape(b) << "\"";
        nb++;
    }
    o << "],\n";

    o << "  \"settings\": {"
      << "\"turnSeconds\": " << settings.turnSeconds
      << ", \"maxPlayers\": " << (int)settings.maxPlayers
      << ", \"lateJoin\": " << (int)settings.lateJoin
      << ", \"absent\": " << (int)settings.absent
      << ", \"assignment\": " << (int)settings.assignment
      << ", \"bindAll\": " << (settings.bindAll ? "true" : "false")
      << ", \"listed\": " << (settings.listed ? "true" : "false")
      << ", \"port\": " << settings.port
      << ", \"voiceLink\": \"" << jsonEscape(settings.voiceLink) << "\""
      << "},\n";

    o << "  \"campaign\": {"
      << "\"inGame\": " << (campaign.inGame ? "true" : "false")
      << ", \"openTurn\": " << campaign.openTurn
      << ", \"turnDeadlineMs\": " << campaign.turnDeadlineMs
      << ", \"sessionCode\": \"" << jsonEscape(campaign.sessionCode) << "\""
      << "}\n}\n";
    return o.str();
}

bool HostBook::decode(const std::string& json, HostBook& out) {
    out = HostBook{};
    if (json.empty()) return false;

    out.mapId = httpJsonString(json, "mapId", 128);
    out.turnNumber = (uint32_t)std::max(0LL, httpJsonNumber(json, "turnNumber", 0));

    // Walked by hand rather than with a general parser: each record is read
    // from the offset of the one before, so a "psid" belonging to record two
    // can never be paired with a "countryId" from record five.
    size_t at = json.find("\"seats\"");
    if (at == std::string::npos) return true;   // a book with no seats is valid

    while (out.seats.size() < kMaxSeats) {
        const size_t rec = json.find("\"psid\"", at);
        if (rec == std::string::npos) break;
        const size_t end = json.find('}', rec);
        if (end == std::string::npos) break;

        const std::string one = json.substr(rec, end - rec + 1);
        SeatRecord s;
        s.psid = httpJsonString(one, "psid", 128);
        s.name = httpJsonString(one, "name", 64);
        const long long cid = httpJsonNumber(one, "countryId", 0);
        s.countryId = (cid > 0 && cid <= 65535) ? (uint16_t)cid : 0;

        if (!s.psid.empty() && s.countryId != 0) out.seats.push_back(std::move(s));
        at = end + 1;
    }

    // Read from the "bans" key onward, so a psid in a SEAT can never be
    // mistaken for a ban -- which would bar the player it belongs to.
    const size_t bansAt = json.find("\"bans\"");
    if (bansAt != std::string::npos) {
        for (const std::string& b :
             httpJsonStringArray(json, "bans", kMaxSeats, bansAt)) {
            if (!b.empty()) out.bans.push_back(b);
        }
    }

    const size_t setAt = json.find("\"settings\"");
    if (setAt != std::string::npos) {
        auto num = [&](const char* k, long long fallback, long long lo, long long hi) {
            const long long v = httpJsonNumber(json, k, fallback, setAt);
            return v < lo || v > hi ? fallback : v;
        };
        out.settings.turnSeconds = (uint32_t)num("turnSeconds", 0, 0, 2592000);
        out.settings.maxPlayers  = (uint8_t)num("maxPlayers", 8, 2, 32);
        out.settings.lateJoin    = (uint8_t)num("lateJoin", 0, 0, 1);
        out.settings.absent      = (uint8_t)num("absent", 0, 0, 1);
        out.settings.assignment  = (uint8_t)num("assignment", 0, 0, 1);
        out.settings.port        = (uint16_t)num("port", 27015, 0, 65535);
        out.settings.bindAll     = httpJsonBool(json, "bindAll", false, setAt);
        out.settings.listed      = httpJsonBool(json, "listed", false, setAt);
        out.settings.voiceLink   = httpJsonString(json, "voiceLink", 200, setAt);
    }

    const size_t campAt = json.find("\"campaign\"");
    if (campAt != std::string::npos) {
        out.campaign.inGame = httpJsonBool(json, "inGame", false, campAt);
        const long long open = httpJsonNumber(json, "openTurn", 0, campAt);
        out.campaign.openTurn = (open > 0 && open < 0xFFFFFFFFLL) ? (uint32_t)open : 0;
        const long long due = httpJsonNumber(json, "turnDeadlineMs", 0, campAt);
        out.campaign.turnDeadlineMs = due > 0 ? (int64_t)due : 0;
        out.campaign.sessionCode = httpJsonString(json, "sessionCode", 32, campAt);
    }
    return true;
}

std::string HostBook::pathFor(const std::string& savePath) {
    return savePath + ".odhost";
}

bool hostBookWriteAtomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), (std::streamsize)text.size());
        f.flush();
        if (!f.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Windows will not rename over a file some other process has open. The
        // old file is the safer thing to keep than a missing one.
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool HostBook::save(const std::string& savePath) const {
    if (savePath.empty()) return false;
    return hostBookWriteAtomic(pathFor(savePath), encode());
}

bool HostBook::load(const std::string& savePath, HostBook& out) {
    out = HostBook{};
    if (savePath.empty()) return false;
    std::ifstream f(pathFor(savePath), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return decode(ss.str(), out);
}

// ---------------------------------------------------------- pending orders --

namespace {

/** Bounded so a corrupt file cannot make us allocate freely. */
constexpr size_t kMaxOrderBytes = 4u << 20;

std::string toHex(const std::vector<uint8_t>& bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) { out += hex[b >> 4]; out += hex[b & 0xF]; }
    return out;
}

bool fromHex(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.size() % 2 != 0 || text.size() / 2 > kMaxOrderBytes) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = nib(text[i]), lo = nib(text[i + 1]);
        if (hi < 0 || lo < 0) { out.clear(); return false; }
        out.push_back((uint8_t)(hi << 4 | lo));
    }
    return true;
}

}  // namespace

// One line per entry, not JSON: the payload is opaque bytes, and a line
// format needs no escaping to round-trip them.
//
//   odorders 1
//   turn <n>
//   order <psid> <0|1 malformed> <hex>
std::string PendingOrdersBook::encode() const {
    std::string out = "odorders 1\nturn " + std::to_string(turn) + "\n";
    size_t n = 0;
    for (const PendingOrder& e : entries) {
        if (e.psid.empty() || e.psid.find_first_of(" \n\r") != std::string::npos) continue;
        if (n++ >= kMaxSeats) break;
        out += "order " + e.psid + " " + (e.malformed ? "1" : "0") + " " +
               toHex(e.orders) + "\n";
    }
    return out;
}

bool PendingOrdersBook::decode(const std::string& text, PendingOrdersBook& out) {
    out = PendingOrdersBook{};
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != "odorders 1") return false;
    if (!std::getline(in, line) || line.compare(0, 5, "turn ") != 0) return false;
    const long long turn = std::atoll(line.c_str() + 5);
    if (turn <= 0 || turn > 0xFFFFFFFFLL) return false;
    out.turn = (uint32_t)turn;
    while (std::getline(in, line) && out.entries.size() < kMaxSeats) {
        if (line.compare(0, 6, "order ") != 0) continue;
        std::istringstream f(line.substr(6));
        PendingOrder e;
        std::string flag, hex;
        if (!(f >> e.psid >> flag)) continue;
        f >> hex;   // empty orders are a legitimate "ready, nothing to do"
        if (flag != "0" && flag != "1") continue;
        e.malformed = flag == "1";
        if (!fromHex(hex, e.orders)) continue;
        out.entries.push_back(std::move(e));
    }
    return true;
}

std::string PendingOrdersBook::pathFor(const std::string& savePath) {
    return savePath + ".odorders";
}

bool PendingOrdersBook::save(const std::string& savePath) const {
    if (savePath.empty()) return false;
    return hostBookWriteAtomic(pathFor(savePath), encode());
}

bool PendingOrdersBook::load(const std::string& savePath, PendingOrdersBook& out) {
    out = PendingOrdersBook{};
    if (savePath.empty()) return false;
    std::ifstream f(pathFor(savePath), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return decode(ss.str(), out);
}

void PendingOrdersBook::remove(const std::string& savePath) {
    if (savePath.empty()) return;
    std::error_code ec;
    std::filesystem::remove(pathFor(savePath), ec);
}
