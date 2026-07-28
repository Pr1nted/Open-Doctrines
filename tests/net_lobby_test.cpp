// The authoritative lobby.
//
// Almost every case here is about a rule a client must not be able to break by
// asking nicely: one country each, an atomic swap, a reconnect that is the same
// player rather than a new one, and a roster that closes when the game starts.
//
// Build target: NetLobbyTest. Run it; non-zero exit means a case failed.

#include "net/Lobby.h"
#include "net/TurnRunner.h"

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

const char* kOfficial = "https://official.example";

// The relay hands out peer ids, so the test does too. A fresh one per call
// mirrors the relay issuing a new handle for every connection.
uint16_t g_nextRelayId = 1;

uint16_t join(Lobby& l, const char* psid, const char* name, bool official = true,
              const char* issuer = kOfficial) {
    const uint16_t id = g_nextRelayId++;
    return l.admit(id, psid, name, "", issuer, official) == LobbyDenial::None ? id : 0;
}

/** A reconnect: same psid, NEW relay handle, as the relay actually behaves. */
uint16_t rejoin(Lobby& l, const char* psid, const char* name) {
    const uint16_t id = g_nextRelayId++;
    return l.admit(id, psid, name, "", kOfficial, true) == LobbyDenial::None ? id : 0;
}

Lobby makeLobby(uint32_t maxPlayers = 8,
                NetAssignment assign = NetAssignment::PlayersPick,
                NetLateJoin late = NetLateJoin::Spectate) {
    Lobby l;
    LobbySettings s;
    s.maxPlayers = maxPlayers;
    s.assignment = assign;
    s.lateJoin = late;
    l.configure(s);
    return l;
}

// ------------------------------------------------------------------ admit ----

void testAdmission() {
    printf("\n=== admission ===\n");

    Lobby l = makeLobby(2);
    uint16_t a = join(l, "psid-a", "Alice");
    uint16_t b = join(l, "psid-b", "Bob");
    check("two players fit", a != 0 && b != 0 && a != b);

    check("a third is refused when full",
          l.admit(g_nextRelayId++, "psid-c", "Carol", "", kOfficial, true) ==
          LobbyDenial::SessionFull);

    // The relay issues a NEW handle each time, so a reconnect changes peer id
    // while staying the same player. Conflating the two would attribute every
    // later message to the wrong person.
    const uint16_t again = rejoin(l, "psid-a", "Alice");
    check("a returning psid is the same member", again != 0 &&
          l.find(again) && l.find(again)->psid == "psid-a");
    check("under a new handle", again != a);
    check("and does not consume another slot", l.members().size() == 2);
    check("the old handle no longer resolves", l.find(a) == nullptr);

    Lobby strict = makeLobby();
    check("a non-official issuer is refused by default",
          strict.admit(1, "p", "X", "", "https://other.example", false) ==
          LobbyDenial::IssuerNotAccepted);

    Lobby permissive;
    LobbySettings s;
    s.acceptedIssuers = {"https://other.example"};
    permissive.configure(s);
    check("but accepted when the server lists it",
          permissive.admit(1, "p", "Y", "", "https://other.example", false) ==
          LobbyDenial::None);
    check("and an unlisted one is still refused",
          permissive.admit(2, "q", "Z", "", "https://third.example", false) ==
          LobbyDenial::IssuerNotAccepted);
    check("a peer id already in use is refused",
          permissive.admit(1, "r", "R", "", "https://other.example", false) ==
          LobbyDenial::NoSuchPeer);
    check("and peer id 0 is not a peer",
          permissive.admit(0, "s", "S", "", "https://other.example", false) ==
          LobbyDenial::NoSuchPeer);
}

