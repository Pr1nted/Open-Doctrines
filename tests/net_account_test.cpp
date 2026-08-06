// The pure parts of the account client: the JSON reader that parses replies
// from the network, and the local nickname check.
//
// The JSON reader is the interesting half. It is a hand-written scanner rather
// than a parser, running against input from a server, so what matters is that
// a malformed reply reads as "no value" and never as a crash.
//
// Build target: NetAccountTest. Run it; non-zero exit means a case failed.

#include "net/AccountClient.h"
#include "net/HttpClient.h"
#include "net/BadgeStyle.h"
#include "net/ServerBook.h"
#include "net/TurnStore.h"
#include "net/TurnStoreRunner.h"

#include <chrono>
#include <cstdio>
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

// A reply shaped like the real ones.
const char* kAccountJson = R"({"account":{"id":"abc123","nickname":"Vlad",)"
                           R"("badges":["developer","playtester"],)"
                           R"("created":1750000000,"linked":["discord","github"]}})";

void testJsonStrings() {
    printf("\n=== json strings ===\n");

    check("reads a plain string", httpJsonString(kAccountJson, "nickname") == "Vlad");
    check("reads an id", httpJsonString(kAccountJson, "id") == "abc123");
    check("an absent key is empty", httpJsonString(kAccountJson, "email").empty());

    check("handles escapes",
          httpJsonString(R"({"m":"a\"b\\c\nd"})", "m") == "a\"b\\c\nd");
    check("handles \\u escapes",
          httpJsonString(R"({"m":"café"})", "m") == "caf\xc3\xa9");
    check("tolerates whitespace around the colon",
          httpJsonString("{\"m\"  :   \"x\"}", "m") == "x");

    // Everything below is a reply from the network.
    check("an unterminated string yields nothing",
          httpJsonString(R"({"m":"abc)", "m").empty());
    check("a truncated \\u escape yields nothing",
          httpJsonString(R"({"m":"a\u00)", "m").empty());
    check("a bad \\u escape yields nothing",
          httpJsonString(R"({"m":"a\uZZZZ"})", "m").empty());
    check("an unknown escape yields nothing",
          httpJsonString(R"({"m":"a\qb"})", "m").empty());
    check("a key with no value yields nothing", httpJsonString(R"({"m"})", "m").empty());
    check("a non-string value yields nothing", httpJsonString(R"({"m":42})", "m").empty());
    check("empty input yields nothing", httpJsonString("", "m").empty());

    // A value longer than the caller's ceiling is refused rather than
    // truncated: half a token is worse than none.
    const std::string big = R"({"m":")" + std::string(5000, 'x') + R"("})";
    check("a value past the ceiling yields nothing", httpJsonString(big, "m", 100).empty());
    check("but is fine under a larger one", httpJsonString(big, "m", 8000).size() == 5000);

    // A lone surrogate is dropped rather than half-decoded into invalid UTF-8
    // that the font renderer would then have to survive.
    check("a lone surrogate is dropped, not half-decoded",
          httpJsonString(R"({"m":"a\ud800b"})", "m") == "ab");
}

void testJsonScalars() {
    printf("\n=== json numbers and bools ===\n");

    check("reads a number", httpJsonNumber(kAccountJson, "created") == 1750000000LL);
    check("reads a negative number", httpJsonNumber(R"({"n":-5})", "n") == -5);
    check("an absent key gives the fallback", httpJsonNumber("{}", "n", 7) == 7);
    check("a non-numeric value gives the fallback",
          httpJsonNumber(R"({"n":"x"})", "n", 7) == 7);

    check("reads true", httpJsonBool(R"({"b":true})", "b"));
    check("reads false", !httpJsonBool(R"({"b":false})", "b", true));
    check("an absent key gives the fallback", httpJsonBool("{}", "b", true));
    check("a non-bool gives the fallback", httpJsonBool(R"({"b":"yes"})", "b", false) == false);
}

void testJsonEscape() {
    printf("\n=== json escaping ===\n");

    check("escapes quotes and backslashes",
          httpJsonEscape("a\"b\\c") == "a\\\"b\\\\c");
    check("escapes newlines", httpJsonEscape("a\nb") == "a\\nb");
    check("escapes control characters as \\u",
          httpJsonEscape(std::string("a\x01""b")) == "a\\u0001b");
    check("leaves ordinary text alone", httpJsonEscape("Vlad_99") == "Vlad_99");

    // A nickname goes into a JSON body. If escaping were wrong, a name
    // containing a quote would let the caller add fields to the request.
    const std::string hostile = R"(x","admin":true,"y":")";
    const std::string body = "{\"nickname\":\"" + httpJsonEscape(hostile) + "\"}";
    check("an injected field does not survive escaping",
          body.find("\"admin\":true") == std::string::npos, body);
}

void testNicknameValidation() {
    printf("\n=== local nickname check ===\n");

    std::string why;
    for (const char* good : {"Vlad", "vlad_99", "a-b-c", "Some Name", "A.B.C"}) {
        check((std::string("accepts ") + good).c_str(),
              AccountClient::nicknameLooksValid(good, why), why);
    }

    struct Case { const char* name; const char* expect; };
    const Case bad[] = {
        {"ab", "3 characters"},
        {"aaaaaaaaaaaaaaaaaaaaaaaaaaa", "24 characters"},
        {"hello!", "Letters"},
        {"_vlad", "start or end"},
        {"vlad_", "start or end"},
        {"vlad__x", "one separator"},
        {"12345", "one letter"},
    };
    for (const auto& c : bad) {
        const bool ok = AccountClient::nicknameLooksValid(c.name, why);
        check((std::string("refuses ") + c.name).c_str(),
              !ok && why.find(c.expect) != std::string::npos, why);
    }

    // Length is counted in code points. A multi-byte character will be refused
    // for its charset, but must not first be miscounted as too long.
    check("counts multi-byte characters once",
          !AccountClient::nicknameLooksValid("caf\xc3\xa9", why) &&
          why.find("Letters") != std::string::npos, why);

    // This check is deliberately weaker than the server's: the blocklist is
    // deployment data and never reaches a client, so "looks valid" is not a
    // promise the name will be accepted.
    check("does not pretend to know the blocklist",
          AccountClient::nicknameLooksValid("admin", why), why);
}


void testKeyVsValue() {
    printf("\n=== a key is only a key if a colon follows ===\n");

    // THE BUG THIS EXISTS FOR: the link reply's top level contains
    // "kind":"linked", and a naive search for the `linked` key matched that
    // VALUE, then took the next '[' -- which was the badges array. The UI
    // therefore showed badges as linked providers and never updated.
    const std::string linkReply =
        R"({"status":"ready","kind":"linked","provider":"google",)"
        R"("account":{"id":"a1","nickname":"Vlad","badges":["developer"],)"
        R"("created":1,"linked":["github","google"]}})";

    const size_t at = httpJsonScope(linkReply, "account");
    check("scoping finds the account object", at > 0);

    auto linked = httpJsonStringArray(linkReply, "linked", 8, at);
    check("linked reads the providers, not the badges",
          linked.size() == 2 && linked[0] == "github" && linked[1] == "google",
          linked.empty() ? "(empty)" : linked[0]);

    auto badges = httpJsonStringArray(linkReply, "badges", 8, at);
    check("badges still reads badges", badges.size() == 1 && badges[0] == "developer");

    // Even unscoped, the reader must skip the value and find the real key.
    auto unscoped = httpJsonStringArray(linkReply, "linked", 8, 0);
    check("even unscoped it skips the \"kind\":\"linked\" value",
          unscoped.size() == 2 && unscoped[0] == "github",
          unscoped.empty() ? "(empty)" : unscoped[0]);

    check("a value that looks like a key is not treated as one",
          httpJsonString(R"({"kind":"nickname","nickname":"Real"})", "nickname") == "Real");

    check("a key with no colon yields nothing",
          httpJsonString(R"({"a":"nickname"})", "nickname").empty());

    check("string arrays are bounded",
          httpJsonStringArray(R"({"a":["1","2","3","4","5"]})", "a", 2).size() == 2);

    check("a malformed array yields nothing",
          httpJsonStringArray(R"({"a":["unterminated})", "a").empty());
    check("a non-array value yields nothing",
          httpJsonStringArray(R"({"a":"notanarray"})", "a").empty());
}


void testBadgeStyle() {
    printf("\n=== badge styling ===\n");

    check("developer renders as [DEVELOPER]", badgeTag("developer") == "[DEVELOPER]");
    check("playtester renders as [PLAYTESTER]", badgeTag("playtester") == "[PLAYTESTER]");

    const uint32_t plain = 0xFFFFFFFFu;
    check("an un-badged name keeps the caller's colour",
          badgeNameColor({}, plain) == plain);
    check("a developer's name is not the default",
          badgeNameColor({"developer"}, plain) != plain);

    // Green: G must dominate R and B, or "dark green" is not what anyone sees.
    const uint32_t dev = badgeNameColor({"developer"});
    const uint8_t r = (dev >> 24) & 0xFF, g = (dev >> 16) & 0xFF, b = (dev >> 8) & 0xFF;
    check("and it is green", g > r && g > b,
          std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b));
    // Legible on the near-black menu background rather than merging into it.
    check("and light enough to read on a dark background", g >= 100,
          std::to_string(g));
    check("and dark enough to read as deep green, not a highlight", g <= 200,
          std::to_string(g));

    // One name, one colour: holding both badges must not depend on the order
    // the server happened to serialise them in.
    check("precedence does not depend on array order",
          badgeNameColor({"developer", "playtester"}) ==
          badgeNameColor({"playtester", "developer"}));
    check("and developer outranks playtester",
          badgeNameColor({"developer", "playtester"}) == badgeNameColor({"developer"}));

    // A badge from a newer server is shown neutrally rather than hidden --
    // hiding it would look to the player like the badge did not exist.
    check("an unknown badge still renders", badgeTag("archivist") == "[ARCHIVIST]");
    check("with a neutral colour", badgeTagColor("archivist") != badgeTagColor("developer"));

    const std::string official = "https://opendoctrines-net.opendoctrines.workers.dev";
    check("the official issuer is recognised", badgeIssuerIsOfficial(official, official));
    check("another issuer is not", !badgeIssuerIsOfficial("https://evil.example", official));
    // A prefix test would accept a URL that merely CONTAINS the official one.
    check("nor is one that merely contains it",
          !badgeIssuerIsOfficial("https://evil.example/?x=" + official, official));
    check("an empty official issuer trusts nothing",
          !badgeIssuerIsOfficial(official, ""));
}


