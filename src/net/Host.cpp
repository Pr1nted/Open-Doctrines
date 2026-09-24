#include "Host.h"
#include "RelayLink.h"
#include "WebSocket.h"

#include "HttpClient.h"
#include "JoinTicket.h"
#include "ModAttest.h"
#include "ChatRules.h"
#include "RateLimit.h"
#include "WsServer.h"

#include <atomic>
#include <deque>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>

namespace {

constexpr int kOpenTimeoutMs = 20000;

// How long a connection may sit without presenting a valid ticket. Generous
// enough for a slow link and a round trip to the account service, short enough
// that opening sockets and saying nothing costs an attacker something.
constexpr long long kAuthTimeoutSeconds = 30;

long long nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * Monotonic seconds, for things that measure ELAPSED time rather than say when.
 *
 * nowSeconds() above is the wall clock: it is the right answer for "is this
 * ticket expired", and the wrong one for a rate limit. It has one-second
 * granularity, so every budget in a sub-second window refills by zero or by a
 * whole second's worth depending on which side of a tick the frame landed; and
 * it steps when the machine syncs its clock, which on a host left running for a
 * tournament is a certainty rather than a hypothetical.
 */
double nowMonotonic() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * A challenge nonce.
 *
 * Unpredictability is the whole job: a nonce someone can guess lets a ticket be
 * minted before the connection exists, which is what binding a ticket to one
 * socket is meant to prevent. `random_device` is the OS entropy source on every
 * platform this ships on.
 *
 * THE LENGTH IS A HARD CONSTRAINT, NOT A PREFERENCE. The nonce makes a round
 * trip through the account service, which stores at most 64 characters of it
 * (`String(body.nonce).slice(0, 64)` in net/src/index.ts), and comes back in a
 * ticket that is read with a 64-character ceiling (kMaxNonce in JoinTicket.cpp).
 * Generate more than 64 and what returns is a TRUNCATION of what was sent, so
 * the comparison fails and nobody can ever join -- which is exactly what
 * happened when this produced 66. 128 bits is ample for a nonce; the room left
 * over is deliberate.
 */
std::string makeNonce() {
    static const char* kHex = "0123456789abcdef";
    std::random_device rd;
    std::string s = "n_";
    for (int i = 0; i < 4; i++) {           // 4 x 32 bits = 128 bits
        const uint32_t v = rd();
        for (int b = 0; b < 8; b++) s += kHex[(v >> (b * 4)) & 0xF];
    }
    return s;                               // "n_" + 32 hex = 34 characters
}

}  // namespace

const char* netHostPhaseName(NetHost::Phase p) {
    switch (p) {
        case NetHost::Phase::Idle:       return "Idle";
        case NetHost::Phase::Opening:    return "Opening";
        case NetHost::Phase::Connecting: return "Connecting";
        case NetHost::Phase::Live:       return "Live";
        case NetHost::Phase::Closed:     return "Closed";
    }
    return "Unknown";
}

// ------------------------------------------------------------------ impl ----

struct NetHost::Impl {
    mutable std::mutex mutex;
    std::atomic<Phase> phase{Phase::Idle};
    std::atomic<bool>  abandon{false};

    Config config;
    Lobby  lobby;
    // Per-peer chat budgets. Lives beside the lobby because the host is the
    // only thing that can enforce them; the RULE is in ChatRules.h so it can be
    // tested without a socket. See tests/net_chat_test.cpp.
    netchat::Gate chatGate;
    // Every client->server frame passes these. See handlePeerMessage.
    netrate::PerPeer frameBudget{25.0, 50.0};              // frames per second
    netrate::PerPeer byteBudget{4.0 * 1024 * 1024,         // bytes per second
                                16.0 * 1024 * 1024};

    std::string errorText;
    std::string code;
    std::string hostPsid;
    std::string hostName;
    std::string hostBadges;

    /** Set once we have connected as host and been accepted. */
    uint16_t hostPeerId = 0;
    /** The opener has a seat to hand over; update() takes it. See takeHostSeat. */
    bool seatHostPending = false;
    uint32_t turnNumber = 0;

    WsServer  server;

    // ── HOSTING THROUGH THE RELAY ──
    //
    // A host normally LISTENS and players dial in. That cannot work for a
    // player in a browser: a Discord Activity may only reach the hosts named in
    // its URL mappings, so nothing it types will open a socket to a home
    // connection or a tunnel. The account service relays for exactly this, and
    // both ends reach it through the mapping the rest of the service uses.
    //
    // The difference is not merely which socket. Over the relay THE RELAY
    // VERIFIES THE TICKET and hands over an identity it has already checked, so
    // the whole HELLO-and-verify path below is not walked at all. What stays
    // identical is everything after admission: the lobby, the frames, the turn
    // logic. That is the point -- one set of rules, two ways in.
    bool      viaRelay = false;
    WebSocket relaySock;
    bool      relaySaidHello = false;
    bool      relaySeated = false;
    std::string relayHello;          ///< minted at open, sent once connected

    /**
     * Relay peer ids live in a tagged range of the WsConnId space.
     *
     * The rest of this file addresses a peer by WsConnId, and a relay peer has
     * no socket of its own to be identified by. Tagging rather than reusing the
     * low range means a relay id can never be mistaken for a listening socket's
     * id even if both were somehow live at once.
     */
    static constexpr WsConnId kRelayTag = 0x80000000u;
    static WsConnId connOfRelay(uint16_t peerId) { return kRelayTag | peerId; }
    static bool     isRelayConn(WsConnId c) { return (c & kRelayTag) != 0; }
    static uint16_t relayOfConn(WsConnId c) { return (uint16_t)(c & 0xFFFFu); }

    /**
     * Send one frame to one peer, whichever way it is attached.
     *
     * Every send in this file goes through here. Under the relay a frame is
     * wrapped with the header the relay routes on; direct, it is the socket's
     * own send. Nothing above this needs to know which.
     */
    void sendToConn(WsConnId conn, const std::vector<uint8_t>& frame) {
        if (!conn) return;
        if (isRelayConn(conn)) {
            const auto wrapped = netrelay::encodeFromHost(
                netrelay::FromHost::ToPeer, relayOfConn(conn),
                frame.data(), frame.size());
            relaySock.send(wrapped);
            return;
        }
        server.send(conn, frame);
    }

    /** Drop one peer, whichever way it is attached. */
    void closePeer(WsConnId conn, const std::string& why) { closePeer(conn, why.c_str()); }
    void closePeer(WsConnId conn, const char* why) {
        if (!conn) return;
        if (isRelayConn(conn)) {
            const std::string reason = why ? why : "";
            const auto wrapped = netrelay::encodeFromHost(
                netrelay::FromHost::Kick, relayOfConn(conn),
                (const uint8_t*)reason.data(), reason.size());
            relaySock.send(wrapped);
            return;
        }
        server.closeConn(conn, why);
    }

    uint16_t  boundPort = 0;
    std::string listenNote;
    std::thread opener;

    // What is needed to believe a ticket, all of it fetched once at open and
    // then never again: a game keeps running if the account service goes down.
    std::vector<NetIssuerKey> issuerKeys;
    NetIssuerClock            issuerClock;
    NetTicketReplayGuard      replay;

    /**
     * A connection that is up but has not proved anything yet.
     *
     * It holds no seat, no country and no vote. Until a ticket verifies, the
     * only thing the host will do with it is time it out.
     */
    struct Pending {
        WsConnId    conn = 0;
        std::string nonce;
        std::string peerAddress;
        long long   since = 0;
    };
    std::vector<Pending> pending;

