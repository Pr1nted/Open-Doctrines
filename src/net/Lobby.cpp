#include "Lobby.h"

#include <algorithm>

const char* lobbyDenialText(LobbyDenial d) {
    switch (d) {
        case LobbyDenial::None:              return "";
        case LobbyDenial::NotInLobby:        return "Countries can only be chosen before the game starts.";
        case LobbyDenial::Spectator:         return "Spectators do not hold a country.";
        case LobbyDenial::CountryTaken:      return "Another player already has that country.";
        case LobbyDenial::NoSuchCountry:     return "That country is not on this map.";
        case LobbyDenial::NoSuchPeer:        return "That player is not here.";
        case LobbyDenial::NotYours:          return "You do not hold a country to offer.";
        case LobbyDenial::HostOnly:          return "Only the host can do that.";
        case LobbyDenial::SelfSwap:          return "You cannot swap with yourself.";
        case LobbyDenial::NoOffer:           return "That offer is no longer open.";
        case LobbyDenial::SessionFull:       return "This game is full.";
        case LobbyDenial::GameInProgress:    return "That game has already started.";
        case LobbyDenial::Banned:
            return "the host has barred you from this game";
        case LobbyDenial::IssuerNotAccepted: return "This server does not accept that account service.";
    }
    return "";
}

// ---------------------------------------------------------------- lookups ----

const LobbyMember* Lobby::find(uint16_t peerId) const {
    for (const auto& m : m_members) if (m.peerId == peerId) return &m;
    return nullptr;
}

LobbyMember* Lobby::mutableFind(uint16_t peerId) {
    for (auto& m : m_members) if (m.peerId == peerId) return &m;
    return nullptr;
}

const LobbyMember* Lobby::findByPsid(const std::string& psid) const {
    if (psid.empty()) return nullptr;
    for (const auto& m : m_members) if (m.psid == psid) return &m;
    return nullptr;
}

const LobbyMember* Lobby::holderOf(uint16_t countryId) const {
    if (countryId == 0) return nullptr;
    for (const auto& m : m_members) {
        if (!m.spectator && m.countryId == countryId) return &m;
    }
    return nullptr;
}

bool Lobby::issuerAccepted(const std::string& issuer, bool official) const {
    // Empty list means "the official one only". Expressed as a default rather
    // than as an empty allowlist meaning "anything", which is the reading that
    // would silently accept every account service in existence.
    if (m_settings.acceptedIssuers.empty()) return official;
    for (const auto& a : m_settings.acceptedIssuers) if (a == issuer) return true;
    return false;
}

// ------------------------------------------------------------------ admit ----

LobbyDenial Lobby::admit(uint16_t peerId, const std::string& psid,
                         const std::string& name, const std::string& badges,
                         const std::string& issuer, bool officialIssuer) {
    if (peerId == 0) return LobbyDenial::NoSuchPeer;
    // Before the issuer check, before the reconnect path: a ban is about the
    // person, and nothing they present should get them past it.
    if (isBanned(psid)) return LobbyDenial::Banned;
    if (!issuerAccepted(issuer, officialIssuer)) return LobbyDenial::IssuerNotAccepted;

    // A psid we have seen is the same person coming back. Their country and
    // any orders they already submitted are still theirs -- with a long turn
    // interval, closing the game between turns is ordinary.
    //
    // The PEER ID is not: the relay issues a fresh one per connection, so it
    // is updated here. Identity is the pseudonym; the peer id is only a routing
    // handle that happens to change.
    for (auto& m : m_members) {
        if (m.psid == psid) {
            // Any offer aimed at the old handle is dropped rather than
            // silently re-pointed at the new one.
            dropOffers(m.peerId);
            m.peerId = peerId;
            m.connected = true;
            m.name = name;
            m.badges = badges;
            return LobbyDenial::None;
        }
    }

    if (m_state == NetSessionState::Ended) return LobbyDenial::GameInProgress;
    // Two live connections must never share a handle.
    if (find(peerId)) return LobbyDenial::NoSuchPeer;

    LobbyMember m;
    m.peerId = peerId;
    m.psid = psid;
    m.name = name;
    m.badges = badges;
    m.issuer = issuer;
    m.officialIssuer = officialIssuer;
    m.connected = true;

    if (m_state == NetSessionState::Game) {
        // The roster is fixed once the game starts. A newcomer watches or is
        // turned away; they never become a player mid-game.
        if (m_settings.lateJoin == NetLateJoin::Refuse) return LobbyDenial::GameInProgress;
        m.spectator = true;
    } else {
        size_t players = 0;
        for (const auto& e : m_members) if (!e.spectator) players++;
        if (players >= m_settings.maxPlayers) return LobbyDenial::SessionFull;
    }

    m_members.push_back(m);
    return LobbyDenial::None;
}

