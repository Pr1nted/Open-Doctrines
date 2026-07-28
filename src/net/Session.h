#pragma once

// Joining a game, from a code to a live lobby.
//
// The state machine over the transport: fetch the session's public info, mint a
// ticket for it, connect, say hello, and from then on translate frames into
// events the game can drain once a frame. Non-blocking throughout; nothing here
// touches raylib.
//
// WHERE THE TWO HALVES MEET
//
// The invite (from ServerBook) says WHERE. The account (from AccountClient)
// says WHO. They come together only here, in memory, for the length of one
// join: a ticket is minted for exactly this session, spent at the host, and
// never written anywhere. That is why ServerBook stores no credential and
// account.json stores no address.
//
// THE ORDER IS CONNECT FIRST, THEN AUTHENTICATE
//
// The host challenges each connection with a nonce, so there is nothing to mint
// a ticket for until the socket is open. So: connect, take the challenge, get a
// ticket that answers it, send it. Under the relay it was the other way round.
//
// WHY THE DESCRIPTOR IS NOT TAKEN FROM THE HOST
//
// A ticket's pseudonym is derived from the SERVER it is minted for. If the host
// supplied the descriptor, a malicious one could hand over some other server's
// descriptor and be told the pseudonym the player uses THERE -- which is exactly
// the cross-server linkage the pairwise design exists to prevent. So the
// descriptor is fetched from the account service using the code the player
// already had, and only the nonce comes from the host.
//
// WHAT THIS REFUSES
//
//   - A server that does not declare who hosts it. Hosting anonymously is
//     fine, but it must be STATED; silence is a misconfiguration or someone
//     hiding, and the player is told which.
//   - A challenge naming an issuer or a session other than the one the player
//     chose to join.
//   - A session whose issuer the local account is not signed in to. A ticket
//     from the wrong service cannot be minted, so this fails early with a
//     sentence rather than late with a rejection.

#include "NetProtocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** Something the game should react to. Drained with nextEvent(). */
struct NetSessionEvent {
    enum class Kind : uint8_t {
        Welcomed,        // handshake done; welcome() is populated
        CountriesKnown,  // the host sent the country catalogue
        LobbyChanged,    // roster or assignment moved
        SwapProposed,    // someone offered you their country
        TurnBegan,
        Snapshot,        // full world, for a joiner
        Delta,           // one turn's changes
        Notice,          // "AI played X because ..."
        Chat,
        Rejected,        // server refused; reason() says why
        Disconnected,
    } kind = Kind::LobbyChanged;

    uint32_t             turnNumber = 0;
    uint32_t             deadlineMs = 0;
    std::vector<uint8_t> payload;      // Snapshot / Delta
    NetSwap              swap;
    NetNotice            notice;
    NetChat              chat;
};

class NetSession {
public:
    NetSession();
    ~NetSession();
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;

    enum class Phase : uint8_t {
        Idle = 0,
        Connecting,    // opening a socket to the host
        Fetching,      // asking the account service about the session
        Minting,       // asking the account service for a ticket
        Handshaking,   // ticket sent, waiting for welcome
        Lobby,
        InGame,
        Closed,        // finished; error() may say why
    };

    /**
     * Begin joining.
     *
     * `addresses` are tried in order until one connects -- a host may be
     * reachable by a tunnel hostname, a public address and a LAN address, and
     * which of them works depends on where the player is. Trying them all is
     * the difference between "joined" and "this game is broken" for anyone on
     * the same network as the host.
     *
     * `issuer` and `code` come from a ServerBook entry; `token` is the account
     * session token, used ONCE to mint a ticket and never stored by this class.
     */
    bool join(const std::vector<std::string>& addresses,
              const std::string& issuer, const std::string& code,
              const std::string& token, const std::string& gameVersion,
              const std::string& modAttestation);

    /** One address. */
    bool join(const std::string& address, const std::string& issuer,
              const std::string& code, const std::string& token,
              const std::string& gameVersion, const std::string& modAttestation);

    /**
     * Which of the candidate addresses is being tried, and how many there are.
     * For a UI that says "trying 2 of 3" rather than appearing to hang.
     */
    size_t addressAttempt() const;
    size_t addressCount() const;

    void leave();

    /** Pump the transport and turn frames into events. Once per frame. */
    void update();

    bool nextEvent(NetSessionEvent& out);

    Phase              phase() const;
    const NetWelcome&  welcome() const;
    const std::vector<NetPeer>& roster() const;

    /**
     * The countries this world has, as the host listed them.
     *
     * Empty until the catalogue arrives. The client cannot derive this
     * itself in the lobby -- it has not loaded the map, and loading one to
     * populate a list would cost seconds and tens of megabytes.
     */
    std::vector<NetCountryList::Entry> countries() const;
    NetSessionState    state() const;
    NetAssignment      assignment() const;

    /** Empty unless something went wrong. A sentence, for a player. */
    std::string error() const;
    NetReject   rejectReason() const;

    /** True once welcomed and holding a country rather than spectating. */
    bool spectating() const;

    // ---- outbound. All are no-ops unless the phase allows them. -------------

    void claimCountry(uint16_t countryId);
    void offerSwap(uint16_t toPeerId);
    void replySwap(uint16_t fromPeerId, bool accept);
    void submitOrders(uint32_t turnNumber, const std::vector<uint8_t>& payload);
    void sendChat(const std::string& text);
    void sendReady();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

const char* netSessionPhaseName(NetSession::Phase p);

/**
 * The sentence a player is shown for a refusal.
 *
 * Kept here rather than in the UI so the wording is one thing, and so the
 * REASON drives it -- a rejection the client does not recognise still gets the
 * server's own text rather than a blank.
 */
std::string netRejectAdvice(NetReject reason, const std::string& serverText);