void testServerBook() {
    printf("\n=== server book ===\n");

    const std::string issuer = "https://acct.example";

    std::string gotIssuer, gotCode;
    check("a bare code uses the configured service",
          ServerBook::parseInvite("ABCD-EFGH", issuer, gotIssuer, gotCode) &&
          gotIssuer == issuer && gotCode == "ABCD-EFGH");

    check("a full URL carries both",
          ServerBook::parseInvite("https://relay.example/session/WXYZ-1234", issuer,
                                  gotIssuer, gotCode) &&
          gotIssuer == "https://relay.example" && gotCode == "WXYZ-1234");

    check("a /ws suffix is tolerated",
          ServerBook::parseInvite("https://relay.example/session/WXYZ-1234/ws", issuer,
                                  gotIssuer, gotCode) && gotCode == "WXYZ-1234");

    check("surrounding whitespace is trimmed",
          ServerBook::parseInvite("  ABCD-EFGH\n", issuer, gotIssuer, gotCode) &&
          gotCode == "ABCD-EFGH");

    // Everything below is pasted text, so it is refused rather than guessed at.
    check("an http invite is refused",
          !ServerBook::parseInvite("http://relay.example/session/ABCD-EFGH", issuer,
                                   gotIssuer, gotCode));
    check("a URL with no session path is refused",
          !ServerBook::parseInvite("https://relay.example/", issuer, gotIssuer, gotCode));
    check("a lowercase code is refused",
          !ServerBook::parseInvite("abcd-efgh", issuer, gotIssuer, gotCode));
    check("a code with punctuation is refused",
          !ServerBook::parseInvite("ABCD;EFGH", issuer, gotIssuer, gotCode));
    check("empty input is refused",
          !ServerBook::parseInvite("", issuer, gotIssuer, gotCode));
    check("a bare code with no configured service is refused",
          !ServerBook::parseInvite("ABCD-EFGH", "", gotIssuer, gotCode));

    // Pasting a fresh code must UPDATE the entry rather than pile up a new row
    // every session, because a code names a session and an entry names a place.
    ServerBook book;
    ServerEntry e;
    e.name = "Friday game"; e.issuer = issuer; e.code = "AAAA-1111"; e.lastJoined = 100;
    book.addOrUpdate(e);

    ServerEntry again;
    again.name = "Friday game"; again.issuer = issuer; again.code = "BBBB-2222";
    book.addOrUpdate(again);

    check("a new code updates the existing entry", book.entries().size() == 1 &&
          book.entries()[0].code == "BBBB-2222");
    check("and does not clear when it was last joined",
          book.entries()[0].lastJoined == 100);

    ServerEntry other;
    other.name = "Tournament"; other.issuer = issuer; other.code = "CCCC-3333";
    book.addOrUpdate(other);
    check("a different name is a different entry", book.entries().size() == 2);

    book.sort();
    check("most recently joined sorts first", book.entries()[0].name == "Friday game");

    check("an invalid code is dropped rather than stored",
          !book.setCode(0, "lower-case"));

    ServerEntry bad;
    bad.name = "no issuer";
    book.addOrUpdate(bad);
    check("an entry with no service is not stored", book.entries().size() == 2);
}