void testLateJoin() {
    printf("\n=== joining after the lobby closes ===\n");

    {
        Lobby l = makeLobby(4, NetAssignment::PlayersPick, NetLateJoin::Spectate);
        uint16_t a = join(l, "a", "Alice");
        l.claimCountry(a, 40);
        std::string why;
        check("the game starts", l.start(why), why);

        const uint16_t late = g_nextRelayId++;
        check("a latecomer is admitted as a spectator",
              l.admit(late, "late", "Late", "", kOfficial, true) == LobbyDenial::None);
        check("and is marked as one", l.find(late) && l.find(late)->spectator);
        check("and holds no country",
              l.claimCountry(late, 41) == LobbyDenial::NotInLobby);
    }
    {
        Lobby l = makeLobby(4, NetAssignment::PlayersPick, NetLateJoin::Refuse);
        uint16_t a = join(l, "a", "Alice");
        l.claimCountry(a, 40);
        std::string why;
        l.start(why);

        check("or refused outright when the host said so",
              l.admit(g_nextRelayId++, "late", "Late", "", kOfficial, true) ==
              LobbyDenial::GameInProgress);
    }
}

// -------------------------------------------------------------- countries ----

void testCountryClaims() {
    printf("\n=== one country each ===\n");

    Lobby l = makeLobby();
    uint16_t a = join(l, "a", "Alice");
    uint16_t b = join(l, "b", "Bob");

    check("a free country can be claimed", l.claimCountry(a, 40) == LobbyDenial::None);
    check("and is then held", l.holderOf(40) && l.holderOf(40)->peerId == a);
    check("a taken country is refused", l.claimCountry(b, 40) == LobbyDenial::CountryTaken);
    check("and did not move", l.holderOf(40)->peerId == a);

    // A client re-sends its claim after reconnecting; refusing would look like
    // a failure for something that is already true.
    check("re-claiming your own is a no-op, not an error",
          l.claimCountry(a, 40) == LobbyDenial::None);

    check("country 0 is not a country", l.claimCountry(b, 0) == LobbyDenial::NoSuchCountry);
    check("an unknown peer cannot claim", l.claimCountry(999, 41) == LobbyDenial::NoSuchPeer);

    check("moving to a free country works", l.claimCountry(a, 41) == LobbyDenial::None);
    // Nothing says the old one is released, so check it explicitly: a player
    // silently holding two countries is exactly the bug this class exists for.
    check("and releases the old one", l.holderOf(40) == nullptr);
}

void testHostAssigns() {
    printf("\n=== host-assigned countries ===\n");

    Lobby l = makeLobby(8, NetAssignment::HostAssigns);
    uint16_t host = join(l, "host", "Hosty");
    uint16_t a = join(l, "a", "Alice");
    l.setHost(host);

    check("a player cannot claim when the host assigns",
          l.claimCountry(a, 40) == LobbyDenial::HostOnly);
    check("the host can assign to a player",
          l.assignCountry(host, a, 40) == LobbyDenial::None);
    check("and it lands", l.find(a)->countryId == 40);

    uint16_t b = join(l, "b", "Bob");
    check("the host cannot double-assign",
          l.assignCountry(host, b, 40) == LobbyDenial::CountryTaken);
    check("a non-host cannot assign",
          l.assignCountry(a, b, 41) == LobbyDenial::HostOnly);
    check("the host can take a country back",
          l.assignCountry(host, a, 0) == LobbyDenial::None && l.find(a)->countryId == 0);
}

// ------------------------------------------------------------------ swaps ----

