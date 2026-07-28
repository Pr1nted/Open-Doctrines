#pragma once

// The multiplayer wire format.
//
// DELIBERATELY FREE OF raylib AND OF Game.h. Everything here is bytes in and
// bytes out, which is what lets it be tested headlessly against hostile input
// -- and this is the layer that parses hostile input, so that matters more
// here than anywhere else in the game. Order payloads, which need the game's
// own structs, are opaque blobs at this level; see NetOrders.h.
//
// FRAMING
//
// There is none. A WebSocket already preserves message boundaries, so one
// message is one frame: `u16 type | u32 length | payload`, little-endian.
// Length is redundant with the WebSocket's own framing and is carried anyway,
// because a length that disagrees with the transport is a cheap way to catch a
// truncation the transport did not notice.
//
// WHAT FLOWS WHICH WAY
//
// Client to server: intentions. Never state.
// Server to client: state. Never a request to compute anything.
//
// That asymmetry is the authority model, expressed as a message list. There is
// no message a client can send that changes the world directly, and no message
// a server sends that asks a client what happened -- so a modified client has
// nothing to lie about that anyone would believe.

#include <cstdint>
#include <string>
#include <vector>

// Bumped on any incompatible change. A mismatch is refused at HELLO with a
// message naming both versions, because "connection closed" is a bad way to
// learn you need to update.
inline constexpr uint16_t kNetProtocolVersion = 1;

enum class NetMsg : uint16_t {
    // ---- client -> server -------------------------------------------------
    Hello    = 1,   // first frame; carries the join ticket and the mod set
    Orders   = 2,   // this player's intentions for a turn
    Ready    = 3,   // "I have applied that delta" / lobby readiness
    Chat     = 4,
    Ping     = 5,
    ClaimCountry = 6,   // lobby: "I want this one"
    SwapOffer    = 7,   // lobby: offer mine to another player
    SwapReply    = 8,   // lobby: accept or decline an offer

    // ---- server -> client -------------------------------------------------
    Welcome  = 64,
    Reject   = 65,  // always the last frame before a close
    Snapshot = 66,  // the whole world, for a joiner
    Delta    = 67,  // one turn's changes, in the .odsv delta format
    Roster   = 68,
    TurnBegin= 69,
    Kick     = 70,
    Pong     = 71,
    ChatFrom = 72,  // chat, attributed by the server
    LobbyState   = 73,  // whole lobby: settings, roster, who holds what
    SwapProposed = 74,  // server -> the player being asked
    Notice       = 75,  // "AI played X's turn because ..."
    Countries    = 76,  // the catalogue a lobby picks from
    Signal       = 77,  // WebRTC offer/answer/candidate, both directions
};

const char* netMsgName(NetMsg m);

// Why a join was refused. An enum as well as a sentence, so the client can act
// (offer an update, open the mod menu) instead of only displaying text.
enum class NetReject : uint16_t {
    Unknown            = 0,
    ProtocolVersion    = 1,
    GameVersion        = 2,
    BadTicket          = 3,
    ModMismatch        = 4,
    SessionFull        = 5,
    Banned             = 6,
    GameInProgress     = 7,   // and this server does not take spectators
    ServerShuttingDown = 8,
    /** The ticket came from an account service this server does not accept. */
    IssuerNotAccepted  = 9,
    /**
     * The server did not say who is hosting it.
     *
     * A refusal rather than a warning: an undeclared host is a
     * misconfiguration or someone hiding, and neither is worth a stranger's
     * evening. Hosting anonymously is fine, but it has to be STATED.
     */
    HostNotDeclared    = 10,
};

const char* netRejectName(NetReject r);

// --------------------------------------------------------------- writer ----

class NetWriter {
public:
    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void i32(int32_t v)   { u32(static_cast<uint32_t>(v)); }
    void f32(float v);
    void f64(double v);

    // Length-prefixed (u32) UTF-8. Not null-terminated.
    void str(const std::string& s);
    void bytes(const void* data, size_t n);
    void blob(const std::vector<uint8_t>& v);   // length-prefixed (u32)

    const std::vector<uint8_t>& data() const { return m_data; }
    std::vector<uint8_t> take() { return std::move(m_data); }

private:
    std::vector<uint8_t> m_data;
};

// --------------------------------------------------------------- reader ----
//
// Every read is bounds-checked, and the FIRST failure latches. After that
// every subsequent read returns a zero value and ok() stays false, so a
// decoder can read a whole message straight through and check once at the end
// rather than after every field. Code that has to remember to check cannot be
// relied on to.

class NetReader {
public:
    NetReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
    explicit NetReader(const std::vector<uint8_t>& v)
        : m_data(v.data()), m_size(v.size()) {}

    uint8_t  u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int32_t  i32() { return static_cast<int32_t>(u32()); }
    float    f32();
    double   f64();

    // `maxLen` is mandatory, not defaulted. A length taken from the wire and
    // used to size an allocation is the classic remote memory exhaustion, and
    // making every call site name its own ceiling is what stops one being
    // forgotten.
    std::string str(uint32_t maxLen);
    std::vector<uint8_t> blob(uint32_t maxLen);

