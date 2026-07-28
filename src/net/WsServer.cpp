// The listening half of RFC 6455.
//
// Everything read here arrives on a public port from a stranger, so the rules
// are the ones that matter when the sender is not cooperating:
//
//   - Every buffer is bounded, and the bound is enforced BEFORE allocating.
//   - A connection that has not finished its handshake in time is dropped, so
//     opening sockets and saying nothing cannot pin the host's memory.
//   - There is a connection cap, applied at accept, so the same trick with many
//     sockets cannot either.
//   - A client frame that is not masked is a protocol violation and closes the
//     connection. This is the asymmetry with WsNative.cpp, which refuses masked
//     frames from a server.
//
// None of this is authentication. A connection reaching Open state has proved
// only that it speaks WebSocket; who it belongs to is Host.cpp's problem, and
// the answer comes from the join ticket, never from the socket.

#include "WsServer.h"

#if !defined(__EMSCRIPTEN__) && defined(OD_ENABLE_NET)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketFd = SOCKET;
  #define OD_BAD_SOCKET   INVALID_SOCKET
  #define OD_CLOSE_SOCKET closesocket
  #define OD_POLL         WSAPoll
#else
  #include <arpa/inet.h>
  #include <cerrno>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketFd = int;
  #define OD_BAD_SOCKET   (-1)
  #define OD_CLOSE_SOCKET ::close
  #define OD_POLL         ::poll
#endif

// Writing to a socket the peer has already closed raises SIGPIPE, whose
// default action is to terminate the process. A player closing their game at
// the wrong moment must not take the host's game down with it, so the signal is
// suppressed per-send on Linux and per-socket on the BSDs, including macOS.
#ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
#endif

namespace {

using Clock = std::chrono::steady_clock;

// Matched to the client in WsNative.cpp. Larger than any snapshot the game
// produces; small enough that the cap times the connection cap is survivable.
constexpr size_t kMaxMessageBytes = 16u * 1024 * 1024;

// The RFC's own limit for control frames. Enforcing it means the control path
// never allocates.
constexpr size_t kMaxControlBytes = 125;

// A handshake is a few hundred bytes. Anything approaching this is either
// broken or probing.
constexpr size_t kMaxHandshakeBytes = 16 * 1024;
constexpr int    kHandshakeTimeoutMs = 10000;

// Enough for any game this runs, and a hard stop on socket exhaustion.
constexpr size_t kMaxConnections = 64;

// If a peer will not drain what we have queued, it is gone. Dropping it beats
// growing without limit on the host's machine.
constexpr size_t kMaxPendingOut = 32u * 1024 * 1024;

enum : uint8_t {
    kOpContinuation = 0x0, kOpText = 0x1, kOpBinary = 0x2,
    kOpClose = 0x8, kOpPing = 0x9, kOpPong = 0xA,
};

// RFC 6455 section 7.4.1, the subset this ever sends.
enum : uint16_t {
    kCloseNormal      = 1000,
    kCloseProtocol    = 1002,
    kCloseUnsupported = 1003,
    kCloseTooBig      = 1009,
};

bool setNonBlocking(SocketFd fd) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0;
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/**
 * A printable peer address.
 *
 * The host sees this; PRIVACY.md says so plainly, because on a direct
 * connection there is no way for it not to.
 */
std::string addressOf(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&ss);
        inet_ntop(AF_INET, &v4->sin_addr, buf, sizeof(buf));
        return buf;
    }
    if (ss.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        inet_ntop(AF_INET6, &v6->sin6_addr, buf, sizeof(buf));
        // A v4 peer on a dual-stack socket arrives as ::ffff:1.2.3.4. Show the
        // address the player would recognise as theirs.
        std::string s = buf;
        if (s.compare(0, 7, "::ffff:") == 0) return s.substr(7);
        return s;
    }
    return {};
}

struct Conn {
    SocketFd    fd = OD_BAD_SOCKET;
    WsConnId    id = 0;
    std::string peer;

