// Desktop WebSocket client: mbedTLS for the transport, RFC 6455 framing here.
//
// WHY NOT A LIBRARY
//
// The project links no networking library and shells out to curl for the one
// download it does. That is not an option for a connection that has to stay
// open for a whole game, so this is the first real network dependency -- which
// is a reason to keep it small and legible rather than to give up and pull in
// something that does everything.
//
// What we own: the framing, which is the whole of RFC 6455 that a client
// needs. What mbedTLS owns, behind TlsSocket: the transport, certificate
// verification and the CSPRNG. That split puts the cryptography in a library
// that is audited and the protocol in code a reviewer can read in one sitting
// -- and TlsSocket is shared with the HTTPS client, so there is exactly one
// certificate-verification policy in the game.
//
// WHAT IS ENFORCED HERE
//
//   - Client frames are masked, as the RFC requires, with bytes from the TLS
//     CSPRNG rather than rand().
//   - A server frame that arrives masked is a protocol violation and closes
//     the connection. So is a reserved bit, an unknown opcode, a fragmented
//     control frame, and a control payload over 125 bytes.
//   - Every length is checked against a ceiling before it is used to size
//     anything.

#include "WebSocket.h"

#if !defined(__EMSCRIPTEN__) && defined(OD_ENABLE_NET)

#include "TlsSocket.h"
#include "WsHandshake.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>

namespace {

// A single message ceiling for both directions. Larger than any snapshot the
// game produces and far smaller than anything that would matter to a host's
// memory.
constexpr size_t kMaxMessageBytes = 16u * 1024 * 1024;

// A control frame is never fragmented and never larger than this; the RFC says
// so, and enforcing it means the control path never allocates.
constexpr size_t kMaxControlBytes = 125;

constexpr int kHandshakeTimeoutMs = 20000;
constexpr int kPollIntervalMs = 20;

enum : uint8_t {
    kOpContinuation = 0x0, kOpText = 0x1, kOpBinary = 0x2,
    kOpClose = 0x8, kOpPing = 0x9, kOpPong = 0xA,
};

struct Outgoing { std::vector<uint8_t> payload; bool text; };

}  // namespace

// ----------------------------------------------------------------- impl ----

struct WebSocket::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;

    std::atomic<WsState> state{WsState::Idle};
    std::atomic<bool>    stopRequested{false};
    std::string          errorText;

    std::deque<Outgoing>             outbox;
    std::deque<std::vector<uint8_t>> inboxBinary;
    std::deque<std::string>          inboxText;

    NetUrl url;
    bool  insecureAllowed = false;

    // Owned by the worker thread once it starts.
    TlsSocket sock;

    // Reassembly of a fragmented message.
    std::vector<uint8_t> assembly;
    uint8_t              assemblyOpcode = 0;
    bool                 assembling = false;

    std::vector<uint8_t> rx;    // bytes read but not yet framed

    void setError(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        if (errorText.empty()) errorText = text;
    }

    void run();
    bool openTransport();
    bool handshake();
    bool pump();
    bool readSome();
    bool consumeFrames();
    bool writeFrame(uint8_t opcode, const uint8_t* data, size_t n);
    bool writeAll(const uint8_t* data, size_t n);
    void teardown();
};

// ------------------------------------------------------------- lifecycle ----

WebSocket::WebSocket() : m_impl(std::make_unique<Impl>()) {}

WebSocket::~WebSocket() {
    close();
    if (m_impl->worker.joinable()) m_impl->worker.join();
}

bool WebSocket::available() { return true; }

bool WebSocket::connect(const std::string& url, bool allowInsecure) {
    if (m_impl->state.load() != WsState::Idle) return false;

    NetUrl parsed;
    if (!NetUrl::parse(url, parsed)) {
        m_impl->setError("that is not a usable WebSocket address");
        m_impl->state.store(WsState::Closed);
        return false;
    }
    // A join ticket travels in the first frame. In the clear it would be
    // readable by anyone on the path, and "short-lived" is not a substitute
    // for "not visible".
    if (!parsed.secure && !allowInsecure) {
        m_impl->setError("refusing to send credentials over an unencrypted ws:// connection");
        m_impl->state.store(WsState::Closed);
        return false;
    }

    m_impl->url = parsed;
    m_impl->insecureAllowed = allowInsecure;
    m_impl->state.store(WsState::Connecting);
    m_impl->worker = std::thread([impl = m_impl.get()] { impl->run(); });
    return true;
}

