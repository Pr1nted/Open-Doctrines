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
#include <csignal>
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

NetHost::Config hostConfig(const std::string& issuer, int port = 0, bool bindAll = false) {
    NetHost::Config c;
    c.issuer = issuer;
    c.token = "mock-session-token";
    c.serverCredential = "mock-server-credential";
    c.sessionName = "Connectivity test";
    c.gameVersion = "test";
    // Port 0: the OS picks one, so running this never collides with whatever
    // else is on the machine. Loopback only -- the test opens nothing.
    //
    // `host` mode overrides both: an outside client has to be told where to
    // connect, and cannot be told a port that was chosen after it asked.
    c.port = port;
    c.bindAll = bindAll;
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

    // ---- the mod channel ---------------------------------------------------
    //
    // Mods on two machines talking to each other. This is the one path where
    // the host relays bytes it does not parse, so the things worth proving are
    // that they arrive INTACT, attributed to the right sender, and only to the
    // mod they were addressed to.
    {
        // Binary, with an embedded NUL: a relay that treats this as a C string
        // truncates it here and nowhere else.
        const std::string body("\x01\x00probe\xff", 8);
        const std::vector<uint8_t> payload(body.begin(), body.end());

        // The attribution check below is only worth anything if these two
        // differ, so that is asserted rather than assumed.
        check("the host and the player have different peer ids",
              host.lobby().hostPeerId() != w.peerId,
              std::to_string(host.lobby().hostPeerId()) + " vs " +
                  std::to_string(w.peerId));

        // Up: client -> host. The host is addressed by its own peer id.
        session.sendModMessage("od.probe", (int32_t)host.lobby().hostPeerId(), payload);
        NetModMsg got;
        const bool up = pumpUntil(&host, &session, [&] {
            drain(&host, &session, seen);
            return host.nextModMessage(got);
        }, 6000);
        check("a mod message reaches the host", up);
        if (up) {
            check("the host reads the mod it was sent for", got.modId == "od.probe",
                  got.modId);
            check("the payload arrives byte for byte",
                  got.payload == body,
                  std::to_string(got.payload.size()) + " bytes");
            // The field the client filled in was the RECIPIENT. What the host
            // reads is the sender, which the host itself wrote -- so there is
            // no field a client could have lied in.
            check("it is attributed to the sender, not to what they wrote",
                  got.peerId == w.peerId,
                  std::to_string(got.peerId) + " vs " + std::to_string(w.peerId));
        }

        // Down: host -> that one client.
        const std::string reply("pong");
        host.sendModMessage("od.probe", (int32_t)w.peerId,
                            std::vector<uint8_t>(reply.begin(), reply.end()));
        NetModMsg back;
        const bool down = pumpUntil(&host, &session, [&] {
            drain(&host, &session, seen);
            return session.nextModMessage(back);
        }, 6000);
        check("a mod message reaches the player", down);
        if (down) {
            check("the reply keeps its mod id", back.modId == "od.probe", back.modId);
            check("the reply arrives whole", back.payload == reply, back.payload);
            check("the player sees it as coming from the host",
                  back.peerId == host.lobby().hostPeerId(),
                  std::to_string(back.peerId));
        }

        // A broadcast reaches the host's own copy too -- it is a participant,
        // and it has no connection to itself to deliver over.
        session.sendModMessage("od.probe", -1,
                               std::vector<uint8_t>(reply.begin(), reply.end()));
        NetModMsg bcast;
        const bool heard = pumpUntil(&host, &session, [&] {
            drain(&host, &session, seen);
            return host.nextModMessage(bcast);
        }, 6000);
        check("a broadcast reaches the host's own copy", heard);

        // Oversized is refused outright rather than clipped: a mod that sends
        // too much must find out, not silently send something else.
        NetModMsg leaked;
        session.sendModMessage("od.probe", (int32_t)host.lobby().hostPeerId(),
                               std::vector<uint8_t>(NetLimits::kModMsg + 1, 'x'));
        // Pumped for a fixed spell rather than until something happens: what
        // is being checked is that nothing does.
        pumpUntil(&host, &session, [&] {
            drain(&host, &session, seen);
            return false;
        }, 600);
        check("an oversized mod message is not sent at all",
              !host.nextModMessage(leaked));
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

/**
 * A player whose mods cannot produce the same world as the host's.
 *
 * The failure this prevents is not a refused join -- it is the one that
 * happens when the join SUCCEEDS: two machines resolving the same turn under
 * different rules, diverging quietly, and surfacing days later as "the game is
 * broken" with nothing pointing at the cause.
 */
int testMods(const std::string& issuer) {
    printf("\n=== a mod set that cannot agree ===\n");

    NetHost host;
    Seen seen;
    NetHost::Config c = hostConfig(issuer);
    // The host requires a rules-changing mod. `shared` is the side that must
    // match: a mod that changes how a turn resolves has to be on both machines
    // or neither.
    c.requiredMods = "od.testrules@1.0.0#"
                     "0000000000000000000000000000000000000000000000000000000000000000\n";
    if (!host.open(c)) {
        check("the host starts", false, host.error());
        return 1;
    }
    const bool live = pumpUntil(&host, nullptr, [&] {
        host.update();
        return host.phase() == NetHost::Phase::Live ||
               host.phase() == NetHost::Phase::Closed;
    });
    check("the host opens", live && host.phase() == NetHost::Phase::Live, host.error());
    if (host.phase() != NetHost::Phase::Live) return 1;

    NetSession session;
    const std::string address = "127.0.0.1:" + std::to_string(host.listenPort());
    // The joiner brings nothing. Its ticket is perfectly good; its world is not.
    session.join(address, issuer, host.code(), "mock-session-token", "test", "");

    const bool settled = pumpUntil(&host, &session, [&] {
        drain(&host, &session, seen);
        return seen.welcomed || seen.rejected || seen.disconnected;
    });

    check("a player without the host's mods is not admitted", !seen.welcomed,
          seen.welcomed ? "the host admitted a mismatched world" : "");
    check("and is told, rather than just dropped",
          settled && seen.rejected);
    size_t others = 0;
    for (const NetPeer& p : host.lobby().roster())
        if (p.peerId != host.lobby().hostPeerId()) others++;
    check("nobody new is seated", others == 0,
          "roster has " + std::to_string(others) + " besides the host");

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

// ------------------------------------------------------- four in one game --
//
// One host and three joiners, which is the smallest party that can go wrong in
// the ways a two-peer test cannot see. Everything below is a thing that is
// FINE with one client and broken with three:
//
//   - seats have to be distinct, so a peer id that is really an index into
//     something reused shows up here and nowhere else;
//   - a country claimed by one player has to be refused to the next, and the
//     refusal has to name the country rather than dropping the claim silently;
//   - a turn has to begin for everyone, not for whoever was pumped last;
//   - the host has to be able to resolve a turn while a player is GONE, which
//     is the case that decides whether an evening of play survives one person's
//     wifi;
//   - and that player has to come back to their OWN country, because a
//     reconnect that re-seats you as somebody else is worse than a refusal.
//
// Identity comes from the token: the stand-in issuer maps `dev-alice` to a
// stable pseudonym for Alice, which is how three clients on one machine are
// three players rather than one player three times. See tools/playtest.sh.
struct Client {
    std::string    who;
    NetSession     session;
    Seen           seen;
    uint16_t       country = 0;
};

/** Pump a host and every client until `done()` or the deadline. */
template <typename F>
bool pumpAll(NetHost* host, std::vector<std::unique_ptr<Client>>& cs, F done, int ms = 15000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        if (host) host->update();
        for (auto& c : cs) c->session.update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void drainAll(NetHost* host, std::vector<std::unique_ptr<Client>>& cs, Seen& hostSeen) {
    drain(host, nullptr, hostSeen);
    for (auto& c : cs) drain(nullptr, &c->session, c->seen);
}

int testParty(const std::string& issuer) {
    printf("\n=== four in one game: a host and three joiners ===\n");

    NetHost host;
    Seen hostSeen;
    if (!openHost(host, issuer, hostSeen)) return 1;

    // One country each, plus a spare nobody takes -- so "everyone is seated"
    // cannot pass by accident just because the catalogue ran out.
    NetCountryList countries;
    countries.countries.push_back({101, "Testland"});
    countries.countries.push_back({102, "Otherstan"});
    countries.countries.push_back({103, "Thirdmark"});
    countries.countries.push_back({104, "Spare Republic"});
    host.setCountries(countries);
    host.setMapName("STDmaps/map");

    const std::string address = "127.0.0.1:" + std::to_string(host.listenPort());
    std::vector<std::unique_ptr<Client>> cs;
    for (const char* who : {"alice", "bob", "carol"}) {
        auto c = std::make_unique<Client>();
        c->who = who;
        const std::string token = std::string("dev-") + who;
        if (!c->session.join(address, issuer, host.code(), token, "test", "")) {
            check((std::string("the join starts for ") + who).c_str(), false,
                  c->session.error());
            return 1;
        }
        cs.push_back(std::move(c));
    }

    const bool allIn = pumpAll(&host, cs, [&] {
        drainAll(&host, cs, hostSeen);
        for (auto& c : cs)
            if (!c->seen.welcomed && !c->seen.rejected && !c->seen.disconnected) return false;
        return true;
    });
    check("all three are welcomed", allIn, allIn ? "" : "timed out");
    for (auto& c : cs)
        check((c->who + " got in").c_str(), c->seen.welcomed,
              c->seen.rejected ? "rejected: " + c->session.error() : "");
    for (auto& c : cs) if (!c->seen.welcomed) return 1;

    // Distinct seats. Three players who are secretly one seat would pass every
    // check above this line.
    std::vector<uint16_t> ids;
    for (auto& c : cs) ids.push_back(c->session.welcome().peerId);
    std::sort(ids.begin(), ids.end());
    check("the three seats are distinct",
          std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Four, not three: a playing host holds a seat like anybody else. Getting
    // this wrong is how you end up with a game that refuses to start because
    // "someone has not picked a country" and no visible someone.
    check("the roster holds four -- the host and the three joiners",
          host.lobby().roster().size() == 4,
          std::to_string(host.lobby().roster().size()));

    // The host's own seat is the one that is not a joiner's.
    uint16_t hostPeer = 0;
    for (const NetPeer& p : host.lobby().roster())
        if (std::find(ids.begin(), ids.end(), p.peerId) == ids.end()) hostPeer = p.peerId;
    check("the host has a seat of its own", hostPeer != 0);

    // Distinct identities, not just distinct sockets: the pseudonym is what a
    // ban and a stat line hang off, and three copies of one player would make
    // both meaningless.
    std::vector<std::string> psids;
    for (const NetPeer& p : host.lobby().roster()) psids.push_back(p.psid);
    std::sort(psids.begin(), psids.end());
    check("the three are three different players",
          std::adjacent_find(psids.begin(), psids.end()) == psids.end());

    pumpAll(&host, cs, [&] {
        drainAll(&host, cs, hostSeen);
        for (auto& c : cs) if (!c->seen.countries) return false;
        return true;
    }, 5000);
    for (auto& c : cs)
        check((c->who + " received the country catalogue").c_str(), c->seen.countries);

    // ---- one country each, host included ----
    const uint16_t want[] = {101, 102, 103};
    for (size_t i = 0; i < cs.size(); ++i) {
        cs[i]->country = want[i];
        cs[i]->session.claimCountry(want[i]);
    }
    // The host picks locally: it is already the authority, so it does not ask
    // itself over a socket.
    const uint16_t hostCountry = 104;
    check("the host takes the fourth country",
          host.lobby().claimCountry(hostPeer, hostCountry) == LobbyDenial::None);

    const bool seated = pumpAll(&host, cs, [&] {
        drainAll(&host, cs, hostSeen);
        size_t held = 0;
        for (const NetPeer& p : host.lobby().roster()) if (p.countryId != 0) held++;
        return held == 4;
    }, 8000);
    check("all four are holding a country", seated, seated ? "" : "timed out");

    std::vector<uint16_t> held;
    for (const NetPeer& p : host.lobby().roster()) held.push_back(p.countryId);
    std::sort(held.begin(), held.end());
    check("no country is held twice",
          std::adjacent_find(held.begin(), held.end()) == held.end());

    // A country somebody already holds must be REFUSED, not quietly taken.
    // This is the check that a lobby with one client cannot make at all.
    {
        const uint16_t taken = cs[0]->country;
        const LobbyDenial d = host.lobby().claimCountry(
            cs[1]->session.welcome().peerId, taken);
        check("claiming a country somebody holds is refused", d != LobbyDenial::None);
    }

    // ---- into the game ----
    std::string why;
    const bool begun = host.startGame(why);
    check("the game starts with all four seated", begun, why);
    if (!begun) return 1;

    host.beginTurn(1, 60000);
    const bool turned = pumpAll(&host, cs, [&] {
        drainAll(&host, cs, hostSeen);
        for (auto& c : cs) if (!c->seen.turnBegan) return false;
        return true;
    }, 8000);
    check("the turn begins for all three", turned, turned ? "" : "timed out");

    const std::vector<uint8_t> delta{0xAB, 0xCD};
    host.broadcastDelta(1, delta);
    const bool gotDelta = pumpAll(&host, cs, [&] {
        drainAll(&host, cs, hostSeen);
        for (auto& c : cs) if (!c->seen.delta) return false;
        return true;
    }, 8000);
    check("the turn's delta reaches all three", gotDelta, gotDelta ? "" : "timed out");
    for (auto& c : cs)
        check((c->who + " got turn 1's delta").c_str(), c->seen.deltaTurn == 1,
              std::to_string(c->seen.deltaTurn));

    // ---- one of them leaves ----
    // Carol closes the game. Her seat has to be remembered, not freed: it is
    // the difference between "back in a minute" and "pick a country again".
    const uint16_t carolCountry = cs[2]->country;
    cs[2]->session.leave();
    pumpAll(&host, cs, [&] { drainAll(&host, cs, hostSeen); return false; }, 1500);

    const LobbyMember* stillHers = host.lobby().holderOf(carolCountry);
    check("her country is still hers while she is away", stillHers != nullptr);
    if (stillHers)
        check("and it is recorded as disconnected rather than empty",
              !stillHers->connected);

    // The turn still resolves without her, which is the whole point of
    // substitution: three people should not wait on one person's router.
    host.beginTurn(2, 60000);
    host.broadcastDelta(2, delta);
    const bool wentOn = pumpAll(&host, cs, [&] {
        drainAll(&host, cs, hostSeen);
        return cs[0]->seen.deltaTurn == 2 && cs[1]->seen.deltaTurn == 2;
    }, 8000);
    check("the game goes on without her", wentOn, wentOn ? "" : "timed out");

    // ---- and comes back ----
    // A FRESH session, not the one she left with: a player who reconnects has
    // restarted the game, so reusing the old object would test a path nobody
    // walks. What carries her identity across is the token, and only that.
    cs[2] = std::make_unique<Client>();
    cs[2]->who = "carol";
    if (!cs[2]->session.join(address, issuer, host.code(), "dev-carol", "test", "")) {
        check("she can reconnect", false, cs[2]->session.error());
        return 1;
    }
    const bool back = pumpAll(&host, cs, [&] {
        drainAll(&host, cs, hostSeen);
        return cs[2]->seen.welcomed || cs[2]->seen.rejected;
    }, 15000);
    check("she reconnects", back && cs[2]->seen.welcomed,
          cs[2]->seen.rejected ? "rejected: " + cs[2]->session.error()
                               : (back ? "" : "timed out"));
    if (cs[2]->seen.welcomed) {
        // Asked of the HOST, which is the side that decides. A client that
        // merely believes it holds a country is the bug this would hide.
        const LobbyMember* hers = host.lobby().holderOf(carolCountry);
        check("the host still has her on her own country",
              hers && hers->peerId == cs[2]->session.welcome().peerId,
              hers ? "held by peer " + std::to_string(hers->peerId) : "held by nobody");
        check("and she is connected again", hers && hers->connected);

        // And the client agrees, because a lobby the two ends disagree about is
        // a lobby that shows one player two different games.
        uint16_t mine = 0;
        for (const NetPeer& p : cs[2]->session.roster())
            if (p.peerId == cs[2]->session.welcome().peerId) mine = p.countryId;
        check("she comes back to her OWN country", mine == carolCountry,
              std::to_string(mine) + " (wanted " + std::to_string(carolCountry) + ")");

        // Four, not five: a reconnect REPLACES a seat. A fifth row here would
        // mean her old seat was left behind holding a country nobody is in.
        check("the roster is four again, not five",
              host.lobby().roster().size() == 4,
              std::to_string(host.lobby().roster().size()));
    }

    for (auto& c : cs) c->session.leave();
    host.close();
    return 0;
}

}  // namespace

// --------------------------------------------------------------- host mode ----

/**
 * A real host that STAYS UP, for a client this process does not control.
 *
 * Every other mode here drives both ends and exits, which proves the transport
 * but cannot answer "can the browser build join?" -- the web client is a
 * separate program that has to connect to something already listening. Hosting
 * needs an issuer, a token and a server credential (Host.cpp: open), so without
 * this the only way to get a joinable host was a real account and a real sign
 * in, on a machine where somebody would rather not use their own.
 *
 * Against the mock issuer it needs neither. Same NetHost, same credential path,
 * same handshake -- just left running until interrupted.
 *
 *     NetConnectTest <issuer-url> host [port] [--all]
 *
 * --all binds every interface instead of loopback, for joining from another
 * device. Off by default: this opens a port, and a test binary should not widen
 * its blast radius unless asked.
 */
static volatile std::sig_atomic_t g_stop = 0;

int runHost(const std::string& issuer, int port, bool bindAll) {
    printf("\n=== host mode: staying up for an external client ===\n");

    NetHost host;
    Seen seen;
    if (!host.open(hostConfig(issuer, port, bindAll))) {
        printf("FAIL: the host did not start: %s\n", host.error().c_str());
        return 1;
    }
    const bool live = pumpUntil(&host, nullptr, [&] {
        drain(&host, nullptr, seen);
        return host.phase() == NetHost::Phase::Live ||
               host.phase() == NetHost::Phase::Closed;
    });
    if (!live || host.phase() != NetHost::Phase::Live) {
        printf("FAIL: the host never reached Live: %s\n",
               host.error().empty() ? "timed out" : host.error().c_str());
        return 1;
    }

    printf("\n  listening : %s\n", host.listenNote().c_str());
    printf("  join code : %s\n", host.code().c_str());
    printf("  url       : ws://%s:%d\n", bindAll ? "<this-machine>" : "127.0.0.1", port);
    printf("\n  A browser on http://localhost is a secure context, so ws:// is\n"
           "  allowed and no certificate is needed for a local test.\n");
    printf("  Ctrl-C to stop.\n\n");

    std::signal(SIGINT, [](int) { g_stop = 1; });

    size_t lastPending = SIZE_MAX;
    while (!g_stop && host.phase() == NetHost::Phase::Live) {
        host.update();
        drain(&host, nullptr, seen);
        // Only when it changes: a heartbeat every 50ms would bury the one line
        // that matters, which is somebody actually arriving.
        const size_t pending = host.unauthenticatedCount();
        if (pending != lastPending) {
            printf("  [host] %zu connection(s) mid-handshake\n", pending);
            lastPending = pending;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    printf("\n  closing.\n");
    host.close();
    return 0;
}

int main(int argc, char** argv) {
    // Unbuffered: if this crashes, the last line printed is the clue.
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        printf("usage: %s <issuer-url> <join|refuse|mods|party|live|host> [port] [--all]\n", argv[0]);
        return 2;
    }
    const std::string issuer = argv[1];
    const std::string mode = argv[2];

    printf("connectivity (%s) against %s\n", mode.c_str(), issuer.c_str());

    if (mode == "join") testJoin(issuer);
    else if (mode == "refuse") testRefuse(issuer);
    else if (mode == "mods") testMods(issuer);
    else if (mode == "party") testParty(issuer);
    else if (mode == "live") testLive(issuer);
    else if (mode == "host") {
        // Not a check-counting mode: it reports its own success and returns,
        // so the "N checks, 0 failed" tail below would be a lie about it.
        int port = (argc > 3 && argv[3][0] != '-') ? atoi(argv[3]) : 7777;
        bool all = false;
        for (int i = 3; i < argc; ++i) if (std::strcmp(argv[i], "--all") == 0) all = true;
        return runHost(issuer, port, all);
    }
    else { printf("unknown mode %s\n", mode.c_str()); return 2; }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
