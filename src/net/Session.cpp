#include "Session.h"

#include "HttpClient.h"
#include "WebSocket.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace {

constexpr int kJoinTimeoutMs = 20000;

}  // namespace

const char* netSessionPhaseName(NetSession::Phase p) {
    switch (p) {
        case NetSession::Phase::Idle:        return "Idle";
        case NetSession::Phase::Fetching:    return "Fetching";
        case NetSession::Phase::Minting:     return "Minting";
        case NetSession::Phase::Connecting:  return "Connecting";
        case NetSession::Phase::Handshaking: return "Handshaking";
        case NetSession::Phase::Lobby:       return "Lobby";
        case NetSession::Phase::InGame:      return "InGame";
        case NetSession::Phase::Closed:      return "Closed";
    }
    return "Unknown";
}

std::string netRejectAdvice(NetReject reason, const std::string& serverText) {
    switch (reason) {
        case NetReject::ProtocolVersion:
            return "This server speaks a different version of the multiplayer "
                   "protocol. One of you needs to update.";
        case NetReject::GameVersion:
            return "This server runs a different version of OpenDoctrines.";
        case NetReject::ModMismatch:
            // The server sends the specifics; they are more useful than
            // anything that could be written here.
            return serverText.empty()
                ? "Your mods do not match this server's." : serverText;
        case NetReject::SessionFull:      return "That game is full.";
        case NetReject::Banned:
            return serverText.empty() ? "You cannot join this server." : serverText;
        case NetReject::GameInProgress:
            return "That game has already started and this server does not take "
                   "spectators.";
        case NetReject::ServerShuttingDown: return "That server is shutting down.";
        case NetReject::IssuerNotAccepted:
            return "This server does not accept accounts from the service you "
                   "signed in with.";
        case NetReject::HostNotDeclared:
            return "This server did not say who is hosting it, so the game "
                   "refused to join. Hosting anonymously is fine, but a server "
                   "has to say so.";
        case NetReject::BadTicket:
            return "Your sign-in was not accepted. Try signing in again.";
        case NetReject::Unknown:
        default:
            return serverText.empty() ? "That server refused the connection."
                                      : serverText;
    }
}

// ------------------------------------------------------------------ impl ----

struct NetSession::Impl {
    mutable std::mutex mutex;

    std::atomic<Phase> phase{Phase::Idle};
    std::string        errorText;
    NetReject          reject = NetReject::Unknown;

    NetWelcome welcome;
    std::vector<NetPeer> roster;
    NetSessionState sessionState = NetSessionState::Lobby;
    NetAssignment   assignment = NetAssignment::PlayersPick;

    std::vector<NetSessionEvent> events;

    /** Mod messages, kept apart from events: mods drain their own queue. */
    std::deque<NetModMsg> modInbox;

    void pushMod(NetModMsg m) {
        std::lock_guard<std::mutex> lock(mutex);
        // Bounded, like every other queue here: a peer that talks faster than
        // the game reads must not grow this without limit.
        if (modInbox.size() >= 256) modInbox.pop_front();
        modInbox.push_back(std::move(m));
    }

    WebSocket socket;
    std::thread joinWorker;
    std::atomic<bool> abandon{false};

    // Held only for the length of the handshake.
    std::string pendingHello;

    /** What the host said this world has. Empty until the catalogue arrives. */
    std::vector<NetCountryList::Entry> countries;

    // Everything needed to answer a challenge once one arrives.
    std::vector<std::string> addresses;
    std::atomic<size_t> attempt{0};
    std::string issuer, code, token, gameVersion, modAttestation;
    std::atomic<bool> challengeSeen{false};

    /** Open a socket to `addresses[attempt]`. False if there is nothing left. */
    bool dialNext();

    void fail(const std::string& text, NetReject why = NetReject::Unknown) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (errorText.empty()) errorText = text;
            reject = why;
        }
        push({NetSessionEvent::Kind::Rejected});
        phase.store(Phase::Closed);
    }

    void push(NetSessionEvent e) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(std::move(e));
    }

    void handleFrame(NetMsg type, const uint8_t* body, size_t size);
    void answerChallenge(const std::string& challenge);
};

NetSession::NetSession() : m_impl(std::make_unique<Impl>()) {}

NetSession::~NetSession() { leave(); }

NetSession::Phase NetSession::phase() const { return m_impl->phase.load(); }

const NetWelcome& NetSession::welcome() const { return m_impl->welcome; }

const std::vector<NetPeer>& NetSession::roster() const { return m_impl->roster; }

std::vector<NetCountryList::Entry> NetSession::countries() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->countries;
}

