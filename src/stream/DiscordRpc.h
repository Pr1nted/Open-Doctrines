#pragma once

// Discord rich presence, over Discord's own local socket.
//
// ── NO SDK ──
//
// The official route is discord-rpc or the Game SDK: a vendored library, a
// shared object to ship per platform, and a dependency that has been deprecated
// once already. What it wraps is a UNIX socket carrying JSON with an eight-byte
// header, which is less code than the build-system entry for the library would
// be. So this speaks it directly, the same way the game already speaks
// WebSocket and HTTP rather than pulling in a client for each.
//
// ── IT IS ENTIRELY LOCAL, AND ENTIRELY OPTIONAL ──
//
// The socket is Discord's, on this machine. If Discord is not running there is
// nothing to connect to and the game says nothing about it -- no error, no
// retry storm, no line in a log. The same is true with no application id
// configured, which is the default: presence is off until somebody sets one.
//
// WHAT IT SENDS is decided in Presence.h, which is pure and has no idea a
// socket exists. Nothing here composes text.

#include "Presence.h"

#include <memory>
#include <string>

namespace discordrpc {

/**
 * Connect, keep connected, and publish what the player is doing.
 *
 * Every call is safe when disconnected or unconfigured; nothing here throws or
 * blocks. `update` is cheap to call every frame -- it sends only when the text
 * has changed and the rate limit allows.
 */
class Rpc {
public:
    Rpc();
    ~Rpc();

    Rpc(const Rpc&) = delete;
    Rpc& operator=(const Rpc&) = delete;

    /// The Discord application id. Empty disconnects and disables everything.
    void configure(const std::string& appId);

    /**
     * Publish `activity` if it differs from what was last sent.
     *
     * Discord rate-limits presence updates; sending faster is not an error but
     * is silently dropped, which is worse than not sending -- the last thing
     * that got through stays on screen. So this holds a minimum gap and sends
     * the newest state after it, rather than the one that happened to be next.
     */
    void update(const presence::Activity& activity, double now);

    /// Clear the presence and close. Called when the game quits.
    void clear();

    bool connected() const;

    /// For diagnostics and the settings screen; empty when all is well.
    std::string status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// The JSON for a SET_ACTIVITY frame. Pure, so the shape is testable.
std::string activityPayload(const presence::Activity& a, long long pid,
                            long long startedAt, const std::string& nonce);

/// The handshake payload for `appId`. Pure.
std::string handshakePayload(const std::string& appId);

}  // namespace discordrpc