    /** conn <-> lobby seat, once authenticated. */
    struct Seated {
        WsConnId conn = 0;
        uint16_t peerId = 0;
        /**
         * When this socket last said anything.
         *
         * A TCP connection that is never written to can stay "open" for a very
         * long time after the machine behind it has gone -- a closed laptop, a
         * dropped tunnel. Without this the seat stays occupied and, with no
         * turn timer, the game waits on somebody who is not there.
         */
        long long lastHeard = 0;
        long long lastPinged = 0;
    };
    std::vector<Seated> seated;

    /** Mod messages addressed to the host's own copy. */
    std::deque<NetModMsg> modInbox;

    void relayModMessage(const NetModMsg& in, uint16_t fromPeerId);

    /** Everything after "who is this", shared by both ways in. */
    void seatPeer(WsConnId conn, const std::string& psid, const std::string& name,
                  const std::string& badges, const std::string& issuer);
    /** Connect, hand over the host ticket, and translate relayed frames. */
    void pumpRelay();
    /**
     * Take the host's own seat, from update(), on the thread that owns the
     * lobby.
     *
     * THE LOBBY HAS NO LOCK, and it never needed one while only update()
     * touched it. The opener thread used to admit the host itself, seconds
     * after the code appeared on screen -- so the frame thread could be walking
     * roster() (drawMpLobby draws it every frame, and "Find players for this
     * game" reads it on a click) while push_back reallocated underneath. That
     * is a crash, and it landed on whoever pressed a button during the two or
     * three seconds a host is "Opening the game...".
     *
     * So the opener only says a seat is due; the seat is taken here, where
     * every other mutation of the lobby already happens. Same rule as the relay
     * socket below: worker threads fetch, update() applies.
     */
    void takeHostSeat();
    /** Seat a peer the relay has already authenticated. */
    void admitRelayPeer(uint16_t relayPeerId, const std::string& identityJson);
    uint16_t nextPeerId = 1;        // 0 is "nobody"

    std::vector<NetHostEvent> events;
    /** Peer id -> the mod set it declared, for the attestation check. */
    std::vector<ModAttestEntry> required;

    /** Sent to every peer after its WELCOME, including late ones. */
    NetCountryList countries;
    std::string    mapName;

    Pending* findPending(WsConnId c) {
        for (Pending& p : pending) if (p.conn == c) return &p;
        return nullptr;
    }
    void dropPending(WsConnId c) {
        for (size_t i = pending.size(); i-- > 0;)
            if (pending[i].conn == c) pending.erase(pending.begin() + static_cast<long>(i));
    }
    WsConnId connFor(uint16_t peerId) const {
        for (const Seated& s : seated) if (s.peerId == peerId) return s.conn;
        return 0;
    }
    uint16_t peerFor(WsConnId c) const {
        for (const Seated& s : seated) if (s.conn == c) return s.peerId;
        return 0;
    }

    void fail(const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (errorText.empty()) errorText = text;
        }
        push({NetHostEvent::Kind::Failed, 0, text, {}});
        phase.store(Phase::Closed);
    }

    void push(NetHostEvent e) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(std::move(e));
    }

    void toPeer(uint16_t peerId, NetMsg type, const std::vector<uint8_t>& payload);
    void broadcast(NetMsg type, const std::vector<uint8_t>& payload);

    NetWelcome welcomeFor(const LobbyMember& m) const;
    void handleConnected(WsConnId conn, const std::string& peerAddress);
    void handleTicket(WsConnId conn, const std::string& text);
    void handleDisconnected(WsConnId conn);
    /** Ping the quiet, drop the silent. */
    void checkLiveness();
    void handlePeerMessage(uint16_t peerId, const uint8_t* body, size_t size);
    void expirePending();
    void broadcastLobbyInternal();
    void flushLobbyBroadcast(double now);
    /// Folded lobby broadcasts. See broadcastLobbyInternal.
    bool   lobbyDirty = false;
    double lastLobbyBroadcast = 0.0;
    static constexpr double kLobbyBroadcastMinGap = 0.25;   // seconds
};

NetHost::NetHost() : m_impl(std::make_unique<Impl>()) {}
NetHost::~NetHost() { close(); }

NetHost::Phase NetHost::phase() const { return m_impl->phase.load(); }

std::string NetHost::error() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->errorText;
}

std::string NetHost::code() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->code;
}

const Lobby& NetHost::lobby() const { return m_impl->lobby; }
Lobby&       NetHost::lobby()       { return m_impl->lobby; }

bool NetHost::nextEvent(NetHostEvent& out) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->events.empty()) return false;
    out = std::move(m_impl->events.front());
    m_impl->events.erase(m_impl->events.begin());
    return true;
}

void NetHost::close() {
    m_impl->abandon.store(true);
    if (m_impl->opener.joinable()) m_impl->opener.join();
    // Closes every player's socket and the listening port with it.
    m_impl->server.stop();
    m_impl->pending.clear();
    m_impl->seated.clear();
    m_impl->phase.store(Phase::Closed);
}

uint16_t NetHost::listenPort() const { return m_impl->boundPort; }

std::string NetHost::listenNote() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->listenNote;
}

NetHost::RelayState NetHost::relayState() const {
    if (!m_impl->viaRelay) return RelayState::NotUsed;
    if (m_impl->relaySeated) return RelayState::Connected;
    if (m_impl->relaySock.state() == WsState::Closed) return RelayState::Failed;
    return RelayState::Connecting;
}

size_t NetHost::unauthenticatedCount() const { return m_impl->pending.size(); }

// ------------------------------------------------------------------ open ----

