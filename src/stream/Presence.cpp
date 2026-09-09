#include "Presence.h"

#include <algorithm>
#include <string>

namespace presence {
namespace {

/// Trimmed, control characters dropped, and cut to what Discord will show.
std::string clean(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        // A newline in a scenario name would break the line in two on Discord's
        // side; control characters are simply not text.
        if (c < 0x20) continue;
        out += (char)c;
    }
    const size_t a = out.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    const size_t b = out.find_last_not_of(' ');
    out = out.substr(a, b - a + 1);

    if (out.size() > kMaxField) {
        // ── WALK WHOLE CHARACTERS, DO NOT CUT AND PATCH ──
        //
        // The first version resized to the cap and then trimmed backwards off
        // the end -- and then appended a three-byte ellipsis, which put it back
        // OVER the cap. Counting complete sequences forward to a budget that
        // already excludes the ellipsis cannot make either mistake, and cannot
        // leave half a code point behind: most of this game's languages are
        // multi-byte, and half a character renders as a replacement glyph.
        static const std::string kEllipsis = "\xE2\x80\xA6";
        const size_t budget = kMaxField - kEllipsis.size();
        size_t at = 0, keep = 0;
        while (at < out.size()) {
            const unsigned char c = (unsigned char)out[at];
            const size_t len = (c < 0x80) ? 1
                             : ((c >> 5) == 0x6) ? 2
                             : ((c >> 4) == 0xE) ? 3
                             : ((c >> 3) == 0x1E) ? 4 : 1;
            if (at + len > budget) break;
            at += len;
            keep = at;
        }
        out.resize(keep);
        out += kEllipsis;
    }
    return out;
}

}  // namespace

Activity describe(Where where, const std::string& scenarioRaw,
                  const std::string& countryRaw) {
    const std::string scenario = clean(scenarioRaw);
    const std::string country = clean(countryRaw);
    Activity a;

    switch (where) {
        case Where::MapEditor:
            a.details = "Working on a map!";
            // The map's name, when there is one. An unsaved map has none, and
            // "Working on a map!" alone is the right thing to say then.
            a.state = scenario;
            return a;

        case Where::Playing:
            // The scenario is the thing worth naming: it is what somebody
            // reading this would ask about.
            a.details = scenario.empty() ? "Playing a world" : ("Playing " + scenario);
            a.state = country.empty() ? "" : ("as " + country);
            return a;

        case Where::Multiplayer:
            // Deliberately NOT the invite code or the session name: this line is
            // read by everybody in every server they are in, and a code there is
            // a code given away.
            a.details = "In a multiplayer lobby";
            return a;

        case Where::Menu:
            break;
    }
    a.details = "In the main menu";
    return a;
}

}  // namespace presence
