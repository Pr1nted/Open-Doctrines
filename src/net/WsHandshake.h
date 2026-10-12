#pragma once

// The RFC 6455 opening handshake, shared by both halves.
//
// This is deliberately one unit rather than a client copy and a server copy.
// The two halves have to agree on `Sec-WebSocket-Accept` exactly -- the client
// computes what it expects and refuses anything else -- so a divergence between
// them is not a cosmetic duplication, it is a handshake that can never succeed.
// That is not hypothetical: the magic GUID below was once mistyped in both
// copies, and because the client's copy was only ever exercised against our own
// (equally wrong) server, nothing caught it. Now there is one of everything and
// the RFC's worked example in tests/net_wsserver_test.cpp covers it.
//
// Everything here is pure string work, so it can be tested against the
// truncated and hostile requests a listening port actually receives without
// opening a socket.

#include <cstdint>
#include <string>

// HOW LONG AN OPENING HANDSHAKE MAY TAKE, for both halves, because the two
// numbers have to be the same one.
//
// They were not: the client waited 20 s for a reply (WsNative.cpp) and the
// server dropped a connection that had not finished in 10 s (WsServer.cpp),
// under the same name, kHandshakeTimeoutMs, in two files. The client's second
// ten seconds could therefore never help -- the server had already hung up --
// and the client still reported "the server did not complete the handshake in
// time", blaming the peer for a deadline the peer had enforced. Diagnosing a
// join failure from that message means reading two constants and knowing which
// one won.
//
// Twenty, not ten. The failures seen in practice are honest clients and
// servers on loaded machines, not probes: a CI runner that spent 68 minutes on
// a test suite starved a host's socket poll past the client's deadline. The
// cost of the larger number is that a connection which opens and says nothing
// holds a socket for twice as long, which is bounded, capped at
// kMaxHandshakeBytes, and cheaper than a player who cannot join.
//
// It lives here for the same reason wsAcceptFor does, one paragraph up: the two
// halves agreeing is the whole point, and a second copy is how they stop.
constexpr int kWsHandshakeTimeoutMs = 20000;

/** Standard base64. Used for the accept digest and for generating a key. */
std::string wsBase64(const uint8_t* data, size_t n);

/** `Sec-WebSocket-Accept` for a given `Sec-WebSocket-Key`. RFC 6455 §4.2.2. */
std::string wsAcceptFor(const std::string& key);

struct WsUpgradeRequest {
    std::string path;
    std::string key;
    int         version = 0;
    bool        upgrade = false;     // Upgrade: websocket
    bool        connectionUpgrade = false;

    /** Everything RFC 6455 requires of a client's opening handshake. */
    bool valid() const {
        return upgrade && connectionUpgrade && version == 13 && !key.empty();
    }
};

/**
 * Parse an opening handshake. False if it is not a complete, well-formed
 * request -- never a partial success, because a half-understood handshake is
 * how a server ends up talking to something that is not a WebSocket client.
 */
bool wsParseUpgrade(const std::string& request, WsUpgradeRequest& out);

/** The 101 response, or a 400 when the request was not usable. */
std::string wsUpgradeResponse(const WsUpgradeRequest& request);

/**
 * A plain HTTP GET for "/healthz" that is not asking to upgrade: a
 * hosting platform's health check or an uptime pinger. See WsServer.cpp.
 */
bool wsIsHealthCheck(const std::string& request);

/** The 200 sent back to one. Closes the connection. */
std::string wsHealthResponse();
