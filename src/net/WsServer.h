#pragma once

// The listening half of RFC 6455: a host accepting connections directly.
//
// The client half is in WsNative.cpp. This is its mirror, and the asymmetries
// between them are the interesting part -- a server MUST reject an unmasked
// frame from a client, where a client must reject a masked one from a server.
//
// WHY THIS EXISTS
//
// Because Cloudflare is the account service and nothing else. A game connection
// goes from a player to the host, and somebody has to be listening.
//
// TLS, AND WHY THIS DOES NOT DO IT
//
// This speaks PLAIN ws://. That is deliberate, and it is not a shortcut:
//
//   - A host on a home connection has no certificate and no way to get one for
//     a bare IP address. Generating a self-signed one would train players to
//     click through certificate warnings, which is worse than no TLS at all.
//   - The realistic path to a public game is a tunnel -- Cloudflare Tunnel,
//     playit.gg, Pinggy, ngrok -- and every one of them TERMINATES TLS and
//     forwards plain traffic to localhost. Doing TLS here would be undone
//     immediately by the thing in front of it.
//
// So: bind locally, put a tunnel in front for anything public. The client
// already refuses to send a join ticket over a plaintext connection to
// anything but localhost, so a host who skips the tunnel gets refused
// connections rather than quietly leaked credentials.

#include "WsHandshake.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** A connection handle. Reused only after the old one is closed and reaped. */
using WsConnId = uint32_t;

struct WsServerEvent {
    enum class Kind : uint8_t { Connected, Text, Binary, Disconnected } kind{};
    WsConnId             conn = 0;
    std::string          text;
    std::vector<uint8_t> data;
    /** The peer's address. The host sees this; see PRIVACY.md on IP exposure. */
    std::string          peerAddress;
};

class WsServer {
public:
    WsServer();
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    /**
     * Start listening.
     *
     * `bindAll` false binds to loopback only, which is what a tunnelled host
     * wants: the tunnel connects from the same machine, and nothing else can
     * reach the port at all. True binds to every interface, for LAN play or a
     * host who has forwarded a port deliberately.
     */
    bool listen(uint16_t port, bool bindAll, std::string& error);

    void stop();
    bool listening() const;
    uint16_t port() const;

    /** Pump accepts and I/O. Call once per frame. */
    void update();

    bool nextEvent(WsServerEvent& out);

    void send(WsConnId conn, const std::vector<uint8_t>& payload);
    void sendText(WsConnId conn, const std::string& text);
    void closeConn(WsConnId conn, const std::string& reason);

    size_t connectionCount() const;

    /** False when built without networking. */
    static bool available();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
