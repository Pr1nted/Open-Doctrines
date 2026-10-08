// The pure rules a long tournament stands on, tested without a socket.
//
// Each section is something a month-long campaign depends on and that is
// wrong silently when it is wrong: a deadline a restart moves, orders a crash
// forgets, a seat that cannot be handed on, a saved server that cannot be
// rejoined in one click, a picker map that decodes into a different world.
// tests/campaign_restart_test.sh drives the same things through the real
// server; this is the fast half, with exact numbers.

#include "net/HostBook.h"
#include "net/Lobby.h"
#include "net/LobbyMap.h"
#include "net/NetProtocol.h"
#include "net/ServerBook.h"
#include "net/TurnClock.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const char* what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what, got.empty() ? "" : "  --  ", got.c_str());
}

constexpr int64_t kMin = 60LL * 1000;
constexpr int64_t kHour = 60 * kMin;
constexpr int64_t kDay = 24 * kHour;
// 2026-10-02 00:00:00 UTC, a fixed point so every number below is exact.
constexpr int64_t kMidnight = 1790899200000LL;

}  // namespace

int main() {
    printf("=== the turn clock ===\n");
    {
        using namespace turnclock;
        check("HH:MM parses", parseAnchor("18:00") == 18 * 60 && parseAnchor("6:05") == 365 &&
                              parseAnchor("0:00") == 0);
        check("nonsense does not", parseAnchor("24:00") < 0 && parseAnchor("18") < 0 &&
                                   parseAnchor("18:60") < 0 && parseAnchor("") < 0 &&
                                   parseAnchor("ab:cd") < 0 && parseAnchor("18:5") < 0);
        check("and formats back", formatAnchor(365) == "06:05");

        check("long-form has no deadline", nextDeadline(kMidnight, 0, -1) == 0);
        check("an interval is now + length",
              nextDeadline(kMidnight + 5 * kHour, 86400, -1) == kMidnight + 5 * kHour + kDay);

        // Daily at 18:00. Opened at 09:00: due today at 18:00 (9h away, over
        // half a day? no -- 9h < 12h, so tomorrow). Opened at 05:00: 13h away,
        // so today.
        const int64_t at18 = 18 * 60;
        check("anchored: far enough ahead today",
              nextDeadline(kMidnight + 5 * kHour, 86400, at18) == kMidnight + 18 * kHour,
              std::to_string(nextDeadline(kMidnight + 5 * kHour, 86400, at18) - kMidnight));
        check("anchored: too close today, so tomorrow",
              nextDeadline(kMidnight + 9 * kHour, 86400, at18) == kMidnight + kDay + 18 * kHour);
        check("anchored: just after the anchor, tomorrow",
              nextDeadline(kMidnight + 18 * kHour + kMin, 86400, at18) ==
                  kMidnight + kDay + 18 * kHour);
        // A 12-hour grid through 06:00: 06:00 and 18:00.
        check("anchored 12h grid",
              nextDeadline(kMidnight + 1 * kHour, 43200, 6 * 60) == kMidnight + 18 * kHour);
        // Every deadline lands on the grid, whatever minute the turn opens.
        bool onGrid = true;
        for (int64_t m = 0; m < 2 * 24 * 60; m += 7) {
            const int64_t d = nextDeadline(kMidnight + m * kMin, 86400, at18);
            if ((d - kMidnight - 18 * kHour) % kDay != 0) onGrid = false;
            if (d - (kMidnight + m * kMin) < 12 * kHour) onGrid = false;
            if (d - (kMidnight + m * kMin) > 36 * kHour) onGrid = false;
        }
        check("anchored deadlines are on the grid and 12-36h away", onGrid);

        check("resume: a future deadline keeps exactly its time",
              resumeRemainingMs(kMidnight + 5 * kHour, kMidnight + 2 * kHour, 120000) == 3 * kHour);
        check("resume: a passed one gets the grace, once",
              resumeRemainingMs(kMidnight, kMidnight + 3 * kDay, 120000) == 120000);
        check("resume: no stored deadline says so",
              resumeRemainingMs(0, kMidnight, 120000) == -1);
        check("the wire clamps rather than wraps",
              clampForWire(-5) == 0 && clampForWire(60LL * kDay) == 0xFFFFFFFFu &&
              clampForWire(kDay) == (uint32_t)kDay);
        check("described in words", describe(kDay + 3 * kHour) == "1d 3h" &&
                                    describe(45 * 1000) == "45s");
    }

    printf("\n=== the campaign book ===\n");
    {
        HostBook b;
        b.mapId = "1914";
        b.turnNumber = 12;
        b.seats.push_back({"p_alice", "Alice", 24});
        b.settings.turnSeconds = 86400;
        b.settings.voiceLink = "https://discord.gg/abc";
        b.campaign.inGame = true;
        b.campaign.openTurn = 13;
        b.campaign.turnDeadlineMs = kMidnight + 18 * kHour;
        b.campaign.sessionCode = "ABCD-EFGH";
        HostBook r;
        check("round-trips", HostBook::decode(b.encode(), r));
        check("the campaign comes back whole",
              r.campaign.inGame && r.campaign.openTurn == 13 &&
              r.campaign.turnDeadlineMs == kMidnight + 18 * kHour &&
              r.campaign.sessionCode == "ABCD-EFGH");
        check("and the voice link", r.settings.voiceLink == "https://discord.gg/abc");
        check("seats are not confused with anything newer",
              r.seats.size() == 1 && r.seats[0].countryId == 24);

        HostBook old;
        check("a book from before campaigns still reads",
              HostBook::decode("{\"mapId\":\"map\",\"seats\":[],\"settings\":{\"turnSeconds\":60}}", old));
        check("as a campaign in its lobby, which is how it would have resumed",
              !old.campaign.inGame && old.campaign.turnDeadlineMs == 0 &&
              old.campaign.sessionCode.empty());

        const std::string dir = (std::filesystem::temp_directory_path() / "od_tournament_test").string();
        std::filesystem::create_directories(dir);
        const std::string save = dir + "/World.odsv";
        check("saves", b.save(save));
        check("leaves no temporary file behind",
              !std::filesystem::exists(HostBook::pathFor(save) + ".tmp"));
        HostBook back;
        check("and loads", HostBook::load(save, back) && back.campaign.openTurn == 13);

        PendingOrdersBook o;
        o.turn = 13;
        o.entries.push_back({"p_alice", false, {'{', '}', 0x00, 0xFF}});
        o.entries.push_back({"p_bob", true, {}});
        o.entries.push_back({"p_carol", false, {}});   // ready, nothing to do
        o.entries.push_back({"bad psid", false, {1}}); // a space would split the line
        PendingOrdersBook p;
        check("orders round-trip", PendingOrdersBook::decode(o.encode(), p) && p.turn == 13);
        check("bytes survive exactly, NUL and 0xFF included",
              p.entries.size() == 3 && p.entries[0].orders == o.entries[0].orders);
        check("a malformed submission stays malformed",
              p.entries[1].psid == "p_bob" && p.entries[1].malformed);
        check("an empty submission stays a submission",
              p.entries[2].psid == "p_carol" && p.entries[2].orders.empty());
        check("garbage is refused, not half-read",
              !PendingOrdersBook::decode("odorders 2\nturn 1\n", p) &&
              !PendingOrdersBook::decode("odorders 1\nturn 0\n", p));
        check("odd hex is skipped",
              PendingOrdersBook::decode("odorders 1\nturn 4\norder a 0 abc\norder b 0 ab\n", p) &&
              p.entries.size() == 1 && p.entries[0].psid == "b");
        check("orders file saves and loads",
              o.save(save) && PendingOrdersBook::load(save, p) && p.entries.size() == 3);
        PendingOrdersBook::remove(save);
        check("and is removed when the turn resolves",
              !std::filesystem::exists(PendingOrdersBook::pathFor(save)));
        std::filesystem::remove_all(dir);
    }

    printf("\n=== seats over a long campaign ===\n");
    {
        Lobby l;
        LobbySettings s;
        s.maxPlayers = 4;
        l.configure(s);
        l.setHost(0xFFFF);
        check("held seats come back", l.reserveSeat("p_alice", "Alice", 10) &&
                                      l.reserveSeat("p_bob", "Bob", 11));
        std::string why;
        check("a dedicated server starts the game itself", l.start(why, true));
        check("orders read back after a restart go to the right seat",
              l.restoreSubmission("p_bob", 7, {1, 2, 3}, false));
        const LobbyMember* bob = l.findByPsid("p_bob");
        check("as submitted, for that turn",
              bob && bob->submitted && bob->submittedTurn == 7 && bob->orders.size() == 3);
        check("a malformed one comes back malformed, with nothing applied",
              l.restoreSubmission("p_alice", 7, {9}, true) &&
              l.findByPsid("p_alice")->malformed && l.findByPsid("p_alice")->orders.empty());
        check("nobody gets orders for a psid they do not hold",
              !l.restoreSubmission("p_stranger", 7, {1}, false));
        auto missing = l.missingSubmissions(7);
        check("the turn waits on Alice, not Bob", missing.size() == 1);

        // Alice returns; then a spectator arrives and is given her country.
        check("Alice reconnects to her seat", l.admit(5, "p_alice", "Alice", "", "", true) ==
                                              LobbyDenial::None &&
                                              l.find(5)->countryId == 10);
        check("a watcher arrives mid-game as a spectator",
              l.admit(6, "p_watch", "Watcher", "", "", true) == LobbyDenial::None &&
              l.find(6)->spectator);
        check("the host can take a player out of their country",
              l.unseat(5) == LobbyDenial::None && l.find(5)->spectator &&
              l.find(5)->countryId == 0 && !l.holderOf(10));
        check("and seat the watcher in it", l.seatSpectator(6, 10) == LobbyDenial::None &&
                                            l.holderOf(10) && l.holderOf(10)->peerId == 6);
        check("the host's own seat cannot be unseated", l.unseat(0xFFFF) != LobbyDenial::None);

        // Passers-by: forty spectators who come and go.
        for (int i = 0; i < 40; ++i) {
            const uint16_t peer = (uint16_t)(100 + i);
            l.admit(peer, "p_visitor_" + std::to_string(i), "v", "", "", true);
            l.disconnect(peer);
            l.pruneSpectators(32);
        }
        size_t gone = 0;
        for (const auto& m : l.members()) if (m.spectator && !m.connected) gone++;
        check("only the last 32 who left are remembered", gone == 32, std::to_string(gone));
        check("and every player is still there",
              l.findByPsid("p_bob") && l.findByPsid("p_watch") && l.findByPsid("p_alice"));
        check("the newest visitor is kept, the oldest forgotten",
              l.findByPsid("p_visitor_39") && !l.findByPsid("p_visitor_0"));
    }

    printf("\n=== servers you are in ===\n");
    {
        ServerBook book;
        const std::string dir = (std::filesystem::temp_directory_path() / "od_tournament_book").string();
        std::filesystem::create_directories(dir);
        book.load(dir + "/servers.json");
        ServerEntry e;
        e.name = "Long war";
        e.issuer = "https://accounts.example";
        e.code = "ABCD-EFGH";
        book.addOrUpdate(e);
        const int i = book.find(e.issuer, e.name);
        check("found by issuer and name", i == 0);
        check("a relayed server is one click", book.entries()[0].oneClick());
        book.setAddress(0, "wss://game.example");
        check("a direct one is not, until the player agreed", !book.entries()[0].oneClick());
        book.setIpConsent(0, true);
        check("after agreeing, it is", book.entries()[0].oneClick());
        check("and that survives a save", book.save());
        ServerBook again;
        again.load(dir + "/servers.json");
        check("...and a load", again.entries().size() == 1 && again.entries()[0].ipConsent);
        again.setAddress(0, "wss://somewhere.else");
        check("a different address forgets the consent", !again.entries()[0].ipConsent);
        again.setAddress(0, "");
        check("the relay needs none", again.entries()[0].oneClick() &&
                                      again.entries()[0].address.empty());
        ServerEntry noCode = e;
        noCode.name = "No code";
        noCode.code.clear();
        again.addOrUpdate(noCode);
        check("an entry without a code asks for one",
              !again.entries()[(size_t)again.find(e.issuer, "No code")].oneClick());
        std::filesystem::remove_all(dir);
    }

    printf("\n=== the lobby map ===\n");
    {
        // A 16x8 world: sea, then country 7 on the left, 9 on the right, and a
        // strip of 7 in the far south (its "colony").
        LobbyMap m = LobbyMap::build(16, 8,
            [](float u, float v) -> uint16_t {
                if (v > 0.85f && u < 0.25f) return 7;
                if (v < 0.2f || v > 0.8f) return 0;
                return u < 0.5f ? 7 : 9;
            },
            [](uint16_t id) -> uint32_t { return id == 7 ? 0xFF0000u : 0x0000FFu; });
        check("built at the size asked", m.width == 16 && m.height == 8 && m.cells.size() == 128);
        check("with a swatch per country that appears", m.colors.size() == 2 &&
                                                        m.colorOf(7) == 0xFF0000u);
        const std::vector<uint8_t> bytes = m.encode();
        LobbyMap d;
        check("decodes", LobbyMap::decode(bytes.data(), bytes.size(), d));
        check("into the same world, cell for cell", d.cells == m.cells && d.width == 16);
        check("a click names the country under it",
              d.countryAt(0.1f, 0.5f) == 7 && d.countryAt(0.9f, 0.5f) == 9 &&
              d.countryAt(0.5f, 0.05f) == 0);
        check("off the map is nobody", d.countryAt(-0.1f, 0.5f) == 0 &&
                                       d.countryAt(1.0f, 0.5f) == 0);
        float u = 0, v = 0;
        check("a label goes on the homeland, not the colony",
              d.labelPoint(7, u, v) && v > 0.3f && v < 0.7f && u < 0.5f);
        check("no label for a country not on it", !d.labelPoint(42, u, v));

        std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 2);
        check("a truncated map is refused whole", !LobbyMap::decode(cut.data(), cut.size(), d) &&
                                                   d.empty());
        std::vector<uint8_t> extra = bytes;
        extra.push_back(0);
        check("and so is one with bytes left over",
              !LobbyMap::decode(extra.data(), extra.size(), d));
        // A hostile header claiming more cells than any map may have.
        NetWriter w;
        w.u8(1); w.u16(60000); w.u16(60000); w.u16(0); w.u32(1); w.u16(1); w.u16(1);
        const std::vector<uint8_t> big = w.take();
        check("an oversized map is refused before anything is allocated",
              !LobbyMap::decode(big.data(), big.size(), d));
        check("a world map stays small on the wire", bytes.size() < 200);
    }

    printf("\n=== searching for a country ===\n");
    {
        const std::vector<std::string> names = {
            "France", "North Korea", "South Korea", "Korea Strait Republic",
            "Japan", "Andorra", "Kingdom of Afghanistan", "Grand Duchy of Finland"};
        auto first = [&](const std::string& q) {
            const auto r = lobbyCountryFilter(names, q);
            return r.empty() ? std::string("(none)") : names[r[0]];
        };
        check("an empty search shows everything", lobbyCountryFilter(names, "").size() == names.size());
        check("case does not matter", first("FRA") == "France");
        check("a name that starts with it comes first", first("kor") == "Korea Strait Republic");
        check("then a word that starts with it",
              lobbyCountryFilter(names, "kor").size() == 3);
        check("\"fin\" finds the Grand Duchy", first("fin") == "Grand Duchy of Finland");
        check("leading spaces are ignored", first("  jap") == "Japan");
        check("no match is no match", lobbyCountryFilter(names, "zzz").empty());
        check("a substring still matches, last",
              lobbyCountryFilter(names, "ghan").size() == 1);
    }

    printf("\n=== the voice link ===\n");
    {
        check("an invite is accepted", netVoiceLinkValid("https://discord.gg/abcDEF"));
        check("http is not", !netVoiceLinkValid("http://discord.gg/abc"));
        check("nor a script", !netVoiceLinkValid("javascript:alert(1)"));
        check("nor a file", !netVoiceLinkValid("file:///etc/passwd"));
        check("nor anything with a space or newline",
              !netVoiceLinkValid("https://a.b/c d") && !netVoiceLinkValid("https://a.b/c\nd"));
        check("nor userinfo pretending to be the host",
              !netVoiceLinkValid("https://discord.gg@evil.example/x"));
        check("nor a host with no dot", !netVoiceLinkValid("https://localhost/x"));
        check("the player is shown the real host",
              netVoiceLinkHost("https://teamspeak.example.org:9987/join?x=1") == "teamspeak.example.org");
        NetSessionInfo info;
        info.voiceLink = "https://discord.gg/abc";
        std::vector<uint8_t> wire = info.encode();
        wire.push_back(7);  // a newer host's next field
        NetSessionInfo back;
        check("session info decodes, ignoring what a newer host added",
              NetSessionInfo::decode(wire.data(), wire.size(), back) &&
              back.voiceLink == "https://discord.gg/abc");
        NetSessionInfo bad;
        bad.voiceLink = "javascript:alert(1)";
        const std::vector<uint8_t> bw = bad.encode();
        check("and drops a link the host should never have sent",
              NetSessionInfo::decode(bw.data(), bw.size(), back) && back.voiceLink.empty());
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
