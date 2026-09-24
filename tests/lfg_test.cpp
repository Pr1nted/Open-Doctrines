// What the game will accept onto the looking-for-a-game board, and what it
// refuses.
//
// A listing is written by a stranger, fetched over the network and drawn on
// your screen with a button that joins a game, so the refusals ARE the feature.
// The same four guidelines the Discord channel pins are checked here; the
// service checks them too (net/test/lfg.test.ts) and its answer is the one that
// decides, but a player must be told which rule they broke while the form is
// still in front of them.
//
// Build target: LfgTest. Non-zero exit means a case failed.

#include "net/Lfg.h"

#include <cstdio>
#include <string>

namespace {

int g_checks = 0, g_fails = 0;

void ok(bool c, const std::string& what) {
    ++g_checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++g_fails;
}
void section(const char* t) { printf("\n== %s ==\n", t); }

std::string board(const std::string& inner) {
    return "{\"listings\":[" + inner + "]}";
}

const char* kHosting =
    "{\"id\":\"abc123\",\"kind\":\"hosting\",\"nick\":\"Vlad\",\"code\":\"ABCD-EFGH\","
    "\"map\":\"1914\",\"mode\":\"rapid\",\"turnSeconds\":120,\"slotsTaken\":2,"
    "\"slotsTotal\":6,\"language\":\"English\",\"region\":\"EU\","
    "\"note\":\"New players welcome\",\"createdAt\":1000,\"expiresAt\":9000}";

odlfg::Draft hostingDraft() {
    odlfg::Draft d;
    d.kind = odlfg::Kind::Hosting;
    d.code = "ABCD-EFGH";
    d.map = "1914";
    d.mode = odlfg::Mode::Rapid;
    d.turnSeconds = 120;
    d.slotsTaken = 2;
    d.slotsTotal = 6;
    return d;
}

}  // namespace

