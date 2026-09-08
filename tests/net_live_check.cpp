// Not part of the suite: it needs the internet. Run by hand after touching the
// connect path, because that path carries accounts and multiplayer too.
#include "net/HttpClient.h"
#include <cstdio>
int main() {
    struct { const char* url; const char* what; } cases[] = {
        {"https://api.github.com/repos/ollama/ollama/releases/latest", "TLS + redirectless GET"},
        {"https://opendoctrines-net.opendoctrines.workers.dev/health", "the account service"},
    };
    int bad = 0;
    for (auto& c : cases) {
        HttpRequest r; r.url = c.url; r.timeoutMs = 15000; r.maxResponseBytes = 1 << 20;
        const HttpResponse res = httpRequest(r);
        // ANY status is a pass. This checks the CONNECT, not the route: a 404
        // means the handshake completed and a server answered, which is the
        // whole question after touching TlsSocket. Asserting 200 would make
        // this fail whenever an endpoint is renamed, and a connectivity check
        // that cries wolf is one nobody runs.
        const bool connected = res.status > 0;
        printf("  %-24s status=%d bytes=%zu %s\n", c.what, res.status, res.body.size(),
               connected ? "connected" : ("NO CONNECTION: " + res.error).c_str());
        if (!connected) ++bad;
    }
    return bad;
}
