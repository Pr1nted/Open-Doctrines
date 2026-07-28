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