void testTurnStoreWarnings() {
    printf("\n=== turn store warnings ===\n");

    for (auto k : {TurnStoreKind::DurableObject, TurnStoreKind::R2,
                   TurnStoreKind::JsonBlob, TurnStoreKind::Manual}) {
        const auto w = turnStoreWarning(k);
        // A store nobody is warned about is a store nobody consented to. Both
        // audiences must be addressed for every backend.
        check((std::string(turnStoreName(k)) + ": warns the host").c_str(),
              !w.forHost.empty());
        check((std::string(turnStoreName(k)) + ": warns the players").c_str(),
              !w.forPlayers.empty());
    }

    const auto own = turnStoreWarning(TurnStoreKind::DurableObject);
    check("the default store is not third-party", !own.thirdParty);
    check("and needs no extra consent", !own.requiresConsent());
    check("and tells the host that being away is expected",
          own.forHost.size() >= 4);

    const auto r2 = turnStoreWarning(TurnStoreKind::R2);
    check("R2 is not flagged as third-party", !r2.thirdParty);
    check("but warns that enabling it wants a payment method",
          r2.forHost.size() >= 2 &&
          r2.forHost[1].find("payment method") != std::string::npos,
          r2.forHost.size() > 1 ? r2.forHost[1] : "(missing)");
    // Even the safe option must say the published turns are readable, since
    // that is what spectating depends on and it is not obvious.
    check("but still says published turns are readable", r2.publiclyReadable);

    const auto jb = turnStoreWarning(TurnStoreKind::JsonBlob);
    check("jsonblob is flagged as third-party", jb.thirdParty);
    check("and as having no guarantee", jb.noGuarantee);
    check("and requires a deliberate choice", jb.requiresConsent());
    check("and warns the host it can lose the tournament",
          jb.forHost.size() >= 4);

    const auto manual = turnStoreWarning(TurnStoreKind::Manual);
    check("manual requires consent too", manual.requiresConsent());
}