    bool open = false;          // handshake finished, Connected reported
    bool closing = false;       // close frame sent; flush then drop
    bool dead = false;          // reap on this pass

    Clock::time_point since = Clock::now();

    std::string          handshake;   // bytes read before the upgrade completes
    std::vector<uint8_t> in;          // undecoded frame bytes
    std::vector<uint8_t> out;         // encoded bytes awaiting the socket

    std::vector<uint8_t> message;     // reassembly across fragments
    uint8_t              messageOp = 0;
    bool                 assembling = false;
};

/** Encode a server-to-client frame. Server frames are never masked. */
void appendFrame(std::vector<uint8_t>& out, uint8_t opcode,
                 const uint8_t* payload, size_t n) {
    out.push_back(static_cast<uint8_t>(0x80 | opcode));   // FIN, no RSV
    if (n < 126) {
        out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(n & 0xFF));
    } else {
        out.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
            out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(n) >> shift) & 0xFF));
    }
    if (n) out.insert(out.end(), payload, payload + n);
}

void appendClose(std::vector<uint8_t>& out, uint16_t code, const std::string& reason) {
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(code & 0xFF));
    // Truncated so the control frame stays inside its 125-byte limit.
    const size_t room = kMaxControlBytes - 2;
    const size_t take = reason.size() < room ? reason.size() : room;
    body.insert(body.end(), reason.begin(), reason.begin() + static_cast<long>(take));
    appendFrame(out, kOpClose, body.data(), body.size());
}

}  // namespace

// ------------------------------------------------------------------ impl ----

struct WsServer::Impl {
    // Usually one socket. Loopback binding needs two, because 127.0.0.1 and
    // ::1 are genuinely different addresses and no single socket covers both --
    // see listen() for why that matters to a tunnelled host.
    std::vector<SocketFd> listenFds;
    uint16_t boundPort = 0;
    WsConnId nextId = 1;

    std::vector<Conn>          conns;
    std::deque<WsServerEvent>  events;

    Conn* find(WsConnId id) {
        for (Conn& c : conns)
            if (c.id == id && !c.dead) return &c;
        return nullptr;
    }

    /**
     * Queue a close and stop reading. The socket goes once the bytes leave.
     *
     * Note what this deliberately does NOT do: touch `c.in`. It is called from
     * inside parseFrames(), which is part-way through that buffer and holding
     * an offset into it -- clearing it here left that offset past the end, and
     * the erase at the end of the parse then ran off the buffer. Every ordinary
     * disconnect sends a close frame, so that was every disconnect. parseFrames
     * drops the remaining input itself once it has stopped.
     */
    void beginClose(Conn& c, uint16_t code, const std::string& reason) {
        if (c.closing) return;
        c.closing = true;
        if (c.open) appendClose(c.out, code, reason);
    }

    void drop(Conn& c) {
        if (c.fd != OD_BAD_SOCKET) { OD_CLOSE_SOCKET(c.fd); c.fd = OD_BAD_SOCKET; }
        if (c.open) {
            WsServerEvent e;
            e.kind = WsServerEvent::Kind::Disconnected;
            e.conn = c.id;
            e.peerAddress = c.peer;
            events.push_back(std::move(e));
            c.open = false;
        }
        c.dead = true;
    }

    void doAccept(SocketFd from);
    void doRead(Conn& c);
    void doWrite(Conn& c);
    void progressHandshake(Conn& c);
    void parseFrames(Conn& c);
    void deliver(Conn& c, uint8_t opcode, std::vector<uint8_t>&& payload);
};

void WsServer::Impl::doAccept(SocketFd from) {
    for (;;) {
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        const SocketFd fd = ::accept(from, reinterpret_cast<sockaddr*>(&ss), &len);
        if (fd == OD_BAD_SOCKET) return;    // drained, or nothing pending

        // Applied here rather than after the handshake: the point is to bound
        // how many sockets a stranger can hold open at once.
        size_t live = 0;
        for (const Conn& c : conns) if (!c.dead) live++;
        if (live >= kMaxConnections) { OD_CLOSE_SOCKET(fd); continue; }

        if (!setNonBlocking(fd)) { OD_CLOSE_SOCKET(fd); continue; }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                     reinterpret_cast<const char*>(&one), sizeof(one));
#endif

        Conn c;
        c.fd = fd;
        c.id = nextId++;
        c.peer = addressOf(ss);
        c.since = Clock::now();
        conns.push_back(std::move(c));
    }
}