int main() {
    std::string why;

    section("a listing that is all there");
    {
        const std::vector<odlfg::Listing> got = odlfg::parseBoard(board(kHosting), why);
        ok(got.size() == 1, "one listing read");
        if (got.size() == 1) {
            const odlfg::Listing& l = got[0];
            ok(l.id == "abc123" && l.nick == "Vlad", "who posted it");
            ok(l.code == "ABCD-EFGH" && l.joinable(), "joinable, by its code");
            ok(l.paceLine() == "Rapid, 2 min a turn", "the pace, in words");
            ok(l.seatsLine() == "2/6 players", "the seats, in words");
            ok(l.closesIn(8400) == "closes in 10 min", "how long it has left");
            ok(l.closesIn(9100) == "closing", "and after that");
        }
    }

    section("nothing to draw a board from");
    {
        odlfg::parseBoard("", why);
        ok(!why.empty(), "an empty reply is not a board");
        odlfg::parseBoard("{\"ok\":true}", why);
        ok(!why.empty(), "nor is a reply with no listings in it");
        ok(odlfg::parseBoard(board(""), why).empty() && why.empty(),
           "an empty board is not an error");
        ok(odlfg::parseBoard(std::string(70 * 1024, 'x'), why).empty(),
           "an oversized document is refused whole");
    }

    section("a listing the game cannot honour is dropped, not half-drawn");
    {
        // A hosting listing with no code would draw a Join button that does
        // nothing at all.
        const std::string noCode =
            "{\"id\":\"x\",\"kind\":\"hosting\",\"nick\":\"n\",\"map\":\"m\","
            "\"mode\":\"rapid\",\"turnSeconds\":60}";
        ok(odlfg::parseBoard(board(noCode), why).empty(), "hosting with no code");

        // A code the join path would refuse is refused HERE, before it reaches
        // the join path at all.
        const std::string badCode =
            "{\"id\":\"x\",\"kind\":\"hosting\",\"nick\":\"n\",\"map\":\"m\",\"mode\":\"rapid\","
            "\"turnSeconds\":60,\"code\":\"../../etc/passwd\"}";
        ok(odlfg::parseBoard(board(badCode), why).empty(), "a code that is not a code");

        const std::string noMap =
            "{\"id\":\"x\",\"kind\":\"looking\",\"nick\":\"n\",\"mode\":\"rapid\",\"turnSeconds\":60}";
        ok(odlfg::parseBoard(board(noMap), why).empty(), "no map named");

        const std::string noPace =
            "{\"id\":\"x\",\"kind\":\"looking\",\"nick\":\"n\",\"map\":\"m\",\"mode\":\"rapid\"}";
        ok(odlfg::parseBoard(board(noPace), why).empty(), "a pace nobody could read");
    }

    section("one bad listing does not take the board down with it");
    {
        const std::string mixed =
            std::string(kHosting) + ",{\"id\":\"\",\"map\":\"\"}," + kHosting;
        const std::vector<odlfg::Listing> got = odlfg::parseBoard(board(mixed), why);
        ok(got.size() == 2, "the two good ones survive");
    }

    section("a field one listing omits is never read from the next");
    {
        // The reason readListing is handed the object and not an offset: a
        // "looking" entry followed by a hosting one must not inherit its code.
        const std::string looking =
            "{\"id\":\"l1\",\"kind\":\"looking\",\"nick\":\"a\",\"map\":\"any\","
            "\"mode\":\"longform\",\"turnHours\":24}";
        const std::vector<odlfg::Listing> got =
            odlfg::parseBoard(board(looking + "," + kHosting), why);
        ok(got.size() == 2, "both read");
        if (got.size() == 2) {
            ok(got[0].code.empty(), "the looking listing carries no code");
            ok(!got[0].joinable(), "and so cannot be joined");
            ok(got[1].code == "ABCD-EFGH", "the hosting one keeps its own");
        }
    }

    section("a note is bounded and carries no links");
    {
        const std::string linky =
            "{\"id\":\"x\",\"kind\":\"looking\",\"nick\":\"n\",\"map\":\"m\",\"mode\":\"rapid\","
            "\"turnSeconds\":60,\"note\":\"come to discord.gg/elsewhere\"}";
        const std::vector<odlfg::Listing> got = odlfg::parseBoard(board(linky), why);
        ok(got.size() == 1 && got[0].note.empty(),
           "the listing stays, the link does not");
    }

    section("the board only shows what is still open");
    {
        const std::vector<odlfg::Listing> all = odlfg::parseBoard(board(kHosting), why);
        ok(odlfg::live(all, 8000).size() == 1, "still open at 8000");
        ok(odlfg::live(all, 9001).empty(), "expired at 9001");
    }

    section("post the parameters of the game");
    {
        ok(odlfg::problemWith(hostingDraft()).empty(), "a complete listing is fine");

        odlfg::Draft d = hostingDraft();
        d.map.clear();
        ok(!odlfg::problemWith(d).empty(), "no map");

        d = hostingDraft();
        d.turnSeconds = 5;
        ok(!odlfg::problemWith(d).empty(), "a turn nobody could play");

        d = hostingDraft();
        d.mode = odlfg::Mode::Longform;
        d.turnHours = 1000;
        ok(!odlfg::problemWith(d).empty(), "a turn longer than a week");

        d = hostingDraft();
        d.slotsTaken = 9;
        ok(!odlfg::problemWith(d).empty(), "more players than seats");
    }

    section("use the correct tag");
    {
        odlfg::Draft d = hostingDraft();
        d.code.clear();
        ok(!odlfg::problemWith(d).empty(), "hosting with no invite code");

        d = hostingDraft();
        d.kind = odlfg::Kind::Looking;
        ok(!odlfg::problemWith(d).empty(), "looking, but carrying a code");

        odlfg::Draft l;
        l.kind = odlfg::Kind::Looking;
        l.map = "any";
        l.mode = odlfg::Mode::Longform;
        l.turnHours = 24;
        ok(odlfg::problemWith(l).empty(), "looking, with no code and no seats");
    }

    section("Open Doctrines games only");
    {
        for (const char* note : {"join us at https://example.com", "discord.gg/abcdef",
                                 "we play hoi4.com on fridays", "www.somewhere.net"}) {
            odlfg::Draft d = hostingDraft();
            d.note = note;
            ok(!odlfg::problemWith(d).empty(), std::string("refused: ") + note);
        }
        odlfg::Draft d = hostingDraft();
        d.note = "New players welcome, we explain the rules.";
        ok(odlfg::problemWith(d).empty(), "an ordinary note is fine");
    }

    section("be respectful: no markup, no mentions");
    {
        odlfg::Draft d = hostingDraft();
        d.note = "<@everyone> get in here";
        ok(!odlfg::problemWith(d).empty(), "a mention is not plain text");
    }

    section("what goes on the wire");
    {
        const std::string body = odlfg::postBody(hostingDraft());
        ok(body.find("\"kind\":\"hosting\"") != std::string::npos, "the tag");
        ok(body.find("\"code\":\"ABCD-EFGH\"") != std::string::npos, "the code");
        ok(body.find("\"turnSeconds\":120") != std::string::npos, "the pace");
        ok(body.find("turnHours") == std::string::npos, "and not the other pace");

        odlfg::Draft l;
        l.kind = odlfg::Kind::Looking;
        l.map = "any";
        l.mode = odlfg::Mode::Longform;
        l.turnHours = 24;
        l.code = "LEFTOVER";     // whatever was typed before the tag changed
        const std::string lb = odlfg::postBody(l);
        ok(lb.find("code") == std::string::npos,
           "a looking listing never sends a code, whatever is in the draft");

        // A quote in a note must not be able to end the field it is in.
        odlfg::Draft q = hostingDraft();
        q.note = "we call it \"the big one\"";
        ok(odlfg::postBody(q).find("\\\"the big one\\\"") != std::string::npos,
           "quotes in a note are escaped");
    }

    // ── ONE BOARD, TWO REASONS TO OPEN IT ──
    //
    // A player who wants a game now reads the hosting listings; a host with an
    // empty lobby reads the looking ones. Before this the board was one list in
    // arrival order and you scrolled past half of it either way.
    section("the board can be read as one tag or the other");
    {
        auto make = [](odlfg::Kind k, const char* id, int taken, int total) {
            odlfg::Listing l;
            l.kind = k; l.id = id; l.nick = id; l.map = "1914";
            l.code = (k == odlfg::Kind::Hosting) ? "AAAA-BBBB" : "";
            l.slotsTaken = taken; l.slotsTotal = total;
            return l;
        };
        std::vector<odlfg::Listing> all = {
            make(odlfg::Kind::Hosting, "full",  6, 6),
            make(odlfg::Kind::Looking, "waiting", 0, 0),
            make(odlfg::Kind::Hosting, "open",  2, 6),
            make(odlfg::Kind::Looking, "waiting2", 0, 0),
        };

        const odlfg::Counts c = odlfg::count(all);
        ok(c.hosting == 2 && c.looking == 2, "each tag is counted for its chip");

        const std::vector<odlfg::Listing> everything = odlfg::view(all, odlfg::Filter::All);
        ok(everything.size() == 4, "everything shows everything");

        const std::vector<odlfg::Listing> hosting = odlfg::view(all, odlfg::Filter::Hosting);
        ok(hosting.size() == 2, "games to join shows only the hosts");
        for (const odlfg::Listing& l : hosting)
            ok(l.kind == odlfg::Kind::Hosting, "and nothing else got in");
        ok(!hosting.empty() && hosting[0].id == "open",
           "a game with a seat left is above one that is full");

        const std::vector<odlfg::Listing> looking = odlfg::view(all, odlfg::Filter::Looking);
        ok(looking.size() == 2, "players looking shows only the players");
        ok(looking.size() == 2 && looking[0].id == "waiting" && looking[1].id == "waiting2",
           "and keeps the order the service sent, which is newest first");

        ok(odlfg::view({}, odlfg::Filter::Hosting).empty(), "an empty board filters to nothing");
    }

    section("the guidelines are the channel's, in the channel's order");
    {
        const std::vector<std::string> rules = odlfg::guidelines();
        ok(rules.size() == 4, "four of them");
        ok(!rules.empty() && rules[0].find("respect") != std::string::npos, "be respectful first");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
