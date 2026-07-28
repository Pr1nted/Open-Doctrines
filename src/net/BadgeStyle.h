#pragma once

// How a badge changes the way a name is shown.
//
// Deliberately here rather than in the account screen, and deliberately free of
// raylib: a name with a badge will appear in the multiplayer roster, in chat and
// over the map as well as on the account screen, and a developer whose name is
// green in one place and white in another is worse than one with no colour at
// all. One definition, every surface.
//
// Colours are 0xRRGGBBAA so this file needs no renderer type. The UI converts.
//
// WHAT A BADGE MEANS, AND WHAT IT DOES NOT
//
// A badge says the account service vouched for something. It is NOT a claim
// about the player's conduct and grants no in-game power. It is also only
// trustworthy when the issuer is the official one -- a server configured
// against a third-party account provider can mint whatever badges it likes,
// which is why NetPeer carries the issuer and why the roster must render an
// unofficial badge differently rather than identically.

#include <cstdint>
#include <string>
#include <vector>

/** Badge ids as the account service spells them. */
inline constexpr const char* kBadgeDeveloper  = "developer";
inline constexpr const char* kBadgePlaytester = "playtester";

/** "[DEVELOPER]" for a known badge, or an uppercased fallback. */
std::string badgeTag(const std::string& badge);

/**
 * Colour a name should be drawn in, given the badges on it.
 *
 * The highest-ranking badge wins so that a name has ONE colour: someone with
 * both badges must not flicker between them depending on array order.
 * Un-badged names return `fallbackRgba` so callers keep their own default
 * rather than having a white forced on them.
 */
uint32_t badgeNameColor(const std::vector<std::string>& badges,
                        uint32_t fallbackRgba = 0xFFFFFFFFu);

/** Colour of the tag itself. Brighter than the name, since it is smaller. */
uint32_t badgeTagColor(const std::string& badge);

/**
 * True when a badge came from somewhere other than the official issuer.
 *
 * Such a badge must be shown differently -- see the header note. `official` is
 * whatever the client was built to trust; `issuer` is what the peer presented.
 */
bool badgeIssuerIsOfficial(const std::string& issuer, const std::string& official);
