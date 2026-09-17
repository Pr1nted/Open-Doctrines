// Reading a tunnel's public address out of what it prints.
//
// The parsing is the part that quietly goes wrong. These tools print a great
// deal that looks like a URL -- documentation links, terms of service, update
// notices, a banner -- and handing players the first one would give them an
// address that is not the game, with no obvious symptom beyond "it does not
// work". So the samples below are the real shape of that output, banner and all.
//
// Build target: NetTunnelTest.

#include "net/Tunnel.h"

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

// What cloudflared actually prints, boxed banner and all.
const char* kCloudflaredOutput =
    "2024-05-01T10:00:00Z INF Thank you for trying Cloudflare Tunnel.\n"
    "2024-05-01T10:00:00Z INF Requests will be sent to your local webserver\n"
    "2024-05-01T10:00:01Z INF +--------------------------------------------+\n"
    "2024-05-01T10:00:01Z INF |  Your quick Tunnel has been created!       |\n"
    "2024-05-01T10:00:01Z INF |  https://odd-mint-badger-42.trycloudflare.com |\n"
    "2024-05-01T10:00:01Z INF +--------------------------------------------+\n"
    "2024-05-01T10:00:01Z INF Cannot determine default configuration path.\n";

const char* kLocalhostRunOutput =
    "Warning: Permanently added 'localhost.run' to the list of known hosts.\n"
    "===============================================================================\n"
    "Welcome to localhost.run!\n"
    "Follow your favourite reverse tunnel at https://twitter.com/localhost_run.\n"
    "**You need a suscription to use custom domains.** Head over to\n"
    "https://localhost.run/docs/custom-domains to get set up.\n"
    "To set up and manage custom domains, head over to https://admin.localhost.run/\n"
    "===============================================================================\n"
    "9f3a2b1c4d.lhr.life tunneled with tls termination, https://9f3a2b1c4d.lhr.life\n";

}  // namespace

int main() {
    printf("tunnel address parsing\n");

    printf("\n=== cloudflared ===\n");
    {
        const std::string a = tunnelParseAddress(TunnelProvider::Cloudflared,
                                                 kCloudflaredOutput);
        check("the quick tunnel address is found",
              a == "wss://odd-mint-badger-42.trycloudflare.com", a);
    }
    check("nothing is found before it announces one",
          tunnelParseAddress(TunnelProvider::Cloudflared,
                             "INF Thank you for trying Cloudflare Tunnel.\n").empty());

    printf("\n=== localhost.run ===\n");
    {
        const std::string a = tunnelParseAddress(TunnelProvider::LocalhostRun,
                                                 kLocalhostRunOutput);
        check("the tunnel address is found", a == "wss://9f3a2b1c4d.lhr.life", a);

        // The banner contains twitter.com and localhost.run/docs links. Picking
        // a URL rather than a hostname under the right domain would hand
        // players one of those.
        check("the banner's own links are not mistaken for the tunnel",
              a.find("twitter") == std::string::npos &&
              a.find("docs") == std::string::npos, a);

        // The one that actually bit: the banner advertises the provider's own
        // dashboard at admin.localhost.run. Picking it gave players an address
        // that answered 200 with somebody else's website.
        check("the provider's admin dashboard is not mistaken for the tunnel",
              a.find("admin.localhost.run") == std::string::npos, a);
    }
    check("the provider's own domain alone is not an address",
          tunnelParseAddress(TunnelProvider::LocalhostRun,
                             "Welcome to localhost.run!\n").empty());
    check("a banner with no tunnel line yields nothing",
          tunnelParseAddress(TunnelProvider::LocalhostRun,
                             "head over to https://admin.localhost.run/\n").empty());

    printf("\n=== the shape it hands back ===\n");
    {
        const std::string a = tunnelParseAddress(TunnelProvider::Cloudflared,
                                                 kCloudflaredOutput);
        // wss:// because every one of these terminates TLS at the provider and
        // forwards plaintext to our loopback port. A player given ws:// would
        // be refused by their own client for sending a ticket in the clear.
        check("it is a wss:// address", a.rfind("wss://", 0) == 0, a);
        check("it carries no path or port", a.find('/', 6) == std::string::npos &&
                                            a.find(':', 6) == std::string::npos, a);
    }

    printf("\n=== nothing to parse ===\n");
    check("empty output yields nothing",
          tunnelParseAddress(TunnelProvider::Cloudflared, "").empty());
    check("no provider yields nothing",
          tunnelParseAddress(TunnelProvider::None, kCloudflaredOutput).empty());
    check("another provider's output is not accepted",
          tunnelParseAddress(TunnelProvider::LocalhostRun, kCloudflaredOutput).empty());

    printf("\n=== what a host is told ===\n");
    check("every provider names itself",
          std::string(tunnelProviderName(TunnelProvider::Cloudflared)).size() > 3 &&
          std::string(tunnelProviderName(TunnelProvider::LocalhostRun)).size() > 3);
    check("and says how to get it",
          !tunnelProviderHowToGet(TunnelProvider::Cloudflared).empty() &&
          !tunnelProviderHowToGet(TunnelProvider::LocalhostRun).empty());
    check("and there is advice even with no tunnel at all",
          !tunnelProviderHowToGet(TunnelProvider::None).empty());

    // ── WHO IS ALLOWED TO OPEN ONE ──
    //
    // This is the privacy decision, and it is tested here because the path that
    // carries it -- mpOpenHost, after a session opens -- needs an account and a
    // live server registration, so nothing automated can reach it.
    //
    // What went wrong: the dedicated server maps eleven config fields onto the
    // host screen's members and never mapped this checkbox, which defaults to
    // ON. So `--no-tunnel` opened a cloudflared tunnel anyway, reported "no
    // tunnel is running", and announced a public trycloudflare address five
    // seconds later. Confirmed twice on 2026-09-17 with two different
    // hostnames, so each run opened its own.
    printf("\n=== who may open a tunnel ===\n");
    {
        // A host at the screen, who asked for one, behind a router: the only
        // case that should start anything.
        TunnelWanted screen;
        check("a host who asked for one gets one", tunnelWantedByHost(screen));

        TunnelWanted off; off.wanted = false;
        check("a host who declined does not", !tunnelWantedByHost(off));

        // THE REGRESSION. Note `wanted` is left at its default of true, which
        // is exactly the state the dedicated server was in.
        TunnelWanted headless; headless.headless = true;
        check("a headless process never opens one, whatever the checkbox says",
              !tunnelWantedByHost(headless));

        TunnelWanted direct; direct.bindAll = true;
        check("a directly reachable host does not need one",
              !tunnelWantedByHost(direct));

        TunnelWanted relayed; relayed.viaRelay = true;
        check("a relayed host has no port for one to reach",
              !tunnelWantedByHost(relayed));

        // Any single reason is enough on its own: a rule that only refused when
        // several lined up would be one an operator could not rely on.
        TunnelWanted all; all.headless = all.bindAll = all.viaRelay = true;
        check("the reasons do not have to agree to count", !tunnelWantedByHost(all));
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
