#include "SeatBook.h"

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

std::string SeatBook::encode() const {
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
    o << (written ? "\n  ]\n}\n" : "]\n}\n");
    return o.str();
}

bool SeatBook::decode(const std::string& json, SeatBook& out) {
    out = SeatBook{};
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
    return true;
}

std::string SeatBook::pathFor(const std::string& savePath) {
    return savePath + ".seats.json";
}

bool SeatBook::save(const std::string& savePath) const {
    if (savePath.empty()) return false;
    std::ofstream f(pathFor(savePath), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const std::string text = encode();
    f.write(text.data(), (std::streamsize)text.size());
    return f.good();
}

bool SeatBook::load(const std::string& savePath, SeatBook& out) {
    out = SeatBook{};
    if (savePath.empty()) return false;
    std::ifstream f(pathFor(savePath), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return decode(ss.str(), out);
}