NetSessionState NetSession::state() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->sessionState;
}

NetAssignment NetSession::assignment() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->assignment;
}

std::string NetSession::error() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->errorText;
}

NetReject NetSession::rejectReason() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->reject;
}

bool NetSession::spectating() const { return m_impl->welcome.spectator; }

bool NetSession::nextEvent(NetSessionEvent& out) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->events.empty()) return false;
    out = std::move(m_impl->events.front());
    m_impl->events.erase(m_impl->events.begin());
    return true;
}

void NetSession::leave() {
    m_impl->abandon.store(true);
    m_impl->socket.close();
    if (m_impl->joinWorker.joinable()) m_impl->joinWorker.join();
    m_impl->phase.store(Phase::Closed);
}

// ------------------------------------------------------------------ join ----

bool NetSession::Impl::dialNext() {
    const size_t i = attempt.load();
    if (i >= addresses.size()) return false;

    std::string url = addresses[i];
    // A bare host, or host:port, is the common thing to be handed. Assume the
    // plain scheme rather than refusing something a player clearly meant.
    if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0)
        url = "ws://" + url;

    // Plaintext is expected here and is not a mistake: a host on a home
    // connection has no certificate, and the realistic path to a public game is
    // a tunnel that terminates TLS in front of it. See WsServer.h.
    const bool insecure = url.rfind("ws://", 0) == 0;
    return socket.connect(url, insecure);
}

bool NetSession::join(const std::string& address, const std::string& issuer,
                      const std::string& code, const std::string& token,
                      const std::string& gameVersion,
                      const std::string& modAttestation) {
    return join(std::vector<std::string>{address}, issuer, code, token,
                gameVersion, modAttestation);
}

bool NetSession::join(const std::vector<std::string>& addresses,
                      const std::string& issuer, const std::string& code,
                      const std::string& token, const std::string& gameVersion,
                      const std::string& modAttestation) {
    if (m_impl->phase.load() != Phase::Idle) return false;
    if (issuer.empty() || code.empty() || token.empty()) {
        m_impl->fail("Sign in before joining a game.");
        return false;
    }
    if (addresses.empty()) {
        m_impl->fail("That server has no address to connect to.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->addresses = addresses;
        m_impl->issuer = issuer;
        m_impl->code = code;
        m_impl->token = token;
        m_impl->gameVersion = gameVersion;
        m_impl->modAttestation = modAttestation;
    }
    m_impl->attempt.store(0);
    m_impl->challengeSeen.store(false);
    m_impl->abandon.store(false);
    m_impl->phase.store(Phase::Connecting);

    // Straight to the host. Nothing is asked of the account service until the
    // host has said what it wants answered.
    if (!m_impl->dialNext()) {
        m_impl->fail(m_impl->socket.error().empty()
            ? "Could not open a connection to that server."
            : m_impl->socket.error());
        return false;
    }
    return true;
}

size_t NetSession::addressAttempt() const { return m_impl->attempt.load() + 1; }

size_t NetSession::addressCount() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->addresses.size();
}

// ------------------------------------------------------------- challenged ----