void WebSocket::send(const std::vector<uint8_t>& payload) {
    if (payload.size() > kMaxMessageBytes) return;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const WsState s = m_impl->state.load();
        if (s != WsState::Open && s != WsState::Connecting) return;
        m_impl->outbox.push_back(Outgoing{payload, false});
    }
    m_impl->wake.notify_all();
}

void WebSocket::sendText(const std::string& text) {
    if (text.size() > kMaxMessageBytes) return;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const WsState s = m_impl->state.load();
        if (s != WsState::Open && s != WsState::Connecting) return;
        m_impl->outbox.push_back(Outgoing{
            std::vector<uint8_t>(text.begin(), text.end()), true});
    }
    m_impl->wake.notify_all();
}

bool WebSocket::poll(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->inboxBinary.empty()) return false;
    out = std::move(m_impl->inboxBinary.front());
    m_impl->inboxBinary.pop_front();
    return true;
}

bool WebSocket::pollText(std::string& out) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->inboxText.empty()) return false;
    out = std::move(m_impl->inboxText.front());
    m_impl->inboxText.pop_front();
    return true;
}

void WebSocket::close() {
    m_impl->stopRequested.store(true);
    m_impl->wake.notify_all();
}

WsState WebSocket::state() const { return m_impl->state.load(); }

std::string WebSocket::error() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->errorText;
}

// ---------------------------------------------------------------- worker ----

void WebSocket::Impl::run() {
    if (openTransport() && handshake()) {
        state.store(WsState::Open);
        while (!stopRequested.load() && pump()) {}
    }
    if (state.load() == WsState::Open) {
        // Best effort: a close frame is politeness, and a peer that has
        // already gone will not miss it.
        const uint8_t reason[2] = {0x03, 0xE8};   // 1000, normal closure
        writeFrame(kOpClose, reason, sizeof(reason));
    }
    teardown();
    state.store(WsState::Closed);
}

bool WebSocket::Impl::openTransport() {
    // One thread does both directions here, so a read must not be allowed to
    // sit indefinitely: everything queued to send waits behind it. With a short
    // timeout the loop comes back round promptly and flushes the outbox.
    sock.setReadTimeoutMs(kPollIntervalMs);
    std::string err;
    if (!sock.open(url.host, url.port, url.secure, err)) {
        setError(err);
        return false;
    }
    return true;
}

bool WebSocket::Impl::writeAll(const uint8_t* data, size_t n) {
    return sock.writeAll(data, n);
}

