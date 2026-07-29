// Seats remembered across sessions, and the reconnect path they feed.
//
// The point of a HostBook is that a player returning to a campaign days later
// takes the SAME code path as one whose connection dropped -- so most of what
// is tested here is that a reserved seat really does turn into a reconnect.

#include "net/Lobby.h"
#include "net/HostBook.h"

#include <algorithm>
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

HostBook roundTrip(const HostBook& in) {
    HostBook out;
    HostBook::decode(in.encode(), out);
    return out;
}

}  // namespace

int main() {
    printf("=== the book itself ===\n");
    {
        HostBook b;
        b.mapId = "map";
        b.turnNumber = 14;
        b.seats.push_back({"p_alice", "Alice", 24});
        b.seats.push_back({"p_bob", "Bob", 3});

        const HostBook r = roundTrip(b);
        check("the map survives", r.mapId == "map", r.mapId);
        check("the turn number survives", r.turnNumber == 14,
              std::to_string(r.turnNumber));
        check("both seats survive", r.seats.size() == 2,
              std::to_string(r.seats.size()));
        // The pairing is what matters: a psid glued to the wrong country would
        // hand somebody else's empire to a returning player.
        check("the first seat keeps its own country",
              r.seats.size() == 2 && r.seats[0].psid == "p_alice" &&
              r.seats[0].countryId == 24);
        check("the second seat keeps its own country",
              r.seats.size() == 2 && r.seats[1].psid == "p_bob" &&
              r.seats[1].countryId == 3);
        check("names survive", r.seats.size() == 2 && r.seats[1].name == "Bob");
    }

    printf("\n=== names are data, not syntax ===\n");
    {
        HostBook b;
        b.mapId = "map";
        // A name arrives from another machine. If it could end the string it
        // could rewrite the file into something that reads back differently.
        b.seats.push_back({"p_x", "Bob\", \"countryId\": 999, \"x\": \"", 7});
        const HostBook r = roundTrip(b);
        check("a quote in a name does not move the country",
              r.seats.size() == 1 && r.seats[0].countryId == 7,
              r.seats.empty() ? "no seats" : std::to_string(r.seats[0].countryId));
        check("a newline in a name is escaped",
              HostBook{"m", 0, {{"p", "a\nb", 1}}}.encode().find("a\nb") ==
                  std::string::npos);
    }

    printf("\n=== a damaged or absent book is not a crash ===\n");
    {
        HostBook r;
        check("empty text decodes to nothing", !HostBook::decode("", r));
        check("junk yields an empty book",
              HostBook::decode("{{{not json", r) && r.seats.empty());
        check("a truncated record is dropped",
              HostBook::decode("{\"seats\": [{\"psid\": \"p_a\"", r) &&
              r.seats.empty());
        check("a seat with no country is not held",
              HostBook::decode("{\"seats\": [{\"psid\":\"p_a\",\"countryId\":0}]}", r) &&
              r.seats.empty());
        check("a missing file is a clean miss",
              !HostBook::load("/nonexistent/nowhere.odsv", r));
    }

    printf("\n=== a reserved seat becomes a reconnect ===\n");
    {
        Lobby lobby;
        LobbySettings s;
        s.maxPlayers = 8;
        lobby.configure(s);

        check("a seat can be held for an absent player",
              lobby.reserveSeat("p_alice", "Alice", 24));
        check("the held country reads as taken",
              lobby.holderOf(24) != nullptr);
        check("the holder is not shown as connected",
              lobby.findByPsid("p_alice") &&
              !lobby.findByPsid("p_alice")->connected);

        // The whole point: Alice comes back days later and does not re-pick.
        const LobbyDenial d =
            lobby.admit(7, "p_alice", "Alice", "", "https://issuer.example", true);
        check("she is admitted", d == LobbyDenial::None);
        check("she gets her own country back",
              lobby.findByPsid("p_alice") &&
              lobby.findByPsid("p_alice")->countryId == 24);
        check("she is connected under the new peer id",
              lobby.findByPsid("p_alice") &&
              lobby.findByPsid("p_alice")->peerId == 7 &&
              lobby.findByPsid("p_alice")->connected);

        // And somebody else must not be able to take a held seat meanwhile.
        check("a stranger cannot claim a held country",
              lobby.admit(8, "p_mallory", "M", "", "https://issuer.example", true) ==
                  LobbyDenial::None &&
              lobby.claimCountry(8, 24) != LobbyDenial::None);
    }

    printf("\n=== reservations do not collide ===\n");
    {
        Lobby lobby;
        LobbySettings s;
        s.maxPlayers = 8;
        lobby.configure(s);
        check("the first reservation is taken", lobby.reserveSeat("p_a", "A", 5));
        check("the same country cannot be held twice",
              !lobby.reserveSeat("p_b", "B", 5));
        check("the same person cannot hold two countries",
              !lobby.reserveSeat("p_a", "A", 6));
        check("an empty psid holds nothing", !lobby.reserveSeat("", "A", 7));
        check("country 0 holds nothing", !lobby.reserveSeat("p_c", "C", 0));
    }

    printf("\n=== a held seat can be given up ===\n");
    {
        Lobby lobby;
        LobbySettings s;
        s.maxPlayers = 2;               // deliberately tight
        lobby.configure(s);

        lobby.reserveSeat("p_gone", "Gone", 24);
        lobby.reserveSeat("p_here", "Here", 3);

        // The reason this exists: two held seats fill a two-seat lobby, and
        // somebody new cannot get in until one is given up.
        check("a full lobby of held seats refuses a newcomer",
              lobby.admit(9, "p_new", "New", "", "https://issuer.example", true) !=
                  LobbyDenial::None);

        check("the seat is released", lobby.releaseSeat("p_gone"));
        check("the holder is gone", lobby.findByPsid("p_gone") == nullptr);
        check("their country is free again", lobby.holderOf(24) == nullptr);
        check("releasing it twice changes nothing", !lobby.releaseSeat("p_gone"));
        check("the other seat is untouched",
              lobby.findByPsid("p_here") &&
              lobby.findByPsid("p_here")->countryId == 3);

        check("now somebody new fits",
              lobby.admit(9, "p_new", "New", "", "https://issuer.example", true) ==
                  LobbyDenial::None);
        check("and can take the freed country",
              lobby.claimCountry(9, 24) == LobbyDenial::None);
    }

    printf("\n=== releasing is not a kick ===\n");
    {
        Lobby lobby;
        LobbySettings s;
        s.maxPlayers = 8;
        lobby.configure(s);
        lobby.admit(4, "p_live", "Live", "", "https://issuer.example", true);
        lobby.claimCountry(4, 11);

        // Somebody sitting right there must not vanish through the quiet door.
        check("a connected player is not released",
              !lobby.releaseSeat("p_live"));
        check("they keep their country",
              lobby.findByPsid("p_live") &&
              lobby.findByPsid("p_live")->countryId == 11);

        // Once they drop, the seat is held -- and may then be given up.
        lobby.disconnect(4);
        check("after they drop it can be released", lobby.releaseSeat("p_live"));
    }

    printf("\n=== starting without the stragglers ===\n");
    {
        Lobby lobby;
        LobbySettings st;
        st.maxPlayers = 8;
        lobby.configure(st);
        lobby.admit(1, "p_ready", "Ready", "", "https://issuer.example", true);
        lobby.claimCountry(1, 5);
        lobby.admit(2, "p_slow", "Slow", "", "https://issuer.example", true);

        std::string why;
        check("it refuses while somebody is choosing", !lobby.start(why));
        check("and says who", why.find("Slow") != std::string::npos, why);
        check("who is listed", lobby.stillChoosing().size() == 1 &&
                               lobby.stillChoosing()[0] == "Slow");

        check("forcing it starts", lobby.start(why, true), why);
        // Left watching, not left as a player with no land: a countryless
        // player would be waited on every turn with nothing to submit.
        check("the straggler became a spectator",
              lobby.findByPsid("p_slow") && lobby.findByPsid("p_slow")->spectator);
        check("the player who chose is untouched",
              lobby.findByPsid("p_ready") &&
              !lobby.findByPsid("p_ready")->spectator &&
              lobby.findByPsid("p_ready")->countryId == 5);
        check("a spectator is not waited on",
              lobby.missingSubmissions(1).size() == 1);
    }

    printf("\n=== taking ready back ===\n");
    {
        Lobby lobby;
        LobbySettings st;
        st.maxPlayers = 4;
        lobby.configure(st);
        lobby.admit(1, "p_a", "A", "", "https://issuer.example", true);
        lobby.claimCountry(1, 5);
        std::string why;
        lobby.start(why);

        check("submitting clears the wait",
              lobby.submitOrders(1, 7, {1, 2, 3}) == LobbyDenial::None &&
              lobby.missingSubmissions(7).empty());
        check("it can be taken back",
              lobby.withdrawOrders(1, 7) == LobbyDenial::None);
        check("and the turn waits again", lobby.missingSubmissions(7).size() == 1);
        check("the orders went with it",
              lobby.find(1) && lobby.find(1)->orders.empty());
        // A withdrawal for a turn that already resolved must not un-ready
        // somebody for the turn they are now in.
        lobby.submitOrders(1, 8, {9});
        check("a stale withdrawal is refused",
              lobby.withdrawOrders(1, 7) != LobbyDenial::None);
        check("so the current turn still counts them in",
              lobby.missingSubmissions(8).empty());
    }

    printf("\n=== kicking is not banning ===\n");
    {
        Lobby lobby;
        LobbySettings st;
        st.maxPlayers = 8;
        lobby.configure(st);
        lobby.admit(1, "p_rude", "Rude", "", "https://issuer.example", true);
        lobby.claimCountry(1, 9);

        // A kick is "leave this game", not "never again" -- otherwise every
        // kick is permanent and a host stops using it.
        lobby.evict(1);
        check("a kick removes them", lobby.findByPsid("p_rude") == nullptr);
        check("their country is free", lobby.holderOf(9) == nullptr);
        check("but they can come back",
              lobby.admit(2, "p_rude", "Rude", "", "https://issuer.example", true) ==
                  LobbyDenial::None);

        // A ban is the other thing.
        lobby.ban("p_rude");
        check("a ban removes them too", lobby.findByPsid("p_rude") == nullptr);
        check("and refuses them at the door",
              lobby.admit(3, "p_rude", "Rude", "", "https://issuer.example", true) ==
                  LobbyDenial::Banned);
        check("watching is not a way around it",
              lobby.admit(4, "p_rude", "Rude", "", "https://issuer.example", true) ==
                  LobbyDenial::Banned);
        check("somebody else is unaffected",
              lobby.admit(5, "p_fine", "Fine", "", "https://issuer.example", true) ==
                  LobbyDenial::None);
        lobby.unban("p_rude");
        check("lifting it lets them back",
              lobby.admit(6, "p_rude", "Rude", "", "https://issuer.example", true) ==
                  LobbyDenial::None);
    }

    printf("\n=== a spectator can take a vacated seat ===\n");
    {
        Lobby lobby;
        LobbySettings st;
        st.maxPlayers = 8;
        lobby.configure(st);
        lobby.admit(1, "p_left", "Left", "", "https://issuer.example", true);
        lobby.claimCountry(1, 12);
        lobby.admit(2, "p_watch", "Watcher", "", "https://issuer.example", true);
        std::string why;
        lobby.start(why, true);            // Watcher had no country: spectator

        check("the watcher is spectating",
              lobby.findByPsid("p_watch") && lobby.findByPsid("p_watch")->spectator);
        check("one spectator is counted", lobby.spectators().size() == 1);

        // Somebody leaves; the seat should not die with them.
        lobby.evict(1);
        check("the country is free", lobby.holderOf(12) == nullptr);
        check("the spectator takes it",
              lobby.seatSpectator(2, 12) == LobbyDenial::None);
        check("they are a player now",
              lobby.findByPsid("p_watch") && !lobby.findByPsid("p_watch")->spectator &&
              lobby.findByPsid("p_watch")->countryId == 12);
        check("and owe orders like anyone else",
              lobby.missingSubmissions(1).size() == 1);
        check("no spectators left", lobby.spectators().empty());
        check("a held country cannot be handed out twice",
              lobby.seatSpectator(2, 12) != LobbyDenial::None);
    }

    printf("\n=== the host's file carries bans and settings ===\n");
    {
        HostBook b;
        b.mapId = "map";
        b.turnNumber = 3;
        b.seats.push_back({"p_keep", "Keep", 8});
        b.bans.push_back("p_barred");
        b.bans.push_back("p_also");
        b.settings.turnSeconds = 86400;      // a day per turn
        b.settings.maxPlayers = 12;
        b.settings.lateJoin = 1;
        b.settings.absent = 1;
        b.settings.bindAll = true;
        b.settings.listed = true;
        b.settings.port = 27016;

        const HostBook r = roundTrip(b);
        check("bans survive", r.bans.size() == 2, std::to_string(r.bans.size()));
        check("the right people are barred",
              r.bans.size() == 2 && r.bans[0] == "p_barred" && r.bans[1] == "p_also");
        // The failure that would matter: a seated player's psid read back as a
        // ban would bar the very person whose seat it is.
        check("a seat is not mistaken for a ban",
              std::find(r.bans.begin(), r.bans.end(), "p_keep") == r.bans.end());
        check("the seat is still a seat",
              r.seats.size() == 1 && r.seats[0].psid == "p_keep");

        check("turn length survives", r.settings.turnSeconds == 86400,
              std::to_string(r.settings.turnSeconds));
        check("seats survive", r.settings.maxPlayers == 12);
        check("late join survives", r.settings.lateJoin == 1);
        check("absent rule survives", r.settings.absent == 1);
        check("bind-all survives", r.settings.bindAll);
        check("listed survives", r.settings.listed);
        check("port survives", r.settings.port == 27016);
    }

    printf("\n=== a file with no bans or settings still loads ===\n");
    {
        HostBook r;
        check("an old-shaped book decodes",
              HostBook::decode("{\"mapId\":\"map\",\"seats\":[]}", r));
        check("with no bans", r.bans.empty());
        check("and workable defaults",
              r.settings.maxPlayers == 8 && r.settings.turnSeconds == 0);
        // Values outside what the game accepts must not come back through.
        check("a nonsense seat count is refused",
              HostBook::decode("{\"settings\":{\"maxPlayers\":999}}", r) &&
              r.settings.maxPlayers == 8,
              std::to_string(r.settings.maxPlayers));
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