void NetSession::Impl::answerChallenge(const std::string& challenge) {
    if (challengeSeen.exchange(true)) return;   // one challenge per connection

    const std::string nonce      = httpJsonString(challenge, "nonce", 128);
    const std::string theirCode  = httpJsonString(challenge, "session", 64);
    const std::string theirIssuer = httpJsonString(challenge, "issuer", 256);

    if (nonce.empty()) {
        fail("That server did not ask for a sign-in, so it cannot be joined.");
        return;
    }

    // The host does not get to decide who vouches for the player. If it names
    // a different account service, that is the alternate-provider case and the
    // player must have chosen it deliberately, not had it swapped in mid-join.
    std::string wantIssuer, wantCode;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wantIssuer = issuer;
        wantCode = code;
    }
    if (!theirIssuer.empty() && theirIssuer != wantIssuer) {
        fail("That server uses a different account service (" + theirIssuer +
             ") to the one you signed in to. Add it deliberately if you trust it.",
             NetReject::IssuerNotAccepted);
        return;
    }
    if (!theirCode.empty() && theirCode != wantCode) {
        fail("That server is running a different game to the one you asked for.");
        return;
    }

    phase.store(Phase::Fetching);

    // Blocking HTTP, so off the render thread. The socket stays open meanwhile;
    // the host is holding a slot for this nonce until it times out.
    auto fetchTicket = [this, nonce] {
        std::string iss, c, tok;
        {
            std::lock_guard<std::mutex> lock(mutex);
            iss = issuer; c = code; tok = token;
        }
        const bool local = iss.rfind("http://localhost", 0) == 0 ||
                           iss.rfind("http://127.0.0.1", 0) == 0;

        // 1. The descriptor, from the ACCOUNT SERVICE, keyed by the code the
        //    player already had. Not from the host -- see Session.h.
        HttpRequest info;
        info.url = iss + "/session/" + c;
        info.allowInsecure = local;
        info.timeoutMs = kJoinTimeoutMs;
        const HttpResponse infoRes = httpRequest(info);
        if (abandon.load()) return;
        if (!infoRes.ok()) {
            fail(infoRes.error.empty() ? "No game is running under that code."
                                       : infoRes.error);
            return;
        }
        const std::string descriptor = httpJsonString(infoRes.body, "descriptor", 4096);
        if (descriptor.empty()) {
            fail("That game session is no longer valid.");
            return;
        }

        // 2. A ticket for exactly this session, answering exactly this host's
        //    challenge. The token is used here and nowhere else.
        phase.store(Phase::Minting);
        HttpRequest mint;
        mint.method = "POST";
        mint.url = iss + "/ticket";
        mint.bearer = tok;
        mint.allowInsecure = local;
        mint.timeoutMs = kJoinTimeoutMs;
        mint.body = "{\"descriptor\":\"" + httpJsonEscape(descriptor) +
                    "\",\"nonce\":\"" + httpJsonEscape(nonce) + "\"}";
        const HttpResponse ticketRes = httpRequest(mint);
        if (abandon.load()) return;

        if (!ticketRes.ok()) {
            const std::string why = !ticketRes.error.empty() ? ticketRes.error
                : httpJsonString(ticketRes.body, "message", 512);
            fail(why.empty() ? "Could not get permission to join." : why,
                 ticketRes.status == 403 ? NetReject::Banned : NetReject::BadTicket);
            return;
        }
        const std::string ticket = httpJsonString(ticketRes.body, "ticket", 4096);
        if (ticket.empty()) {
            fail("The account service sent an unusable reply.");
            return;
        }

        // 3. Answer. The ticket goes in a frame rather than the URL, so it
        //    never lands in a log or a history.
        {
            std::lock_guard<std::mutex> lock(mutex);
            // The ticket says WHO. These two say WHETHER WE CAN PLAY AT ALL:
            // a protocol the host cannot speak, or a mod set that does not
            // match theirs, are both better refused here with a reason than
            // discovered as a desync three turns in.
            pendingHello = "{\"ticket\":\"" + httpJsonEscape(ticket) + "\"" +
                           ",\"protocol\":" + std::to_string(kNetProtocolVersion) +
                           ",\"mods\":\"" + httpJsonEscape(modAttestation) + "\"}";
        }
    };

    // A browser has no thread to put this on: the emscripten build is
    // single-threaded and compiled without exceptions, so constructing a
    // std::thread there does not fail -- it aborts the tab.
    //
    // Running it inline is safe there for a specific reason, not as a
    // compromise: httpRequest() on emscripten is a stub that returns
    // "signing in from the web build is not supported yet" without touching
    // the network. There is nothing to block on, so there is no frame to
    // stall. What the player gets is that sentence, which is the honest
    // answer, instead of a tab that dies on the Join button.
    //
    // If web HTTP is ever implemented, this has to become asyncify-aware
    // rather than staying inline -- a real request here WOULD stall the frame.
#ifdef __EMSCRIPTEN__
    fetchTicket();
#else
    if (joinWorker.joinable()) joinWorker.join();
    joinWorker = std::thread(std::move(fetchTicket));
#endif
    return;
}

// ---------------------------------------------------------------- update ----