void WsServer::Impl::progressHandshake(Conn& c) {
    const size_t end = c.handshake.find("\r\n\r\n");
    if (end == std::string::npos) {
        // Not yet complete. Bounded, so a peer that never finishes cannot make
        // us buffer indefinitely.
        if (c.handshake.size() > kMaxHandshakeBytes) {
            const std::string bad = wsUpgradeResponse(WsUpgradeRequest{});
            c.out.assign(bad.begin(), bad.end());
            c.closing = true;
        }
        return;
    }

    WsUpgradeRequest req;
    if (!wsParseUpgrade(c.handshake, req)) {
        // A browser, a port scanner, a stray HTTPS probe. Answer once, plainly,
        // and close -- never fall through into framing.
        const std::string bad = wsUpgradeResponse(WsUpgradeRequest{});
        c.out.assign(bad.begin(), bad.end());
        c.closing = true;
        return;
    }

    const std::string ok = wsUpgradeResponse(req);
    c.out.insert(c.out.end(), ok.begin(), ok.end());
    c.open = true;

    // Anything after the header block is already frame data.
    const std::string rest = c.handshake.substr(end + 4);
    c.in.assign(rest.begin(), rest.end());
    c.handshake.clear();

    WsServerEvent e;
    e.kind = WsServerEvent::Kind::Connected;
    e.conn = c.id;
    e.peerAddress = c.peer;
    events.push_back(std::move(e));

    parseFrames(c);
}

void WsServer::Impl::deliver(Conn& c, uint8_t opcode, std::vector<uint8_t>&& payload) {
    WsServerEvent e;
    e.conn = c.id;
    e.peerAddress = c.peer;
    if (opcode == kOpText) {
        e.kind = WsServerEvent::Kind::Text;
        e.text.assign(payload.begin(), payload.end());
    } else {
        e.kind = WsServerEvent::Kind::Binary;
        e.data = std::move(payload);
    }
    events.push_back(std::move(e));
}