bool WebSocket::Impl::handshake() {
    uint8_t keyBytes[16];
    if (!sock.random(keyBytes, sizeof(keyBytes))) {
        setError("could not generate a handshake key");
        return false;
    }
    const std::string key = wsBase64(keyBytes, sizeof(keyBytes));

    // Host carries the port only when it is not the default, which is what
    // every other client does and what virtual hosts expect.
    const bool defaultPort = (url.secure && url.port == 443) ||
                             (!url.secure && url.port == 80);
    const std::string hostHeader = defaultPort
        ? url.host : url.host + ":" + std::to_string(url.port);

    const std::string request =
        "GET " + url.path + " HTTP/1.1\r\n"
        "Host: " + hostHeader + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: OpenDoctrines\r\n"
        "\r\n";

    if (!writeAll(reinterpret_cast<const uint8_t*>(request.data()), request.size())) {
        setError("the connection closed during the handshake");
        return false;
    }

    // The same function the server uses to produce it, so the two can never
    // disagree about what a correct reply looks like.
    const std::string wantAccept = wsAcceptFor(key);

    // Read until the end of the headers. Bounded, because a server that never
    // sends one must not be able to make us buffer forever.
    std::string response;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kHandshakeTimeoutMs);
    while (response.find("\r\n\r\n") == std::string::npos) {
        if (std::chrono::steady_clock::now() > deadline) {
            setError("the server did not complete the handshake in time");
            return false;
        }
        if (response.size() > 16 * 1024) {
            setError("the server sent an oversized handshake reply");
            return false;
        }
        uint8_t buf[1024];
        const int rc = sock.read(buf, sizeof(buf));
        if (rc == TlsSocket::kRetry) continue;
        if (rc <= 0) {
            setError("the connection closed during the handshake");
            return false;
        }
        response.append(reinterpret_cast<char*>(buf), static_cast<size_t>(rc));
    }

    if (response.compare(0, 12, "HTTP/1.1 101") != 0 &&
        response.compare(0, 12, "HTTP/1.0 101") != 0) {
        const size_t eol = response.find("\r\n");
        setError("the server refused the connection: " +
                 response.substr(0, eol == std::string::npos ? 40 : eol));
        return false;
    }

    // Case-insensitive header search: the value proves the peer actually
    // spoke WebSocket rather than echoing a 101 at us.
    std::string lower = response;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t at = lower.find("sec-websocket-accept:");
    if (at == std::string::npos) {
        setError("the server's handshake reply was not a WebSocket upgrade");
        return false;
    }
    size_t valueStart = at + strlen("sec-websocket-accept:");
    while (valueStart < response.size() && response[valueStart] == ' ') valueStart++;
    const size_t valueEnd = response.find("\r\n", valueStart);
    const std::string got = response.substr(valueStart, valueEnd - valueStart);
    if (got != wantAccept) {
        setError("the server's handshake reply did not match the request");
        return false;
    }

    // Anything after the header block is already WebSocket data.
    const size_t bodyAt = response.find("\r\n\r\n") + 4;
    if (bodyAt < response.size()) {
        rx.insert(rx.end(), response.begin() + static_cast<long>(bodyAt), response.end());
    }
    return true;
}

bool WebSocket::Impl::readSome() {
    uint8_t buf[8192];
    const int rc = sock.read(buf, sizeof(buf));
    if (rc == TlsSocket::kRetry) return true;
    if (rc == TlsSocket::kClosed) return false;
    if (rc < 0) { setError("the connection was lost"); return false; }

    // The framing buffer holds at most one message plus a partial header, so
    // a peer cannot make us grow it without bound by never finishing a frame.
    if (rx.size() + static_cast<size_t>(rc) > kMaxMessageBytes + 64 * 1024) {
        setError("the server sent more than this client will buffer");
        return false;
    }
    rx.insert(rx.end(), buf, buf + rc);
    return true;
}