void NetSession::update() {
    Impl& impl = *m_impl;
    const Phase p = impl.phase.load();
    if (p == Phase::Idle || p == Phase::Closed) return;

    const WsState ws = impl.socket.state();

    // The host's challenge arrives as text; everything after is binary.
    std::string challenge;
    while (impl.socket.pollText(challenge)) impl.answerChallenge(challenge);

    // Sent from here rather than from the worker so the socket is only ever
    // touched from this thread.
    if (p == Phase::Minting || p == Phase::Fetching) {
        std::string hello;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            hello.swap(impl.pendingHello);
        }
        if (!hello.empty()) {
            impl.socket.sendText(hello);
            impl.phase.store(Phase::Handshaking);
        }
    }

    // A candidate address that did not come up is not a failure while others
    // remain. A host may be reachable by a tunnel from outside and only by a
    // LAN address from inside, and the player cannot be expected to know which.
    if (ws == WsState::Closed && p == Phase::Connecting &&
        !impl.challengeSeen.load()) {
        const size_t next = impl.attempt.load() + 1;
        size_t total;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            total = impl.addresses.size();
        }
        if (next < total) {
            impl.attempt.store(next);
            if (impl.dialNext()) return;
        }
        impl.fail(impl.socket.error().empty()
            ? (total > 1 ? "None of that server's addresses answered."
                         : "That server did not answer.")
            : impl.socket.error());
        return;
    }

    if (ws == WsState::Closed &&
        p != Phase::Fetching && p != Phase::Minting) {
        const std::string err = impl.socket.error();
        bool alreadyExplained;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            alreadyExplained = !impl.errorText.empty();
        }
        // A close that follows a REJECT already has its reason on screen;
        // overwriting it with "the connection was lost" would replace the
        // useful message with a useless one.
        if (alreadyExplained) {
            impl.push({NetSessionEvent::Kind::Disconnected});
            impl.phase.store(Phase::Closed);
        } else {
            impl.fail(err.empty() ? "The connection to that server was lost." : err);
        }
        return;
    }

    // The relay's own ok/refusal is a text frame; game traffic is binary.
    std::string text;
    while (impl.socket.pollText(text)) {
        if (httpJsonBool(text, "ok", false)) continue;   // accepted; wait for WELCOME
        const std::string why = httpJsonString(text, "error", 256);
        impl.fail(why.empty() ? "That server refused the connection." : why);
        return;
    }

    std::vector<uint8_t> frame;
    while (impl.socket.poll(frame)) {
        NetMsg type;
        const uint8_t* body = nullptr;
        size_t size = 0;
        // A frame we cannot decode is dropped, not fatal: a newer server may
        // send something this build predates, and disconnecting over it would
        // make every protocol addition a breaking change.
        if (!netDecodeFrame(frame.data(), frame.size(), type, body, size)) continue;
        impl.handleFrame(type, body, size);
    }
}

void NetSession::Impl::handleFrame(NetMsg type, const uint8_t* body, size_t size) {
    switch (type) {
        case NetMsg::Welcome: {
            NetWelcome w;
            if (!NetWelcome::decode(body, size, w)) return;

            // A server that did not say who runs it is refused here, on the
            // client, rather than trusted. Anonymous hosting is legitimate --
            // but it has to be declared as such.
            if (!w.host.declared()) {
                fail(netRejectAdvice(NetReject::HostNotDeclared, ""),
                     NetReject::HostNotDeclared);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                welcome = w;
                roster = w.roster;
                sessionState = w.state;
                assignment = w.assignment;
            }
            phase.store(w.state == NetSessionState::Game ? Phase::InGame : Phase::Lobby);
            push({NetSessionEvent::Kind::Welcomed});
            return;
        }
        case NetMsg::Reject: {
            NetRejectMsg r;
            if (!NetRejectMsg::decode(body, size, r)) return;
            fail(netRejectAdvice(r.reason, r.text), r.reason);
            return;
        }
        case NetMsg::LobbyState: {
            NetLobbyState s;
            if (!NetLobbyState::decode(body, size, s)) return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                roster = s.roster;
                sessionState = s.state;
                assignment = s.assignment;
            }
            phase.store(s.state == NetSessionState::Game ? Phase::InGame : Phase::Lobby);
            push({NetSessionEvent::Kind::LobbyChanged});
            return;
        }
        case NetMsg::Roster: {
            NetRosterMsg r;
            if (!NetRosterMsg::decode(body, size, r)) return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                roster = r.peers;
            }
            push({NetSessionEvent::Kind::LobbyChanged});
            return;
        }
        case NetMsg::SwapProposed: {
            NetSessionEvent e{NetSessionEvent::Kind::SwapProposed};
            if (!NetSwap::decode(body, size, e.swap)) return;
            push(std::move(e));
            return;
        }
        case NetMsg::TurnBegin: {
            NetTurnBegin t;
            if (!NetTurnBegin::decode(body, size, t)) return;
            NetSessionEvent e{NetSessionEvent::Kind::TurnBegan};
            e.turnNumber = t.turnNumber;
            e.deadlineMs = t.deadlineMs;
            phase.store(Phase::InGame);
            push(std::move(e));
            return;
        }
        case NetMsg::Snapshot:
        case NetMsg::Delta: {
            NetWorld world;
            if (!NetWorld::decode(body, size, world)) return;
            NetSessionEvent e{type == NetMsg::Snapshot ? NetSessionEvent::Kind::Snapshot
                                                       : NetSessionEvent::Kind::Delta};
            e.turnNumber = world.turnNumber;
            e.payload = std::move(world.payload);
            push(std::move(e));
            return;
        }
        case NetMsg::Countries: {
            NetCountryList list;
            if (!NetCountryList::decode(body, size, list)) return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                countries = std::move(list.countries);
            }
            push(NetSessionEvent{NetSessionEvent::Kind::CountriesKnown});
            return;
        }
        case NetMsg::Notice: {
            NetSessionEvent e{NetSessionEvent::Kind::Notice};
            if (!NetNotice::decode(body, size, e.notice)) return;
            push(std::move(e));
            return;
        }
        case NetMsg::ModMsgFrom: {
            NetModMsg m;
            if (!NetModMsg::decode(body, size, m)) return;
            pushMod(std::move(m));
            return;
        }
        case NetMsg::ChatFrom: {
            NetSessionEvent e{NetSessionEvent::Kind::Chat};
            if (!NetChat::decode(body, size, e.chat)) return;
            push(std::move(e));
            return;
        }
        case NetMsg::Kick: {
            NetRejectMsg r;
            NetRejectMsg::decode(body, size, r);
            fail(r.text.empty() ? "You were removed from that game." : r.text);
            return;
        }
        default:
            // Unknown, or a server->client message this build predates.
            return;
    }
}

