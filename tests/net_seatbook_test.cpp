// Seats remembered across sessions, and the reconnect path they feed.
//
// The point of a SeatBook is that a player returning to a campaign days later
// takes the SAME code path as one whose connection dropped -- so most of what
// is tested here is that a reserved seat really does turn into a reconnect.

#include "net/Lobby.h"
#include "net/SeatBook.h"

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

SeatBook roundTrip(const SeatBook& in) {
    SeatBook out;
    SeatBook::decode(in.encode(), out);
    return out;
}

}  // namespace

int main() {
    printf("=== the book itself ===\n");
    {
        SeatBook b;
        b.mapId = "map";
        b.turnNumber = 14;
        b.seats.push_back({"p_alice", "Alice", 24});
        b.seats.push_back({"p_bob", "Bob", 3});

        const SeatBook r = roundTrip(b);
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
        SeatBook b;
        b.mapId = "map";
        // A name arrives from another machine. If it could end the string it
        // could rewrite the file into something that reads back differently.
        b.seats.push_back({"p_x", "Bob\", \"countryId\": 999, \"x\": \"", 7});
        const SeatBook r = roundTrip(b);
        check("a quote in a name does not move the country",
              r.seats.size() == 1 && r.seats[0].countryId == 7,
              r.seats.empty() ? "no seats" : std::to_string(r.seats[0].countryId));
        check("a newline in a name is escaped",
              SeatBook{"m", 0, {{"p", "a\nb", 1}}}.encode().find("a\nb") ==
                  std::string::npos);
    }

    printf("\n=== a damaged or absent book is not a crash ===\n");
    {
        SeatBook r;
        check("empty text decodes to nothing", !SeatBook::decode("", r));
        check("junk yields an empty book",
              SeatBook::decode("{{{not json", r) && r.seats.empty());
        check("a truncated record is dropped",
              SeatBook::decode("{\"seats\": [{\"psid\": \"p_a\"", r) &&
              r.seats.empty());
        check("a seat with no country is not held",
              SeatBook::decode("{\"seats\": [{\"psid\":\"p_a\",\"countryId\":0}]}", r) &&
              r.seats.empty());
        check("a missing file is a clean miss",
              !SeatBook::load("/nonexistent/nowhere.odsv", r));
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

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
