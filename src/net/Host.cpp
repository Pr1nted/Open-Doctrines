#include "Host.h"

#include "HttpClient.h"
#include "JoinTicket.h"
#include "ModAttest.h"
#include "WsServer.h"

#include <atomic>
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

    std::string errorText;
    std::string code;
    std::string hostPsid;
    std::string hostName;
    std::string hostBadges;

    /** Set once we have connected as host and been accepted. */
    uint16_t hostPeerId = 0;
    uint32_t turnNumber = 0;

    WsServer  server;
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
    };
    std::vector<Seated> seated;
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
    void handlePeerMessage(uint16_t peerId, const uint8_t* body, size_t size);
    void expirePending();
    void broadcastLobbyInternal();
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

    // Bind BEFORE talking to the account service. A port that cannot be opened
    // is the most common way hosting fails, and finding out first means the
    // host is told about it instead of registering a session nobody can reach.
    {
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

        uint16_t hostSeat = 0;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->issuerKeys = std::move(parsed);
            if (!c.dedicated) hostSeat = impl->nextPeerId++;
        }

        // A host that plays holds a seat like anyone else, but takes it
        // locally: there is no socket to itself and no ticket to present.
        // A dedicated host holds none, which is what host-only mode is.
        if (hostSeat) {
            impl->lobby.admit(hostSeat, impl->hostPsid,
                              c.anonymous ? "" : impl->hostName,
                              c.anonymous ? "" : impl->hostBadges,
                              c.issuer, true);
        }
        impl->hostPeerId = hostSeat;
        impl->lobby.setHost(hostSeat);

        impl->phase.store(Phase::Live);
        impl->push({NetHostEvent::Kind::Opened, 0, code, {}});
    });
    return true;
}

// ---------------------------------------------------------------- update ----

void NetHost::update() {
    Impl& impl = *m_impl;
    const Phase p = impl.phase.load();
    if (p == Phase::Idle || p == Phase::Closed) return;

    impl.server.update();

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
                if (peerId) impl.handlePeerMessage(peerId, e.data.data(), e.data.size());
                break;
            }
            case WsServerEvent::Kind::Disconnected:
                impl.handleDisconnected(e.conn);
                break;
        }
    }

    impl.expirePending();
}

// -------------------------------------------------------------- addressing ----
//
// Each peer has its own socket now, so "send to one" is the simple case and
// "send to all" is the loop. Under the relay it was the other way round.

void NetHost::Impl::toPeer(uint16_t peerId, NetMsg type,
                           const std::vector<uint8_t>& payload) {
    // The host's own seat has no socket. Its UI reads the lobby directly, so
    // there is nothing to deliver and nothing has gone wrong.
    const WsConnId conn = connFor(peerId);
    if (conn) server.send(conn, netEncodeFrame(type, payload));
}