bool NetHost::open(const Config& config) {
    if (m_impl->phase.load() != Phase::Idle) return false;
    if (config.issuer.empty() || config.token.empty() ||
        config.serverCredential.empty()) {
        m_impl->fail("Sign in and register this server before hosting.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->config = config;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->hostName = config.hostName;
        m_impl->hostBadges = config.hostBadges;
    }
    m_impl->lobby.configure(config.lobby);
    m_impl->phase.store(Phase::Opening);
    m_impl->abandon.store(false);

    m_impl->viaRelay = config.viaRelay;

    // Bind BEFORE talking to the account service. A port that cannot be opened
    // is the most common way hosting fails, and finding out first means the
    // host is told about it instead of registering a session nobody can reach.
    //
    // A relayed host binds nothing: there is no port for anyone to reach, which
    // is the whole point of going that way.
    if (!config.viaRelay) {
        std::string why;
        if (!m_impl->server.listen(config.port, config.bindAll, why)) {
            if (config.portFallback && config.port != 0 &&
                m_impl->server.listen(0, config.bindAll, why)) {
                // Hosting beats holding out for one port number -- but say so,
                // because a forwarded port that moved silently is a game nobody
                // can join for reasons nobody can see.
                m_impl->listenNote =
                    "Port " + std::to_string(config.port) + " was already in use, so this "
                    "game is on port " + std::to_string(m_impl->server.port()) +
                    " instead. If you forwarded " + std::to_string(config.port) +
                    " on your router, or pointed a tunnel at it, update it to " +
                    std::to_string(m_impl->server.port()) + ".";
            } else {
                m_impl->fail(why.empty() ? "Could not listen for players." : why);
                return false;
            }
        }
        m_impl->boundPort = m_impl->server.port();
    }

    // Parse the required mod set once, here, rather than per joining peer.
    ModAttestation mine;
    if (modAttestDecode(config.requiredMods, mine)) {
        m_impl->required = mine.shared();
    }

    m_impl->opener = std::thread([impl = m_impl.get()] {
        Config c;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            c = impl->config;
        }
        const bool local = c.issuer.rfind("http://localhost", 0) == 0 ||
                           c.issuer.rfind("http://127.0.0.1", 0) == 0;

        std::string body = "{\"serverCredential\":\"" + httpJsonEscape(c.serverCredential) +
                           "\",\"settings\":{\"name\":\"" + httpJsonEscape(c.sessionName) +
                           "\",\"listed\":" + (c.listed ? "true" : "false") +
                           ",\"maxPlayers\":" + std::to_string(c.lobby.maxPlayers) +
                           ",\"showBadges\":" + (c.showBadges ? "true" : "false") +
                           ",\"requiredMods\":[]}}";

        HttpRequest req;
        req.method = "POST";
        req.url = c.issuer + "/session";
        req.bearer = c.token;
        req.body = body;
        req.allowInsecure = local;
        req.timeoutMs = kOpenTimeoutMs;
        const HttpResponse res = httpRequest(req);
        if (impl->abandon.load()) return;

        if (!res.ok()) {
            const std::string why = !res.error.empty() ? res.error
                : httpJsonString(res.body, "message", 512);
            impl->fail(why.empty() ? "Could not open a session." : why);
            return;
        }

        const std::string descriptor = httpJsonString(res.body, "descriptor", 4096);
        const std::string code  = httpJsonString(res.body, "code", 32);
        const std::string psid  = httpJsonString(res.body, "hostPsid", 128);
        if (code.empty() || psid.empty()) {
            impl->fail("The account service sent an unusable reply.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->code = code;
            impl->hostPsid = psid;
        }

        // The key every join is checked against. Fetched ONCE, here, and then
        // never again: after this the host can seat players with the account
        // service unreachable, down, or permanently gone.
        //
        // It is also where the issuer's clock is learned, so that a host whose
        // own clock is wrong still admits people. See NetIssuerClock.
        impl->phase.store(Phase::Connecting);

        HttpRequest keys;
        keys.url = c.issuer + "/.well-known/od-keys.json";
        keys.allowInsecure = local;
        keys.timeoutMs = kOpenTimeoutMs;

        std::vector<NetIssuerKey> parsed;
        for (int attempt = 0; attempt < 3 && parsed.empty(); attempt++) {
            if (impl->abandon.load()) return;
            const HttpResponse res = httpRequest(keys);
            if (impl->abandon.load()) return;
            parsed = netParseIssuerKeys(res.body);
            if (!parsed.empty()) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->issuerClock.observe(res.serverTime, nowSeconds());
            } else if (attempt < 2) {
                // One dropped request should not cost somebody their game.
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
        }
        if (parsed.empty()) {
            impl->fail("Could not fetch the account service's verification key, "
                       "so players could not be checked. Check your connection "
                       "and try again.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->issuerKeys = std::move(parsed);
            // A host that plays holds a seat like anyone else, but takes it
            // locally: there is no socket to itself and no ticket to present.
            // A dedicated host holds none, which is what host-only mode is.
            // Either way the seat is taken in update(); see takeHostSeat.
            impl->seatHostPending = true;
        }

        // ── THE HOST'S OWN TICKET, WHEN HOSTING THROUGH THE RELAY ──
        //
        // The relay authenticates the host exactly as it authenticates a
        // player: a ticket minted against a nonce it issued, checked against
        // the account that opened the session. So the host does the same dance
        // a joiner does -- ask /info for a nonce, spend it at /ticket -- and
        // the socket is opened from update(), on the thread that owns it.
        //
        // Minted HERE rather than there because this is already a worker: the
        // frame thread must not block on two round trips.
        if (impl->viaRelay) {
            HttpRequest info;
            info.url = c.issuer + "/session/" + code;
            info.allowInsecure = local;
            info.timeoutMs = kOpenTimeoutMs;
            const HttpResponse infoRes = httpRequest(info);
            if (impl->abandon.load()) return;
            const std::string nonce = httpJsonString(infoRes.body, "nonce", 128);
            if (!infoRes.ok() || nonce.empty()) {
                impl->fail("The relay would not issue a challenge for this session.");
                return;
            }

            HttpRequest mint;
            mint.method = "POST";
            mint.url = c.issuer + "/ticket";
            mint.bearer = c.token;
            mint.allowInsecure = local;
            mint.timeoutMs = kOpenTimeoutMs;
            mint.body = "{\"descriptor\":\"" + httpJsonEscape(descriptor) +
                        "\",\"nonce\":\"" + httpJsonEscape(nonce) + "\"}";
            const HttpResponse tRes = httpRequest(mint);
            if (impl->abandon.load()) return;
            const std::string ticket = httpJsonString(tRes.body, "ticket", 4096);
            if (!tRes.ok() || ticket.empty()) {
                const std::string why = httpJsonString(tRes.body, "message", 512);
                impl->fail(why.empty() ? "The relay refused this server's own ticket."
                                       : why);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->relayHello = netrelay::helloFrame(ticket);
            }
            // NOT Live yet: a relayed host is not open for business until the
            // relay has accepted it. update() finishes the job.
            impl->push({NetHostEvent::Kind::Opened, 0, code, {}});
            return;
        }

        impl->phase.store(Phase::Live);
        impl->push({NetHostEvent::Kind::Opened, 0, code, {}});
    });
    return true;
}

void NetHost::Impl::takeHostSeat() {
    Config c;
    std::string psid, name, badges;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!seatHostPending) return;
        seatHostPending = false;
        c = config;
        psid = hostPsid;
        name = hostName;
        badges = hostBadges;
    }
    const uint16_t seat = c.dedicated ? 0 : nextPeerId++;
    if (seat) {
        lobby.admit(seat, psid, c.anonymous ? "" : name, c.anonymous ? "" : badges,
                    c.issuer, true);
    }
    hostPeerId = seat;
    lobby.setHost(seat);
}

// ---------------------------------------------------------------- update ----

void NetHost::update() {
    Impl& impl = *m_impl;
    const Phase p = impl.phase.load();
    if (p == Phase::Idle || p == Phase::Closed) return;

    // FIRST, so the host holds its seat before any peer is admitted or any
    // event the caller polls afterwards is drawn.
    impl.takeHostSeat();

    impl.server.update();
    impl.pumpRelay();

    WsServerEvent e;
    while (impl.server.nextEvent(e)) {
        switch (e.kind) {
            case WsServerEvent::Kind::Connected:
                impl.handleConnected(e.conn, e.peerAddress);
                break;
            case WsServerEvent::Kind::Text:
                impl.handleTicket(e.conn, e.text);
                break;
            case WsServerEvent::Kind::Binary: {
                // Only an authenticated connection has a seat, so an unseated
                // one is simply not listened to. It cannot act as anybody.
                const uint16_t peerId = impl.peerFor(e.conn);
                if (peerId) {
                    for (auto& s2 : impl.seated)
                        if (s2.conn == e.conn) s2.lastHeard = nowSeconds();
                    impl.handlePeerMessage(peerId, e.data.data(), e.data.size());
                }
                break;
            }
            case WsServerEvent::Kind::Disconnected:
                impl.handleDisconnected(e.conn);
                break;
        }
    }

    impl.expirePending();
    impl.checkLiveness();

    // Any lobby broadcast folded away by the coalescing window goes out here,
    // on the same thread that handles frames. Nothing is dropped -- this is
    // what guarantees a burst still ends in exactly one send.
    impl.flushLobbyBroadcast(nowMonotonic());
}

