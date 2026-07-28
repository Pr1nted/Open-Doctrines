#pragma once

// A direct peer-to-peer link, for when a host cannot be reached any other way.
//
// WHY THIS IS NOT JUST ANOTHER SOCKET
//
// A WebSocket is opened by dialling an address. There is no address here: the
// entire point is that the host has none a player can reach. So a link is
// arranged by exchanging a few small blobs through the account service --
// `NetSignal` -- after which the two machines talk directly and nothing of ours
// is in the path. Matchmaking, not relaying.
//
// The consequence for callers: this cannot be "connected" in one call. It
// produces signals to pass along and consumes the ones that come back, and
// somewhere in the middle it becomes Open. `Session` and `Host` drive that the
// same way they drive everything else -- a poll per frame.
//
// TWO BACKENDS, ONE INTERFACE
//
//   Desktop -- libdatachannel (ICE via libjuice, DTLS on the mbedTLS already
//              linked, SCTP via usrsctp). Built only when OD_ENABLE_P2P is on.
//   Browser -- the RTCPeerConnection the browser already has. Nothing is
//              shipped for it, and nothing could be: a page cannot open a
//              listening socket or run a tunnel, so for the web build this is
//              not a fallback at all. It is the only way to play.
//
// Both speak the same wire protocol as the WebSocket path, because what travels
// over the link is the same framed `NetProtocol` messages. The transport
// changes; nothing above it does.
//
// WHAT IT DOES NOT SOLVE
//
// Peers behind symmetric NAT on both ends cannot be introduced by STUN alone;
// that needs TURN, and TURN is a relay. Those connections fail, and the player
// is told to ask the host for a reachable address rather than being quietly
// routed through somebody's server.

#include "NetProtocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** Where a peer link is in its handshake. Mirrors WsState deliberately. */
enum class PeerState : uint8_t {
    Idle = 0,
    Signalling,   // exchanging offer/answer/candidates
    Open,
    Closed,       // finished or failed; error() may say why
};

const char* peerStateName(PeerState s);

/**
 * Free public STUN servers, used to discover one's own public address.
 *
 * They see a couple of packets and never the game. More than one because any
 * single free service can be down, and a failed address discovery is a player
 * who cannot join for reasons they will never work out.
 */
std::vector<std::string> peerDefaultStunServers();

class PeerLink {
public:
    PeerLink();
    ~PeerLink();
    PeerLink(const PeerLink&) = delete;
    PeerLink& operator=(const PeerLink&) = delete;

    /**
     * Which side of the handshake this is.
     *
     * The JOINER offers. That is not arbitrary: the offerer creates the data
     * channel, and having the side that wants to join drive it means a host
     * sitting in a lobby does no work until somebody actually arrives.
     */
    enum class Role : uint8_t { Offerer = 0, Answerer = 1 };

    bool begin(Role role, std::string& error);

    /** Pump callbacks into the queues below. Call once a frame. */
    void update();

    /**
     * Take the next signal this side has produced. Send it to the peer through
     * whatever channel is carrying signalling, and keep calling until false.
     */
    bool nextSignal(NetSignal& out);

    /** Feed in a signal that arrived from the peer. */
    void acceptSignal(const NetSignal& in);

    void send(const std::vector<uint8_t>& payload);
    bool poll(std::vector<uint8_t>& out);

    void close();

    PeerState   state() const;
    std::string error() const;

    /** False when this build has no peer-to-peer transport at all. */
    static bool available();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
