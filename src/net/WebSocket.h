#pragma once

// A WebSocket client, and the only thing in the game that opens a socket.
//
// WHY A WEBSOCKET AT ALL
//
// Because everyone dials OUT. The host connects to the relay exactly the same
// way a player does, so there is no listening port on anybody's machine and
// nothing to forward: no UPnP, no NAT punchthrough, no "open port 7777 on your
// router", and it works from a browser tab, which nothing else here would.
//
// TWO BACKENDS, ONE INTERFACE
//
//   WsNative.cpp   desktop. mbedTLS for the TLS, and RFC 6455 framing we own.
//   WsWeb.cpp      Emscripten. The browser already has a WebSocket and already
//                  has the certificate store; we would not be allowed to open
//                  a raw socket anyway.
//
// THREADING
//
// The native backend runs its own thread and this class is the boundary. Send
// from the game thread, poll from the game thread; everything else happens
// behind the mutex. Nothing here calls into the game, so there is no callback
// that could run on the wrong thread -- the game asks, at a time of its
// choosing, which is what keeps this out of the frame loop's hair.

#include "NetUrl.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class WsState {
    Idle = 0,       // never connected
    Connecting,
    Open,
    Closing,
    Closed,         // finished, cleanly or otherwise; see error()
};

const char* wsStateName(WsState s);

class WebSocket {
public:
    WebSocket();
    ~WebSocket();

    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // Returns false only for a URL this cannot use. Everything else -- DNS,
    // TLS, the handshake -- happens asynchronously and surfaces as
    // WsState::Closed with an error().
    //
    // PLAINTEXT ws:// IS REFUSED unless `allowInsecure`, which exists for
    // `wrangler dev` on localhost and nothing else. A join ticket travels in
    // the first frame, and putting one on the wire in the clear would undo the
    // point of it being short-lived.
    bool connect(const std::string& url, bool allowInsecure = false);

    // Queues a binary message. Silently dropped once closed -- a caller that
    // had to check the state before every send would eventually forget, and
    // the state can change between the check and the send anyway.
    void send(const std::vector<uint8_t>& payload);

    // Pops one received message. False when there is nothing waiting.
    bool poll(std::vector<uint8_t>& out);

    // The relay's HELLO reply is text; everything after it is binary. Kept
    // separate so a text frame can never be mistaken for a protocol frame.
    void sendText(const std::string& text);
    bool pollText(std::string& out);

    void close();

    WsState     state() const;
    std::string error() const;   // empty unless something went wrong

    // True when this build has a transport at all. False on a desktop build
    // configured with -DOD_ENABLE_NET=OFF, which the multiplayer menu reports
    // rather than pretending to connect -- the same shape as the mod runtime's
    // "backend unavailable" path.
    static bool available();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