void testSwaps() {
    printf("\n=== swaps are atomic ===\n");

    Lobby l = makeLobby();
    uint16_t a = join(l, "a", "Alice");
    uint16_t b = join(l, "b", "Bob");
    l.claimCountry(a, 40);
    l.claimCountry(b, 41);

    check("an offer is accepted for delivery", l.offerSwap(a, b) == LobbyDenial::None);
    check("declining changes nothing",
          l.replySwap(b, a, false) == LobbyDenial::None &&
          l.find(a)->countryId == 40 && l.find(b)->countryId == 41);

    check("a declined offer cannot be accepted afterwards",
          l.replySwap(b, a, true) == LobbyDenial::NoOffer);
    check("and still nothing moved", l.find(a)->countryId == 40);

    l.offerSwap(a, b);
    check("accepting exchanges both at once",
          l.replySwap(b, a, true) == LobbyDenial::None &&
          l.find(a)->countryId == 41 && l.find(b)->countryId == 40);

    check("the offer is spent", l.replySwap(b, a, true) == LobbyDenial::NoOffer);
    check("you cannot swap with yourself", l.offerSwap(a, a) == LobbyDenial::SelfSwap);

    uint16_t c = join(l, "c", "Carol");
    check("someone holding nothing cannot offer",
          l.offerSwap(c, a) == LobbyDenial::NotYours);

    // A dropped connection must not leave an offer that can still be taken.
    l.offerSwap(a, b);
    l.disconnect(a);
    check("an offer dies with the offerer's connection",
          l.replySwap(b, a, true) == LobbyDenial::NoOffer);

    // Re-offering replaces rather than stacks, so an accept cannot apply an
    // offer the player believed they had superseded.
    l.offerSwap(b, c);
    l.offerSwap(b, c);
    check("re-offering does not stack",
          l.replySwap(c, b, false) == LobbyDenial::None &&
          l.replySwap(c, b, false) == LobbyDenial::NoOffer);
}

// ----------------------------------------------------------------- orders ----

void testOrdersSurviveReconnect() {
    printf("\n=== orders survive a reconnect ===\n");

    Lobby l = makeLobby();
    uint16_t a = join(l, "psid-a", "Alice");
    uint16_t b = join(l, "psid-b", "Bob");
    l.claimCountry(a, 40);
    l.claimCountry(b, 41);
    std::string why;
    l.start(why);

    const std::vector<uint8_t> orders = {1, 2, 3, 4};
    check("orders are accepted", l.submitOrders(a, 7, orders) == LobbyDenial::None);
    check("and recorded", l.find(a)->submitted && l.find(a)->orders == orders);

    // The whole point: with a long turn interval, closing the game between
    // turns is ordinary, and redoing your orders would be intolerable.
    l.disconnect(a);
    const uint16_t again = rejoin(l, "psid-a", "Alice");
    check("reconnecting keeps the same member", again != 0 && again != a);
    check("the country is still held", l.find(again)->countryId == 40);
    check("and the orders are still there",
          l.find(again)->submitted && l.find(again)->orders == orders);

    check("who is missing counts Bob, not Alice",
          l.missingSubmissions(7).size() == 1 && l.missingSubmissions(7)[0] == b);

    // Submitting for one turn must not count for the next.
    check("orders for an older turn do not count for this one",
          l.missingSubmissions(8).size() == 2);

    l.clearSubmissions();
    check("clearing wipes every submission", !l.find(again)->submitted &&
          l.find(again)->orders.empty());

    // A disconnected player who DID submit has done their part.
    l.submitOrders(b, 8, orders);
    l.disconnect(b);
    check("a disconnected submitter is not counted as missing",
          l.missingSubmissions(8).size() == 1 && l.missingSubmissions(8)[0] == again);
}

void testSpectatorsHoldNothing() {
    printf("\n=== spectators ===\n");

    Lobby l = makeLobby(4, NetAssignment::PlayersPick, NetLateJoin::Spectate);
    uint16_t a = join(l, "a", "Alice");
    l.claimCountry(a, 40);
    std::string why;
    l.start(why);

    const uint16_t s = g_nextRelayId++;
    l.admit(s, "spec", "Spec", "", kOfficial, true);
    check("a spectator's orders are refused, not stored",
          l.submitOrders(s, 7, {1, 2}) == LobbyDenial::Spectator);
    check("and they are never counted as missing",
          l.missingSubmissions(7).size() == 1 && l.missingSubmissions(7)[0] == a);

    l.returnToLobby();
    check("returning to the lobby makes them a player again",
          l.find(s) && !l.find(s)->spectator);
    check("and clears every country", l.find(a)->countryId == 0);
}

