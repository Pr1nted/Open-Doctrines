// Can a player actually join a host? End to end, over a real socket.
//
// This is the one test that exercises the SEAM. Everything either side of it is
// already covered on its own -- the handshake against RFC 6455's vectors,
// ticket verification against RFC 8032's, the lobby rules with no socket at all
// -- and none of that proves a NetHost and a NetSession can find each other.
//
// What runs here is the real thing on both sides: the real listener, the real
// challenge, the real ticket verification, the real lobby. Only the account
// service is a stand-in (tests/mock_issuer.mjs), because the alternative is a
// network and somebody's real credentials.
//
// Usage (see tests/connectivity_test.sh, which sets all this up):
//     NetConnectTest <issuer-url> join      -- expect a player to get in
//     NetConnectTest <issuer-url> refuse    -- expect the host to refuse them
//     NetConnectTest <issuer-url> live      -- the same, against the REAL
//                                              service (tests/live_smoke_test.sh)
//
// The `refuse` mode points at a mock publishing a key it does NOT sign with. It
// matters as much as `join`: a host that admits everyone would pass every
// positive case in this file.

#include "net/HttpClient.h"
#include "net/Host.h"
#include "net/Session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const char* what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what, got.empty() ? "" : "  --  ", got.c_str());
}

using Clock = std::chrono::steady_clock;

