// Connecting through Tor, against a stand-in for Tor.
//
// The real Tor network cannot be part of a test suite -- it is slow, it is
// somebody else's, and CI may not reach it -- but everything the GAME does is
// on this side of the SOCKS port: the greeting, naming the destination by
// HOSTNAME so this machine never resolves it, reading the reply, turning Tor's
// refusal codes into sentences, and then speaking WebSocket over the result as
// if nothing were in between. So this runs a SOCKS5 server on loopback that
// records what it was asked for and pipes the connection to a real WsServer,
// and checks each of those.
//
// What it cannot check is Tor itself; docs/tournaments.md says how to try a
// real onion by hand.

#include "net/Socks5.h"
#include "net/TorClient.h"
#include "net/TlsSocket.h"
#include "net/WebSocket.h"
#include "net/WsServer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int g_checks = 0, g_failures = 0;
void check(const std::string& what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what.c_str()); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what.c_str(), got.empty() ? "" : "  --  ", got.c_str());
}

bool readN(int fd, uint8_t* p, size_t n) {
    while (n) { const ssize_t r = recv(fd, p, n, 0); if (r <= 0) return false; p += r; n -= (size_t)r; }
    return true;
}

/**
 * A SOCKS5 server that behaves like Tor's: one method, CONNECT by name, a
 * reply, then bytes both ways to `targetPort` on loopback -- whatever name it
 * was asked for, the way an onion resolves to somewhere the client cannot see.
 */
struct FakeTor {
    int listenFd = -1;
    uint16_t port = 0;
    uint8_t replyCode = 0x00;
    uint16_t targetPort = 0;
    std::string askedHost;
    uint16_t askedPort = 0;
    std::atomic<bool> stop{false};
    std::thread th;

    bool start() {
        listenFd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listenFd, (sockaddr*)&a, sizeof a) != 0 || listen(listenFd, 4) != 0) return false;
        socklen_t len = sizeof a;
        getsockname(listenFd, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { run(); });
        return true;
    }

    void run() {
        while (!stop.load()) {
            pollfd p{listenFd, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            const int c = accept(listenFd, nullptr, nullptr);
            if (c < 0) continue;
            serve(c);
        }
    }

    void serve(int c) {
        uint8_t g[3];
        if (!readN(c, g, 3) || g[0] != 5) { close(c); return; }
        const uint8_t ok[2] = {5, 0};
        send(c, ok, 2, 0);
        uint8_t h[5];
        if (!readN(c, h, 5) || h[1] != 1) { close(c); return; }
        if (h[3] == 3) {
            std::string name(h[4], '\0');
            readN(c, (uint8_t*)&name[0], name.size());
            askedHost = name;
        } else {
            askedHost = "(an address, not a name)";
            uint8_t skip[32];
            readN(c, skip, h[3] == 1 ? 3 : 15);
        }
        uint8_t pp[2];
        readN(c, pp, 2);
        askedPort = (uint16_t)((pp[0] << 8) | pp[1]);
        const uint8_t reply[10] = {5, replyCode, 0, 1, 127, 0, 0, 1, 0, 0};
        send(c, reply, sizeof reply, 0);
        if (replyCode != 0) { close(c); return; }

        // Pipe to the real server until either side closes.
        const int t = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(targetPort);
        if (connect(t, (sockaddr*)&a, sizeof a) != 0) { close(c); close(t); return; }
        std::thread([c, t, this] {
            uint8_t buf[4096];
            while (!stop.load()) {
                pollfd ps[2] = {{c, POLLIN, 0}, {t, POLLIN, 0}};
                if (poll(ps, 2, 100) <= 0) continue;
                for (int i = 0; i < 2; ++i) {
                    if (!(ps[i].revents & (POLLIN | POLLHUP))) continue;
                    const ssize_t r = recv(ps[i].fd, buf, sizeof buf, 0);
                    if (r <= 0) { close(c); close(t); return; }
                    send(i == 0 ? t : c, buf, (size_t)r, 0);
                }
            }
            close(c); close(t);
        }).detach();
    }

    ~FakeTor() {
        stop.store(true);
        if (th.joinable()) th.join();
        if (listenFd >= 0) close(listenFd);
    }
};

}  // namespace

