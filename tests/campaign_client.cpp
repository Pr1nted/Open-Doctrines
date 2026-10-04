// A player with no window, for driving a real dedicated server from a script.
//
// WHY THIS EXISTS
//
// tests/net_connect_test.cpp drives a NetHost and NetSessions inside ONE
// process, which proves the protocol and nothing about the server binary:
// whether OpenDoctrinesServer loads a world, publishes its countries, starts a
// game, keeps a turn clock across a restart, or hands a returning player their
// own country back after being killed. Those are what a month-long tournament
// stands on, and they only exist in the real binary.
//
// So this is a client that joins, does one scripted thing, and prints every
// event as a line a shell script can grep:
//
//   WELCOMED peer=3 spectator=0
//   COUNTRIES n=53
//   LOBBYMAP w=512 h=256 colors=54
//   VOICE https://discord.gg/xyz
//   ROSTER me=101 submitted=1 state=game
//   TURN 4 deadline_ms=86399120
//   SNAPSHOT turn=3
//   SUBMITTED 4
//   ACKED 4          (the host's roster says it has them)
//   REJECTED <reason>
//   DONE
//
//   campaign_client --issuer URL --address HOST:PORT --code CODE --token dev-bob
//                   [--claim <cid> | --claim-first | --claim-index N]
//                   [--submit | --submit-after SECONDS] [--marker TEXT]
//                   [--seconds N] [--until "<line prefix>"]
//                   [--tor-port N | --tor-data DIR] [--tor-all]
//
// --until stops as soon as a line starting with that text is printed, so a
// script can wait for a specific moment without sleeping for a guessed time.

#include "net/LobbyMap.h"
#include "net/NetProtocol.h"
#include "net/Session.h"
#include "net/Socks5.h"
#include "net/TorClient.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string g_until;
bool g_stop = false;

void say(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    if (!g_until.empty() && line.rfind(g_until, 0) == 0) g_stop = true;
}