void WsServer::Impl::parseFrames(Conn& c) {
    size_t at = 0;
    for (;;) {
        if (c.closing) break;
        const size_t avail = c.in.size() - at;
        if (avail < 2) break;

        const uint8_t b0 = c.in[at];
        const uint8_t b1 = c.in[at + 1];

        const bool    fin    = (b0 & 0x80) != 0;
        const uint8_t rsv    = b0 & 0x70;
        const uint8_t opcode = b0 & 0x0F;
        const bool    masked = (b1 & 0x80) != 0;
        uint64_t      len    = b1 & 0x7F;

        // No extension was negotiated, so a reserved bit is meaningless and a
        // frame carrying one cannot be interpreted.
        if (rsv) { beginClose(c, kCloseProtocol, "reserved bits are set"); break; }

        const bool control = (opcode & 0x08) != 0;
        if (control && (!fin || len > kMaxControlBytes)) {
            beginClose(c, kCloseProtocol, "malformed control frame");
            break;
        }
        if (opcode != kOpContinuation && opcode != kOpText && opcode != kOpBinary &&
            opcode != kOpClose && opcode != kOpPing && opcode != kOpPong) {
            beginClose(c, kCloseProtocol, "unknown opcode");
            break;
        }

        size_t header = 2;
        if (len == 126) {
            if (avail < 4) break;
            len = (static_cast<uint64_t>(c.in[at + 2]) << 8) | c.in[at + 3];
            header = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = (len << 8) | c.in[at + 2 + static_cast<size_t>(i)];
            // The high bit must be clear per the RFC, and anything remotely
            // near it is refused by the size cap below in any case.
            if (len & 0x8000000000000000ULL) {
                beginClose(c, kCloseProtocol, "bad payload length");
                break;
            }
            header = 10;
        }

        // THE asymmetry with the client: a client MUST mask. An unmasked frame
        // is either a broken implementation or something that is not a client.
        if (!masked) { beginClose(c, kCloseProtocol, "client frames must be masked"); break; }
        header += 4;

        // Checked before any allocation, and against the size already buffered
        // for this message so fragments cannot add up past the cap.
        if (len > kMaxMessageBytes ||
            c.message.size() + static_cast<size_t>(len) > kMaxMessageBytes) {
            beginClose(c, kCloseTooBig, "message too large");
            break;
        }
        if (avail < header + len) break;      // wait for the rest

        const uint8_t* mask = &c.in[at + header - 4];
        uint8_t* payload = &c.in[at + header];
        for (uint64_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];

        const size_t n = static_cast<size_t>(len);

        if (control) {
            if (opcode == kOpClose) {
                beginClose(c, kCloseNormal, "");
                at += header + n;
                break;
            }
            if (opcode == kOpPing) appendFrame(c.out, kOpPong, payload, n);
            // A pong is a keepalive reply and needs no action.
            at += header + n;
            continue;
        }

        if (opcode == kOpContinuation) {
            if (!c.assembling) {
                beginClose(c, kCloseProtocol, "continuation without a start");
                break;
            }
        } else {
            if (c.assembling) {
                beginClose(c, kCloseProtocol, "interleaved messages");
                break;
            }
            c.assembling = true;
            c.messageOp = opcode;
            c.message.clear();
        }

        c.message.insert(c.message.end(), payload, payload + n);
        at += header + n;

        if (fin) {
            const uint8_t op = c.messageOp;
            c.assembling = false;
            std::vector<uint8_t> done;
            done.swap(c.message);
            deliver(c, op, std::move(done));
        }
    }

    // Bounded against the buffer as it stands now, not as it stood when the
    // loop began. Anything already consumed goes; a connection that is closing
    // keeps nothing, because nothing further will be read from it.
    if (c.closing) {
        c.in.clear();
    } else if (at) {
        if (at >= c.in.size()) c.in.clear();
        else c.in.erase(c.in.begin(), c.in.begin() + static_cast<long>(at));
    }
}

void WsServer::Impl::doRead(Conn& c) {
    for (;;) {
        uint8_t buf[4096];
        const auto rc = ::recv(c.fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (rc == 0) { drop(c); return; }
        if (rc < 0) { if (!wouldBlock()) drop(c); return; }

        const size_t n = static_cast<size_t>(rc);
        if (!c.open) {
            c.handshake.append(reinterpret_cast<char*>(buf), n);
            progressHandshake(c);
            if (c.closing) return;
        } else {
            if (c.closing) return;      // going away; nothing more to interpret
            if (c.in.size() + n > kMaxMessageBytes * 2) {
                beginClose(c, kCloseTooBig, "message too large");
                return;
            }
            c.in.insert(c.in.end(), buf, buf + n);
            parseFrames(c);
        }
    }
}

void WsServer::Impl::doWrite(Conn& c) {
    while (!c.out.empty()) {
        const auto rc = ::send(c.fd, reinterpret_cast<const char*>(c.out.data()),
#ifdef _WIN32
                               static_cast<int>(c.out.size()), 0);
#else
                               c.out.size(), MSG_NOSIGNAL);
#endif
        if (rc <= 0) { if (!wouldBlock()) drop(c); return; }
        c.out.erase(c.out.begin(), c.out.begin() + static_cast<long>(rc));
    }
    // Everything the peer was owed has gone out, including the close frame.
    if (c.closing) drop(c);
}

// ------------------------------------------------------------- interface ----

WsServer::WsServer() : m_impl(std::make_unique<Impl>()) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

WsServer::~WsServer() { stop(); }

bool WsServer::available() { return true; }