void Lobby::dropOffers(uint16_t peerId) {
    // Any offer this player was part of dies with the connection. Leaving one
    // open would let an offer be accepted by someone who has gone, or be
    // re-pointed at whoever next holds that routing handle.
    m_offers.erase(std::remove_if(m_offers.begin(), m_offers.end(),
                                  [peerId](const Offer& o) {
                                      return o.from == peerId || o.to == peerId;
                                  }),
                   m_offers.end());
}

bool Lobby::reserveSeat(const std::string& psid, const std::string& name,
                        uint16_t countryId) {
    if (psid.empty() || countryId == 0) return false;
    if (findByPsid(psid)) return false;
    if (holderOf(countryId)) return false;

    LobbyMember m;
    // peerId 0 is deliberately not a valid connection handle: this member is a
    // reservation, and admit() will give it a real one when the player arrives.
    m.peerId = 0;
    m.psid = psid;
    m.name = name;
    m.countryId = countryId;
    m.connected = false;
    m_members.push_back(std::move(m));
    return true;
}

bool Lobby::releaseSeat(const std::string& psid) {
    if (psid.empty()) return false;
    for (auto it = m_members.begin(); it != m_members.end(); ++it) {
        if (it->psid != psid) continue;
        if (it->connected) return false;   // that would be a kick, not a release
        dropOffers(it->peerId);
        m_members.erase(it);
        return true;
    }
    return false;
}

void Lobby::ban(const std::string& psid) {
    if (psid.empty()) return;
    if (!isBanned(psid)) m_bans.push_back(psid);
    // Remove them now as well: a ban that only took effect on their next
    // attempt would leave the person sitting there.
    for (auto it = m_members.begin(); it != m_members.end(); ++it) {
        if (it->psid != psid) continue;
        dropOffers(it->peerId);
        m_members.erase(it);
        return;
    }
}

void Lobby::unban(const std::string& psid) {
    for (auto it = m_bans.begin(); it != m_bans.end(); ++it) {
        if (*it == psid) { m_bans.erase(it); return; }
    }
}

bool Lobby::isBanned(const std::string& psid) const {
    for (const auto& b : m_bans) if (b == psid) return true;
    return false;
}

std::vector<const LobbyMember*> Lobby::spectators() const {
    std::vector<const LobbyMember*> out;
    for (const auto& m : m_members) if (m.spectator) out.push_back(&m);
    return out;
}

LobbyDenial Lobby::seatSpectator(uint16_t peerId, uint16_t countryId) {
    if (countryId == 0) return LobbyDenial::NoSuchCountry;
    LobbyMember* me = mutableFind(peerId);
    if (!me) return LobbyDenial::NoSuchPeer;
    if (!me->spectator) return LobbyDenial::NotYours;
    if (holderOf(countryId)) return LobbyDenial::CountryTaken;

    me->spectator = false;
    me->countryId = countryId;
    // They arrive mid-turn owing orders like anybody else.
    me->submitted = false;
    me->orders.clear();
    return LobbyDenial::None;
}

void Lobby::disconnect(uint16_t peerId) {
    if (LobbyMember* m = mutableFind(peerId)) m->connected = false;
    dropOffers(peerId);
}

void Lobby::evict(uint16_t peerId) {
    disconnect(peerId);
    m_members.erase(std::remove_if(m_members.begin(), m_members.end(),
                                   [peerId](const LobbyMember& m) { return m.peerId == peerId; }),
                    m_members.end());
}

// -------------------------------------------------------------- countries ----

