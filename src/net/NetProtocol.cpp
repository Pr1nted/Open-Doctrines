#include "NetProtocol.h"

#include <cstring>

// Little-endian on the wire, written byte by byte rather than by memcpy of a
// native word. Every platform this ships on is little-endian today, and the
// day one is not, a silent corruption is a far worse failure than the few
// nanoseconds this costs.

const char* netMsgName(NetMsg m) {
    switch (m) {
        case NetMsg::Hello:     return "Hello";
        case NetMsg::Orders:    return "Orders";
        case NetMsg::Ready:     return "Ready";
        case NetMsg::Chat:      return "Chat";
        case NetMsg::Ping:      return "Ping";
        case NetMsg::Welcome:   return "Welcome";
        case NetMsg::Reject:    return "Reject";
        case NetMsg::Snapshot:  return "Snapshot";
        case NetMsg::Delta:     return "Delta";
        case NetMsg::Roster:    return "Roster";
        case NetMsg::TurnBegin: return "TurnBegin";
        case NetMsg::Kick:      return "Kick";
        case NetMsg::Pong:      return "Pong";
        case NetMsg::ChatFrom:  return "ChatFrom";
        case NetMsg::ClaimCountry: return "ClaimCountry";
        case NetMsg::SwapOffer:    return "SwapOffer";
        case NetMsg::SwapReply:    return "SwapReply";
        case NetMsg::LobbyState:   return "LobbyState";
        case NetMsg::SwapProposed: return "SwapProposed";
        case NetMsg::Notice:       return "Notice";
        case NetMsg::Countries:    return "Countries";
        case NetMsg::Signal:       return "Signal";
    }
    return "Unknown";
}

const char* netSubstitutionReason(NetSubstitution s) {
    switch (s) {
        case NetSubstitution::None:         return "";
        case NetSubstitution::Malformed:    return "their orders could not be read";
        case NetSubstitution::NotSubmitted: return "they did not submit orders in time";
        case NetSubstitution::Disconnected: return "they were not connected";
    }
    return "";
}

const char* netRejectName(NetReject r) {
    switch (r) {
        case NetReject::Unknown:            return "Unknown";
        case NetReject::ProtocolVersion:    return "ProtocolVersion";
        case NetReject::GameVersion:        return "GameVersion";
        case NetReject::BadTicket:          return "BadTicket";
        case NetReject::ModMismatch:        return "ModMismatch";
        case NetReject::SessionFull:        return "SessionFull";
        case NetReject::Banned:             return "Banned";
        case NetReject::GameInProgress:     return "GameInProgress";
        case NetReject::ServerShuttingDown: return "ServerShuttingDown";
        case NetReject::IssuerNotAccepted:  return "IssuerNotAccepted";
        case NetReject::HostNotDeclared:    return "HostNotDeclared";
    }
    return "Unknown";
}

// --------------------------------------------------------------- writer ----

void NetWriter::u8(uint8_t v) { m_data.push_back(v); }

void NetWriter::u16(uint16_t v) {
    m_data.push_back(static_cast<uint8_t>(v));
    m_data.push_back(static_cast<uint8_t>(v >> 8));
}

void NetWriter::u32(uint32_t v) {
    for (int i = 0; i < 4; i++) m_data.push_back(static_cast<uint8_t>(v >> (i * 8)));
}

void NetWriter::u64(uint64_t v) {
    for (int i = 0; i < 8; i++) m_data.push_back(static_cast<uint8_t>(v >> (i * 8)));
}

void NetWriter::f32(float v) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v), "float is not 32 bits");
    std::memcpy(&bits, &v, sizeof(bits));
    u32(bits);
}

void NetWriter::f64(double v) {
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(v), "double is not 64 bits");
    std::memcpy(&bits, &v, sizeof(bits));
    u64(bits);
}

void NetWriter::str(const std::string& s) {
    u32(static_cast<uint32_t>(s.size()));
    bytes(s.data(), s.size());
}

void NetWriter::bytes(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    m_data.insert(m_data.end(), p, p + n);
}

void NetWriter::blob(const std::vector<uint8_t>& v) {
    u32(static_cast<uint32_t>(v.size()));
    bytes(v.data(), v.size());
}

// --------------------------------------------------------------- reader ----

bool NetReader::want(size_t n) {
    if (!m_ok) return false;
    // m_size - m_pos rather than m_pos + n, which could wrap on a huge n.
    if (n > m_size - m_pos) { m_ok = false; return false; }
    return true;
}

uint8_t NetReader::u8() {
    if (!want(1)) return 0;
    return m_data[m_pos++];
}

uint16_t NetReader::u16() {
    if (!want(2)) return 0;
    uint16_t v = static_cast<uint16_t>(m_data[m_pos]) |
                 static_cast<uint16_t>(m_data[m_pos + 1] << 8);
    m_pos += 2;
    return v;
}