void NetHost::Impl::broadcast(NetMsg type, const std::vector<uint8_t>& payload) {
    const std::vector<uint8_t> frame = netEncodeFrame(type, payload);
    for (const Seated& s : seated) server.send(s.conn, frame);
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
    if (p.nonce.size() > 64) { server.closeConn(conn, "internal"); return; }
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
        server.send(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
        server.closeConn(conn, "unverified");
        dropPending(conn);
        return;
    }

    // Single use, so the same live ticket cannot seat two connections at once.
    if (!replay.useOnce(ticket.jti, ticket.expires, expect.now)) {
        NetRejectMsg r;
        r.reason = NetReject::Unknown;
        r.text = "That sign-in was already used. Try joining again.";
        server.send(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
        server.closeConn(conn, "replayed");
        dropPending(conn);
        return;
    }

    std::string badges;
    for (const std::string& b : ticket.badges) {
        if (!badges.empty()) badges += ",";
        badges += b;
    }

    // A returning player keeps their seat: Lobby matches on the pseudonym and
    // moves the handle, so a reconnect does not cost a country or submitted
    // orders. That is why the psid and not the socket is the identity.
    const uint16_t peerId = nextPeerId++;
    const LobbyDenial denial = lobby.admit(peerId, ticket.psid, ticket.name, badges,
                                           ticket.issuer, ticket.issuer == config.issuer);
    if (denial != LobbyDenial::None) {
        NetRejectMsg r;
        r.reason = denial == LobbyDenial::SessionFull       ? NetReject::SessionFull
                 : denial == LobbyDenial::GameInProgress    ? NetReject::GameInProgress
                 : denial == LobbyDenial::IssuerNotAccepted ? NetReject::IssuerNotAccepted
                 : NetReject::Unknown;
        r.text = lobbyDenialText(denial);
        server.send(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
        server.closeConn(conn, "refused");
        dropPending(conn);
        return;
    }

    // Whatever seat the lobby settled on -- a fresh one, or the one this
    // pseudonym already held -- is the seat this socket now speaks for.
    uint16_t settled = peerId;
    if (const LobbyMember* m = lobby.findByPsid(ticket.psid)) settled = m->peerId;

    // A reconnect supersedes the older socket rather than sitting alongside it.
    for (size_t i = seated.size(); i-- > 0;) {
        if (seated[i].peerId == settled && seated[i].conn != conn) {
            server.closeConn(seated[i].conn, "reconnected elsewhere");
            seated.erase(seated.begin() + static_cast<long>(i));
        }
    }

    dropPending(conn);
    seated.push_back(Seated{conn, settled});

    const LobbyMember* m = lobby.find(settled);
    if (!m) {
        // Admitted, but no seat can be found for it. This should be
        // unreachable; it is handled because the alternative is replying with
        // NOTHING, and a client that is neither welcomed nor refused just hangs
        // until it times out with no idea why. Every path out of here answers.
        NetRejectMsg r;
        r.reason = NetReject::Unknown;
        r.text = "The server could not seat you. Try joining again.";
        server.send(conn, netEncodeFrame(NetMsg::Reject, r.encode()));
        server.closeConn(conn, "unseated");
        dropPending(conn);
        return;
    }

    server.send(conn, netEncodeFrame(NetMsg::Welcome, welcomeFor(*m).encode()));
    // Straight after the welcome, so a player can pick a country without
    // having to load the map first.
    if (!countries.countries.empty())
        server.send(conn, netEncodeFrame(NetMsg::Countries, countries.encode()));
    push({NetHostEvent::Kind::PeerJoined, settled, ticket.name, {}});
    broadcastLobbyInternal();
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
    push({NetHostEvent::Kind::PeerLeft, peerId, "", {}});
    broadcastLobbyInternal();
}

void NetHost::Impl::expirePending() {
    const long long now = nowSeconds();
    for (size_t i = pending.size(); i-- > 0;) {
        if (now - pending[i].since <= kAuthTimeoutSeconds) continue;
        server.closeConn(pending[i].conn, "no ticket");
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
            if (lobby.submitOrders(peerId, o.turnNumber, o.payload) == LobbyDenial::None) {
                push({NetHostEvent::Kind::OrdersReceived, peerId, "", {}});
                broadcastLobbyInternal();
            }
            return;
        }
        case NetMsg::Chat: {
            NetChat c;
            if (!NetChat::decode(payload, payloadSize, c)) return;
            c.fromPeerId = peerId;        // attribution is ours
            NetHostEvent e{NetHostEvent::Kind::Chat, peerId, c.text, c};
            push(std::move(e));
            broadcast(NetMsg::ChatFrom, c.encode());
            return;
        }
        default:
            return;
    }
}

// ------------------------------------------------------------- broadcasts ----

void NetHost::Impl::broadcastLobbyInternal() {
    NetLobbyState s;
    s.state = lobby.state();
    s.assignment = lobby.settings().assignment;
    s.roster = lobby.roster();
    broadcast(NetMsg::LobbyState, s.encode());
    push({NetHostEvent::Kind::LobbyChanged});
}

void NetHost::broadcastLobby() { m_impl->broadcastLobbyInternal(); }

bool NetHost::startGame(std::string& why) {
    if (!m_impl->lobby.start(why)) return false;
    m_impl->broadcastLobbyInternal();
    return true;
}

void NetHost::setCountries(const NetCountryList& list) {
    m_impl->countries = list;
    // Anyone already here gets it now; anyone arriving later gets it with
    // their welcome.
    const std::vector<uint8_t> frame = netEncodeFrame(NetMsg::Countries, list.encode());
    for (const auto& s : m_impl->seated) m_impl->server.send(s.conn, frame);
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

void NetHost::sendSnapshot(uint16_t peerId, uint32_t turnNumber,
                           const std::vector<uint8_t>& payload) {
    NetWorld w;
    w.turnNumber = turnNumber;
    w.payload = payload;
    m_impl->toPeer(peerId, NetMsg::Snapshot, w.encode());
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

void NetHost::kick(uint16_t peerId, const std::string& reason) {
    NetRejectMsg r;
    r.text = reason;
    m_impl->toPeer(peerId, NetMsg::Kick, r.encode());
    // Told first, then disconnected: a kick that arrives as a dead socket is
    // indistinguishable from the game crashing.
    if (const WsConnId conn = m_impl->connFor(peerId)) {
        m_impl->server.closeConn(conn, reason);
        for (size_t i = m_impl->seated.size(); i-- > 0;)
            if (m_impl->seated[i].conn == conn)
                m_impl->seated.erase(m_impl->seated.begin() + static_cast<long>(i));
    }
    m_impl->lobby.evict(peerId);
    m_impl->broadcastLobbyInternal();
}
