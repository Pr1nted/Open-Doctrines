// Does HttpRequest::timeoutMs now bound the CONNECT as well as the read?
#include "net/HttpClient.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
// The sockets this test drives directly. It already had a _WIN32 branch for
// closeSock, so the Windows path was half-written -- but the POSIX headers were
// included unconditionally, so MSVC never got as far as it. winsock2.h must
// precede windows.h, and NOMINMAX with it for the usual reason.
#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "net/TlsSocket.h"

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& what) {
    ++checks; printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str()); if (!c) ++fails;
}

static long long msFor(const char* url, int timeoutMs) {
    HttpRequest r;
    r.url = url;
    r.timeoutMs = timeoutMs;
    r.maxResponseBytes = 4096;
    const auto t0 = std::chrono::steady_clock::now();
    (void)httpRequest(r);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

// ── The address walk, driven directly ──
//
// RFC5737 reserves these for documentation and guarantees they are not routed,
// so they BLACKHOLE rather than refuse -- which is the only way to measure how
// the budget divides. "localhost" has two addresses but both refuse instantly,
// so it shows the walk happens and nothing about the timing.
#if defined(_WIN32)
static void closeSock(int fd) { closesocket((SOCKET)fd); }
#else
static void closeSock(int fd) { ::close(fd); }
#endif

static addrinfo* makeAddrPort(const char* ip, uint16_t port, addrinfo* next) {
    auto* sa = new sockaddr_in{};
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    inet_pton(AF_INET, ip, &sa->sin_addr);
    auto* ai = new addrinfo{};
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_protocol = IPPROTO_TCP;
    ai->ai_addr = (sockaddr*)sa;
    ai->ai_addrlen = sizeof(sockaddr_in);
    ai->ai_next = next;
    return ai;
}

static addrinfo* makeAddr(const char* ip, addrinfo* next) {
    return makeAddrPort(ip, 443, next);
}

static long long walkMs(const std::vector<const char*>& ips, int timeoutMs) {
    addrinfo* list = nullptr;
    for (auto it = ips.rbegin(); it != ips.rend(); ++it) list = makeAddr(*it, list);
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const int fd = odnet::connectAny(list, timeoutMs, "test", err);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    if (fd >= 0) printf("  UNEXPECTED: something answered on a TEST-NET address\n");
    for (addrinfo* a = list; a;) { addrinfo* n = a->ai_next; delete (sockaddr_in*)a->ai_addr; delete a; a = n; }
    return ms;
}

int main() {
    printf("HttpClient connect timeout\n\n");
#if defined(_WIN32)
    // socket() fails outright on Windows until the process has done this, and
    // the failure is a silent -1 rather than anything that names the cause.
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("  FAIL  WSAStartup\n");
        return 1;
    }
#endif
    // 10.255.255.1 is RFC1918 space with nothing on it: packets are dropped
    // rather than refused, which is exactly the shape that used to hang.
    const long long blackholed = msFor("https://10.255.255.1/x", 5000);
    printf("  blackholed host, timeoutMs=5000 -> %lld ms\n", blackholed);
    ok(blackholed < 20000, "a host that drops packets no longer waits out the OS (~75s)");
    ok(blackholed >= 2500,  "and it is not failing instantly for some other reason");

    const long long refused = msFor("http://127.0.0.1:9/x", 5000);
    printf("  refused port, timeoutMs=5000     -> %lld ms\n", refused);
    ok(refused < 3000, "a refused connection still fails at once, not on the timeout");

    // MULTIPLE ADDRESSES MUST SHARE ONE BUDGET, not get one each. "localhost"
    // resolves to ::1 and 127.0.0.1 on every platform this ships on, so this
    // exercises the walk; both refuse, so it also shows that walking a list
    // does not multiply the wait.
    const long long twoAddrs = msFor("http://localhost:9/x", 5000);
    printf("  localhost:9 (2 addresses)        -> %lld ms\n", twoAddrs);
    ok(twoAddrs < 3000, "walking several addresses does not multiply the budget");

    // THE ASSERTION THE OLD SHAPE COULD NOT MAKE. Two blackholed addresses
    // share one budget, so this is ~3s, not ~6s. If the budget were per
    // address -- the obvious way to write this loop -- it would be 6s and the
    // wait would grow with the size of the DNS answer.
    const long long two = walkMs({"192.0.2.1", "198.51.100.1"}, 3000);
    printf("  2 blackholed addresses, 3000ms   -> %lld ms\n", two);
    ok(two < 4500, "two dead addresses share one budget, not one each");
    ok(two > 2000,  "and the whole budget is actually used before giving up");

    // ── THE PROPERTY THE DIVISION ACTUALLY PROTECTS ──
    //
    // A first assertion here claimed the division is what bounds the total.
    // It is not: the `remaining <= 0` break does that, and a deliberately
    // per-address budget still passed the timing checks above. What the
    // division buys is that a DEAD FIRST ADDRESS DOES NOT STARVE A WORKING
    // SECOND ONE -- which is the entire bug this was written for, a broken
    // IPv6 route ahead of a working IPv4 one.
    {
        const int srv = (int)socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;                       // any free port
        bind(srv, (sockaddr*)&addr, sizeof(addr));
        listen(srv, 4);
        socklen_t alen = sizeof(addr);
        getsockname(srv, (sockaddr*)&addr, &alen);
        const uint16_t port = ntohs(addr.sin_port);

        addrinfo* second = makeAddrPort("127.0.0.1", port, nullptr);
        addrinfo* list = makeAddrPort("192.0.2.1", 443, second);   // dead first
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const int fd = odnet::connectAny(list, 3000, "test", err);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        printf("  dead first, live second, 3000ms  -> %lld ms, %s\n", (long long)ms,
               fd >= 0 ? "connected" : ("no connection: " + err).c_str());
        ok(fd >= 0, "a blackholed first address does not starve a working second");
        ok(ms < 2900, "and it moves on well before the whole budget is gone");
        if (fd >= 0) closeSock(fd);
        closeSock(srv);
        for (addrinfo* a = list; a;) { addrinfo* n = a->ai_next; delete (sockaddr_in*)a->ai_addr; delete a; a = n; }
    }

    // Three forces the 1200ms floor on the earlier attempts; the clamp to
    // `remaining` is what stops the last one running past the deadline. Two
    // guards that have to agree, which is the part worth measuring.
    const long long three = walkMs({"192.0.2.1", "198.51.100.1", "203.0.113.1"}, 3000);
    printf("  3 blackholed addresses, 3000ms   -> %lld ms\n", three);
    ok(three < 4500, "the per-attempt floor cannot push the total past the deadline");

    printf("\n%d checks, %d failed\n", checks, fails);
#if defined(_WIN32)
    WSACleanup();
#endif
    return fails == 0 ? 0 : 1;
}