int main() {
    printf("=== the rule: what goes through Tor ===\n");
    socks5::setRouteAll(false);
    check("an onion always does", socks5::wanted("abcdefg.onion") && socks5::wanted("X.ONION"));
    check("an ordinary host does not, unless asked", !socks5::wanted("example.com"));
    socks5::setRouteAll(true);
    check("with hide-my-IP on, it does", socks5::wanted("example.com"));
    check("this machine never does", !socks5::wanted("localhost") && !socks5::wanted("127.0.0.1") &&
                                     !socks5::wanted("::1"));

    printf("\n=== the wire ===\n");
    {
        const auto req = socks5::connectRequest("game.onion", 80);
        const std::vector<uint8_t> want = {5, 1, 0, 3, 10, 'g', 'a', 'm', 'e', '.', 'o', 'n',
                                           'i', 'o', 'n', 0, 80};
        check("CONNECT names the host, for Tor to resolve", req == want);
        const uint8_t v4[5] = {5, 0, 0, 1, 127}, v6[5] = {5, 0, 0, 4, 0}, nm[5] = {5, 0, 0, 3, 9};
        check("reply lengths by address type",
              socks5::replyRemainder(v4) == 5 && socks5::replyRemainder(v6) == 17 &&
              socks5::replyRemainder(nm) == 11);
        check("Tor's onion codes are sentences",
              socks5::replyText(0xF0).find("onion service") != std::string::npos);
    }

    printf("\n=== a WebSocket to an onion, through a stand-in for Tor ===\n");
    WsServer server;
    std::string why;
    check("a game server listens on loopback", server.listen(0, false, why), why);
    FakeTor tor;
    tor.targetPort = server.port();
    check("the stand-in Tor listens", tor.start());
    socks5::setPort(tor.port);
    socks5::setRouteAll(false);

    {
        WebSocket ws;
        const std::string onion = "ws://abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuv.onion";
        check("connecting to an onion starts", ws.connect(onion, true), ws.error());
        bool opened = false, echoed = false;
        WsConnId peer = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < until && !echoed) {
            server.update();
            WsServerEvent e;
            while (server.nextEvent(e)) {
                if (e.kind == WsServerEvent::Kind::Connected) peer = e.conn;
                if (e.kind == WsServerEvent::Kind::Binary) server.send(e.conn, e.data);  // echo
            }
            if (!opened && ws.state() == WsState::Open) {
                opened = true;
                ws.send({1, 2, 3, 4});
            }
            std::vector<uint8_t> got;
            if (ws.poll(got) && got == std::vector<uint8_t>{1, 2, 3, 4}) echoed = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check("the handshake completes through it", opened, wsStateName(ws.state()));
        check("frames go both ways", echoed);
        check("the server saw a connection", peer != 0);
        check("Tor was given the NAME, so this machine never resolved it",
              tor.askedHost == onion.substr(5), tor.askedHost);
        check("on port 80, where an onion service is published", tor.askedPort == 80,
              std::to_string(tor.askedPort));
        ws.close();
    }

    printf("\n=== refusals become sentences ===\n");
    {
        tor.replyCode = 0xF0;
        TlsSocket s;
        std::string err;
        const bool ok = s.open("nosuchonionaddressnosuchonionaddressnosuchonionaddress.onion", 80,
                               false, err);
        check("an onion Tor cannot find is refused", !ok);
        check("with a reason a player can act on", err.find("could not be found") != std::string::npos, err);
        tor.replyCode = 0x00;
    }
    {
        socks5::setPort(1);   // nothing listens there
        torclient::setAutoStart(false);
        TlsSocket s;
        std::string err;
        const bool ok = s.open("x.onion", 80, false, err);
        check("with no Tor running, joining an onion fails", !ok);
        check("and says to start Tor", err.find("Tor is not running") != std::string::npos, err);
        socks5::setPort(tor.port);
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
