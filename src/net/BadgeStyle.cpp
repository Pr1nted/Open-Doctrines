#include "BadgeStyle.h"

#include <cctype>

namespace {

// The deep green a developer's name is drawn in.
//
// Chosen against the game's near-black menu background (#14161A) rather than
// in the abstract: a genuinely dark green there reads as grey at small sizes,
// so this keeps enough luminance to stay legible while still reading as dark
// green rather than as a highlight colour. Change it here and every surface
// that shows a name follows.
constexpr uint32_t kDeveloperName = 0x2E8B57FFu;   // sea green, deepened
constexpr uint32_t kDeveloperTag  = 0x46C07Bu << 8 | 0xFFu;

constexpr uint32_t kPlaytesterName = 0x4FA3A5FFu;  // muted teal
constexpr uint32_t kPlaytesterTag  = 0x6FD0D2FFu;

// Order is precedence, highest first. A name gets ONE colour, so when someone
// holds both badges this decides which -- not the order the server happened to
// serialise them in.
struct Rank { const char* id; uint32_t name; uint32_t tag; };
constexpr Rank kRanked[] = {
    { kBadgeDeveloper,  kDeveloperName,  kDeveloperTag  },
    { kBadgePlaytester, kPlaytesterName, kPlaytesterTag },
};

}  // namespace

std::string badgeTag(const std::string& badge) {
    std::string upper;
    upper.reserve(badge.size() + 2);
    for (unsigned char c : badge) {
        // Only ASCII is uppercased. Badge ids are ours and ASCII by
        // definition, and touching bytes above 0x7F would corrupt UTF-8 if a
        // future one ever were not.
        upper += (c < 0x80) ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
    }
    return "[" + upper + "]";
}

uint32_t badgeNameColor(const std::vector<std::string>& badges, uint32_t fallbackRgba) {
    for (const auto& rank : kRanked) {
        for (const auto& held : badges) {
            if (held == rank.id) return rank.name;
        }
    }
    return fallbackRgba;
}

uint32_t badgeTagColor(const std::string& badge) {
    for (const auto& rank : kRanked) {
        if (badge == rank.id) return rank.tag;
    }
    // An unknown badge from a newer server still gets shown, just neutrally.
    // Hiding it would be worse: the player would see nothing and assume the
    // badge did not exist.
    return 0xB4B4C8FFu;
}

bool badgeIssuerIsOfficial(const std::string& issuer, const std::string& official) {
    // Exact match. A prefix or suffix test would accept
    // "https://evil.example/?x=https://official" and similar.
    return !official.empty() && issuer == official;
}
