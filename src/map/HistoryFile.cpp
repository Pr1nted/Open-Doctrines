#include "HistoryFile.h"

#include "../json.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace history {
namespace {

/// Read a list of ISO codes, upper-cased and de-duplicated. Anything that is
/// not a three-letter string is dropped rather than refused: a map author
/// mistyping one country should lose that country, not the whole file.
std::vector<std::string> isoList(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& e : *it) {
        if (!e.is_string()) continue;
        std::string s = e.get<std::string>();
        if (s.size() != 3) continue;
        for (char& ch : s) ch = (char)std::toupper((unsigned char)ch);
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    return out;
}

template <typename T>
void take(const nlohmann::json& j, const char* key, T& dst) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try { dst = it->get<T>(); } catch (...) {}
}

}  // namespace

bool parse(const std::string& json, Doctrines& out) {
    out.clear();
    nlohmann::json j;
    try { j = nlohmann::json::parse(json); } catch (...) { return false; }
    if (!j.is_object()) return false;

    for (auto it = j.begin(); it != j.end(); ++it) {
        std::string iso = it.key();
        if (iso.size() != 3) continue;              // "comment" keys and the like
        for (char& ch : iso) ch = (char)std::toupper((unsigned char)ch);
        if (!it->is_array()) continue;

        std::vector<Window> windows;
        for (const auto& w : *it) {
            if (!w.is_object()) continue;
            Window win;
            take(w, "from", win.fromYear);
            take(w, "to", win.toYear);
            take(w, "press", win.press);
            take(w, "avoid", win.avoid);
            take(w, "note", win.note);
            win.expand = isoList(w, "expand");
            win.restrain = isoList(w, "restrain");
            // A window that says nothing is dropped, so an empty entry cannot
            // quietly shadow a later one that does say something.
            if (win.expand.empty() && win.restrain.empty()) continue;
            windows.push_back(std::move(win));
        }
        if (!windows.empty()) out[iso] = std::move(windows);
    }
    return true;
}

int yearOf(const std::string& mapDate) {
    // "<Month|Season> <Year> <AD|BC>". The month is not read: only the year
    // matters here, and seasons sit in the same slot as months.
    std::istringstream in(mapDate);
    std::string month, year, era;
    if (!(in >> month >> year >> era)) return kNoYear;
    { std::string extra; if (in >> extra) return kNoYear; }
    if (era != "AD" && era != "BC") return kNoYear;
    if (year.empty() || year.find_first_not_of("0123456789") != std::string::npos)
        return kNoYear;
    long long v = 0;
    try { v = std::stoll(year); } catch (...) { return kNoYear; }
    if (v > 1000000) return kNoYear;
    return (int)(era == "BC" ? -v : v);
}

float pressure(const Doctrines& d, const std::string& attacker,
               const std::string& defender, int year) {
    // No explicit test for kNoYear, deliberately: it is INT_MIN+1, so it falls
    // before the start of any window a map could write and is skipped by the
    // comparison below. A guard here would look like the thing keeping prose
    // dates inert while never actually running -- which is what it did, until a
    // deliberately broken build kept passing and gave it away.
    if (d.empty()) return 1.0f;
    auto it = d.find(attacker);
    if (it == d.end()) return 1.0f;

    // Windows compound. A country can be both pressing east and restrained
    // towards a patron in the same year, and both should be heard; the product
    // is what a single combined judgement would have been.
    float f = 1.0f;
    for (const Window& w : it->second) {
        if (year < w.fromYear || year > w.toYear) continue;
        if (std::find(w.expand.begin(), w.expand.end(), defender) != w.expand.end())
            f *= w.press;
        if (std::find(w.restrain.begin(), w.restrain.end(), defender) != w.restrain.end())
            f *= w.avoid;
    }
    // Bounded. A doctrine tilts the scales; it does not get to decide, and a
    // map author cannot turn a country suicidal or catatonic with one number.
    return std::clamp(f, 0.15f, 3.0f);
}

}  // namespace history