// -------------------------------------------------------------- outbound ----

namespace {

bool inLobby(NetSession::Phase p) { return p == NetSession::Phase::Lobby; }
bool joined(NetSession::Phase p) {
    return p == NetSession::Phase::Lobby || p == NetSession::Phase::InGame;
}

}  // namespace

void NetSession::claimCountry(uint16_t countryId) {
    // Countries are only chosen in the lobby. The server enforces this too --
    // this is so the client does not send something it knows will be refused.
    if (!inLobby(phase()) || spectating()) return;
    NetClaimCountry c{countryId};
    m_impl->socket.send(netEncodeFrame(NetMsg::ClaimCountry, c.encode()));
}

void NetSession::offerSwap(uint16_t toPeerId) {
    if (!inLobby(phase()) || spectating()) return;
    NetSwap s;
    s.toPeerId = toPeerId;
    m_impl->socket.send(netEncodeFrame(NetMsg::SwapOffer, s.encode()));
}

void NetSession::replySwap(uint16_t fromPeerId, bool accept) {
    if (!inLobby(phase()) || spectating()) return;
    NetSwap s;
    s.fromPeerId = fromPeerId;
    s.accepted = accept;
    m_impl->socket.send(netEncodeFrame(NetMsg::SwapReply, s.encode()));
}

void NetSession::withdrawOrders(uint32_t turnNumber) {
    if (phase() != Phase::InGame || spectating()) return;
    NetOrdersMsg o;
    o.turnNumber = turnNumber;          // payload deliberately empty
    m_impl->socket.send(netEncodeFrame(NetMsg::Withdraw, o.encode()));
}

void NetSession::submitOrders(uint32_t turnNumber, const std::vector<uint8_t>& payload) {
    // A spectator's orders are discarded rather than merely ignored: not
    // sending them at all means there is nothing for a server to mishandle.
    if (phase() != Phase::InGame || spectating()) return;
    NetOrdersMsg o;
    o.turnNumber = turnNumber;
    o.payload = payload;
    m_impl->socket.send(netEncodeFrame(NetMsg::Orders, o.encode()));
}

void NetSession::sendModMessage(const std::string& modId, int32_t toPeer,
                                const std::vector<uint8_t>& payload) {
    if (payload.size() > NetLimits::kModMsg) return;
    NetModMsg m;
    m.modId  = modId;
    m.peerId = toPeer < 0 ? NetModMsg::kBroadcast : (uint16_t)toPeer;
    m.payload.assign(payload.begin(), payload.end());
    m_impl->socket.send(netEncodeFrame(NetMsg::ModMsg, m.encode()));
}

bool NetSession::nextModMessage(NetModMsg& out) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->modInbox.empty()) return false;
    out = std::move(m_impl->modInbox.front());
    m_impl->modInbox.pop_front();
    return true;
}

void NetSession::sendChat(const std::string& text) {
    if (!joined(phase()) || text.empty()) return;
    NetChat c;
    c.text = text.size() > NetLimits::kChat ? text.substr(0, NetLimits::kChat) : text;
    m_impl->socket.send(netEncodeFrame(NetMsg::Chat, c.encode()));
}

void NetSession::sendReady() {
    if (!joined(phase())) return;
    m_impl->socket.send(netEncodeFrame(NetMsg::Ready, {}));
}
