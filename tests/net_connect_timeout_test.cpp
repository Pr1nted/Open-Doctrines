// Does HttpRequest::timeoutMs now bound the CONNECT as well as the read?
#include "net/HttpClient.h"
#include <cerrno>
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
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
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

// ── A blackhole on this machine ──
//
// The check below used 10.255.255.1 and trusted the network to drop the
// packets. It does not everywhere: on a network that is itself in 10/8 the
// address is routed, and something upstream answered with an error after 0 to
// 4 s -- so "is not failing instantly" failed one run in three, measuring the
// router rather than HttpClient.
//
// A listener with a full backlog that never accepts is the same shape with no
// network in it: macOS and Linux drop a SYN they have no room for, silently,
// so the connect waits exactly as it would on a dead host. Windows answers a
// full backlog with a reset instead, which blackhole() detects (the probe
// connection does not stay pending) and reports as unavailable.
static bool setNonBlocking(int fd) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &on) == 0;
#else
    const int f = fcntl(fd, F_GETFL, 0);
    return f >= 0 && fcntl(fd, F_SETFL, f | O_NONBLOCK) == 0;
#endif
}

// Starts a non-blocking connect; true when it is still pending after waitMs.
static bool connectStaysPending(int fd, const sockaddr_in& to, int waitMs) {
    setNonBlocking(fd);
    if (connect(fd, (const sockaddr*)&to, sizeof(to)) == 0) return false;
#if defined(_WIN32)
    if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
    WSAPOLLFD p{(SOCKET)fd, POLLOUT, 0};
    return WSAPoll(&p, 1, waitMs) == 0;
#else
    if (errno != EINPROGRESS) return false;
    pollfd p{fd, POLLOUT, 0};
    return poll(&p, 1, waitMs) == 0;
#endif
}

struct Blackhole {
    int listener = -1;
    std::vector<int> fillers;
    uint16_t port = 0;
    ~Blackhole() {
        for (int f : fillers) closeSock(f);
        if (listener >= 0) closeSock(listener);
    }
};

// Fills a loopback listener's backlog until a further connection is left
// waiting. False, with the reason, when this platform does not do that.
static bool blackhole(Blackhole& bh, std::string& why) {
    bh.listener = (int)socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t alen = sizeof(addr);
    if (bh.listener < 0 || bind(bh.listener, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(bh.listener, 1) != 0 || getsockname(bh.listener, (sockaddr*)&addr, &alen) != 0) {
        why = "could not open a loopback listener";
        return false;
    }
    bh.port = ntohs(addr.sin_port);
    for (int i = 0; i < 64; ++i) {
        const int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { why = "socket() failed while filling the backlog"; return false; }
        bh.fillers.push_back(fd);
        if (connectStaysPending(fd, addr, 300)) return true;   // the queue is full
    }
    why = "the listener never stopped completing connections (a full backlog is refused here, not dropped)";
    return false;
}

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

    // ── CAN THIS MACHINE VERIFY ANYBODY AT ALL? ──
    //
    // With no root certificates the game refuses every HTTPS connection --
    // which is right, since it cannot check who it is talking to -- and the
    // player sees "cannot reach the account service" and reasonably concludes
    // the service is down. Every platform keeps its roots somewhere different
    // (a file, a directory, an API), and nothing could ask whether THIS one
    // had any without opening a socket, so nobody asked until a player on the
    // affected platform said sign-in did not work.
    //
    // First in this file because it is the cheapest question and the one whose
    // wrong answer explains the most.
    {
        std::string from;
        const int roots = tlsSystemRootCount(from);
        ok(roots > 0, "this platform gives the game root certificates (" +
                      std::to_string(roots) + " from " +
                      (from.empty() ? "nowhere" : from) + ")");
    }
#if defined(_WIN32)
    // socket() fails outright on Windows until the process has done this, and
    // the failure is a silent -1 rather than anything that names the cause.
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("  FAIL  WSAStartup\n");
        return 1;
    }
#endif
    // A host that takes the SYN and never answers -- the shape that used to
    // hang for the OS's ~75 s. Local where the platform allows it (see
    // blackhole()); otherwise 10.255.255.1, which only a network that drops
    // it can test, so an address that answers fast is a SKIP, not a result.
    {
        Blackhole bh;
        std::string why;
        if (blackhole(bh, why)) {
            // Bounded by the budget, not by 20 s. The OS does not wait ~75 s
            // on a full local backlog the way it does on a dead remote host:
            // macOS gave up after 7.8 s with the timeout deliberately ignored,
            // which a 20 s ceiling passed. 3000 ms is HttpClient's connect
            // floor (it clamps the connect to 3..15 s), so the connect should
            // take 3 s; the 5 s ceiling still fails when it is not honoured.
            const std::string url = "https://127.0.0.1:" + std::to_string(bh.port) + "/x";
            const long long blackholed = msFor(url.c_str(), 3000);
            printf("  blackholed host (full local backlog), timeoutMs=3000 -> %lld ms\n", blackholed);
            ok(blackholed < 5000, "a host that drops packets waits out timeoutMs, not the OS");
            ok(blackholed >= 2000, "and it is not failing instantly for some other reason");
        } else {
            printf("  no local blackhole: %s\n", why.c_str());
            const long long blackholed = msFor("https://10.255.255.1/x", 5000);
            printf("  blackholed host (10.255.255.1), timeoutMs=5000 -> %lld ms\n", blackholed);
            ok(blackholed < 20000, "a host that drops packets no longer waits out the OS (~75s)");
            if (blackholed >= 2500)
                ok(true, "and it is not failing instantly for some other reason");
            else
                printf("  SKIP  the network answered 10.255.255.1 in %lld ms instead of dropping it,\n"
                       "        so this machine cannot show the timeout being waited out\n", blackholed);
        }
    }

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