void testManualTurnText() {
    printf("\n=== manual turn text ===\n");

    // Manual mode has no infrastructure: a player copies a block of text out
    // and pastes one back. The header exists for the human holding it --
    // pasting last turn's orders is otherwise an invisible mistake.
    const std::vector<uint8_t> payload{0x00, 0x01, 0xFE, 0xFF, 0x42};
    const std::string text = turnStoreEncodeText("orders", 12, payload);

    check("it says what it is", text.find("OpenDoctrines orders turn 12") != std::string::npos);
    check("it is delimited", text.find("--- end ---") != std::string::npos);

    std::string what;
    uint32_t turn = 0;
    std::vector<uint8_t> back;
    check("it reads back", turnStoreDecodeText(text, what, turn, back));
    check("what it is survives", what == "orders", what);
    check("the turn survives", turn == 12, std::to_string(turn));
    check("the bytes survive", back == payload);

    // Pasted text picks up whitespace and stray newlines on the way.
    {
        std::string messy = "  \n" + text + "\n\n";
        std::string w; uint32_t t = 0; std::vector<uint8_t> b;
        check("surrounding whitespace does not matter",
              turnStoreDecodeText(messy, w, t, b) && b == payload);
    }

    // A big payload wraps across lines; it must still read back.
    {
        std::vector<uint8_t> big(5000);
        for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 31 + 7);
        std::string w; uint32_t t = 0; std::vector<uint8_t> b;
        check("a wrapped payload reads back",
              turnStoreDecodeText(turnStoreEncodeText("turn", 3, big), w, t, b) &&
              b == big && t == 3);
    }

    {
        std::string w; uint32_t t = 0; std::vector<uint8_t> b;
        check("junk is refused", !turnStoreDecodeText("hello", w, t, b));
        check("a missing end marker is refused",
              !turnStoreDecodeText("--- OpenDoctrines orders turn 1 ---\nAAAA\n", w, t, b));
        check("an empty string is refused", !turnStoreDecodeText("", w, t, b));
    }
}