/** Pump both sides until `done()` or the deadline. Returns whether it landed. */
template <typename F>
bool pumpUntil(NetHost* host, NetSession* session, F done, int ms = 15000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        if (host) host->update();
        if (session) session->update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/** Drain events so queues do not grow, remembering what was seen. */
struct Seen {
    bool welcomed = false;
    bool countries = false;
    bool rejected = false;
    bool disconnected = false;
    bool peerJoined = false;
    bool turnBegan = false;
    bool delta = false;
    uint32_t deltaTurn = 0;
    std::string hostError;
    std::string sessionError;
};

void drain(NetHost* host, NetSession* session, Seen& seen) {
    if (host) {
        NetHostEvent e;
        while (host->nextEvent(e)) {
            if (e.kind == NetHostEvent::Kind::PeerJoined) seen.peerJoined = true;
            if (e.kind == NetHostEvent::Kind::Failed) seen.hostError = e.text;
        }
    }
    if (session) {
        NetSessionEvent e;
        while (session->nextEvent(e)) {
            switch (e.kind) {
                case NetSessionEvent::Kind::Welcomed:       seen.welcomed = true; break;
                case NetSessionEvent::Kind::CountriesKnown: seen.countries = true; break;
                case NetSessionEvent::Kind::Rejected:       seen.rejected = true; break;
                case NetSessionEvent::Kind::Disconnected:   seen.disconnected = true; break;
                case NetSessionEvent::Kind::TurnBegan:      seen.turnBegan = true; break;
                case NetSessionEvent::Kind::Delta:
                    seen.delta = true; seen.deltaTurn = e.turnNumber; break;
                default: break;
            }
        }
    }
}

NetHost::Config hostConfig(const std::string& issuer) {
    NetHost::Config c;
    c.issuer = issuer;
    c.token = "mock-session-token";
    c.serverCredential = "mock-server-credential";
    c.sessionName = "Connectivity test";
    c.gameVersion = "test";
    // Port 0: the OS picks one, so running this never collides with whatever
    // else is on the machine. Loopback only -- the test opens nothing.
    c.port = 0;
    c.bindAll = false;
    c.lobby.maxPlayers = 8;
    return c;
}

/** Bring a host up and wait until it is actually accepting players. */
bool openHost(NetHost& host, const std::string& issuer, Seen& seen) {
    if (!host.open(hostConfig(issuer))) {
        check("the host starts", false, host.error());
        return false;
    }
    const bool live = pumpUntil(&host, nullptr, [&] {
        drain(&host, nullptr, seen);
        return host.phase() == NetHost::Phase::Live ||
               host.phase() == NetHost::Phase::Closed;
    });
    if (!live || host.phase() != NetHost::Phase::Live) {
        check("the host reaches Live", false,
              host.error().empty() ? "timed out" : host.error());
        return false;
    }
    return true;
}

// ------------------------------------------------------------------- cases --

int testJoin(const std::string& issuer) {
    printf("\n=== a player joins a host ===\n");

    NetHost host;
    Seen seen;
    if (!openHost(host, issuer, seen)) return 1;

    check("the host is live", host.phase() == NetHost::Phase::Live);
    check("it has an invite code", !host.code().empty(), host.code());
    check("it bound a port", host.listenPort() != 0,
          std::to_string(host.listenPort()));

    // The catalogue a lobby picks from. Real hosts read this from the loaded
    // world; here it is stated directly, because the world is not what is
    // under test.
    NetCountryList countries;
    countries.countries.push_back({101, "Testland"});
    countries.countries.push_back({102, "Otherstan"});
    host.setCountries(countries);
    host.setMapName("STDmaps/map");

    NetSession session;
    const std::string address = "127.0.0.1:" + std::to_string(host.listenPort());
    const bool started = session.join(address, issuer, host.code(),
                                      "mock-session-token", "test", "");
    check("the join starts", started, session.error());
    if (!started) return 1;

    const bool welcomed = pumpUntil(&host, &session, [&] {
        drain(&host, &session, seen);
        return seen.welcomed || seen.rejected || seen.disconnected;
    });

    check("the player is welcomed", welcomed && seen.welcomed,
          seen.rejected ? "rejected: " + session.error()
                        : (welcomed ? "" : "timed out"));
    if (!seen.welcomed) return 1;

    const NetWelcome& w = session.welcome();
    check("the session name came through", w.sessionName == "Connectivity test",
          w.sessionName);
    check("the map name came through", w.mapName == "STDmaps/map", w.mapName);
    check("the host declared itself", w.host.declared());
    check("the player was given a seat", w.peerId != 0);
    check("the player is not a spectator", !w.spectator);

    // The host's own view has to agree, or the two ends have different ideas
    // about who is in the game.
    check("the host saw the join", seen.peerJoined);
    bool hostSeesPlayer = false;
    for (const NetPeer& p : host.lobby().roster())
        if (p.peerId == w.peerId) hostSeesPlayer = true;
    check("the host's roster holds that seat", hostSeesPlayer);

    // The name and badge came from the TICKET, not from anything the client
    // said about itself. That is what makes them worth showing.
    bool namedByTicket = false;
    for (const NetPeer& p : host.lobby().roster())
        if (p.name.rfind("Tester", 0) == 0) namedByTicket = true;
    check("the player's name came from the ticket", namedByTicket);

    // The catalogue must arrive without being asked for.
    pumpUntil(&host, &session, [&] { drain(&host, &session, seen); return seen.countries; }, 4000);
    check("the country catalogue arrived", seen.countries);
    check("it has the countries the host published",
          session.countries().size() == 2 &&
          session.countries()[0].name == "Testland",
          "got " + std::to_string(session.countries().size()));

    // Claiming a country has to move the HOST's state; the client asking is
    // not the same as the server agreeing.
    session.claimCountry(101);
    const bool claimed = pumpUntil(&host, &session, [&] {
        drain(&host, &session, seen);
        for (const NetPeer& p : host.lobby().roster())
            if (p.peerId == w.peerId && p.countryId == 101) return true;
        return false;
    }, 5000);
    check("a country claim reaches the host", claimed);

    // ---- the turn loop -------------------------------------------------
    //
    // The host is the authority: it declares a turn, collects orders, and
    // sends back the delta. Only the transport is exercised here -- resolving
    // a turn needs a loaded world, which is the game's job, not this test's.
    {
        // The host must hold a country too, or start() refuses.
        host.lobby().claimCountry(host.lobby().hostPeerId(), 102);
        std::string why;
        const bool started = host.startGame(why);
        check("the host can start the game", started, why);

        if (started) {
            host.beginTurn(1, 60000);
            const bool told = pumpUntil(&host, &session, [&] {
                drain(&host, &session, seen);
                return seen.turnBegan;
            }, 5000);
            check("the player is told the turn began", told);

            // Before anything is submitted, the host is still waiting on this
            // player -- otherwise a turn would resolve with nobody's orders.
            const auto missingBefore = host.lobby().missingSubmissions(1);
            check("the host is waiting for this player",
                  std::find(missingBefore.begin(), missingBefore.end(), w.peerId)
                      != missingBefore.end());

            const std::string ordersText = "{\"pendingMoveOrders\":[]}";
            session.submitOrders(1, std::vector<uint8_t>(ordersText.begin(),
                                                         ordersText.end()));
            const bool got = pumpUntil(&host, &session, [&] {
                drain(&host, &session, seen);
                const auto missing = host.lobby().missingSubmissions(1);
                return std::find(missing.begin(), missing.end(), w.peerId) == missing.end();
            }, 6000);
            check("submitted orders reach the host", got);

            // And the delta comes back down.
            const std::string deltaBytes = "not-a-real-delta";
            host.broadcastDelta(1, std::vector<uint8_t>(deltaBytes.begin(), deltaBytes.end()));
            const bool delta = pumpUntil(&host, &session, [&] {
                drain(&host, &session, seen);
                return seen.delta;
            }, 5000);
            check("the turn delta reaches the player", delta);
            check("it carries the turn number", seen.deltaTurn == 1,
                  std::to_string(seen.deltaTurn));
        }
    }

    // And leaving has to be noticed, or a disconnected player holds a seat
    // forever.
    session.leave();
    const bool left = pumpUntil(&host, nullptr, [&] {
        drain(&host, nullptr, seen);
        for (const NetPeer& p : host.lobby().roster())
            if (p.peerId == w.peerId) return !p.connected;
        return true;
    }, 6000);
    check("the host notices the player leave", left);

    host.close();
    return 0;
}

int testRefuse(const std::string& issuer) {
    printf("\n=== a ticket the host cannot verify ===\n");
    printf("  (the mock publishes a key it does not sign with)\n");

    NetHost host;
    Seen seen;
    if (!openHost(host, issuer, seen)) return 1;

    NetSession session;
    const std::string address = "127.0.0.1:" + std::to_string(host.listenPort());
    session.join(address, issuer, host.code(), "mock-session-token", "test", "");

    const bool settled = pumpUntil(&host, &session, [&] {
        drain(&host, &session, seen);
        return seen.welcomed || seen.rejected || seen.disconnected;
    });

    check("the join does not succeed", !seen.welcomed,
          seen.welcomed ? "the host admitted an unverifiable ticket" : "");
    check("the player is told", settled && (seen.rejected || seen.disconnected));
    // The host holds a seat of its own (it is playing), so "empty" is the
    // wrong question -- what matters is that nobody NEW got one.
    size_t others = 0;
    for (const NetPeer& p : host.lobby().roster())
        if (p.peerId != host.lobby().hostPeerId()) others++;
    check("nobody new is seated", others == 0,
          "roster has " + std::to_string(others) + " besides the host");

    host.close();
    return 0;
}

/**
 * The same join, against the REAL account service.
 *
 * Differs from testJoin in exactly two ways, and both are the point: the token
 * is a genuine one from the environment, and the server credential must be
 * fetched rather than invented. Everything the game does afterwards is
 * identical, which is what makes this a check on DRIFT between the stand-in and
 * what is deployed.
 */
int testLive(const std::string& issuer) {
    printf("\n=== a real join against the deployed service ===\n");

    const char* token = getenv("OD_LIVE_TOKEN");
    if (!token || !*token) {
        check("a session token was supplied", false,
              "set OD_LIVE_TOKEN, or run tests/live_smoke_test.sh");
        return 1;
    }

    // A real server credential, from the real service. The stand-in accepts any
    // string here; the deployed one does not, which is exactly the sort of
    // difference this test exists to surface.
    HttpRequest reg;
    reg.method = "POST";
    reg.url = issuer + "/server/register";
    reg.bearer = token;
    reg.allowInsecure = issuer.rfind("http://localhost", 0) == 0;
    const HttpResponse regRes = httpRequest(reg);
    const std::string credential = httpJsonString(regRes.body, "serverCredential", 4096);
    check("the service issues a server credential", !credential.empty(),
          regRes.error.empty() ? httpJsonString(regRes.body, "message", 256)
                               : regRes.error);
    if (credential.empty()) return 1;

    NetHost host;
    Seen seen;
    NetHost::Config c = hostConfig(issuer);
    c.token = token;
    c.serverCredential = credential;
    c.sessionName = "Live smoke test";
    if (!host.open(c)) {
        check("the host starts", false, host.error());
        return 1;
    }
    const bool live = pumpUntil(&host, nullptr, [&] {
        drain(&host, nullptr, seen);
        return host.phase() == NetHost::Phase::Live ||
               host.phase() == NetHost::Phase::Closed;
    }, 30000);
    check("the host opens a real session", live && host.phase() == NetHost::Phase::Live,
          host.error().empty() ? "timed out" : host.error());
    if (host.phase() != NetHost::Phase::Live) return 1;
    check("the service gave it an invite code", !host.code().empty(), host.code());

    NetCountryList countries;
    countries.countries.push_back({101, "Testland"});
    host.setCountries(countries);
    host.setMapName("STDmaps/map");

    NetSession session;
    const std::string address = "127.0.0.1:" + std::to_string(host.listenPort());
    session.join(address, issuer, host.code(), token, "test", "");

    // Generous, because this is a real network: two HTTPS round trips to the
    // account service, either of which may be waiting on a cold Worker. The
    // loopback test can afford to be impatient; this cannot.
    const bool settled = pumpUntil(&host, &session, [&] {
        drain(&host, &session, seen);
        return seen.welcomed || seen.rejected || seen.disconnected;
    }, 90000);

    check("a ticket minted by the real service is accepted",
          settled && seen.welcomed,
          seen.rejected ? "rejected: " + session.error()
                        : (settled ? "" : std::string("stuck in ") +
                              netSessionPhaseName(session.phase()) +
                              (session.error().empty() ? "" : " -- " + session.error())));

    if (seen.welcomed) {
        // The name came out of a ticket the real service signed, so this also
        // says the pairwise pseudonym and nickname survived the round trip.
        bool named = false;
        for (const NetPeer& p : host.lobby().roster())
            if (p.peerId == session.welcome().peerId && !p.name.empty()) named = true;
        check("the player has the name the service gave them", named);
    }

    session.leave();
    host.close();
    printf("  (the session opened here is empty and now closed)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered: if this crashes, the last line printed is the clue.
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        printf("usage: %s <issuer-url> <join|refuse>\n", argv[0]);
        return 2;
    }
    const std::string issuer = argv[1];
    const std::string mode = argv[2];

    printf("connectivity (%s) against %s\n", mode.c_str(), issuer.c_str());

    if (mode == "join") testJoin(issuer);
    else if (mode == "refuse") testRefuse(issuer);
    else if (mode == "live") testLive(issuer);
    else { printf("unknown mode %s\n", mode.c_str()); return 2; }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