void testStartRequiresEveryone() {
    printf("\n=== starting ===\n");

    Lobby l = makeLobby();
    uint16_t a = join(l, "a", "Alice");
    uint16_t b = join(l, "b", "Bob");
    l.claimCountry(a, 40);

    std::string why;
    check("cannot start while someone holds nothing", !l.start(why));
    check("and it names them", why.find("Bob") != std::string::npos, why);

    l.claimCountry(b, 41);
    check("starts once everyone has one", l.start(why), why);
    check("state is Game", l.state() == NetSessionState::Game);
    check("and countries can no longer be claimed",
          l.claimCountry(a, 42) == LobbyDenial::NotInLobby);
    check("nor swapped", l.offerSwap(a, b) == LobbyDenial::NotInLobby);
    check("starting twice is refused", !l.start(why));
}

void testDenialsAllSpeak() {
    printf("\n=== every denial has words ===\n");

    // A denial with no sentence surfaces to a player as silence.
    const LobbyDenial all[] = {
        LobbyDenial::NotInLobby, LobbyDenial::Spectator, LobbyDenial::CountryTaken,
        LobbyDenial::NoSuchCountry, LobbyDenial::NoSuchPeer, LobbyDenial::NotYours,
        LobbyDenial::HostOnly, LobbyDenial::SelfSwap, LobbyDenial::NoOffer,
        LobbyDenial::SessionFull, LobbyDenial::GameInProgress,
        LobbyDenial::IssuerNotAccepted,
    };
    bool allSpeak = true;
    for (auto d : all) if (std::string(lobbyDenialText(d)).empty()) allSpeak = false;
    check("every refusal can be explained to a player", allSpeak);
    check("and None says nothing", std::string(lobbyDenialText(LobbyDenial::None)).empty());
}


// -------------------------------------------------------------- turn loop ----

void testTurnClock() {
    printf("\n=== the turn clock ===\n");

    TurnRunner r;
    r.configure({60, NetAbsent::Ai});
    r.beginTurn(1, 1000);

    check("not due immediately", !r.due(1000));
    check("counts down", r.remainingMs(1000) == 60000 && r.remainingMs(31000) == 30000);
    check("not due a millisecond early", !r.due(60999));
    check("due on the deadline", r.due(61000));
    check("and after it", r.due(999999));
    check("remaining bottoms out at zero", r.remainingMs(999999) == 0);

    r.stop();
    check("a stopped clock is never due", !r.due(999999));

    // Long-form has no deadline at all: the host may be offline for days, so
    // nothing may fire on a timer.
    TurnRunner slow;
    slow.configure({0, NetAbsent::Ai});
    slow.beginTurn(1, 1000);
    check("long-form is never due", !slow.due(999999999LL));
    check("and shows no countdown", slow.remainingMs(1000) == 0);
}

void testResolution() {
    printf("\n=== whose orders count ===\n");

    Lobby l = makeLobby();
    const uint16_t a = join(l, "a", "Alice");
    const uint16_t b = join(l, "b", "Bob");
    const uint16_t c = join(l, "c", "Carol");
    l.claimCountry(a, 40);
    l.claimCountry(b, 41);
    l.claimCountry(c, 42);
    std::string why;
    l.start(why);

    const std::vector<uint8_t> orders = {9, 9, 9};
    l.submitOrders(a, 5, orders);
    l.markMalformed(b, 5);
    // Carol sends nothing at all.

    TurnRunner r;
    r.configure({60, NetAbsent::Ai});
    const auto res = r.resolve(l, 5);
    check("one resolution per player country", res.size() == 3);

    auto forPeer = [&](uint16_t p) -> const TurnResolution* {
        for (const auto& one : res) if (one.peerId == p) return &one;
        return nullptr;
    };

    check("a good submission is used",
          forPeer(a)->usePlayerOrders && forPeer(a)->orders == orders &&
          forPeer(a)->substitution == NetSubstitution::None);

    // The whole submission goes, not part of it.
    check("a malformed one is discarded entirely",
          !forPeer(b)->usePlayerOrders && forPeer(b)->orders.empty() &&
          forPeer(b)->substitution == NetSubstitution::Malformed);
    check("and the AI plays it", forPeer(b)->aiPlays);
    check("and it is announced", !forPeer(b)->announcement.empty());

    check("nothing submitted is a different reason",
          forPeer(c)->substitution == NetSubstitution::NotSubmitted);

    check("something was substituted", TurnRunner::anySubstituted(res));

    // Orders for turn 5 must not satisfy turn 6.
    const auto next = r.resolve(l, 6);
    bool noneUsed = true;
    for (const auto& one : next) if (one.usePlayerOrders) noneUsed = false;
    check("last turn's orders do not count for this one", noneUsed);
}

