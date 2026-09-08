// Does HttpRequest::timeoutMs now bound the CONNECT as well as the read?
#include "net/HttpClient.h"
#include <chrono>
#include <cstdio>
#include <string>

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

int main() {
    printf("HttpClient connect timeout\n\n");
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

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
