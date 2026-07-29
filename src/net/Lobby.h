#pragma once

// The authoritative lobby: who is here, what they hold, and who may change it.
//
// Pure logic. No sockets, no raylib, no game headers -- it moves peer ids and
// country ids around and answers yes or no. That is what lets the rules below
// be tested exhaustively rather than observed to work once.
//
// THE RULES IT EXISTS TO ENFORCE
//
//   One country per player, decided HERE. A claim is a request; the answer is
//   the server's. A client that believes it holds France does not make it so.
//
//   A swap is one operation. Two independent "set my country" messages can
//   interleave into both players holding one country, or neither. `acceptSwap`
//   exchanges or refuses -- there is no state in between.
//
//   A reconnect is the same player, not a new one. Identity is the pairwise
//   pseudonym, so someone who closed the game between turns comes back to their
//   country AND to orders they already submitted. With a long turn interval
//   that is the normal case, not the exceptional one.
//
//   A late arrival never becomes a player. Once the game starts the roster is
//   fixed; a newcomer spectates or is refused, per the host's setting.

#include "NetProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

/** Why the lobby said no. Each maps to a sentence the player is shown. */
enum class LobbyDenial : uint8_t {
    None = 0,
    NotInLobby,        // countries are only chosen before the game starts
    Spectator,         // spectators hold nothing
    CountryTaken,
    NoSuchCountry,
    NoSuchPeer,
    NotYours,          // tried to give away a country you do not hold
    HostOnly,          // only the host may do that
    SelfSwap,
    NoOffer,           // replying to an offer that is not there
    SessionFull,
    GameInProgress,
    IssuerNotAccepted,
    Banned,            // told to go, and told not to come back
};

const char* lobbyDenialText(LobbyDenial d);

struct LobbyMember {
    uint16_t    peerId = 0;
    std::string psid;          // identity across reconnects
    std::string name;
    std::string badges;
    std::string issuer;
    bool        officialIssuer = false;
    bool        spectator = false;
    bool        connected = true;
    uint16_t    countryId = 0;   // 0 = none yet

    /** Orders for the current turn, held by the server across reconnects. */
    bool                 submitted = false;
    uint32_t             submittedTurn = 0;
    std::vector<uint8_t> orders;

    /**
     * A submission arrived for `submittedTurn` and could not be read.
     *
     * Distinct from "nothing arrived": the player tried, and is owed a
     * different explanation. It also means the whole submission is discarded
     * rather than partly applied -- half a player's intent is worse than none
     * of it, and is not recoverable into anything they would recognise.
     */
    bool malformed = false;
};

struct LobbySettings {
    uint32_t      maxPlayers = 8;
    NetAssignment assignment = NetAssignment::PlayersPick;
    NetLateJoin   lateJoin = NetLateJoin::Spectate;
    NetAbsent     absent = NetAbsent::Ai;

    /**
     * Account services this server accepts. Empty means the official one only,
     * which is decided by whoever constructs this rather than in here.
     */
    std::vector<std::string> acceptedIssuers;
};

class Lobby {
public:
    void configure(const LobbySettings& s) { m_settings = s; }
    const LobbySettings& settings() const { return m_settings; }

    NetSessionState state() const { return m_state; }

    /** Lobby -> Game. Refuses while any player still holds no country. */
    /**
     * Begin the game.
     *
     * Refuses while anybody is still choosing, and `why` names the first such
     * player -- somebody who is mid-decision has not agreed to be left out.
     *
     * `force` starts anyway, and everyone still without a country becomes a
     * SPECTATOR rather than a player with no land: a seat that exists but owns
     * nothing would be waited on every turn by a player who cannot act. They
     * keep watching, and a lobby that allows late joiners can seat them later.
     */
    bool start(std::string& why, bool force = false);

    /** Players still choosing. Empty means start() will not refuse. */
    std::vector<std::string> stillChoosing() const;

    /** Game -> Lobby, at the host's word. Orders and countries are cleared. */
    void returnToLobby();

    void end() { m_state = NetSessionState::Ended; }

    /**
     * Admit a peer, or say why not.
     *
     * `peerId` is the RELAY's id and is taken, not invented. The relay is what
     * routes messages, so a lobby that numbered seats itself would attribute
     * every later message to the wrong player.
     *
     * `psid` decides identity. A psid already present is a RECONNECT: it keeps
     * its country and its submitted orders, and its peer id is updated to the
     * new connection's -- the relay issues a fresh one each time, so the two
     * are not the same thing and must not be conflated.
     */
    LobbyDenial admit(uint16_t peerId, const std::string& psid,
                      const std::string& name, const std::string& badges,
                      const std::string& issuer, bool officialIssuer);