void testAbsentPolicy() {
    printf("\n=== absent players ===\n");

    Lobby l = makeLobby();
    const uint16_t a = join(l, "a", "Alice");
    const uint16_t b = join(l, "b", "Bob");
    l.claimCountry(a, 40);
    l.claimCountry(b, 41);
    std::string why;
    l.start(why);
    l.markMalformed(b, 5);

    auto forPeer = [](const std::vector<TurnResolution>& v, uint16_t p) {
        for (const auto& one : v) if (one.peerId == p) return one;
        return TurnResolution{};
    };

    TurnRunner idle;
    idle.configure({60, NetAbsent::Idle});
    const auto res = idle.resolve(l, 5);

    check("an absent player idles when the host said so",
          !forPeer(res, a).aiPlays &&
          forPeer(res, a).substitution == NetSubstitution::NotSubmitted);
    check("and the idling is still announced",
          !forPeer(res, a).announcement.empty());

    // "Idle" is for a player who chose not to act. Someone whose orders were
    // mangled chose nothing, and freezing their country would punish them for
    // a transport failure.
    check("but a mangled submission is still played by the AI",
          forPeer(res, b).aiPlays &&
          forPeer(res, b).substitution == NetSubstitution::Malformed);
}

void testSubmissionEdges() {
    printf("\n=== submission edges ===\n");

    Lobby l = makeLobby();
    const uint16_t a = join(l, "a", "Alice");
    l.claimCountry(a, 40);
    std::string why;
    l.start(why);

    const std::vector<uint8_t> good = {1, 2, 3};
    l.submitOrders(a, 5, good);
    l.markMalformed(a, 5);
    check("a later unreadable message does not destroy readable orders",
          l.find(a)->submitted && l.find(a)->orders == good && !l.find(a)->malformed);

    // The other order: garbage first, then a good submission, which wins.
    Lobby l2 = makeLobby();
    const uint16_t x = join(l2, "x", "X");
    l2.claimCountry(x, 40);
    l2.start(why);
    l2.markMalformed(x, 5);
    check("a bad submission is recorded as malformed", l2.find(x)->malformed);
    l2.submitOrders(x, 5, good);
    check("and a good one afterwards replaces it",
          l2.find(x)->submitted && !l2.find(x)->malformed);

    TurnRunner r;
    r.configure({60, NetAbsent::Ai});
    // A player with no country resolves to nothing rather than to a country 0.
    Lobby l3 = makeLobby();
    join(l3, "y", "Y");
    check("a player holding no country produces no resolution",
          r.resolve(l3, 1).empty());
}

}  // namespace

int main() {
    printf("lobby\n");
    testAdmission();
    testLateJoin();
    testCountryClaims();
    testHostAssigns();
    testSwaps();
    testOrdersSurviveReconnect();
    testSpectatorsHoldNothing();
    testStartRequiresEveryone();
    testDenialsAllSpeak();
    testTurnClock();
    testResolution();
    testAbsentPolicy();
    testSubmissionEdges();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
