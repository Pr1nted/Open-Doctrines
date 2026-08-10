#pragma once

// Running a game: opening a session, admitting people, and being the authority.
//
// This is the plumbing around `Lobby`, which holds the rules. The split is
// deliberate -- every decision about who may do what is in Lobby and tested
// without a socket in sight, so this file is transport and nothing else.
//
// THE HOST LISTENS, AND VERIFIES FOR ITSELF
//
// Players connect straight here; the account service is not in the path. Two
// consequences shape this file:
//
//   - There is a listening port, so a host has to become reachable somehow --
//     a tunnel in front of a loopback bind (recommended), a forwarded port, or
//     the peer-to-peer fallback. See WsServer.h and docs/multiplayer-hosting.md.
//   - Nobody else can say who a peer is. The host challenges each connection
//     with a nonce, and the reply must be a join ticket that verifies against
//     the account service's published key. See JoinTicket.h.
//
// WHAT THE HOST MUST DECLARE
//
// Every WELCOME carries who is hosting. Hosting anonymously is legitimate and
// supported -- `Config::anonymous` -- but it is DECLARED as anonymous rather
// than left blank, because a client refuses a server that says nothing at all.
// The difference between "hosted by someone who chose not to be named" and
// "hosted by something that would not answer" is the whole point.

#include "Lobby.h"
#include "NetProtocol.h"
#include "TurnStore.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** Something that happened, for the host's own UI to react to. */
struct NetHostEvent {
    enum class Kind : uint8_t {
        Opened,          // session live; code() is shareable
        PeerJoined,
        PeerLeft,
        LobbyChanged,
        OrdersReceived,
        Chat,
        Failed,          // error() says why
        Closed,
    } kind = Kind::LobbyChanged;

    uint16_t    peerId = 0;
    std::string text;
    NetChat     chat;
};

class NetHost {
public:
    NetHost();
    ~NetHost();
    NetHost(const NetHost&) = delete;
    NetHost& operator=(const NetHost&) = delete;

    struct Config {
        std::string issuer;             // account service
        std::string token;              // the host's account session token
        std::string serverCredential;   // proves WHICH server this is

        /**
         * The host's own display name and badges.
         *
         * Passed in rather than looked up, because this layer knows nothing
         * about the account client. Leaving them empty makes a host look
         * anonymous to everyone -- which is a legitimate choice, but only when
         * it IS a choice, so `anonymous` below is what expresses it.
         */
        std::string hostName;
        std::string hostBadges;

        std::string sessionName = "OpenDoctrines game";
        std::string gameVersion;
        /** `both`-side mods a client must match. See ModAttest.h. */
        std::string requiredMods;

        /**
         * Host without publishing a name. Still declared: clients are told the
         * host chose not to be named, which is different from silence.
         */
        bool anonymous = false;

        /** No country and no renderer; reuses the headless path. */
        bool dedicated = false;

        bool listed = false;            // show in the public directory
        bool showBadges = true;
        uint32_t turnSeconds = 0;       // 0 = long-form, no countdown
        TurnStoreKind store = TurnStoreKind::DurableObject;

        /**
         * The session's order key, base64url, for long-form games.
         *
         * Handed to each SEATED player over this connection and to nobody else.
         * It travels here rather than through the store because a store that
         * carried the key could read everything in it -- see TurnSeal.h. Empty
         * for a rapid game, which has no store and nothing to seal.
         */
        std::string sealKey;

        // ---------------------------------------------------------- listening --

        /** Port to listen on. 0 lets the machine pick a free one. */
        uint16_t port = 27015;

        /**
         * Listen on every interface rather than loopback only.
         *
         * False is the tunnelled setup: the tunnel runs on this machine and
         * connects locally, so nothing else on the network can reach the port.
         * True is for LAN play or a deliberately forwarded port.
         */
        bool bindAll = false;

        /**
         * Fall back to any free port when `port` is taken.
         *
         * On by default because failing to host at all is the worse outcome. A
         * host who forwarded a specific port needs to know it moved, so
         * `listenNote()` says so in words rather than the game quietly
         * listening somewhere else.
         */
        bool portFallback = true;

        LobbySettings lobby;
    };

    enum class Phase : uint8_t {
        Idle = 0, Opening, Connecting, Live, Closed,
    };

    /** Opens a session and connects as its host. Non-blocking. */
    bool open(const Config& config);
    void close();
    void update();

    bool nextEvent(NetHostEvent& out);

    Phase       phase() const;
    std::string error() const;

    /** The invite code, once Live. This is what players are given. */
    std::string code() const;

    /** The port actually bound, which is not always the one requested. */
    uint16_t listenPort() const;

    /**
     * Anything the host should be told about how it ended up listening, in
     * words -- empty when it got exactly what it asked for. A port that moved
     * silently is a forwarded port that stops working for no visible reason.
     */
    std::string listenNote() const;

    /** How many connections are up but have not yet proved who they are. */
    size_t unauthenticatedCount() const;

    const Lobby& lobby() const;
    Lobby&       lobby();

    /** Lobby -> Game. Refuses, with a reason, while anyone holds no country. */
    /** `force` seats nobody who is still choosing -- they become spectators. */
    bool startGame(std::string& why, bool force = false);
    void returnToLobby();

    /** Push the whole lobby to everyone. Called after any change. */
    void broadcastLobby();

    /** Begin a turn: tells everyone the number and the deadline. */
    void beginTurn(uint32_t turnNumber, uint32_t deadlineMs);

    /** Send a turn's changes to everyone. `payload` is an .odsv delta. */
    void broadcastDelta(uint32_t turnNumber, const std::vector<uint8_t>& payload);

    /**
     * Tell everyone which countries this world has.
     *
     * Held so that a peer joining later gets it too, without the host having to
     * remember to resend. See NetCountryList.
     */
    void setCountries(const NetCountryList& list);

    /** The map this game is played on, by name. Carried in every WELCOME. */
    void setMapName(const std::string& name);

    /** Send the whole world to one peer, for a joiner or a spectator. */
    void sendSnapshot(uint16_t peerId, uint32_t turnNumber,
                      const std::vector<uint8_t>& payload);

    /**
     * Tell one player where this game's turns live, and give them the key.
     *
     * Sent automatically when a player is admitted holding a country. Call it
     * again when somebody who was spectating takes a seat: until they hold one
     * they have no orders to submit, and the key is not theirs to have.
     *
     * A no-op for a spectator, for a rapid game, and for Manual -- where the
     * player is the transport and there is nothing to address.
     */
    void sendTurnStoreInfo(uint16_t peerId);

    /** Announce that something was done on a player's behalf, and why. */
    void announceSubstitution(uint16_t countryId, NetSubstitution reason,
                              const std::string& text);

    /**
     * Say something to everybody, as the host.
     *
     * Goes out as the same ChatFrom a relayed player line does, attributed to
     * the host's peer id, so a client needs no second code path to show it.
     * This is what the dedicated server's `say` command sends.
     */
    void sendChat(const std::string& text);

    void kick(uint16_t peerId, const std::string& reason);

    /**
     * Pass a mod's message on. `toPeer` below zero means every other player.
     *
     * The mod id comes from the mod host, which stamps it; nothing a mod says
     * chooses it. This is the only route mod traffic has, and it carries
     * nothing else -- orders and deltas do not travel here.
     */
    void sendModMessage(const std::string& modId, int32_t toPeer,
                        const std::vector<uint8_t>& payload);

    /** Take a mod message addressed to this machine. */
    bool nextModMessage(NetModMsg& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

const char* netHostPhaseName(NetHost::Phase p);