LobbyDenial Lobby::claimCountry(uint16_t peerId, uint16_t countryId) {
    if (m_state != NetSessionState::Lobby) return LobbyDenial::NotInLobby;

    LobbyMember* me = mutableFind(peerId);
    if (!me) return LobbyDenial::NoSuchPeer;
    if (me->spectator) return LobbyDenial::Spectator;
    if (countryId == 0) return LobbyDenial::NoSuchCountry;

    if (m_settings.assignment == NetAssignment::HostAssigns && peerId != m_hostPeerId) {
        return LobbyDenial::HostOnly;
    }

    if (const LobbyMember* holder = holderOf(countryId)) {
        // Re-claiming what you already hold is a no-op rather than an error:
        // it is what a client does after a reconnect, and refusing would make
        // that look like a failure.
        if (holder->peerId == peerId) return LobbyDenial::None;
        return LobbyDenial::CountryTaken;
    }

    me->countryId = countryId;
    return LobbyDenial::None;
}

LobbyDenial Lobby::assignCountry(uint16_t byPeerId, uint16_t targetPeerId,
                                 uint16_t countryId) {
    if (m_state != NetSessionState::Lobby) return LobbyDenial::NotInLobby;
    if (byPeerId != m_hostPeerId) return LobbyDenial::HostOnly;

    LobbyMember* target = mutableFind(targetPeerId);
    if (!target) return LobbyDenial::NoSuchPeer;
    if (target->spectator) return LobbyDenial::Spectator;

    // 0 means "take it away", which the host must be able to do.
    if (countryId != 0) {
        if (const LobbyMember* holder = holderOf(countryId)) {
            if (holder->peerId != targetPeerId) return LobbyDenial::CountryTaken;
        }
    }
    target->countryId = countryId;
    return LobbyDenial::None;
}

// ------------------------------------------------------------------ swaps ----

LobbyDenial Lobby::offerSwap(uint16_t fromPeerId, uint16_t toPeerId) {
    if (m_state != NetSessionState::Lobby) return LobbyDenial::NotInLobby;
    if (fromPeerId == toPeerId) return LobbyDenial::SelfSwap;

    const LobbyMember* from = find(fromPeerId);
    const LobbyMember* to = find(toPeerId);
    if (!from || !to) return LobbyDenial::NoSuchPeer;
    if (from->spectator || to->spectator) return LobbyDenial::Spectator;
    if (from->countryId == 0) return LobbyDenial::NotYours;

    // One live offer per pair. Re-offering replaces rather than stacks, so an
    // accept can never apply an offer the player thought they had superseded.
    m_offers.erase(std::remove_if(m_offers.begin(), m_offers.end(),
                                  [&](const Offer& o) {
                                      return o.from == fromPeerId && o.to == toPeerId;
                                  }),
                   m_offers.end());
    m_offers.push_back({fromPeerId, toPeerId});
    return LobbyDenial::None;
}

LobbyDenial Lobby::replySwap(uint16_t toPeerId, uint16_t fromPeerId, bool accept) {
    if (m_state != NetSessionState::Lobby) return LobbyDenial::NotInLobby;

    const auto it = std::find_if(m_offers.begin(), m_offers.end(),
                                 [&](const Offer& o) {
                                     return o.from == fromPeerId && o.to == toPeerId;
                                 });
    if (it == m_offers.end()) return LobbyDenial::NoOffer;
    m_offers.erase(it);

    if (!accept) return LobbyDenial::None;

    LobbyMember* from = mutableFind(fromPeerId);
    LobbyMember* to = mutableFind(toPeerId);
    if (!from || !to) return LobbyDenial::NoSuchPeer;
    if (from->spectator || to->spectator) return LobbyDenial::Spectator;

    // Checked before anything moves. The exchange either happens completely or
    // not at all -- there is no point at which one player holds both countries
    // or neither does.
    if (from->countryId == 0) return LobbyDenial::NotYours;

    std::swap(from->countryId, to->countryId);
    return LobbyDenial::None;
}

// ----------------------------------------------------------------- orders ----

LobbyDenial Lobby::submitOrders(uint16_t peerId, uint32_t turnNumber,
                                const std::vector<uint8_t>& payload) {
    LobbyMember* me = mutableFind(peerId);
    if (!me) return LobbyDenial::NoSuchPeer;
    // A spectator's orders are discarded rather than stored. Keeping them would
    // mean the turn logic had to remember to ignore them later.
    if (me->spectator) return LobbyDenial::Spectator;
    if (m_state != NetSessionState::Game) return LobbyDenial::NotInLobby;

    me->orders = payload;
    me->submitted = true;
    me->submittedTurn = turnNumber;
    me->malformed = false;
    return LobbyDenial::None;
}