const char* stateName(NetSessionState s) {
    switch (s) {
        case NetSessionState::Lobby: return "lobby";
        case NetSessionState::Game:  return "game";
        case NetSessionState::Ended: return "ended";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    std::string issuer, address, code, token, marker = "probe";
    int claim = 0, claimIndex = -1;
    bool claimFirst = false, submit = false;
    double seconds = 30.0;
    double submitAfter = 0.0;      // seconds after a turn opens; a late player
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--issuer") issuer = next();
        else if (a == "--address") address = next();
        else if (a == "--code") code = next();
        else if (a == "--token") token = next();
        else if (a == "--claim") claim = std::atoi(next().c_str());
        else if (a == "--claim-first") claimFirst = true;
        else if (a == "--claim-index") claimIndex = std::atoi(next().c_str());
        else if (a == "--submit") submit = true;
        else if (a == "--marker") marker = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--submit-after") { submit = true; submitAfter = std::atof(next().c_str()); }
        else if (a == "--until") g_until = next();
        else if (a == "--tor-port") socks5::setPort(std::atoi(next().c_str()));
        else if (a == "--tor-all") socks5::setRouteAll(true);
        else if (a == "--tor-data") torclient::setDataDir(next());  // start <dir>/tor/tor if no Tor answers
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (issuer.empty() || code.empty() || token.empty()) {
        std::fprintf(stderr, "need --issuer, --code and --token\n");
        return 2;
    }

    NetSession session;
    // No address means the relay, which NetSession is asked for with an empty
    // list -- a list holding one empty string is an address it cannot use.
    const std::vector<std::string> addresses =
        address.empty() ? std::vector<std::string>{} : std::vector<std::string>{address};
    if (!session.join(addresses, issuer, code, token, "test", "")) {
        say("REJECTED " + session.error());
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    uint16_t me = 0;
    bool claimed = false;
    uint32_t submittedFor = 0, ackedFor = 0;
    uint32_t pendingTurn = 0;
    double pendingAt = 0.0;
    std::string lastRoster;
    int rc = 0;

    while (!g_stop) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > seconds) { say("TIMEOUT"); rc = 3; break; }

        session.update();
        NetSessionEvent e;
        while (!g_stop && session.nextEvent(e)) {
            switch (e.kind) {
                case NetSessionEvent::Kind::Welcomed:
                    me = session.welcome().peerId;
                    say("WELCOMED peer=" + std::to_string(me) +
                        " spectator=" + (session.spectating() ? "1" : "0"));
                    break;
                case NetSessionEvent::Kind::CountriesKnown: {
                    const auto list = session.countries();
                    say("COUNTRIES n=" + std::to_string(list.size()));
                    if (!claimed && (claim || claimFirst || claimIndex >= 0) && !list.empty()) {
                        uint16_t id = list.front().id;
                        if (claim) id = (uint16_t)claim;
                        else if (claimIndex >= 0 && claimIndex < (int)list.size())
                            id = list[(size_t)claimIndex].id;
                        session.claimCountry(id);
                        claimed = true;
                    }
                    break;
                }
                case NetSessionEvent::Kind::LobbyMapKnown: {
                    const std::vector<uint8_t> bytes = session.lobbyMap();
                    LobbyMap m;
                    if (LobbyMap::decode(bytes.data(), bytes.size(), m))
                        say("LOBBYMAP w=" + std::to_string(m.width) + " h=" +
                            std::to_string(m.height) + " colors=" +
                            std::to_string(m.colors.size()) + " bytes=" +
                            std::to_string(bytes.size()));
                    else
                        say("LOBBYMAP undecodable");
                    break;
                }
                case NetSessionEvent::Kind::SessionInfoKnown:
                    say("VOICE " + session.sessionInfo().voiceLink);
                    break;
                case NetSessionEvent::Kind::TurnBegan:
                    say("TURN " + std::to_string(e.turnNumber) +
                        " deadline_ms=" + std::to_string(e.deadlineMs));
                    if (submit && submittedFor != e.turnNumber) {
                        pendingTurn = e.turnNumber;
                        pendingAt = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - start).count() + submitAfter;
                    }
                    break;
                case NetSessionEvent::Kind::Snapshot:
                    say("SNAPSHOT turn=" + std::to_string(e.turnNumber) +
                        " bytes=" + std::to_string(e.payload.size()));
                    break;
                case NetSessionEvent::Kind::Delta:
                    say("DELTA turn=" + std::to_string(e.turnNumber));
                    break;
                case NetSessionEvent::Kind::Rejected:
                    say("REJECTED " + session.error());
                    rc = 1;
                    g_stop = true;
                    break;
                case NetSessionEvent::Kind::Disconnected:
                    say("DISCONNECTED " + session.error());
                    rc = 1;
                    g_stop = true;
                    break;
                default:
                    break;
            }
        }

        // A submission that was waiting for its moment.
        if (pendingTurn != 0 && elapsed >= pendingAt) {
            const std::string body = "{\"marker\":\"" + marker + "\"}";
            session.submitOrders(pendingTurn, std::vector<uint8_t>(body.begin(), body.end()));
            submittedFor = pendingTurn;
            say("SUBMITTED " + std::to_string(pendingTurn));
            pendingTurn = 0;
        }

        // The roster line, only when what it says changes.
        for (const NetPeer& p : session.roster()) {
            if (p.peerId != me || me == 0) continue;
            const std::string line = "ROSTER me=" + std::to_string(p.countryId) +
                                     " submitted=" + (p.submitted ? "1" : "0") +
                                     " state=" + stateName(session.state()) +
                                     " peers=" + std::to_string(session.roster().size());
            if (line != lastRoster) { lastRoster = line; say(line); }
            // The host has the orders: until this, they may still be in our
            // send queue, and leaving now would drop them on the floor.
            if (submittedFor != 0 && p.submitted && ackedFor != submittedFor) {
                ackedFor = submittedFor;
                say("ACKED " + std::to_string(submittedFor));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    session.leave();
    if (rc == 0) say("DONE");
    torclient::shutdown();
    return rc;
}
