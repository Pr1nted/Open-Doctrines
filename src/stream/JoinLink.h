#pragma once

// "opendoctrines://join/<code>[?at=<host[:port]>]" — a viewer arriving from a
// link on a stream, in Discord, or on the account service's join page.
//
// A URL scheme handler is INPUT FROM OUTSIDE THE GAME: anything on the machine
// can hand the game one of these, and on macOS a web page can too. So it is
// allowed to express exactly two things, and neither of them acts by itself:
// which game, and where it is.
//
// ── WHY THERE IS A SECOND PARAMETER NOW ──
//
// The code was the whole link, on the reasoning that one parameter is one way
// to be wrong and two is more. What that missed is that a code alone is joined
// THROUGH THE RELAY: a host that listens -- a forwarded port, a cloudflared
// tunnel -- is reached by an address, and a link to one of those games opened
// the game, filled in the code and failed. Which reads as the game being
// broken rather than as a link that could not say what it meant.
//
// ── AND WHY IT IS STILL SAFE ──
//
//   1. `at` is a HOST AND AT MOST A PORT, checked by netHostAddressValid --
//      the same rule the board and the account service apply. No scheme, no
//      path, no query of its own, no credentials. A link cannot point the game
//      at a file, a page, or a protocol of the sender's choosing.
//   2. IT IS PREFILLED, NEVER DIALLED. The code already worked this way and
//      the address is no different: both land in the fields on the join
//      screen, and the tickbox that says the host will see your IP address is
//      cleared, so nothing connects until the player says so. A link that
//      drops somebody into a game the moment they click it is a link that can
//      be used to drag people into anything.
//   3. ANYTHING UNRECOGNISED IS DROPPED, not decoded. An address that fails
//      the rule leaves the code alone rather than failing the whole link: the
//      player then gets the relay attempt they would have got before, which
//      is the same answer an older build gives.
//
// An older build ignores the query entirely -- it cuts the code at the first
// `?` -- so a link with an address in it degrades to a code, which is right.
//
// Pure, so every way of getting it wrong is a test rather than a thing somebody
// has to click. See tests/join_link_test.cpp.

#include <string>

namespace joinlink {

/** What a join URL said. `address` is empty unless the link carried a usable one. */
struct Invite {
    std::string code;
    std::string address;

    bool valid() const { return !code.empty(); }
};

/** The whole of a join URL, or an empty Invite for anything that is not one. */
Invite parse(const std::string& url);

/// The code in a join URL, or empty for anything that is not exactly one.
std::string codeFrom(const std::string& url);

}  // namespace joinlink