// -------------------------------------------------------------- addressing ----
//
// Each peer has its own socket now, so "send to one" is the simple case and
// "send to all" is the loop. Under the relay it was the other way round.

// ── SEATING SOMEBODY WHOSE IDENTITY IS ALREADY ESTABLISHED ──
//
// Shared by both ways in, and it is everything AFTER the question "who is
// this". A direct connection answers that by presenting a ticket this host
// verifies; a relayed one arrives with the relay's answer, already checked
// against the same issuer. From here on there is no difference, and there must
// not be one -- a player seated over the relay gets the same welcome, the same
// country list and the same turn-store key as one that dialled in.
void NetHost::Impl::seatPeer(WsConnId conn, const std::string& psid,
                             const std::string& name, const std::string& badges,
                             const std::string& issuer) {
    // A returning player keeps their seat: Lobby matches on the pseudonym and
    // moves the handle, so a reconnect does not cost a country or submitted
    // orders. That is why the psid and not the socket is the identity.
    const uint16_t peerId = nextPeerId++;
    const LobbyDenial denial = lobby.admit(peerId, psid, name, badges,
                                           issuer, issuer == config.issuer);
    if (denial != LobbyDenial::None) {
        NetRejectMsg r;
        r.reason = denial == LobbyDenial::SessionFull       ? NetReject::SessionFull
                 : denial == LobbyDenial::GameInProgress    ? NetReject::GameInProgress
                 : denial == LobbyDenial::IssuerNotAccepted ? NetReject::IssuerNotAccepted
                 : NetReject::Unknown;
        r.text = lobbyDenialText(denial);
        sendToConn(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
        // THE HOST IS THE ONLY PERSON WHO CAN DO ANYTHING ABOUT THIS.
        //
        // A refusal is delivered to the person refused and nowhere else, so a
        // host whose seats are full, whose mod list does not match, or who has
        // banned somebody sees an empty lobby and no sign that anyone tried.
        // "Nobody is joining" and "everybody is being turned away" look
        // identical from here, and only one of them is the host's to fix.
        push({NetHostEvent::Kind::JoinRefused, 0,
              name.empty() ? std::string("Someone") : name, {}});
        closePeer(conn, "refused");
        dropPending(conn);
        return;
    }

    // Whatever seat the lobby settled on -- a fresh one, or the one this
    // pseudonym already held -- is the seat this socket now speaks for.
    uint16_t settled = peerId;
    if (const LobbyMember* m = lobby.findByPsid(psid)) settled = m->peerId;

    // A reconnect supersedes the older socket rather than sitting alongside it.
    for (size_t i = seated.size(); i-- > 0;) {
        if (seated[i].peerId == settled && seated[i].conn != conn) {
            closePeer(seated[i].conn, "reconnected elsewhere");
            seated.erase(seated.begin() + static_cast<long>(i));
        }
    }

    dropPending(conn);
    seated.push_back(Seated{conn, settled, nowSeconds(), 0});

    const LobbyMember* m = lobby.find(settled);
    if (!m) {
        // Admitted, but no seat can be found for it. This should be
        // unreachable; it is handled because the alternative is replying with
        // NOTHING, and a client that is neither welcomed nor refused just hangs
        // until it times out with no idea why. Every path out of here answers.
        NetRejectMsg r;
        r.reason = NetReject::Unknown;
        r.text = "The server could not seat you. Try joining again.";
        sendToConn(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
        closePeer(conn, "unseated");
        dropPending(conn);
        return;
    }

    sendToConn(conn, netEncodeFrame(NetMsg::Welcome, welcomeFor(*m).encode()));
    // Straight after the welcome, so a player can pick a country without
    // having to load the map first.
    if (!countries.countries.empty())
        sendToConn(conn, netEncodeFrame(NetMsg::Countries, countries.encode()));

    // Long-form: where the turns live, and the key to seal orders with. Sent
    // here so a returning player has it before they do anything else -- and
    // gated inside, because a spectator must not receive the key at all.
    if (config.turnSeconds == 0 && config.store != TurnStoreKind::Manual &&
        !m->spectator && m->countryId != 0) {
        NetTurnStoreInfo info;
        info.store       = static_cast<uint8_t>(config.store);
        info.sessionCode = code;
        info.sealKey     = config.sealKey;
        sendToConn(conn, netEncodeFrame(NetMsg::TurnStoreInfo, info.encode()));
    }

    push({NetHostEvent::Kind::PeerJoined, settled, name, {}});
    broadcastLobbyInternal();
}


// ── A PEER THE RELAY HAS ALREADY VOUCHED FOR ──
//
// No ticket is verified here, and that is the trade relay hosting makes. The
// relay checked it against the same issuer this host would have used, burned
// the nonce and the jti, and refuses a banned pseudonym -- then hands over the
// identity it established. A host that re-derived any of that would be
// checking a claim it cannot see the evidence for.
//
// What is NOT trusted is the shape of the document: every field is read with a
// ceiling, and a peer with no pseudonym is not seated at all.
void NetHost::Impl::admitRelayPeer(uint16_t relayPeerId, const std::string& identityJson) {
    const std::string psid   = httpJsonString(identityJson, "psid",   128);
    const std::string name   = httpJsonString(identityJson, "name",   64);
    const std::string issuer = httpJsonString(identityJson, "issuer", 256);
    if (psid.empty()) return;

    // Badges arrive as a JSON array; the lobby wants them comma-separated.
    std::string badges;
    const size_t at = identityJson.find("\"badges\"");
    if (at != std::string::npos) {
        const size_t open = identityJson.find('[', at);
        const size_t close = (open == std::string::npos)
                           ? std::string::npos : identityJson.find(']', open);
        if (open != std::string::npos && close != std::string::npos) {
            std::string one;
            bool inQuotes = false;
            for (size_t i = open + 1; i < close && badges.size() < 256; ++i) {
                const char ch = identityJson[i];
                if (ch == '"') {
                    inQuotes = !inQuotes;
                    if (!inQuotes && !one.empty()) {
                        if (!badges.empty()) badges += ",";
                        badges += one;
                        one.clear();
                    }
                } else if (inQuotes) {
                    one += ch;
                }
            }
        }
    }

    seatPeer(connOfRelay(relayPeerId), psid, name, badges,
             issuer.empty() ? config.issuer : issuer);
}

// ── THE RELAY SIDE OF update() ──
//
// Connect when the ticket is ready, say hello, and then translate. Everything
// after admission is the same code a directly-connected peer goes through --
// only the way a frame arrives differs.
void NetHost::Impl::pumpRelay() {
    if (!viaRelay) return;

    std::string hello;
    {
        std::lock_guard<std::mutex> lock(mutex);
        hello = relayHello;
    }
    if (hello.empty()) return;              // the opener has not minted it yet

    // One connect, from this thread, once.
    if (relaySock.state() == WsState::Idle) {
        std::string iss, c;
        {
            std::lock_guard<std::mutex> lock(mutex);
            iss = config.issuer; c = code;
        }
        const std::string url = netrelay::relayUrl(iss, c, "host");
        if (url.empty()) { fail("This service's address cannot carry a relay."); return; }
        const bool insecure = url.rfind("ws://", 0) == 0;
        if (!relaySock.connect(url, insecure)) {
            fail(relaySock.error().empty() ? "Could not reach the relay."
                                           : relaySock.error());
        }
        return;
    }

    if (relaySock.state() == WsState::Closed) {
        if (phase.load() == Phase::Live) {
            // Losing the relay mid-game is losing every player at once, and
            // pretending otherwise would leave a lobby full of ghosts.
            fail(relaySock.error().empty() ? "The relay connection was lost."
                                           : relaySock.error());
        }
        return;
    }
    if (relaySock.state() != WsState::Open) return;

    if (!relaySaidHello) {
        relaySaidHello = true;
        relaySock.sendText(hello);
        return;                              // the answer arrives next frame
    }

    // The relay answers the hello as text, and says nothing else in text ever.
    std::string text;
    while (relaySock.pollText(text)) {
        uint16_t assigned = 0;
        std::string role;
        if (!netrelay::parseHelloReply(text, assigned, role)) {
            fail("The relay refused this server.");
            return;
        }
        // THE ROLE IS CHECKED, NOT ASSUMED.
        //
        // Going Live on "ok" alone hid a real failure: the relay was granting
        // this socket "player" because the role never reached it, and a host
        // that is not the host is swept as an idle player moments later. The
        // symptom was a session that opened and vanished; the cause was two
        // layers away and invisible from here.
        if (role != "host") {
            fail("The relay seated this server as \"" +
                 (role.empty() ? std::string("(none)") : role) +
                 "\" rather than as the host, so nobody could have joined.");
            return;
        }
        if (!relaySeated) {
            relaySeated = true;
            phase.store(Phase::Live);
        }
    }

    std::vector<uint8_t> frame;
    while (relaySock.poll(frame)) {
        netrelay::Inbound in;
        if (!netrelay::decodeToHost(frame.data(), frame.size(), in)) continue;
        switch (in.kind) {
            case netrelay::ToHost::PeerJoined:
                admitRelayPeer(in.peerId, std::string(in.payload.begin(), in.payload.end()));
                break;
            case netrelay::ToHost::PeerLeft:
                handleDisconnected(connOfRelay(in.peerId));
                break;
            case netrelay::ToHost::Data: {
                const WsConnId conn = connOfRelay(in.peerId);
                const uint16_t seat = peerFor(conn);
                if (!seat) break;            // not seated: not listened to
                for (auto& s2 : seated)
                    if (s2.conn == conn) s2.lastHeard = nowSeconds();
                handlePeerMessage(seat, in.payload.data(), in.payload.size());
                break;
            }
        }
    }
}

void NetHost::Impl::toPeer(uint16_t peerId, NetMsg type,
                           const std::vector<uint8_t>& payload) {
    // The host's own seat has no socket. Its UI reads the lobby directly, so
    // there is nothing to deliver and nothing has gone wrong.
    const WsConnId conn = connFor(peerId);
    if (conn) sendToConn(conn, netEncodeFrame(type, payload));
}

void NetHost::Impl::broadcast(NetMsg type, const std::vector<uint8_t>& payload) {
    const std::vector<uint8_t> frame = netEncodeFrame(type, payload);
    for (const Seated& s : seated) sendToConn(s.conn, frame);
}

// ------------------------------------------------------------------ joins ----

void NetHost::Impl::handleConnected(WsConnId conn, const std::string& peerAddress) {
    // Nothing is granted here. The connection gets a challenge and a deadline;
    // everything else waits on a ticket that verifies.
    Pending p;
    p.conn = conn;
    p.nonce = makeNonce();
    // The account service keeps only the first 64 characters, so anything
    // longer comes back truncated and can never match. Caught here rather than
    // as "nobody can join" with no other symptom.
    if (p.nonce.size() > 64) { closePeer(conn, "internal"); return; }
    p.peerAddress = peerAddress;
    p.since = nowSeconds();
    pending.push_back(p);

    // The nonce binds a ticket to THIS socket. The issuer is named so the
    // client knows who to ask, and refuses if it is not one it accepts.
    server.sendText(conn, "{\"nonce\":\"" + httpJsonEscape(p.nonce) +
                          "\",\"session\":\"" + httpJsonEscape(code) +
                          "\",\"issuer\":\"" + httpJsonEscape(config.issuer) + "\"}");
}

void NetHost::Impl::handleTicket(WsConnId conn, const std::string& text) {
    Pending* p = findPending(conn);
    if (!p) return;              // already seated: a second ticket is ignored

    const std::string token = httpJsonString(text, "ticket", 4096);

    // ---- can we play together at all? --------------------------------------
    //
    // Both of these are checked BEFORE the ticket. Verifying who somebody is
    // costs signature work and burns a single-use jti; refusing them a moment
    // later for speaking the wrong protocol wastes both and leaves them unable
    // to retry without minting a fresh ticket.

    // A version we do not speak. Named in both directions, because "connection
    // closed" is a miserable way to learn you need to update -- and whichever
    // side is older, the person reading it can tell which.
    const long long theirs = httpJsonNumber(text, "protocol", -1);
    if (theirs != (long long)kNetProtocolVersion) {
        NetRejectMsg r;
        r.reason = NetReject::ProtocolVersion;
        r.text = theirs < 0
            ? "That client is too old to say which version it speaks. This "
              "server speaks version " + std::to_string(kNetProtocolVersion) + "."
            : "You speak network version " + std::to_string(theirs) +
              "; this server speaks " + std::to_string(kNetProtocolVersion) +
              ". Whichever of you is older needs to update.";
        sendToConn(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
        closePeer(conn, "protocol");
        dropPending(conn);
        return;
    }

    // A mod set that cannot produce the same world. A mod that changes rules
    // must be on BOTH machines or neither: one side resolving a turn with a
    // rule the other has never heard of is a desync that surfaces later as
    // "the game is broken", with nothing pointing at the cause.
    {
        ModAttestation offered;
        modAttestDecode(httpJsonString(text, "mods", 8192), offered);
        const ModAttestResult verdict = modAttestCompare(required, offered);
        if (!verdict.ok) {
            NetRejectMsg r;
            r.reason = NetReject::ModMismatch;
            r.text = verdict.summary();
            sendToConn(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
            closePeer(conn, "mods");
            dropPending(conn);
            return;
        }
    }

    NetTicketCheck expect;
    expect.issuer = config.issuer;
    expect.audience = "od-relay:" + code;
    expect.nonce = p->nonce;
    expect.now = issuerClock.now(nowSeconds());

    NetJoinTicket ticket;
    if (!netVerifyJoinTicket(token, issuerKeys, expect, ticket)) {
        // One message for every kind of failure. Saying which check failed
        // would let someone probe for live session ids and nonces.
        NetRejectMsg r;
        r.reason = NetReject::Unknown;
        r.text = "That sign-in could not be verified. Try joining again.";
        sendToConn(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
        closePeer(conn, "unverified");
        dropPending(conn);
        return;
    }

    // Single use, so the same live ticket cannot seat two connections at once.
    if (!replay.useOnce(ticket.jti, ticket.expires, expect.now)) {
        NetRejectMsg r;
        r.reason = NetReject::Unknown;
        r.text = "That sign-in was already used. Try joining again.";
        sendToConn(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
        closePeer(conn, "replayed");
        dropPending(conn);
        return;
    }

    std::string badges;
    for (const std::string& b : ticket.badges) {
        if (!badges.empty()) badges += ",";
        badges += b;
    }

    seatPeer(conn, ticket.psid, ticket.name, badges, ticket.issuer);
}

void NetHost::Impl::checkLiveness() {
    // Quiet for this long and we ask; silent for this long and we stop asking.
    // Generous, because a player thinking about a turn sends nothing at all,
    // and being dropped for concentrating would be absurd.
    constexpr long long kQuietSeconds = 20;
    // Settable ONLY so a test can watch a peer be kept alive, or dropped, in
    // seconds rather than in a minute. Nothing in the game sets it.
    static const long long kDeadSeconds = [] {
        if (const char* s = getenv("OD_NET_DEAD_SECONDS")) {
            const long long v = atoll(s);
            if (v > 0) return v;
        }
        return 60LL;
    }();

    const long long now = nowSeconds();
    for (size_t i = seated.size(); i-- > 0;) {
        Seated& s = seated[i];
        // ── WHAT COUNTS AS BEING THERE ──
        //
        // Anything at all on the socket, which is what the transport knows:
        // an application frame, or the answer to a ping. Counting only
        // application frames meant a client that was LISTENING -- the whole of
        // a lobby, and the whole of a world transfer -- was dropped at 60
        // seconds for not speaking, and the host then showed nobody to wait
        // for. Time the host itself spent not pumping does not count either;
        // WsServer::quietSeconds subtracts it.
        //
        // And a peer we are still sending to cannot be silent: the bytes we
        // owe it are the reason it has said nothing.
        //
        // ONLY FOR PEERS THE TRANSPORT HOLDS. A relayed peer's conn id is a
        // tagged number, not one of WsServer's, so quietSeconds() answers 0 for
        // it -- and taking the smaller of the two would make every relayed
        // player immortal, seat and all, however long ago their browser died.
        // What keeps a relayed player alive instead is the session keepalive
        // they send through the relay (see NetSession::update).
        const bool relayed = (s.conn & kRelayTag) != 0;
        const long long quiet = relayed
            ? now - s.lastHeard
            : std::min<long long>(now - s.lastHeard,
                                  (long long)server.quietSeconds(s.conn));

        if (quiet >= kDeadSeconds && (relayed || server.pendingBytes(s.conn) == 0)) {
            // The seat is KEPT: this is a lost connection, not a departure,
            // and the psid still owns that country. Lobby::disconnect marks
            // them away so the turn can stop waiting on them.
            const uint16_t peerId = s.peerId;
            closePeer(s.conn, "silent");
            seated.erase(seated.begin() + static_cast<long>(i));
            lobby.disconnect(peerId);
            chatGate.forget(peerId);
            frameBudget.forget(peerId);
            byteBudget.forget(peerId);
            push({NetHostEvent::Kind::PeerLeft, peerId, "lost connection", {}});
            broadcastLobbyInternal();
            continue;
        }

        // One ping per quiet period, not one per frame.
        if (quiet >= kQuietSeconds && now - s.lastPinged >= kQuietSeconds) {
            s.lastPinged = now;
            sendToConn(s.conn, netEncodeFrame(NetMsg::Pong, {}));
        }
    }
}

void NetHost::Impl::handleDisconnected(WsConnId conn) {
    dropPending(conn);

    const uint16_t peerId = peerFor(conn);
    if (!peerId) return;         // never got a seat: nothing to announce

    for (size_t i = seated.size(); i-- > 0;)
        if (seated[i].conn == conn) seated.erase(seated.begin() + static_cast<long>(i));

    // Disconnected, not evicted: the seat, the country and any orders are kept
    // so the same player can come back to them.
    lobby.disconnect(peerId);
    chatGate.forget(peerId);   // relay handles are reused; see ChatRules.h
    frameBudget.forget(peerId);
    byteBudget.forget(peerId);
    push({NetHostEvent::Kind::PeerLeft, peerId, "", {}});
    broadcastLobbyInternal();
}

void NetHost::Impl::expirePending() {
    const long long now = nowSeconds();
    for (size_t i = pending.size(); i-- > 0;) {
        if (now - pending[i].since <= kAuthTimeoutSeconds) continue;
        closePeer(pending[i].conn, "no ticket");
        pending.erase(pending.begin() + static_cast<long>(i));
    }
    replay.sweep(issuerClock.now(now));
}

// ------------------------------------------------------------------ peers ----

NetWelcome NetHost::Impl::welcomeFor(const LobbyMember& m) const {
    NetWelcome w;
    w.peerId = m.peerId;
    w.sessionName = config.sessionName;
    w.turnSeconds = config.turnSeconds;
    w.turnNumber = turnNumber;
    w.showBadges = config.showBadges;
    w.issuer = config.issuer;
    w.requiredMods = config.requiredMods;
    w.mapName = mapName;
    w.state = lobby.state();
    w.assignment = lobby.settings().assignment;
    w.lateJoin = lobby.settings().lateJoin;
    w.absent = lobby.settings().absent;
    w.spectator = m.spectator;

    // Always declared. An anonymous host still says WHO IT IS NOT -- a blank
    // name with a real issuer reads as "chose not to be named", which is a very
    // different thing from a server that answered nothing.
    w.host.psid = hostPsid;
    w.host.name = config.anonymous ? "" : hostName;
    w.host.badges = config.anonymous ? "" : hostBadges;
    w.host.issuer = config.issuer;
    w.host.verified = !config.anonymous;

    w.roster = lobby.roster();
    return w;
}

void NetHost::Impl::handlePeerMessage(uint16_t peerId, const uint8_t* body, size_t size) {
    // ── A BUDGET IN FRONT OF EVERYTHING, NOT JUST CHAT ──
    //
    // Chat got a limit because it is broadcast. The worse case had none: a
    // MALFORMED Orders frame is the cheapest thing a client can send -- it
    // fails to decode, is recorded, and used to broadcast the whole lobby to
    // every peer. That particular fan-out is coalesced now, but the underlying
    // hole was that nothing counted frames at all, so a peer could drive any
    // handler on this switch as fast as it could write, decode cost and all.
    //
    // Two budgets, because a client can be expensive in two different ways.
    // Both are far above what a real client does: a game sends a handful of
    // frames a turn and a Ping every few seconds, so 25/s sustained with 50 in
    // hand is orders of magnitude of headroom, while a flood is stopped dead.
    // The byte budget exists because one frame may be megabytes -- orders on a
    // large map -- and a hundred of those is a different attack from a hundred
    // small ones.
    //
    // A refused frame is DROPPED, not answered. Telling a flooder why would be
    // a reply per flood frame, which is the thing being prevented; and a client
    // that is merely enthusiastic recovers on its own within a second.
    const double now = nowMonotonic();
    if (!frameBudget.allow(peerId, 1.0, now)) return;
    if (!byteBudget.allow(peerId, (double)size, now)) return;

    NetMsg type;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    if (!netDecodeFrame(body, size, type, payload, payloadSize)) return;

    switch (type) {
        case NetMsg::ClaimCountry: {
            NetClaimCountry c;
            if (!NetClaimCountry::decode(payload, payloadSize, c)) return;
            // Called ONCE. Calling it again to build the message would apply
            // the claim a second time, which for a successful claim is
            // harmless and for a failed one is a different answer.
            const LobbyDenial denial = lobby.claimCountry(peerId, c.countryId);
            if (denial == LobbyDenial::None) {
                broadcastLobbyInternal();
            } else {
                // Told, rather than silently ignored -- a claim that vanishes
                // looks like a broken button.
                NetNotice n;
                n.countryId = c.countryId;
                n.text = lobbyDenialText(denial);
                toPeer(peerId, NetMsg::Notice, n.encode());
            }
            return;
        }
        case NetMsg::SwapOffer: {
            NetSwap s;
            if (!NetSwap::decode(payload, payloadSize, s)) return;
            if (lobby.offerSwap(peerId, s.toPeerId) != LobbyDenial::None) return;
            NetSwap out;
            out.fromPeerId = peerId;      // attributed by us, not by the sender
            out.toPeerId = s.toPeerId;
            toPeer(s.toPeerId, NetMsg::SwapProposed, out.encode());
            return;
        }
        case NetMsg::SwapReply: {
            NetSwap s;
            if (!NetSwap::decode(payload, payloadSize, s)) return;
            if (lobby.replySwap(peerId, s.fromPeerId, s.accepted) == LobbyDenial::None) {
                broadcastLobbyInternal();
            }
            return;
        }
        case NetMsg::Orders: {
            NetOrdersMsg o;
            if (!NetOrdersMsg::decode(payload, payloadSize, o)) {
                // A submission that will not decode is discarded WHOLE, and
                // RECORDED as unreadable -- which is a different state from
                // "nothing arrived" and earns the player a different
                // explanation when the turn resolves.
                lobby.markMalformed(peerId, turnNumber);
                push({NetHostEvent::Kind::OrdersReceived, peerId, "malformed", {}});
                broadcastLobbyInternal();
                return;
            }
            // ── FOR THE TURN THAT IS RUNNING ──
            //
            // The number came straight off the wire and was stored unchecked,
            // so a client that had fallen a turn behind submitted for the turn
            // it thought was current, the lobby recorded it, and the roster
            // told that player their orders were in -- while the turn actually
            // running still counted them missing and let the AI play them.
            // Every turn, invisibly, for as long as they stayed behind.
            if (turnNumber != 0 && o.turnNumber != turnNumber) {
                push({NetHostEvent::Kind::OrdersReceived, peerId, "for another turn", {}});
                return;
            }
            if (lobby.submitOrders(peerId, o.turnNumber, o.payload) == LobbyDenial::None) {
                push({NetHostEvent::Kind::OrdersReceived, peerId, "", {}});
                broadcastLobbyInternal();
            }
            return;
        }
        case NetMsg::Withdraw: {
            NetOrdersMsg o;
            // PAYLOAD, like every other case here. This one was decoding the
            // whole frame, header included, so the length it read was nonsense
            // and the decode always failed: "Withdraw" told the player their
            // orders were taken back and told the host nothing at all, leaving
            // the stale orders to resolve and everybody else waiting on a
            // player the lobby thought had already submitted.
            if (!NetOrdersMsg::decode(payload, payloadSize, o)) return;
            if (lobby.withdrawOrders(peerId, o.turnNumber) == LobbyDenial::None) {
                // Everyone is told, because "waiting for" just changed and the
                // other players are reading exactly that.
                broadcastLobbyInternal();
            }
            return;
        }
        case NetMsg::ModMsg: {
            NetModMsg m;
            if (!NetModMsg::decode(payload, payloadSize, m)) return;
            relayModMessage(m, peerId);
            return;
        }
        case NetMsg::Chat: {
            NetChat c;
            if (!NetChat::decode(payload, payloadSize, c)) return;
            c.fromPeerId = peerId;        // attribution is ours

            // ── THE RULES, BEFORE THE AMPLIFIER ──
            //
            // Every accepted line is sent to every peer, so the host is a fan-
            // out of however many people are in the game. Unrated, that made
            // whoever typed fastest into everyone else's problem. Decided in
            // ChatRules.h, applied here, which is the only place that can.
            netchat::Policy pol = chatGate.policy();
            pol.enabled = lobby.settings().chat;
            chatGate.configure(pol);
            const netchat::Verdict v = chatGate.admit(peerId, c.text, nowMonotonic());
            if (v != netchat::Verdict::Allowed) {
                // TO THE SENDER, AND NOBODY ELSE. Announcing a refusal to the
                // room would hand a flooder the broadcast they were refused,
                // which is the whole thing being prevented.
                const char* why = netchat::explain(v);
                if (*why) {
                    NetChat back;
                    back.fromPeerId = 0;      // 0 = the server speaking
                    back.text = why;
                    toPeer(peerId, NetMsg::ChatFrom, back.encode());
                }
                return;
            }

            NetHostEvent e{NetHostEvent::Kind::Chat, peerId, c.text, c};
            push(std::move(e));
            broadcast(NetMsg::ChatFrom, c.encode());
            return;
        }
        case NetMsg::PlayerReport: {
            NetPlayerReport r;
            if (!NetPlayerReport::decode(payload, payloadSize, r)) return;
            r.fromPeerId = peerId;        // attribution is ours, not theirs
            // Raised to the host's own client and NOWHERE else. Not broadcast,
            // and in particular not sent to the person being reported: a report
            // that announces itself is a report nobody files.
            NetHostEvent e{NetHostEvent::Kind::PlayerReport, peerId, r.reason, {}, r};
            push(std::move(e));
            return;
        }
        default:
            return;
    }
}

// ------------------------------------------------------------- broadcasts ----

void NetHost::Impl::relayModMessage(const NetModMsg& in, uint16_t fromPeerId) {
    NetModMsg out = in;
    out.peerId = fromPeerId;          // attribution is ours, as with chat

    // Addressed to the host's own copy of the mod. The host has no connection
    // to itself, so this is the only way it can be delivered.
    if (in.peerId == lobby.hostPeerId()) {
        modInbox.push_back(out);
        if (modInbox.size() > 256) modInbox.pop_front();
        return;
    }

    const std::vector<uint8_t> frame = netEncodeFrame(NetMsg::ModMsgFrom, out.encode());
    if (in.peerId != NetModMsg::kBroadcast) {
        if (WsConnId c = connFor(in.peerId)) sendToConn(c, frame);
        return;
    }

    // Broadcast reaches the host too -- its copy of the mod is as much a
    // participant as anyone else's.
    modInbox.push_back(out);
    if (modInbox.size() > 256) modInbox.pop_front();
    for (const Seated& s : seated)
        if (s.peerId != fromPeerId) sendToConn(s.conn, frame);
}

// ── COALESCED, BECAUSE A CLIENT CHOOSES HOW OFTEN THIS RUNS ──
//
// Fifteen call sites, and several of them are reached straight from a frame a
// client sent. The cheapest is a MALFORMED Orders message: it fails to decode,
// is recorded, and broadcasts the whole lobby -- roster and all -- to every
// peer, then returns. So the least effort a client can spend is also the
// largest fan-out the host performs, which in an eight-player tournament is
// one bad actor turning a stream of junk into eight roster broadcasts apiece.
//
// The state is a SNAPSHOT, not a stream of edits: sending it once after a burst
// says exactly what sending it thirty times would. So a broadcast that lands
// within the window is folded into a single pending one and flushed by the
// host's own tick. Nothing is dropped -- `dirty` guarantees a send follows --
// only merged.
void NetHost::Impl::broadcastLobbyInternal() {
    lobbyDirty = true;
    const double now = nowMonotonic();
    // The first one goes out immediately: a lobby that takes a beat to show a
    // player who just joined feels broken, and the burst is what needs damping,
    // not the first event.
    if (now - lastLobbyBroadcast < kLobbyBroadcastMinGap) return;
    flushLobbyBroadcast(now);
}

void NetHost::Impl::flushLobbyBroadcast(double now) {
    if (!lobbyDirty) return;
    lobbyDirty = false;
    lastLobbyBroadcast = now;

    NetLobbyState s;
    s.state = lobby.state();
    s.assignment = lobby.settings().assignment;
    s.roster = lobby.roster();
    broadcast(NetMsg::LobbyState, s.encode());
    push({NetHostEvent::Kind::LobbyChanged});
}

void NetHost::broadcastLobby() { m_impl->broadcastLobbyInternal(); }

void NetHost::sendModMessage(const std::string& modId, int32_t toPeer,
                             const std::vector<uint8_t>& payload) {
    if (payload.size() > NetLimits::kModMsg) return;
    NetModMsg m;
    m.modId   = modId;
    m.peerId  = toPeer < 0 ? NetModMsg::kBroadcast : (uint16_t)toPeer;
    m.payload.assign(payload.begin(), payload.end());
    // Sent as if it had arrived from the host's own peer id, so a message the
    // host sends and one a player sends travel the same path.
    m_impl->relayModMessage(m, m_impl->lobby.hostPeerId());
}

bool NetHost::nextModMessage(NetModMsg& out) {
    if (m_impl->modInbox.empty()) return false;
    out = std::move(m_impl->modInbox.front());
    m_impl->modInbox.pop_front();
    return true;
}

bool NetHost::startGame(std::string& why, bool force) {
    if (!m_impl->lobby.start(why, force)) return false;
    m_impl->broadcastLobbyInternal();
    return true;
}

void NetHost::setCountries(const NetCountryList& list) {
    m_impl->countries = list;
    // AND THE LOBBY IS TOLD, from the one place the list is set.
    //
    // This list is what the picker draws; the lobby is what decides a claim.
    // While only the first knew, a country left out of the list was left out
    // of the UI and nowhere else -- asking for it by id worked. Forwarding
    // here rather than at the call sites is what keeps the two from drifting.
    std::vector<uint16_t> playable;
    playable.reserve(list.countries.size());
    for (const auto& e : list.countries) playable.push_back(e.id);
    m_impl->lobby.setPlayableCountries(std::move(playable));
    // Anyone already here gets it now; anyone arriving later gets it with
    // their welcome.
    const std::vector<uint8_t> frame = netEncodeFrame(NetMsg::Countries, list.encode());
    for (const auto& s : m_impl->seated) m_impl->sendToConn(s.conn, frame);
}

void NetHost::setMapName(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->mapName = name;
}

void NetHost::returnToLobby() {
    m_impl->lobby.returnToLobby();
    m_impl->broadcastLobbyInternal();
}

void NetHost::beginTurn(uint32_t turnNumber, uint32_t deadlineMs) {
    m_impl->turnNumber = turnNumber;
    m_impl->lobby.clearSubmissions();
    NetTurnBegin t{turnNumber, deadlineMs};
    m_impl->broadcast(NetMsg::TurnBegin, t.encode());
    m_impl->broadcastLobbyInternal();
}

void NetHost::broadcastDelta(uint32_t turnNumber, const std::vector<uint8_t>& payload) {
    NetWorld w;
    w.turnNumber = turnNumber;
    w.payload = payload;
    m_impl->broadcast(NetMsg::Delta, w.encode());
}

void NetHost::broadcastTurnOrders(uint32_t turnNumber,
                                 const std::vector<uint8_t>& payload) {
    NetTurnOrders t;
    t.turnNumber = turnNumber;
    t.payload = payload;
    m_impl->broadcast(NetMsg::TurnOrders, t.encode());
}

bool NetHost::sendSnapshot(uint16_t peerId, uint32_t turnNumber,
                           const std::vector<uint8_t>& payload) {
    NetWorld w;
    w.turnNumber = turnNumber;
    w.payload = payload;
    const std::vector<uint8_t> frame = w.encode();
    // The frame carries a 6-byte header on top of this, and the ceiling is on
    // the whole frame. Checked here rather than at the socket because this is
    // the last place that knows WHO it was for.
    if (frame.size() + 6 > kNetMaxFrameBytes) {
        m_impl->push({NetHostEvent::Kind::Failed, peerId,
                      "This game has grown too large to send to a joining player (" +
                      std::to_string(frame.size() / (1024 * 1024)) + " MB, and " +
                      std::to_string(kNetMaxFrameBytes / (1024 * 1024)) +
                      " MB is the most that can be sent). They cannot be let in.", {}});
        return false;
    }
    m_impl->toPeer(peerId, NetMsg::Snapshot, frame);
    return true;
}

void NetHost::sendTurnStoreInfo(uint16_t peerId) {
    // Long-form only: a rapid game has no store, and turnSeconds is what says
    // which this is.
    if (m_impl->config.turnSeconds != 0) return;
    // Manual has no address to give out and no key to distribute -- the player
    // carries the bytes themselves.
    if (m_impl->config.store == TurnStoreKind::Manual) return;

    // A SPECTATOR NEVER GETS THE KEY. It opens every player's orders and a
    // spectator has none of their own to submit, so there is no reading of
    // "watching the game" that requires it.
    const LobbyMember* m = m_impl->lobby.find(peerId);
    if (!m || m->spectator || m->countryId == 0) return;

    NetTurnStoreInfo info;
    info.store       = static_cast<uint8_t>(m_impl->config.store);
    info.sessionCode = m_impl->code;
    info.sealKey     = m_impl->config.sealKey;
    m_impl->toPeer(peerId, NetMsg::TurnStoreInfo, info.encode());
}

void NetHost::announceSubstitution(uint16_t countryId, NetSubstitution reason,
                                   const std::string& text) {
    // Always announced. A player must never learn from the map that something
    // else moved their armies.
    NetNotice n;
    n.countryId = countryId;
    n.reason = reason;
    n.text = text.empty() ? netSubstitutionReason(reason) : text;
    m_impl->broadcast(NetMsg::Notice, n.encode());
}

void NetHost::sendChat(const std::string& text) {
    if (text.empty()) return;
    // Attributed to the host's own peer id, through the same ChatFrom message a
    // relayed player line uses. Clients then render it with whatever they show
    // for the host rather than needing to learn a second kind of chat -- which
    // is what makes `say` from a dedicated server's console arrive looking like
    // something a person said, because it is.
    NetChat c;
    c.fromPeerId = m_impl->lobby.hostPeerId();
    c.text = text;
    m_impl->broadcast(NetMsg::ChatFrom, c.encode());
}

void NetHost::kick(uint16_t peerId, const std::string& reason) {
    NetRejectMsg r;
    r.text = reason;
    m_impl->toPeer(peerId, NetMsg::Kick, r.encode());
    // Told first, then disconnected: a kick that arrives as a dead socket is
    // indistinguishable from the game crashing.
    if (const WsConnId conn = m_impl->connFor(peerId)) {
        m_impl->closePeer(conn, reason);
        for (size_t i = m_impl->seated.size(); i-- > 0;)
            if (m_impl->seated[i].conn == conn)
                m_impl->seated.erase(m_impl->seated.begin() + static_cast<long>(i));
    }
    m_impl->lobby.evict(peerId);
    // The per-peer budgets go with the peer, as they do on a disconnect and on
    // a timeout. Relay peer ids are handed out again, so a bucket left
    // exhausted here was inherited by whoever arrived on that handle next and
    // had their frames dropped for somebody else's behaviour.
    m_impl->chatGate.forget(peerId);
    m_impl->frameBudget.forget(peerId);
    m_impl->byteBudget.forget(peerId);
    m_impl->broadcastLobbyInternal();
}