namespace {

/**
 * Bind and listen on one address family. `port` is updated when it was 0, so a
 * caller opening a second socket can match the port the first was given.
 */
SocketFd openListener(int family, uint16_t& port, bool bindAll, bool dualStack) {
    SocketFd fd = ::socket(family, SOCK_STREAM, 0);
    if (fd == OD_BAD_SOCKET) return OD_BAD_SOCKET;

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    if (family == AF_INET6) {
        // When this socket is the only one, let it serve IPv4 too. When it is
        // one of a loopback pair, keep them apart or the second bind collides.
        int v6only = dualStack ? 0 : 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    }

    int rc;
    if (family == AF_INET6) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        a.sin6_addr = bindAll ? in6addr_any : in6addr_loopback;
        rc = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(bindAll ? INADDR_ANY : INADDR_LOOPBACK);
        rc = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    }
    if (rc != 0 || ::listen(fd, 16) != 0 || !setNonBlocking(fd)) {
        OD_CLOSE_SOCKET(fd);
        return OD_BAD_SOCKET;
    }

    if (port == 0) {
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) == 0) {
            port = ss.ss_family == AF_INET6
                 ? ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port)
                 : ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
        }
    }
    return fd;
}

}  // namespace

bool WsServer::listen(uint16_t port, bool bindAll, std::string& error) {
    stop();
    auto& fds = m_impl->listenFds;

    if (bindAll) {
        // One dual-stack socket covers both families, and an IPv4 peer arrives
        // as ::ffff:a.b.c.d, which addressOf() turns back into a plain address.
        SocketFd fd = openListener(AF_INET6, port, true, /*dualStack=*/true);
        if (fd == OD_BAD_SOCKET) fd = openListener(AF_INET, port, true, false);
        if (fd != OD_BAD_SOCKET) fds.push_back(fd);
    } else {
        // 127.0.0.1 and ::1 are separate addresses; binding one does NOT serve
        // the other even on a dual-stack socket. This is not pedantry: the
        // documented way to host is a tunnel in front of a loopback port, and
        // which of the two the tunnel dials depends on the tool and on how the
        // machine resolves "localhost". Binding only one makes hosting fail for
        // some users and work for others, with nothing to see in either case.
        if (SocketFd v4 = openListener(AF_INET, port, false, false);
            v4 != OD_BAD_SOCKET) fds.push_back(v4);
        if (SocketFd v6 = openListener(AF_INET6, port, false, /*dualStack=*/false);
            v6 != OD_BAD_SOCKET) fds.push_back(v6);
    }

    if (fds.empty()) {
        error = "port " + std::to_string(port) +
                " is already in use, or this machine will not allow binding it";
        m_impl->boundPort = 0;
        return false;
    }

    m_impl->boundPort = port;
    error.clear();
    return true;
}

void WsServer::stop() {
    if (!m_impl) return;
    for (Conn& c : m_impl->conns)
        if (c.fd != OD_BAD_SOCKET) OD_CLOSE_SOCKET(c.fd);
    m_impl->conns.clear();
    m_impl->events.clear();
    for (SocketFd fd : m_impl->listenFds) OD_CLOSE_SOCKET(fd);
    m_impl->listenFds.clear();
    m_impl->boundPort = 0;
}

bool WsServer::listening() const { return !m_impl->listenFds.empty(); }
uint16_t WsServer::port() const { return m_impl->boundPort; }