    bool   ok() const { return m_ok; }
    size_t remaining() const { return m_ok ? m_size - m_pos : 0; }

    // True when the message was fully consumed and nothing failed. Trailing
    // bytes are treated as a decode failure: they mean the sender and this
    // build disagree about the shape, and guessing which of us is right is how
    // a parser ends up with a confused-deputy bug.
    bool done() const { return m_ok && m_pos == m_size; }

private:
    bool want(size_t n);

    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;
    bool   m_ok = true;
};

// ---------------------------------------------------------------- frame ----

// Largest frame accepted from anyone, before the per-direction limits the
// relay also applies. A snapshot of a big map is the only thing that comes
// near it.
inline constexpr uint32_t kNetMaxFrameBytes = 16u * 1024 * 1024;

std::vector<uint8_t> netEncodeFrame(NetMsg type, const std::vector<uint8_t>& payload);

// Splits a received message into its type and payload. False if the header is
// short, the length disagrees with the transport, or the frame is oversized.
bool netDecodeFrame(const uint8_t* data, size_t size,
                    NetMsg& type, const uint8_t*& payload, size_t& payloadSize);

// --------------------------------------------------------------- limits ----
//
// Field ceilings, in one place so the encoder and the decoder cannot disagree
// about them and so a reviewer can see the whole attack surface at once.
struct NetLimits {
    static constexpr uint32_t kTicket      = 4096;
    static constexpr uint32_t kVersion     = 64;
    static constexpr uint32_t kModAttest   = 128 * 1024;
    static constexpr uint32_t kName        = 64;
    static constexpr uint32_t kPsid        = 64;
    static constexpr uint32_t kBadges      = 256;
    static constexpr uint32_t kChat        = 512;
    static constexpr uint32_t kReason      = 512;
    static constexpr uint32_t kNotice      = 1024;
    static constexpr uint32_t kIssuer      = 256;
    static constexpr uint32_t kSessionName = 64;
    static constexpr uint32_t kOrders      = 1024 * 1024;
    static constexpr uint32_t kWorld       = kNetMaxFrameBytes;
    static constexpr uint32_t kRoster      = 64;      // entries
    // A world can legitimately have a lot of countries; this is a ceiling
    // on a list from a host, not a design limit on maps.
    static constexpr uint32_t kCountries   = 4096;    // entries
    // SDP is the largest thing here and is a few KB at most; an ICE
    // candidate is a single line.
    static constexpr uint32_t kSignal      = 16 * 1024;
};

/** What the session is doing. Joining is only open in Lobby. */
enum class NetSessionState : uint8_t { Lobby = 0, Game = 1, Ended = 2 };

/** How countries get handed out. */
enum class NetAssignment : uint8_t { HostAssigns = 0, PlayersPick = 1 };

/** What happens to someone who arrives after the lobby closed. */
enum class NetLateJoin : uint8_t { Refuse = 0, Spectate = 1 };

/** What happens to a country whose player submitted nothing. */
enum class NetAbsent : uint8_t { Ai = 0, Idle = 1 };

/** Why the server played a country instead of its player. */
enum class NetSubstitution : uint8_t {
    None = 0,
    Malformed = 1,      // the submission did not decode, or failed its MAC
    NotSubmitted = 2,   // nothing arrived before the deadline
    Disconnected = 3,
};

const char* netSubstitutionReason(NetSubstitution s);

// -------------------------------------------------------------- messages ----

struct NetHello {
    uint16_t    protocolVersion = kNetProtocolVersion;
    std::string gameVersion;
    std::string ticket;       // consumed by the relay; never reaches the host
    std::string modAttestation;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetHello& out);
};

// One player, as the server describes them to everyone. Note what is NOT here:
// no account id, no provider identity. `psid` is a pseudonym that means
// something on this server and nowhere else.
struct NetPeer {
    uint16_t    peerId = 0;
    std::string psid;
    std::string name;
    std::string badges;        // comma-separated; empty when withheld
    bool        officialIssuer = false;
    uint16_t    countryId = 0; // 0 until they have picked one
    bool        connected = true;
    bool        spectator = false;
    /**
     * Whether orders for the current turn are in.
     *
     * Held by the SERVER against the psid, so it survives the player closing
     * the game between turns -- which with a long interval is normal rather
     * than exceptional.
     */
    bool        submitted = false;
};

/**
 * Who is running this server.
 *
 * Sent in WELCOME so a joining player can decide whether to trust the evening
 * to them. `verified` is false for an anonymous host AND for one vouched for by
 * a non-official account service; the two are different and the client says
 * which, but neither may be presented as the official issuer would be.
 */
struct NetHostIdentity {
    std::string psid;
    std::string name;      // empty when hosting anonymously
    std::string badges;
    std::string issuer;
    bool        verified = false;

    /** True when the server said nothing at all, which is grounds to refuse. */
    bool declared() const { return !issuer.empty(); }
};

