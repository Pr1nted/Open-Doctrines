#pragma once

// Talking to the relay, which is the only way into a game from a browser.
//
// ── WHY THIS EXISTS ──
//
// The game's own multiplayer is direct: the host LISTENS on a port, players
// connect straight to it, and the host verifies every join ticket itself. That
// works everywhere except the one place it now has to -- inside a Discord
// Activity, where a Content Security Policy allows exactly the hosts named in
// the app's URL mappings and nothing else. A player there cannot open a socket
// to somebody's home connection or to a trycloudflare hostname, whatever they
// type into the address box.
//
// The account service already ships a relay for this (net/src/lobby/LobbyDO.ts
// -- "The relay. One instance per game session."). Both sides connect OUT to
// it, so neither needs a port open and both are reachable through the single
// /api mapping. It has been deployed and used by nothing: the game never had a
// client for it. This is that client.
//
// ── THE SHAPE OF IT ──
//
// Both directions frame as [kind, peerLo, peerHi] followed by the payload. The
// enums differ because the two ends may do different things:
//
//   relay -> host    Data | PeerJoined | PeerLeft        (netrelay::ToHost)
//   host  -> relay   ToPeer | Broadcast | Kick | Ban     (netrelay::FromHost)
//
// A PLAYER's frames are not framed at all, in either direction: it sends the
// bare game payload and receives the bare game payload. The relay adds the
// header on the way to the host and strips it on the way out. So the client
// side of a relayed game is the ordinary protocol on a different socket, and
// only the host has to think about peer ids.
//
// ── WHAT THIS FILE IS AND IS NOT ──
//
// Pure encoding and decoding, with no socket in it, because the framing is
// where the silent bugs live: a peer id assembled little-endian at one end and
// big-endian at the other loses nobody's connection, it just delivers a turn to
// the wrong player. Everything here is testable without a network, and
// tests/relay_link_test.cpp does exactly that.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace netrelay {

/** Relay -> host. Must match ToHost in net/src/lobby/LobbyDO.ts. */
enum class ToHost : uint8_t { Data = 1, PeerJoined = 2, PeerLeft = 3 };

/** Host -> relay. Must match FromHost in the same file. */
enum class FromHost : uint8_t { ToPeer = 1, Broadcast = 2, Kick = 3, Ban = 4 };

/** One decoded frame from the relay, addressed to the host. */
struct Inbound {
    ToHost               kind = ToHost::Data;
    uint16_t             peerId = 0;
    std::vector<uint8_t> payload;
};

/**
 * Decode a relay->host frame.
 *
 * False for anything shorter than the header or carrying a kind this build
 * does not know -- an unknown tag is a newer relay, not an attack, and the
 * caller drops it rather than tearing the session down.
 */
bool decodeToHost(const uint8_t* data, size_t len, Inbound& out);

/** Encode a host->relay frame. `peerId` is ignored for Broadcast. */
std::vector<uint8_t> encodeFromHost(FromHost kind, uint16_t peerId,
                                    const uint8_t* payload, size_t len);

/**
 * The first frame a socket must send: the join ticket, as JSON text.
 *
 * Text, and the only text frame in the conversation -- everything after it is
 * opaque binary. The relay closes a socket that says anything else first.
 */
std::string helloFrame(const std::string& ticket);

/**
 * Read the relay's answer to a HELLO.
 *
 * `{"ok":true,"peerId":N,"role":"player"}` on success. False for anything
 * else, including a well-formed refusal, because there is nothing a caller can
 * do differently with one.
 */
bool parseHelloReply(const std::string& text, uint16_t& peerId, std::string& role);

/**
 * Where the relay for `code` lives, DERIVED FROM THE ISSUER the player already
 * chose -- never read out of a reply.
 *
 * The service does return a `wsUrl` when a host opens a session, and using it
 * would be the obvious thing. It is deliberately not used: a URL taken from
 * downloaded JSON is a URL somebody else chose, and the whole point of the
 * issuer being a setting is that the player decides who they talk to. Building
 * it here means a compromised or swapped reply cannot move the socket.
 *
 * https -> wss and http -> ws. Empty if `issuer` is not a URL this understands
 * or `code` is not a plausible session code.
 */
std::string relayUrl(const std::string& issuer, const std::string& code,
                     const char* role);

/**
 * Tidy a code somebody typed or pasted into the shape the service issues.
 *
 * Uppercases, drops spaces and any surrounding punctuation, and puts the dash
 * back if it was left out, so "abcd2345", "ABCD 2345" and "abcd-2345" all
 * become "ABCD-2345". An invite gets read off a stream, out of a chat message
 * or aloud, and none of those preserve case.
 *
 * NOTHING IS GUESSED. The alphabet excludes I, L, O, 0 and 1 so that no two
 * characters can be confused in the first place; a code containing one of them
 * is not a misreading to be repaired, it is not a code, and it is refused.
 * Silently turning a wrong character into a plausible one would send somebody
 * to a stranger's game.
 *
 * Returns an empty string if what is left is not a code this service could
 * have issued.
 */
std::string normaliseCode(const std::string& typed);

/** Whether `code` is shaped like one the service could have issued. */
bool isSessionCode(const std::string& code);

}  // namespace netrelay
