#include "HostBook.h"

#include "HttpClient.h"

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
    }
    return true;
}

std::string HostBook::pathFor(const std::string& savePath) {
    return savePath + ".odhost";
}

bool HostBook::save(const std::string& savePath) const {
    if (savePath.empty()) return false;
    std::ofstream f(pathFor(savePath), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const std::string text = encode();
    f.write(text.data(), (std::streamsize)text.size());
    return f.good();
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