uint32_t NetReader::u32() {
    if (!want(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(m_data[m_pos + i]) << (i * 8);
    m_pos += 4;
    return v;
}

uint64_t NetReader::u64() {
    if (!want(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(m_data[m_pos + i]) << (i * 8);
    m_pos += 8;
    return v;
}

float NetReader::f32() {
    uint32_t bits = u32();
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

double NetReader::f64() {
    uint64_t bits = u64();
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::string NetReader::str(uint32_t maxLen) {
    const uint32_t len = u32();
    if (!m_ok) return {};
    // The ceiling is checked BEFORE the bounds check, so a declared length of
    // four billion is refused rather than merely failing to fit.
    if (len > maxLen) { m_ok = false; return {}; }
    if (!want(len)) return {};
    std::string s(reinterpret_cast<const char*>(m_data + m_pos), len);
    m_pos += len;
    return s;
}

std::vector<uint8_t> NetReader::blob(uint32_t maxLen) {
    const uint32_t len = u32();
    if (!m_ok) return {};
    if (len > maxLen) { m_ok = false; return {}; }
    if (!want(len)) return {};
    std::vector<uint8_t> v(m_data + m_pos, m_data + m_pos + len);
    m_pos += len;
    return v;
}

// ---------------------------------------------------------------- frame ----

std::vector<uint8_t> netEncodeFrame(NetMsg type, const std::vector<uint8_t>& payload) {
    NetWriter w;
    w.u16(static_cast<uint16_t>(type));
    w.u32(static_cast<uint32_t>(payload.size()));
    w.bytes(payload.data(), payload.size());
    return w.take();
}

bool netDecodeFrame(const uint8_t* data, size_t size,
                    NetMsg& type, const uint8_t*& payload, size_t& payloadSize) {
    if (size < 6 || size > kNetMaxFrameBytes) return false;

    NetReader r(data, size);
    type = static_cast<NetMsg>(r.u16());
    const uint32_t declared = r.u32();
    if (!r.ok()) return false;

    // The WebSocket already told us how long the message is. If the header
    // disagrees, one of the two is lying and there is no safe way to pick.
    if (declared != size - 6) return false;

    payload = data + 6;
    payloadSize = declared;
    return true;
}

// -------------------------------------------------------------- messages ----

std::vector<uint8_t> NetHello::encode() const {
    NetWriter w;
    w.u16(protocolVersion);
    w.str(gameVersion);
    w.str(ticket);
    w.str(modAttestation);
    return w.take();
}

bool NetHello::decode(const uint8_t* data, size_t size, NetHello& out) {
    NetReader r(data, size);
    out.protocolVersion = r.u16();
    out.gameVersion     = r.str(NetLimits::kVersion);
    out.ticket          = r.str(NetLimits::kTicket);
    out.modAttestation  = r.str(NetLimits::kModAttest);
    return r.done();
}

namespace {

void writePeer(NetWriter& w, const NetPeer& p) {
    w.u16(p.peerId);
    w.str(p.psid);
    w.str(p.name);
    w.str(p.badges);
    w.u8(p.officialIssuer ? 1 : 0);
    w.u16(p.countryId);
    w.u8(p.connected ? 1 : 0);
    w.u8(p.spectator ? 1 : 0);
    w.u8(p.submitted ? 1 : 0);
}

NetPeer readPeer(NetReader& r) {
    NetPeer p;
    p.peerId         = r.u16();
    p.psid           = r.str(NetLimits::kPsid);
    p.name           = r.str(NetLimits::kName);
    p.badges         = r.str(NetLimits::kBadges);
    p.officialIssuer = r.u8() != 0;
    p.countryId      = r.u16();
    p.connected      = r.u8() != 0;
    p.spectator      = r.u8() != 0;
    p.submitted      = r.u8() != 0;
    return p;
}

// An enum read off the wire must be clamped, not cast. A value outside the
// range would otherwise become a switch that falls through every case.
template <typename E>
E readEnum(NetReader& r, uint8_t maxValue, E fallback) {
    const uint8_t v = r.u8();
    return v <= maxValue ? static_cast<E>(v) : fallback;
}

// A count taken from the wire, used to size a loop. Capped before the loop, so
// a claimed roster of four billion costs one comparison rather than an
// allocation.
bool readCount(NetReader& r, uint32_t cap, uint32_t& out) {
    out = r.u32();
    if (!r.ok() || out > cap) return false;
    return true;
}

}  // namespace

std::vector<uint8_t> NetWelcome::encode() const {
    NetWriter w;
    w.u16(peerId);
    w.str(sessionName);
    w.u32(turnSeconds);
    w.u32(turnNumber);
    w.u8(showBadges ? 1 : 0);
    w.str(issuer);
    w.str(authNotice);
    w.str(requiredMods);
    w.str(mapName);

    w.u8(static_cast<uint8_t>(state));
    w.u8(static_cast<uint8_t>(assignment));
    w.u8(static_cast<uint8_t>(lateJoin));
    w.u8(static_cast<uint8_t>(absent));
    w.u8(spectator ? 1 : 0);

    w.str(host.psid);
    w.str(host.name);
    w.str(host.badges);
    w.str(host.issuer);
    w.u8(host.verified ? 1 : 0);

    w.u32(static_cast<uint32_t>(roster.size()));
    for (const auto& p : roster) writePeer(w, p);
    return w.take();
}

bool NetWelcome::decode(const uint8_t* data, size_t size, NetWelcome& out) {
    NetReader r(data, size);
    out.peerId       = r.u16();
    out.sessionName  = r.str(NetLimits::kSessionName);
    out.turnSeconds  = r.u32();
    out.turnNumber   = r.u32();
    out.showBadges   = r.u8() != 0;
    out.issuer       = r.str(NetLimits::kIssuer);
    out.authNotice   = r.str(NetLimits::kNotice);
    out.requiredMods = r.str(NetLimits::kModAttest);
    out.mapName      = r.str(NetLimits::kSessionName);

    out.state      = readEnum(r, 2, NetSessionState::Lobby);
    out.assignment = readEnum(r, 1, NetAssignment::PlayersPick);
    out.lateJoin   = readEnum(r, 1, NetLateJoin::Spectate);
    out.absent     = readEnum(r, 1, NetAbsent::Ai);
    out.spectator  = r.u8() != 0;

    out.host.psid     = r.str(NetLimits::kPsid);
    out.host.name     = r.str(NetLimits::kName);
    out.host.badges   = r.str(NetLimits::kBadges);
    out.host.issuer   = r.str(NetLimits::kIssuer);
    out.host.verified = r.u8() != 0;

    uint32_t count = 0;
    if (!readCount(r, NetLimits::kRoster, count)) return false;
    out.roster.clear();
    out.roster.reserve(count);
    for (uint32_t i = 0; i < count && r.ok(); i++) out.roster.push_back(readPeer(r));
    return r.done();
}

std::vector<uint8_t> NetSignal::encode() const {
    NetWriter w;
    w.u8(static_cast<uint8_t>(kind));
    w.u16(peerId);
    w.str(payload);
    return w.take();
}

bool NetSignal::decode(const uint8_t* data, size_t size, NetSignal& out) {
    NetReader r(data, size);
    // Clamped rather than cast: an unknown kind off the wire must not become a
    // value the switch below has never heard of.
    out.kind    = readEnum(r, 3, NetSignal::Kind::Offer);
    out.peerId  = r.u16();
    out.payload = r.str(NetLimits::kSignal);
    return r.done();
}

std::vector<uint8_t> NetModMsg::encode() const {
    NetWriter w;
    w.str(modId);
    w.u16(peerId);
    w.str(payload);
    return w.take();
}

bool NetModMsg::decode(const uint8_t* data, size_t size, NetModMsg& out) {
    NetReader r(data, size);
    out.modId   = r.str(NetLimits::kModId);
    out.peerId  = r.u16();
    out.payload = r.str(NetLimits::kModMsg);
    return r.done();
}

std::vector<uint8_t> NetCountryList::encode() const {
    NetWriter w;
    w.u32(static_cast<uint32_t>(countries.size()));
    for (const Entry& e : countries) {
        w.u16(e.id);
        w.str(e.name);
    }
    return w.take();
}

bool NetCountryList::decode(const uint8_t* data, size_t size, NetCountryList& out) {
    NetReader r(data, size);
    uint32_t count = 0;
    if (!readCount(r, NetLimits::kCountries, count)) return false;
    out.countries.clear();
    out.countries.reserve(count);
    for (uint32_t i = 0; i < count && r.ok(); i++) {
        Entry e;
        e.id = r.u16();
        e.name = r.str(NetLimits::kName);
        out.countries.push_back(std::move(e));
    }
    return r.done();
}

std::vector<uint8_t> NetRejectMsg::encode() const {
    NetWriter w;
    w.u16(static_cast<uint16_t>(reason));
    w.str(text);
    return w.take();
}

bool NetRejectMsg::decode(const uint8_t* data, size_t size, NetRejectMsg& out) {
    NetReader r(data, size);
    // An unrecognised reason code becomes Unknown rather than a decode failure:
    // a newer server explaining itself in a way we do not understand should
    // still get its sentence shown to the player.
    const uint16_t raw = r.u16();
    out.reason = raw <= static_cast<uint16_t>(NetReject::ServerShuttingDown)
        ? static_cast<NetReject>(raw) : NetReject::Unknown;
    out.text = r.str(NetLimits::kReason);
    return r.done();
}

std::vector<uint8_t> NetWorld::encode() const {
    NetWriter w;
    w.u32(turnNumber);
    w.blob(payload);
    return w.take();
}

bool NetWorld::decode(const uint8_t* data, size_t size, NetWorld& out) {
    NetReader r(data, size);
    out.turnNumber = r.u32();
    out.payload    = r.blob(NetLimits::kWorld);
    return r.done();
}

std::vector<uint8_t> NetOrdersMsg::encode() const {
    NetWriter w;
    w.u32(turnNumber);
    w.blob(payload);
    return w.take();
}

bool NetOrdersMsg::decode(const uint8_t* data, size_t size, NetOrdersMsg& out) {
    NetReader r(data, size);
    out.turnNumber = r.u32();
    out.payload    = r.blob(NetLimits::kOrders);
    return r.done();
}

std::vector<uint8_t> NetTurnBegin::encode() const {
    NetWriter w;
    w.u32(turnNumber);
    w.u32(deadlineMs);
    return w.take();
}

bool NetTurnBegin::decode(const uint8_t* data, size_t size, NetTurnBegin& out) {
    NetReader r(data, size);
    out.turnNumber = r.u32();
    out.deadlineMs = r.u32();
    return r.done();
}

std::vector<uint8_t> NetRosterMsg::encode() const {
    NetWriter w;
    w.u32(static_cast<uint32_t>(peers.size()));
    for (const auto& p : peers) writePeer(w, p);
    return w.take();
}

bool NetRosterMsg::decode(const uint8_t* data, size_t size, NetRosterMsg& out) {
    NetReader r(data, size);
    uint32_t count = 0;
    if (!readCount(r, NetLimits::kRoster, count)) return false;
    out.peers.clear();
    out.peers.reserve(count);
    for (uint32_t i = 0; i < count && r.ok(); i++) out.peers.push_back(readPeer(r));
    return r.done();
}

std::vector<uint8_t> NetChat::encode() const {
    NetWriter w;
    w.u16(fromPeerId);
    w.str(text);
    return w.take();
}

bool NetChat::decode(const uint8_t* data, size_t size, NetChat& out) {
    NetReader r(data, size);
    out.fromPeerId = r.u16();
    out.text       = r.str(NetLimits::kChat);
    return r.done();
}

std::vector<uint8_t> NetLobbyState::encode() const {
    NetWriter w;
    w.u8(static_cast<uint8_t>(state));
    w.u8(static_cast<uint8_t>(assignment));
    w.u32(static_cast<uint32_t>(roster.size()));
    for (const auto& p : roster) writePeer(w, p);
    return w.take();
}

bool NetLobbyState::decode(const uint8_t* data, size_t size, NetLobbyState& out) {
    NetReader r(data, size);
    out.state      = readEnum(r, 2, NetSessionState::Lobby);
    out.assignment = readEnum(r, 1, NetAssignment::PlayersPick);

    uint32_t count = 0;
    if (!readCount(r, NetLimits::kRoster, count)) return false;
    out.roster.clear();
    out.roster.reserve(count);
    for (uint32_t i = 0; i < count && r.ok(); i++) out.roster.push_back(readPeer(r));
    return r.done();
}

std::vector<uint8_t> NetClaimCountry::encode() const {
    NetWriter w;
    w.u16(countryId);
    return w.take();
}

bool NetClaimCountry::decode(const uint8_t* data, size_t size, NetClaimCountry& out) {
    NetReader r(data, size);
    out.countryId = r.u16();
    return r.done();
}

std::vector<uint8_t> NetSwap::encode() const {
    NetWriter w;
    w.u16(fromPeerId);
    w.u16(toPeerId);
    w.u8(accepted ? 1 : 0);
    return w.take();
}

bool NetSwap::decode(const uint8_t* data, size_t size, NetSwap& out) {
    NetReader r(data, size);
    out.fromPeerId = r.u16();
    out.toPeerId   = r.u16();
    out.accepted   = r.u8() != 0;
    return r.done();
}

std::vector<uint8_t> NetNotice::encode() const {
    NetWriter w;
    w.u16(countryId);
    w.u8(static_cast<uint8_t>(reason));
    w.str(text);
    return w.take();
}

bool NetNotice::decode(const uint8_t* data, size_t size, NetNotice& out) {
    NetReader r(data, size);
    out.countryId = r.u16();
    out.reason    = readEnum(r, 3, NetSubstitution::None);
    out.text      = r.str(NetLimits::kNotice);
    return r.done();
}
