// The server side of the WebSocket opening handshake.
//
// Every input here arrives on a listening port from a stranger, so most of the
// cases are malformed rather than correct. A server that is generous about a
// handshake ends up talking framed binary to something that is not a WebSocket
// client at all.
//
// Build target: NetWsServerTest. Run it; non-zero exit means a case failed.

#include "net/WsHandshake.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const char* what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what, got.empty() ? "" : "  --  ", got.c_str());
}

std::string request(const std::string& extra = "",
                    const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==",
                    const std::string& version = "13",
                    const std::string& line = "GET /session HTTP/1.1") {
    std::string r = line + "\r\nHost: example\r\n";
    if (!key.empty())     r += "Sec-WebSocket-Key: " + key + "\r\n";
    if (!version.empty()) r += "Sec-WebSocket-Version: " + version + "\r\n";
    r += "Upgrade: websocket\r\nConnection: Upgrade\r\n" + extra + "\r\n";
    return r;
}

void testBase64() {
    printf("\n=== base64 ===\n");

    // Shared with the client, which uses it to encode its Sec-WebSocket-Key.
    // RFC 4648 section 10, for the padding cases that are easy to get wrong.
    auto b64 = [](const std::string& s) {
        return wsBase64(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };
    check("no padding",  b64("foo") == "Zm9v",     b64("foo"));
    check("one pad",     b64("fo") == "Zm8=",      b64("fo"));
    check("two pads",    b64("f") == "Zg==",       b64("f"));
    check("empty",       b64("").empty());
    check("high bytes",  b64("\xfb\xff\xfe") == "+//+", b64("\xfb\xff\xfe"));
}

void testAccept() {
    printf("\n=== Sec-WebSocket-Accept ===\n");

    // The example from RFC 6455 section 1.3, and the reason it is here: the
    // magic GUID was once mistyped, and since the client computed the value it
    // expected with the SAME wrong constant, our client and our server agreed
    // perfectly while neither could talk to anything else in the world. Only a
    // vector from outside the codebase catches that.
    check("matches the RFC 6455 worked example",
          wsAcceptFor("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
          wsAcceptFor("dGhlIHNhbXBsZSBub25jZQ=="));

    check("an empty key produces nothing", wsAcceptFor("").empty());
    check("a different key gives a different accept",
          wsAcceptFor("AAAAAAAAAAAAAAAAAAAAAA==") !=
          wsAcceptFor("dGhlIHNhbXBsZSBub25jZQ=="));
}

void testParsing() {
    printf("\n=== parsing an upgrade ===\n");

    WsUpgradeRequest r;
    check("a well-formed request parses",
          wsParseUpgrade(request(), r) && r.valid() && r.version == 13 &&
          r.path == "/session");

    // Proxies add tokens, and header names are case-insensitive.
    check("tolerates a multi-token Connection header",
          wsParseUpgrade("GET / HTTP/1.1\r\nsec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "sec-websocket-version: 13\r\nupgrade: WebSocket\r\n"
                         "connection: keep-alive, Upgrade\r\n\r\n", r));

    check("keeps the path", wsParseUpgrade(
          request("", "dGhlIHNhbXBsZSBub25jZQ==", "13", "GET /session/ABCD/ws HTTP/1.1"), r) &&
          r.path == "/session/ABCD/ws");
}

void testRefusals() {
    printf("\n=== what it refuses ===\n");

    WsUpgradeRequest r;

    // Incomplete is not the same as invalid: the caller should read more. But
    // it must not be reported as a success either.
    check("an incomplete header block is refused",
          !wsParseUpgrade("GET / HTTP/1.1\r\nUpgrade: websocket\r\n", r));

    check("no key is refused", !wsParseUpgrade(request("", ""), r));
    check("a short key is refused", !wsParseUpgrade(request("", "dG9vc2hvcnQ="), r));
    check("a key with illegal characters is refused",
          !wsParseUpgrade(request("", "dGhlIHNhbXBsZSBub25jZQ<>"), r));

    check("version 8 is refused", !wsParseUpgrade(request("", "dGhlIHNhbXBsZSBub25jZQ==", "8"), r));
    check("no version is refused", !wsParseUpgrade(request("", "dGhlIHNhbXBsZSBub25jZQ==", ""), r));

    check("a POST is refused",
          !wsParseUpgrade(request("", "dGhlIHNhbXBsZSBub25jZQ==", "13",
                                  "POST /session HTTP/1.1"), r));
    check("HTTP/1.0 is refused",
          !wsParseUpgrade(request("", "dGhlIHNhbXBsZSBub25jZQ==", "13",
                                  "GET /session HTTP/1.0"), r));
    check("a path not starting with / is refused",
          !wsParseUpgrade(request("", "dGhlIHNhbXBsZSBub25jZQ==", "13",
                                  "GET session HTTP/1.1"), r));

    check("empty input is refused", !wsParseUpgrade("", r));
    check("an oversized request is refused",
          !wsParseUpgrade(std::string(20000, 'A') + "\r\n\r\n", r));
    check("an absurd header value is refused",
          !wsParseUpgrade(request("X-Pad: " + std::string(600, 'x') + "\r\n"), r));

    // Plain HTTP arriving on the port -- a browser, a scanner -- must get a
    // clean refusal rather than being upgraded into framing.
    check("an ordinary GET is refused",
          !wsParseUpgrade("GET / HTTP/1.1\r\nHost: x\r\n\r\n", r));
}

void testResponse() {
    printf("\n=== the response ===\n");

    WsUpgradeRequest r;
    wsParseUpgrade(request(), r);
    const std::string ok = wsUpgradeResponse(r);
    check("a good request gets 101",
          ok.compare(0, 12, "HTTP/1.1 101") == 0, ok.substr(0, 20));
    check("and carries the accept",
          ok.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    check("and is a complete header block",
          ok.size() >= 4 && ok.compare(ok.size() - 4, 4, "\r\n\r\n") == 0);

    // No detail about WHICH header was wrong: every one is mandatory, so it
    // helps a real client not at all and helps someone probing the port.
    const std::string bad = wsUpgradeResponse(WsUpgradeRequest{});
    check("a bad request gets a bare 400",
          bad.compare(0, 12, "HTTP/1.1 400") == 0);
    check("and leaks no reason",
          bad.find("Sec-WebSocket") == std::string::npos &&
          bad.find("version") == std::string::npos);
}

}  // namespace

// ------------------------------------------------------- a loopback client --
//
// Hand-built rather than using WebSocket.h, so the server is checked against
// the wire format instead of against the other half of the same assumptions.
// It binds to loopback only, so running the tests opens nothing to a network.

#include "net/WsServer.h"

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kQuiet = MSG_NOSIGNAL;
#else
constexpr int kQuiet = 0;
#endif

struct RawClient {
    int fd = -1;

    bool connectTo(uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
#ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    }
    // The server closes on a refusal, so these writes can hit a dead socket.
    // That must not kill the test process; see MSG_NOSIGNAL in WsServer.cpp.
    void write(const std::string& s) { ::send(fd, s.data(), s.size(), kQuiet); }
    void write(const std::vector<uint8_t>& v) { ::send(fd, v.data(), v.size(), kQuiet); }

    /** Read whatever has arrived, with a bounded wait. */
    std::string read(int ms = 400) {
        std::string out;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            char buf[4096];
            timeval tv{0, 20000};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            const auto rc = ::recv(fd, buf, sizeof(buf), 0);
            if (rc > 0) out.append(buf, static_cast<size_t>(rc));
            else if (rc == 0) break;
            if (!out.empty() && out.size() < 4096) break;
        }
        return out;
    }
    void close() { if (fd >= 0) { ::close(fd); fd = -1; } }
    ~RawClient() { close(); }
};

/** A client-to-server frame. Masked, because the RFC requires it of clients. */
std::vector<uint8_t> clientFrame(uint8_t opcode, const std::string& payload,
                                 bool fin = true, bool mask = true) {
    std::vector<uint8_t> f;
    f.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) | opcode));
    const size_t n = payload.size();
    const uint8_t maskBit = mask ? 0x80 : 0x00;
    if (n < 126) {
        f.push_back(static_cast<uint8_t>(maskBit | n));
    } else {
        f.push_back(static_cast<uint8_t>(maskBit | 126));
        f.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        f.push_back(static_cast<uint8_t>(n & 0xFF));
    }
    const uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};
    if (mask) f.insert(f.end(), key, key + 4);
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = static_cast<uint8_t>(payload[i]);
        f.push_back(mask ? static_cast<uint8_t>(b ^ key[i & 3]) : b);
    }
    return f;
}

