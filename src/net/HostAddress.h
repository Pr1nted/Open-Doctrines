#pragma once

// Is this where a server is, or is it somewhere to send a reader?
//
// ── ONE LIST, BECAUSE THERE ARE THREE PLACES AN ADDRESS ARRIVES FROM ──
//
// A looking-for-a-game listing (src/net/Lfg.h), a join link handed to the game
// by the operating system (src/stream/JoinLink.h), and the account service,
// which enforces the same rule in TypeScript (net/src/lfg/board.ts, ADDRESS).
// All three take an address written by somebody else, and the whole reason
// each of them is allowed to is that an address cannot be a link. Two copies
// of that rule is one copy that will eventually be looser than the other, so
// it lives here and the C++ side has one implementation.
//
// WHAT IS ALLOWED, and nothing else: a hostname or an IPv4 address, with at
// most a `:port`. It must contain a dot. That is it.
//
//   play.example.com            yes
//   tidy-otter.trycloudflare.com:27015    yes
//   198.51.100.7:27015          yes
//   localhost                   no -- no dot, and no reader elsewhere can reach it
//   https://example.com         no -- a scheme
//   example.com/join            no -- a path
//   example.com?ref=x           no -- a query
//   user@example.com            no -- credentials
//
// A scheme, a path, a query, a fragment or credentials would make the field a
// place to send somebody, which is the thing every caller here refuses to be.
// Refusing the SHAPE means nobody has to judge where a given address points.
//
// IPv6 IS NOT ACCEPTED, deliberately. `[::1]:27015` is a fourth syntax with
// its own brackets and its own ways to be wrong, no host this game talks to
// has ever published one, and the day one does is the day to add it with its
// own tests rather than to have guessed at it now.
//
// Pure. No allocation beyond the substring, no network, no dependencies, so
// every way of getting it wrong is a test (tests/join_link_test.cpp).

#include <string>

/** The longest one anybody would write. A URL is longer than this. */
inline constexpr size_t kNetHostAddressMax = 128;

/** Host, and at most a port. See the header comment for what that means. */
bool netHostAddressValid(const std::string& value);