bool WebSocket::Impl::consumeFrames() {
    for (;;) {
        if (rx.size() < 2) return true;

        const uint8_t b0 = rx[0];
        const uint8_t b1 = rx[1];
        const bool fin = (b0 & 0x80) != 0;
        const uint8_t rsv = b0 & 0x70;
        const uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t payloadLen = b1 & 0x7F;
        size_t headerLen = 2;

        // A reserved bit set means an extension we never negotiated. Since we
        // offer none, this can only be a peer that is confused or hostile.
        if (rsv != 0) { setError("the server used an extension we did not agree to"); return false; }
        // Server-to-client frames are never masked. One that is means the peer
        // is not following the protocol, and continuing would mean guessing.
        if (masked) { setError("the server sent a masked frame"); return false; }

        if (payloadLen == 126) {
            if (rx.size() < 4) return true;
            payloadLen = (static_cast<uint64_t>(rx[2]) << 8) | rx[3];
            headerLen = 4;
        } else if (payloadLen == 127) {
            if (rx.size() < 10) return true;
            payloadLen = 0;
            for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | rx[2 + i];
            headerLen = 10;
        }

        const bool control = (opcode & 0x08) != 0;
        if (control) {
            // Control frames are never fragmented and never over 125 bytes, so
            // this path can never allocate.
            if (!fin || payloadLen > kMaxControlBytes) {
                setError("the server sent a malformed control frame");
                return false;
            }
        } else if (payloadLen > kMaxMessageBytes ||
                   assembly.size() + payloadLen > kMaxMessageBytes) {
            setError("the server sent a message larger than this client accepts");
            return false;
        }

        if (rx.size() < headerLen + payloadLen) return true;   // wait for more

        const uint8_t* payload = rx.data() + headerLen;
        const size_t n = static_cast<size_t>(payloadLen);

        switch (opcode) {
            case kOpPing:
                if (!writeFrame(kOpPong, payload, n)) return false;
                break;
            case kOpPong:
                break;
            case kOpClose:
                return false;
            case kOpText:
            case kOpBinary:
                if (assembling) {
                    setError("the server interleaved a new message into a fragmented one");
                    return false;
                }
                if (fin) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (opcode == kOpText) inboxText.emplace_back(payload, payload + n);
                    else inboxBinary.emplace_back(payload, payload + n);
                } else {
                    assembling = true;
                    assemblyOpcode = opcode;
                    assembly.assign(payload, payload + n);
                }
                break;
            case kOpContinuation:
                if (!assembling) {
                    setError("the server sent a continuation with nothing to continue");
                    return false;
                }
                assembly.insert(assembly.end(), payload, payload + n);
                if (fin) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (assemblyOpcode == kOpText)
                        inboxText.emplace_back(assembly.begin(), assembly.end());
                    else
                        inboxBinary.push_back(assembly);
                    assembly.clear();
                    assembling = false;
                }
                break;
            default:
                setError("the server sent an unknown frame type");
                return false;
        }

        rx.erase(rx.begin(), rx.begin() + static_cast<long>(headerLen + n));
    }
}

bool WebSocket::Impl::writeFrame(uint8_t opcode, const uint8_t* data, size_t n) {
    std::vector<uint8_t> frame;
    frame.reserve(n + 14);
    frame.push_back(static_cast<uint8_t>(0x80 | opcode));   // always FIN

    // Every client frame is masked. Not optional: an unmasked one is a
    // protocol violation and intermediaries are entitled to drop the
    // connection over it.
    if (n < 126) {
        frame.push_back(static_cast<uint8_t>(0x80 | n));
    } else if (n <= 0xFFFF) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>(n >> 8));
        frame.push_back(static_cast<uint8_t>(n));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--)
            frame.push_back(static_cast<uint8_t>(static_cast<uint64_t>(n) >> (i * 8)));
    }

    uint8_t mask[4];
    // From the TLS CSPRNG, not rand(). A predictable mask does not break
    // secrecy -- TLS already covers that -- but it is the kind of shortcut
    // that is copied into somewhere it does matter.
    if (!sock.random(mask, sizeof(mask))) return false;
    frame.insert(frame.end(), mask, mask + 4);

    const size_t at = frame.size();
    frame.resize(at + n);
    for (size_t i = 0; i < n; i++) frame[at + i] = data[i] ^ mask[i & 3];

    return writeAll(frame.data(), frame.size());
}

bool WebSocket::Impl::pump() {
    std::deque<Outgoing> pending;
    {
        std::unique_lock<std::mutex> lock(mutex);
        // A short wait rather than a busy loop: there is no portable way to
        // wait on both a socket and a condition variable, and 20 ms of latency
        // is nothing next to a turn timer measured in seconds.
        wake.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs),
                      [this] { return !outbox.empty() || stopRequested.load(); });
        pending.swap(outbox);
    }
    for (const auto& message : pending) {
        if (!writeFrame(message.text ? kOpText : kOpBinary,
                        message.payload.data(), message.payload.size())) {
            setError("the connection was lost while sending");
            return false;
        }
    }
    if (stopRequested.load()) return false;
    if (!readSome()) return false;
    return consumeFrames();
}

void WebSocket::Impl::teardown() {
    sock.close();
}

#endif  // !__EMSCRIPTEN__ && OD_ENABLE_NET