void WsServer::update() {
    if (!listening()) return;
    Impl& s = *m_impl;

    const size_t listeners = s.listenFds.size();
    std::vector<pollfd> fds;
    fds.reserve(s.conns.size() + listeners);
    for (SocketFd fd : s.listenFds) fds.push_back(pollfd{fd, POLLIN, 0});
    for (Conn& c : s.conns) {
        if (c.dead) continue;
        short want = c.closing ? 0 : POLLIN;
        if (!c.out.empty()) want = static_cast<short>(want | POLLOUT);
        fds.push_back(pollfd{c.fd, want, 0});
    }

    if (OD_POLL(fds.data(), static_cast<unsigned>(fds.size()), 0) < 0) return;

    for (size_t k = 0; k < listeners; k++)
        if (fds[k].revents & POLLIN) s.doAccept(s.listenFds[k]);

    // Indices past the listeners line up with the live connections gathered
    // above; doAccept only appends, so the prefix is unchanged.
    size_t i = listeners;
    for (Conn& c : s.conns) {
        if (c.dead) continue;
        if (i >= fds.size()) break;
        const short ev = fds[i].revents;
        i++;
        if (ev & (POLLERR | POLLNVAL)) { s.drop(c); continue; }
        if (ev & POLLIN) s.doRead(c);
        if (c.dead) continue;
        if ((ev & POLLOUT) || !c.out.empty()) s.doWrite(c);
        if (c.dead) continue;
        if (ev & POLLHUP) { s.drop(c); continue; }

        // Connected, silent, and out of time: the classic way to hold a
        // server's sockets open for nothing.
        if (!c.open && !c.closing) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - c.since).count();
            if (ms > kHandshakeTimeoutMs) s.drop(c);
        }
    }

    for (size_t k = s.conns.size(); k-- > 0;)
        if (s.conns[k].dead) s.conns.erase(s.conns.begin() + static_cast<long>(k));
}

bool WsServer::nextEvent(WsServerEvent& out) {
    if (m_impl->events.empty()) return false;
    out = std::move(m_impl->events.front());
    m_impl->events.pop_front();
    return true;
}

void WsServer::send(WsConnId conn, const std::vector<uint8_t>& payload) {
    Conn* c = m_impl->find(conn);
    if (!c || !c->open || c->closing) return;
    if (c->out.size() + payload.size() > kMaxPendingOut) {
        m_impl->beginClose(*c, kCloseTooBig, "the connection could not keep up");
        return;
    }
    appendFrame(c->out, kOpBinary, payload.data(), payload.size());
}

void WsServer::sendText(WsConnId conn, const std::string& text) {
    Conn* c = m_impl->find(conn);
    if (!c || !c->open || c->closing) return;
    if (c->out.size() + text.size() > kMaxPendingOut) {
        m_impl->beginClose(*c, kCloseTooBig, "the connection could not keep up");
        return;
    }
    appendFrame(c->out, kOpText,
                reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void WsServer::closeConn(WsConnId conn, const std::string& reason) {
    if (Conn* c = m_impl->find(conn)) m_impl->beginClose(*c, kCloseNormal, reason);
}

size_t WsServer::connectionCount() const {
    size_t n = 0;
    for (const Conn& c : m_impl->conns) if (c.open && !c.dead) n++;
    return n;
}

#else   // no networking in this build, or a browser that cannot listen

struct WsServer::Impl {};

WsServer::WsServer() : m_impl(std::make_unique<Impl>()) {}
WsServer::~WsServer() = default;

bool WsServer::available() { return false; }

bool WsServer::listen(uint16_t, bool, std::string& error) {
#if defined(__EMSCRIPTEN__)
    // Not an oversight and not fixable here: a page cannot accept TCP
    // connections. Hosting from the browser needs the WebRTC path, where the
    // offer/answer goes through signalling and the media path is peer to peer.
    error = "a browser cannot listen for connections; hosting from the web "
            "build needs the peer-to-peer transport";
#else
    error = "this build of OpenDoctrines was compiled without networking "
            "(-DOD_ENABLE_NET=OFF), so it cannot host a multiplayer game";
#endif
    return false;
}

void WsServer::stop() {}
bool WsServer::listening() const { return false; }
uint16_t WsServer::port() const { return 0; }
void WsServer::update() {}
bool WsServer::nextEvent(WsServerEvent&) { return false; }
void WsServer::send(WsConnId, const std::vector<uint8_t>&) {}
void WsServer::sendText(WsConnId, const std::string&) {}
void WsServer::closeConn(WsConnId, const std::string&) {}
size_t WsServer::connectionCount() const { return 0; }

#endif