void testManualCarriesThePlayer() {
    printf("\n=== manual blocks say whose they are ===\n");

    // Manual mode has no URL to carry the player, and the host cannot open a
    // submission without knowing who sealed it -- the psid is bound in as
    // associated data. So it rides in the block's own label, which means the
    // label has to survive a round trip with a space in it.
    const std::string psid = "PSID-with-22-chars0000";
    const std::vector<uint8_t> sealed{9, 8, 7, 6};
    const std::string text =
        turnStoreEncodeText(("orders " + psid).c_str(), 12, sealed);

    std::string what;
    uint32_t turn = 0;
    std::vector<uint8_t> payload;
    check("an orders block decodes",
          turnStoreDecodeText(text, what, turn, payload));
    check("the label survives whole, space and all", what == "orders " + psid);
    check("the turn survives", turn == 12);
    check("the sealed bytes survive", payload == sealed);

    // And the host's own block must NOT look like a player's, or the two
    // would be routed to each other's handlers.
    const std::string turnText = turnStoreEncodeText("turn", 12, sealed);
    check("a turn block decodes",
          turnStoreDecodeText(turnText, what, turn, payload));
    check("and is distinguishable from orders", what == "turn");
}

void testTurnStoreKindFromWire() {
    printf("\n=== a store kind off the wire ===\n");

    TurnStoreKind k = TurnStoreKind::JsonBlob;
    for (auto expected : {TurnStoreKind::DurableObject, TurnStoreKind::JsonBlob,
                          TurnStoreKind::Manual, TurnStoreKind::R2}) {
        k = TurnStoreKind::JsonBlob;
        check((std::string("round trips ") + turnStoreName(expected)).c_str(),
              turnStoreKindFromWire((uint8_t)expected, k) && k == expected);
    }

    // A host newer than this build. Refused rather than clamped: quietly
    // substituting the default would play somebody's campaign on a store they
    // did not choose, and their turns would go somewhere nobody is looking.
    k = TurnStoreKind::Manual;
    check("a store this build has never heard of is refused",
          !turnStoreKindFromWire(4, k));
    check("and the caller's own value is left alone", k == TurnStoreKind::Manual);
    check("as is a wildly out of range one", !turnStoreKindFromWire(255, k));
}