    /**
     * Hold a country for somebody who is not here yet.
     *
     * Used when a host resumes a saved game: the seats from the previous
     * session are put back before anyone connects, as disconnected members.
     * When that psid does connect, `admit()` sees a psid it already knows and
     * takes the ordinary RECONNECT path -- so returning after three days and
     * returning after a dropped packet are the same operation, not two.
     *
     * False when the psid is already present or the country is already held.
     */
    bool reserveSeat(const std::string& psid, const std::string& name,
                     uint16_t countryId);

    /**
     * Give up a seat being held for somebody who has not come back.
     *
     * By PSID, not peer id: every reservation carries peer id 0, so a release
     * keyed on the handle could not say which one it meant. Refuses to touch a
     * player who is actually connected -- removing one of those is a kick, and
     * a kick should look like a kick to whoever presses it.
     */
    bool releaseSeat(const std::string& psid);

    // ---- moderation --------------------------------------------------------
    //
    // Two different things, deliberately not one:
    //
    //   KICK  -- "leave this game". Removed now; may come back. It is what you
    //            reach for when somebody is stuck, idle, or in the wrong seat.
    //   BAN   -- "do not come back". Kept against the psid, so a reconnect is
    //            refused before it is admitted -- as a player AND as a
    //            spectator, since watching is exactly what a banned person
    //            would otherwise do.
    //
    // Collapsing them would mean every kick was permanent, and a host would
    // stop using it.

    /** Refuse this psid from now on, and remove them if they are here. */
    void ban(const std::string& psid);
    void unban(const std::string& psid);
    bool isBanned(const std::string& psid) const;
    const std::vector<std::string>& bans() const { return m_bans; }

    /**
     * Move a spectator into a country nobody holds.
     *
     * How a game survives somebody leaving: the seat is not lost, it is passed
     * to whoever has been waiting. Fails if the country is held, or if the
     * target is not actually spectating.
     */
    LobbyDenial seatSpectator(uint16_t peerId, uint16_t countryId);

    /** Everyone watching rather than playing. */
    std::vector<const LobbyMember*> spectators() const;

    /** Mark gone. The slot is kept so the player can come back to it. */
    void disconnect(uint16_t peerId);

    /** Remove entirely -- a kick, not a dropped connection. */
    void evict(uint16_t peerId);

    LobbyDenial claimCountry(uint16_t peerId, uint16_t countryId);

    /** Host-driven assignment. `byPeerId` must be the host. */
    LobbyDenial assignCountry(uint16_t byPeerId, uint16_t targetPeerId, uint16_t countryId);

    LobbyDenial offerSwap(uint16_t fromPeerId, uint16_t toPeerId);

    /**
     * Accept or decline. On accept the two countries change hands together;
     * on any failure neither moves.
     */
    LobbyDenial replySwap(uint16_t toPeerId, uint16_t fromPeerId, bool accept);

    /** Records orders against the peer's psid so a reconnect keeps them. */
    LobbyDenial submitOrders(uint16_t peerId, uint32_t turnNumber,
                             const std::vector<uint8_t>& payload);

    /**
     * Take back a submission for `turnNumber`.
     *
     * Only this turn's: a request naming an older turn is a message that
     * arrived late, and honouring it would un-ready somebody for a turn that
     * has already moved on.
     */
    LobbyDenial withdrawOrders(uint16_t peerId, uint32_t turnNumber);

    /** Records that a submission arrived for this turn and was unreadable. */
    LobbyDenial markMalformed(uint16_t peerId, uint32_t turnNumber);

    /** Clears every submission. Called when a turn resolves. */
    void clearSubmissions();

    void setHost(uint16_t peerId) { m_hostPeerId = peerId; }
    uint16_t hostPeerId() const { return m_hostPeerId; }

    const std::vector<LobbyMember>& members() const { return m_members; }
    const LobbyMember* find(uint16_t peerId) const;
    const LobbyMember* findByPsid(const std::string& psid) const;
    const LobbyMember* holderOf(uint16_t countryId) const;

    /** Players (not spectators) who have not submitted for this turn. */
    std::vector<uint16_t> missingSubmissions(uint32_t turnNumber) const;

    /** The wire form, for LobbyState and Welcome. */
    std::vector<NetPeer> roster() const;

private:
    LobbyMember* mutableFind(uint16_t peerId);
    void dropOffers(uint16_t peerId);
    bool issuerAccepted(const std::string& issuer, bool official) const;

    LobbySettings m_settings;
    NetSessionState m_state = NetSessionState::Lobby;
    std::vector<LobbyMember> m_members;
    uint16_t m_hostPeerId = 0;
    std::vector<std::string> m_bans;   // by psid

    struct Offer { uint16_t from; uint16_t to; };
    std::vector<Offer> m_offers;
};