/** Pump the server for a while so it can accept, read and reply. */
void pump(WsServer& s, int ms = 300) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        s.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

/** Drain events, returning the first of a kind. */
bool waitEvent(WsServer& s, WsServerEvent::Kind kind, WsServerEvent& out, int ms = 600) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        s.update();
        WsServerEvent e;
        while (s.nextEvent(e)) {
            if (e.kind == kind) { out = e; return true; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

std::string openingRequest() {
    return "GET /session HTTP/1.1\r\nHost: localhost\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n";
}

void testLoopback() {
    printf("\n=== over a real socket ===\n");

    WsServer server;
    std::string err;
    // Port 0: the OS picks a free one, so the tests never collide with
    // whatever else is running on the machine. Loopback only.
    if (!server.listen(0, /*bindAll=*/false, err)) {
        check("the server can listen on loopback", false, err);
        return;
    }
    check("the server can listen on loopback", server.listening() && server.port() != 0);

    // -- a well-formed client gets through and its messages arrive ----------
    {
        RawClient c;
        check("a client can connect", c.connectTo(server.port()));
        c.write(openingRequest());

        WsServerEvent ev;
        check("the server reports it connected",
              waitEvent(server, WsServerEvent::Kind::Connected, ev));

        const std::string reply = c.read();
        check("the client receives a 101",
              reply.compare(0, 12, "HTTP/1.1 101") == 0, reply.substr(0, 24));

        c.write(clientFrame(0x1, "hello"));
        check("a masked text frame is delivered",
              waitEvent(server, WsServerEvent::Kind::Text, ev) && ev.text == "hello",
              ev.text);

        // Fragmented: two data frames, the second a continuation.
        c.write(clientFrame(0x2, "abc", /*fin=*/false));
        c.write(clientFrame(0x0, "def", /*fin=*/true));
        check("fragments are reassembled",
              waitEvent(server, WsServerEvent::Kind::Binary, ev) &&
              std::string(ev.data.begin(), ev.data.end()) == "abcdef");

        // The server's own frames must NOT be masked.
        server.sendText(ev.conn, "pong-back");
        pump(server);
        const std::string out = c.read();
        check("the server's frame is unmasked",
              out.size() >= 2 && (out[0] & 0x0F) == 0x1 && !(out[1] & 0x80));
        check("and carries the payload",
              out.find("pong-back") != std::string::npos);

        check("the peer address is known", !ev.peerAddress.empty(), ev.peerAddress);

        c.close();
        check("closing is reported",
              waitEvent(server, WsServerEvent::Kind::Disconnected, ev));
    }

    // -- an UNMASKED client frame is a protocol violation -------------------
    // This is the asymmetry with the client half, and the one framing rule a
    // server must not be lenient about.
    {
        RawClient c;
        c.connectTo(server.port());
        c.write(openingRequest());
        WsServerEvent ev;
        waitEvent(server, WsServerEvent::Kind::Connected, ev);
        c.read();

        c.write(clientFrame(0x1, "unmasked", true, /*mask=*/false));
        pump(server);

        const std::string out = c.read();
        check("an unmasked client frame is refused",
              out.size() >= 2 && (out[0] & 0x0F) == 0x8, "no close frame");
        check("and the connection does not stay open",
              waitEvent(server, WsServerEvent::Kind::Disconnected, ev) ||
              server.connectionCount() == 0);
    }

    // -- plain HTTP on the port gets a clean refusal, not framing -----------
    {
        RawClient c;
        c.connectTo(server.port());
        c.write("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        pump(server);
        const std::string out = c.read();
        check("an ordinary browser request gets a 400",
              out.compare(0, 12, "HTTP/1.1 400") == 0, out.substr(0, 24));
    }

    // -- a peer that connects and says nothing is dropped, not held --------
    {
        RawClient c;
        c.connectTo(server.port());
        pump(server, 100);
        check("a silent peer does not count as a connection",
              server.connectionCount() == 0);
    }

    server.stop();
    check("the server stops", !server.listening());
}

}  // namespace

int main() {
    printf("websocket server handshake\n");
    testBase64();
    testAccept();
    testParsing();
    testRefusals();
    testResponse();
    testLoopback();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