struct NetWelcome {
    uint16_t    peerId = 0;
    std::string sessionName;
    uint16_t    turnSeconds = 0;      // 0 = not a rapid game
    uint32_t    turnNumber = 0;
    bool        showBadges = true;
    std::string issuer;               // who vouched for identities here
    std::string authNotice;           // non-empty when that is not the official one
    std::string requiredMods;

    /**
     * The map this game is played on, by name.
     *
     * Carried in the WELCOME rather than waiting for a snapshot so a player who
     * does not have that map is told which one they need while still in the
     * lobby -- instead of after the game starts, when everyone else has gone.
     */
    std::string mapName;

    NetSessionState state = NetSessionState::Lobby;
    NetAssignment   assignment = NetAssignment::PlayersPick;
    NetLateJoin     lateJoin = NetLateJoin::Spectate;
    NetAbsent       absent = NetAbsent::Ai;
    NetHostIdentity host;
    bool            spectator = false;   // what YOU were admitted as

    std::vector<NetPeer> roster;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetWelcome& out);
};

struct NetRejectMsg {
    NetReject   reason = NetReject::Unknown;
    std::string text;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetRejectMsg& out);
};

// Snapshot and Delta share a shape: a turn number and an opaque payload in the
// save system's own format. Reusing that format rather than inventing a second
// one is the single biggest simplification in this design -- it is already
// compact, already tested, and already the description of "what changed".
struct NetWorld {
    uint32_t             turnNumber = 0;
    std::vector<uint8_t> payload;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetWorld& out);
};

struct NetOrdersMsg {
    uint32_t             turnNumber = 0;
    std::vector<uint8_t> payload;    // see NetOrders.h

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetOrdersMsg& out);
};

struct NetTurnBegin {
    uint32_t turnNumber = 0;
    uint32_t deadlineMs = 0;         // 0 = no deadline (long-form play)

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetTurnBegin& out);
};

struct NetRosterMsg {
    std::vector<NetPeer> peers;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetRosterMsg& out);
};

/** Lobby snapshot. Sent whole rather than as deltas: it is small, it changes
 *  rarely, and a lobby that disagrees with the server about who holds what is
 *  worse than one that redraws. */
struct NetLobbyState {
    NetSessionState state = NetSessionState::Lobby;
    NetAssignment   assignment = NetAssignment::PlayersPick;
    std::vector<NetPeer> roster;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetLobbyState& out);
};

/**
 * One step of a peer-to-peer handshake.
 *
 * WHY THIS IS IN THE PROTOCOL AT ALL
 *
 * The fallback exists for a player who cannot reach the host directly. So the
 * offer/answer exchange cannot itself go over a direct connection -- there is
 * not one. It goes through the account service, which forwards these opaque
 * blobs between two peers and reads none of them.
 *
 * That is matchmaking, not game traffic: a handful of small messages to arrange
 * a connection, after which the media path is peer to peer and Cloudflare is
 * out of it again. The distinction matters, and it is the reason this is
 * acceptable when a relay was not.
 *
 * `payload` is SDP or an ICE candidate. It is opaque here on purpose -- the
 * protocol layer has no business parsing it, and whatever ICE implementation
 * sits underneath owns its own format.
 */
struct NetSignal {
    enum class Kind : uint8_t { Offer = 0, Answer = 1, Candidate = 2, Failed = 3 };

    Kind        kind = Kind::Offer;
    /** Who this is to or from. The server fills it in on the way through. */
    uint16_t    peerId = 0;
    std::string payload;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetSignal& out);
};

/**
 * Server -> client: which countries this world has.
 *
 * The client cannot work this out for itself while it is still in the lobby --
 * it has not loaded the map, and loading one to draw a list would cost tens of
 * megabytes and several seconds to answer "what can I pick". So the host, which
 * HAS the world open, says.
 *
 * Sent once, right after the WELCOME.
 */
struct NetCountryList {
    struct Entry {
        uint16_t    id = 0;
        std::string name;
    };
    std::vector<Entry> countries;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetCountryList& out);
};

/** Client -> server: "I want this country." The server may refuse. */
struct NetClaimCountry {
    uint16_t countryId = 0;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetClaimCountry& out);
};

/**
 * A country swap, in one message rather than two independent claims.
 *
 * Two "set my country" messages could interleave into both players holding one
 * country, or neither. The server performs the exchange atomically or not at
 * all.
 */
struct NetSwap {
    uint16_t fromPeerId = 0;   // set by the server on the way out
    uint16_t toPeerId = 0;
    bool     accepted = false; // SwapReply only

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetSwap& out);
};

/** Server -> everyone: something was done on a player's behalf, and why. */
struct NetNotice {
    uint16_t        countryId = 0;
    NetSubstitution reason = NetSubstitution::None;
    std::string     text;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetNotice& out);
};

struct NetChat {
    uint16_t    fromPeerId = 0;      // ignored on the way up; set by the server
    std::string text;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetChat& out);
};
