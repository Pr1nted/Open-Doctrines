// Can this machine actually reach an HTTPS service?
//
// RUN IN CI ON EVERY PLATFORM, which it did not used to be -- and that is how
// Windows shipped a release where the trust store was never loaded, every HTTPS
// call was refused before a socket opened, and sign-in was impossible. The
// suite was green throughout, because the only networking any test did was
// plaintext to localhost: the TLS path, and the per-platform certificate
// loading inside it, was never entered by anything automated.
//
// ── WHAT COUNTS AS A FAILURE ──
//
// Not "the service did not answer". A CI runner's network is somebody else's
// and a check that cries wolf is one that gets deleted. What fails here is
// this machine being UNABLE TO VERIFY ANYBODY: no certificate store, or a
// handshake refused over trust. Those are properties of the build and the
// platform, they are the same every time, and they are exactly what nothing
// else notices.
#include "net/HttpClient.h"
#include "net/TlsSocket.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

bool mentionsTrust(const std::string& error) {
    static const char* const words[] = {
        "certificate", "certificates", "verify", "verified", "trust",
        "secure connection",
    };
    for (const char* w : words)
        if (error.find(w) != std::string::npos) return true;
    return false;
}

}  // namespace

int main() {
    printf("Can this machine reach an HTTPS service?\n\n");

    // First, without opening anything: does the platform give us roots at all?
    // A machine with none refuses every HTTPS connection, correctly, and the
    // player reads that as the service being down.
    {
        std::string from;
        const int roots = tlsSystemRootCount(from);
        printf("  root certificates: %d from %s\n", roots,
               from.empty() ? "nowhere" : from.c_str());
        if (roots <= 0) {
            printf("\nFAILED: this platform gave the game no root certificates, so it\n");
            printf("cannot verify any server and every HTTPS call will be refused.\n");
            return 1;
        }
    }

    struct Case { const char* url; const char* what; };
    const Case cases[] = {
        {"https://opendoctrines-net.opendoctrines.workers.dev/health", "the account service"},
        {"https://api.github.com/repos/ollama/ollama/releases/latest", "a second host"},
    };

    int trustFailures = 0;
    int unreachable = 0;
    for (const Case& c : cases) {
        std::string lastError;
        bool connected = false;
        // Three tries: a runner's DNS or a transient reset is not a finding.
        for (int attempt = 0; attempt < 3 && !connected; ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::seconds(2));
            HttpRequest r;
            r.url = c.url;
            r.timeoutMs = 15000;
            r.maxResponseBytes = 1 << 20;
            const HttpResponse res = httpRequest(r);
            // ANY status is a pass: this checks the CONNECT, not the route. A
            // 404 means the handshake completed and a server answered, which is
            // the whole question.
            connected = res.status > 0;
            lastError = res.error;
        }
        if (connected) {
            printf("  %-22s connected\n", c.what);
            continue;
        }
        if (mentionsTrust(lastError)) {
            printf("  %-22s TRUST FAILURE: %s\n", c.what, lastError.c_str());
            ++trustFailures;
        } else {
            printf("  %-22s not reachable (%s) -- treated as this runner's network\n",
                   c.what, lastError.c_str());
            ++unreachable;
        }
    }

    if (trustFailures) {
        printf("\nFAILED: this build cannot verify a server it reached. That is the\n");
        printf("platform's certificate handling, not the network.\n");
        return 1;
    }
    if (unreachable == 2) {
        // Both hosts unreachable and neither over trust: believe the runner.
        printf("\nskip: nothing was reachable from here, and no failure was about\n");
        printf("certificates. Reporting this as the network rather than the build.\n");
        return 0;
    }
    printf("\nLIVE CHECK OK\n");
    return 0;
}