LobbyDenial Lobby::withdrawOrders(uint16_t peerId, uint32_t turnNumber) {
    LobbyMember* me = mutableFind(peerId);
    if (!me) return LobbyDenial::NoSuchPeer;
    if (me->spectator) return LobbyDenial::Spectator;
    // Not ready for a turn that already went is not a thing to be.
    if (!me->submitted || me->submittedTurn != turnNumber)
        return LobbyDenial::NotInLobby;

    me->submitted = false;
    me->orders.clear();
    return LobbyDenial::None;
}

LobbyDenial Lobby::markMalformed(uint16_t peerId, uint32_t turnNumber) {
    LobbyMember* me = mutableFind(peerId);
    if (!me) return LobbyDenial::NoSuchPeer;
    if (me->spectator) return LobbyDenial::Spectator;

    // The bad submission replaces nothing: anything readable that arrived
    // earlier for this turn stands, because it is a better representation of
    // what the player wanted than a message we could not parse.
    if (me->submitted && me->submittedTurn == turnNumber && !me->malformed) {
        return LobbyDenial::None;
    }
    me->orders.clear();
    me->submitted = false;
    me->submittedTurn = turnNumber;
    me->malformed = true;
    return LobbyDenial::None;
}

void Lobby::clearSubmissions() {
    for (auto& m : m_members) {
        m.submitted = false;
        m.orders.clear();
        m.submittedTurn = 0;
        m.malformed = false;
    }
}

std::vector<uint16_t> Lobby::missingSubmissions(uint32_t turnNumber) const {
    std::vector<uint16_t> out;
    for (const auto& m : m_members) {
        if (m.spectator || m.countryId == 0) continue;
        // Being disconnected is NOT the same as not having submitted. Someone
        // who sent orders and closed the game has done their part.
        if (!m.submitted || m.submittedTurn != turnNumber) out.push_back(m.peerId);
    }
    return out;
}

// ------------------------------------------------------------------ state ----

std::vector<std::string> Lobby::stillChoosing() const {
    std::vector<std::string> out;
    for (const auto& m : m_members) {
        if (m.spectator || m.countryId != 0) continue;
        out.push_back(m.name.empty() ? "someone" : m.name);
    }
    return out;
}

bool Lobby::start(std::string& why, bool force) {
    if (m_state != NetSessionState::Lobby) {
        why = "The game has already started.";
        return false;
    }
    if (!force) {
        for (const auto& m : m_members) {
            if (m.spectator) continue;
            if (m.countryId == 0) {
                why = m.name.empty() ? "Someone has not picked a country yet."
                                     : m.name + " has not picked a country yet.";
                return false;
            }
        }
    } else {
        // Left behind, but watching rather than stranded: a countryless player
        // would otherwise be counted every turn as somebody the game is waiting
        // for, and they have nothing to submit.
        for (auto& m : m_members)
            if (!m.spectator && m.countryId == 0) m.spectator = true;
    }
    m_state = NetSessionState::Game;
    m_offers.clear();
    return true;
}

void Lobby::returnToLobby() {
    m_state = NetSessionState::Lobby;
    m_offers.clear();
    clearSubmissions();
    for (auto& m : m_members) {
        m.countryId = 0;
        // A spectator who sat out a game is a player again in the new lobby;
        // the reason they were spectating was that a game was running.
        m.spectator = false;
    }
}

std::vector<NetPeer> Lobby::roster() const {
    std::vector<NetPeer> out;
    out.reserve(m_members.size());
    for (const auto& m : m_members) {
        NetPeer p;
        p.peerId = m.peerId;
        p.psid = m.psid;
        p.name = m.name;
        p.badges = m.badges;
        p.officialIssuer = m.officialIssuer;
        p.countryId = m.countryId;
        p.connected = m.connected;
        p.spectator = m.spectator;
        p.submitted = m.submitted;
        out.push_back(p);
    }
    return out;
}