void testTurnStoreRefs() {
    printf("\n=== derivable blob locations ===\n");

    TurnStoreClient::Config c;
    c.issuer      = "https://example.invalid";
    c.sessionCode = "ABCD-EFGH";

    for (auto kind : {TurnStoreKind::DurableObject, TurnStoreKind::R2}) {
        c.kind = kind;
        TurnStoreClient client;
        client.configure(c);

        // The whole point: a player who was away can find turn 7 knowing only
        // the session, without anyone having told them an id for it.
        check((std::string(turnStoreName(kind)) + ": a turn is derivable").c_str(),
              client.turnRef(7).url ==
                  "https://example.invalid/session/ABCD-EFGH/turn/7");
        check((std::string(turnStoreName(kind)) + ": so are orders").c_str(),
              client.ordersRef(7, "PSID123").url ==
                  "https://example.invalid/session/ABCD-EFGH/orders/7/PSID123");
    }

    // JsonBlob mints its ids when something is posted, so nothing can be
    // derived and an empty ref is the honest answer rather than a URL that
    // would 404.
    for (auto kind : {TurnStoreKind::JsonBlob, TurnStoreKind::Manual}) {
        c.kind = kind;
        TurnStoreClient client;
        client.configure(c);
        check((std::string(turnStoreName(kind)) + ": nothing is derivable").c_str(),
              client.turnRef(7).empty() && client.ordersRef(7, "PSID123").empty());
    }
}

void testTurnStoreRunner() {
    printf("\n=== the store runner's thread ===\n");

    // Manual, so this exercises the queue, the worker and the result path with
    // no network anywhere near it -- Manual publishes by doing nothing and
    // reporting success, which is exactly the shape needed here.
    TurnStoreClient::Config c;
    c.kind = TurnStoreKind::Manual;

    TurnStoreRunner runner;
    runner.configure(c);
    check("Manual is not an automatic transport", !runner.automatic());
    check("nothing is outstanding to begin with", runner.pending() == 0);

    runner.publishTurn(3, {1, 2, 3});

    TurnStoreResult result;
    bool got = false;
    // Bounded rather than a bare loop: a runner that never answers must fail
    // this test, not hang the suite.
    for (int i = 0; i < 2000 && !got; i++) {
        got = runner.nextResult(result);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    check("the worker answers", got);
    check("with the turn it was given", result.turnNumber == 3);
    check("and reports which job it was",
          result.kind == TurnStoreResult::Kind::TurnPublished);
    check("and it succeeded", result.ok);
    check("and nothing is left outstanding", runner.pending() == 0);
    check("and there is no second result", !runner.nextResult(result));
}

}  // namespace

int main() {
    printf("account client\n");
    testJsonStrings();
    testJsonScalars();
    testJsonEscape();
    testNicknameValidation();
    testKeyVsValue();
    testBadgeStyle();
    testServerBook();
    testTurnStoreWarnings();
    testManualTurnText();
    testManualCarriesThePlayer();
    testTurnStoreKindFromWire();
    testTurnStoreRefs();
    testTurnStoreRunner();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
