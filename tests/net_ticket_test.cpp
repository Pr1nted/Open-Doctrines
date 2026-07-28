// Join-ticket verification: the check that replaced the account service.
//
// This is the gate a stranger passes through to be seated on someone's home
// computer. In the relay design Cloudflare made this decision; it is ours now,
// so almost every case here is a ticket that must be REFUSED.
//
// The fixture below was produced by WebCrypto with the same construction the
// Worker uses -- `od1.<b64url(claims)>` signed with Ed25519, key exported as a
// JWK exactly as `/.well-known/od-keys.json` serves it. It is not something
// this codebase encoded and then read back, which is the only way this test
// says anything about whether real tickets verify.
//
// Build target: NetTicketTest. Run it; non-zero exit means a case failed.

#include "net/HttpClient.h"
#include "net/JoinTicket.h"

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

// Signed by WebCrypto. Claims:
//   iss   https://issuer.example
//   aud   od-relay:ABCD1234
//   psid  p_aaaaaaaaaaaaaaaaaaaa
//   name  Pr1nted        badges ["developer"]
//   nonce n_0123456789abcdef
//   jti   j_zzzzzzzzzzzzzzzzzzzz
//   iat   1800000000     exp    1800000120
const char kToken[] =
    "od1.eyJpc3MiOiJodHRwczovL2lzc3Vlci5leGFtcGxlIiwiYXVkIjoib2QtcmVsYXk6QUJ"
    "DRDEyMzQiLCJwc2lkIjoicF9hYWFhYWFhYWFhYWFhYWFhYWFhYSIsIm5hbWUiOiJQcjFudG"
    "VkIiwiYmFkZ2VzIjpbImRldmVsb3BlciJdLCJub25jZSI6Im5fMDEyMzQ1Njc4OWFiY2RlZ"
    "iIsImp0aSI6Impfenp6enp6enp6enp6enp6enp6enoiLCJpYXQiOjE4MDAwMDAwMDAsImV4"
    "cCI6MTgwMDAwMDEyMH0.mcGrD0EVrE8e_lpFTwNXT5FPICeTivq8-TrpU6mljDdNTVMYdQ6"
    "tqmIiWHs_j1sSTxmlw-MF56vGSXX2zJU6AA";

const char kKeysDoc[] =
    "{\"keys\":[{\"key_ops\":[\"verify\"],\"ext\":true,\"alg\":\"Ed25519\","
    "\"crv\":\"Ed25519\",\"x\":\"3yEFT2pnw24SRZc3QF1dMW5Pj9uq0BJyjy1K8Zgoiek\","
    "\"kty\":\"OKP\"}],\"issuer\":\"https://issuer.example\"}";

constexpr long long kIat = 1800000000;
constexpr long long kExp = 1800000120;

std::vector<NetIssuerKey> goodKeys() { return netParseIssuerKeys(kKeysDoc); }

NetTicketCheck goodCheck() {
    NetTicketCheck c;
    c.issuer = "https://issuer.example";
    c.audience = "od-relay:ABCD1234";
    c.nonce = "n_0123456789abcdef";
    c.now = kIat + 10;
    return c;
}

void testKeys() {
    printf("\n=== reading the published key set ===\n");

    const auto keys = goodKeys();
    check("one Ed25519 key is found", keys.size() == 1,
          "got " + std::to_string(keys.size()));

    check("an empty document yields nothing", netParseIssuerKeys("").empty());
    check("junk yields nothing", netParseIssuerKeys("not json at all").empty());
    check("an empty key set yields nothing",
          netParseIssuerKeys("{\"keys\":[]}").empty());

    // A key for some other algorithm must not be used to verify a signature.
    check("a non-Ed25519 key is ignored",
          netParseIssuerKeys("{\"keys\":[{\"kty\":\"EC\",\"crv\":\"P-256\","
                             "\"x\":\"3yEFT2pnw24SRZc3QF1dMW5Pj9uq0BJyjy1K8Zgoiek\"}]}")
              .empty());

    check("a key of the wrong length is ignored",
          netParseIssuerKeys("{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\","
                             "\"x\":\"c2hvcnQ\"}]}").empty());

    // Rotation: two keys published at once, either of which may have signed.
    const auto two = netParseIssuerKeys(
        "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\","
        "\"x\":\"3yEFT2pnw24SRZc3QF1dMW5Pj9uq0BJyjy1K8Zgoiek\"},"
        "{\"kty\":\"OKP\",\"crv\":\"Ed25519\","
        "\"x\":\"ArrqMZFKRHVWUYVDEK51ruBzjqtJqNXgXLE2hHKzH-I\"}]}");
    check("both keys are read during a rotation", two.size() == 2,
          "got " + std::to_string(two.size()));
}

void testAccepts() {
    printf("\n=== a genuine ticket ===\n");

    NetJoinTicket t;
    const bool ok = netVerifyJoinTicket(kToken, goodKeys(), goodCheck(), t);
    check("a real WebCrypto-signed ticket verifies", ok);
    if (!ok) return;

    check("the pseudonym is read", t.psid == "p_aaaaaaaaaaaaaaaaaaaa", t.psid);
    check("the display name is read", t.name == "Pr1nted", t.name);
    check("badges are read", t.badges.size() == 1 && t.badges[0] == "developer");
    check("the expiry is read", t.expires == kExp);
    check("the jti is read", t.jti == "j_zzzzzzzzzzzzzzzzzzzz", t.jti);

    // A rotation must not lock anyone out: an unrelated key alongside the real
    // one still verifies.
    auto rotating = netParseIssuerKeys(
        "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\","
        "\"x\":\"ArrqMZFKRHVWUYVDEK51ruBzjqtJqNXgXLE2hHKzH-I\"},"
        "{\"kty\":\"OKP\",\"crv\":\"Ed25519\","
        "\"x\":\"3yEFT2pnw24SRZc3QF1dMW5Pj9uq0BJyjy1K8Zgoiek\"}]}");
    NetJoinTicket t2;
    check("it still verifies when a second key is published",
          netVerifyJoinTicket(kToken, rotating, goodCheck(), t2));
}

void testRefuses() {
    printf("\n=== what it refuses ===\n");

    NetJoinTicket t;
    const auto keys = goodKeys();

    // Without this, everything above would pass on a function returning true.
    {
        std::string tampered = kToken;
        tampered[10] = tampered[10] == 'a' ? 'b' : 'a';
        check("a tampered payload is refused",
              !netVerifyJoinTicket(tampered, keys, goodCheck(), t));
    }
    {
        std::string tampered = kToken;
        tampered[tampered.size() - 5] = tampered[tampered.size() - 5] == 'A' ? 'B' : 'A';
        check("a tampered signature is refused",
              !netVerifyJoinTicket(tampered, keys, goodCheck(), t));
    }

    check("no keys at all is a refusal, not a pass",
          !netVerifyJoinTicket(kToken, {}, goodCheck(), t));

    {
        // Signed by somebody else entirely.
        const auto wrong = netParseIssuerKeys(
            "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\","
            "\"x\":\"ArrqMZFKRHVWUYVDEK51ruBzjqtJqNXgXLE2hHKzH-I\"}]}");
        check("a ticket signed by another key is refused",
              !netVerifyJoinTicket(kToken, wrong, goodCheck(), t));
    }

    {
        auto c = goodCheck();
        c.issuer = "https://evil.example";
        check("a different issuer is refused", !netVerifyJoinTicket(kToken, keys, c, t));
    }

    // The audience binds the ticket to ONE session. A prefix must not do.
    {
        auto c = goodCheck();
        c.audience = "od-relay:ABCD123";
        check("a truncated audience is refused (prefixes must not match)",
              !netVerifyJoinTicket(kToken, keys, c, t));
    }
    {
        auto c = goodCheck();
        c.audience = "od-relay:ABCD12345";
        check("a longer audience is refused", !netVerifyJoinTicket(kToken, keys, c, t));
    }
    {
        auto c = goodCheck();
        c.audience = "od-api";
        check("an account-API audience is refused",
              !netVerifyJoinTicket(kToken, keys, c, t));
    }

    // The nonce is this host's challenge on this connection. A ticket minted
    // for a different socket must not be usable here.
    {
        auto c = goodCheck();
        c.nonce = "n_something_else";
        check("a ticket answering a different challenge is refused",
              !netVerifyJoinTicket(kToken, keys, c, t));
    }
    {
        auto c = goodCheck();
        c.nonce.clear();
        check("a host with no challenge of its own verifies nothing",
              !netVerifyJoinTicket(kToken, keys, c, t));
    }

    // Freshness, with the documented clock tolerance.
    {
        auto c = goodCheck();
        c.now = kExp + c.skewSeconds + 1;
        check("an expired ticket is refused", !netVerifyJoinTicket(kToken, keys, c, t));
    }
    {
        auto c = goodCheck();
        c.now = kExp + 5;      // just past exp, inside the skew allowance
        check("a slightly fast host clock still admits a live ticket",
              netVerifyJoinTicket(kToken, keys, c, t));
    }
    {
        auto c = goodCheck();
        c.skewSeconds = 0;
        c.now = kExp + 5;
        check("with no allowance, past the expiry is past it",
              !netVerifyJoinTicket(kToken, keys, c, t));
    }
    {
        auto c = goodCheck();
        c.now = kIat - 600;    // host clock far behind the issuer's
        check("a ticket issued implausibly far ahead is refused",
              !netVerifyJoinTicket(kToken, keys, c, t));
    }

    // Shape.
    check("an empty token is refused", !netVerifyJoinTicket("", keys, goodCheck(), t));
    check("a token with no dots is refused",
          !netVerifyJoinTicket("od1", keys, goodCheck(), t));
    check("a token with two parts is refused",
          !netVerifyJoinTicket("od1.eyJhIjoxfQ", keys, goodCheck(), t));
    {
        std::string extra = std::string(kToken) + ".extra";
        check("a fourth part is refused",
              !netVerifyJoinTicket(extra, keys, goodCheck(), t));
    }
    {
        // The prefix is where the algorithm would live in a JWT. It is fixed
        // here, and a token claiming another format is not ours.
        std::string other = kToken;
        other[2] = '2';
        check("an unknown format prefix is refused",
              !netVerifyJoinTicket(other, keys, goodCheck(), t));
    }
    {
        std::string huge(9000, 'a');
        check("an oversized token is refused before any work",
              !netVerifyJoinTicket("od1." + huge + ".sig", keys, goodCheck(), t));
    }
    {
        // Standard base64 is not base64url; '+' and '/' must not be accepted
        // silently as though they were '-' and '_'.
        std::string swapped = kToken;
        for (char& c : swapped) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
        check("standard base64 is refused",
              !netVerifyJoinTicket(swapped, keys, goodCheck(), t));
    }
}

void testReplay() {
    printf("\n=== burning the jti ===\n");

    NetTicketReplayGuard guard;
    const long long now = kIat;

    check("the first use is allowed", guard.useOnce("j_one", now + 120, now));
    check("the same jti a second time is refused",
          !guard.useOnce("j_one", now + 120, now));
    check("a different jti is allowed", guard.useOnce("j_two", now + 120, now));
    check("an empty jti is never allowed", !guard.useOnce("", now + 120, now));

    // Two sockets presenting the same live ticket at once is the case this
    // exists for: one seat, not two.
    check("a concurrent replay is refused", !guard.useOnce("j_two", now + 120, now));

    // Entries must not accumulate forever, but must not be dropped while the
    // ticket they refer to is still valid either.
    check("a live entry is retained", guard.size() == 2,
          "size " + std::to_string(guard.size()));
    guard.sweep(now + 121);
    check("expired entries are swept", guard.size() == 0,
          "size " + std::to_string(guard.size()));
    check("a jti may be reused once its ticket is long expired",
          guard.useOnce("j_one", now + 300, now + 121));
}

void testIssuerClock() {
    printf("\n=== the issuer's clock ===\n");

    // RFC 7231 IMF-fixdate. The worked example from RFC 7231 section 7.1.1.1
    // is 784111777, which is a value from outside this codebase.
    check("the RFC 7231 example date parses",
          httpParseDate("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777,
          std::to_string(httpParseDate("Sun, 06 Nov 1994 08:49:37 GMT")));

    check("the unix epoch parses",
          httpParseDate("Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    check("a leap day parses",
          httpParseDate("Sat, 29 Feb 2020 12:00:00 GMT") == 1582977600,
          std::to_string(httpParseDate("Sat, 29 Feb 2020 12:00:00 GMT")));
    // Every month, because an off-by-one in the month table is invisible for
    // eleven twelfths of the year.
    const char* months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const long long firstOf2021[12] = {
        1609459200, 1612137600, 1614556800, 1617235200, 1619827200, 1622505600,
        1625097600, 1627776000, 1630454400, 1633046400, 1635724800, 1638316800,
    };
    bool allMonths = true;
    for (int i = 0; i < 12; i++) {
        const std::string s = std::string("Fri, 01 ") + months[i] + " 2021 00:00:00 GMT";
        if (httpParseDate(s) != firstOf2021[i]) {
            allMonths = false;
            printf("        %s -> %lld, wanted %lld\n", s.c_str(),
                   httpParseDate(s), firstOf2021[i]);
        }
    }
    check("every month maps to the right day", allMonths);

    check("a wrong weekday does not stop the clock being read",
          httpParseDate("Mon, 06 Nov 1994 08:49:37 GMT") == 784111777);

    check("junk is not a date", httpParseDate("not a date at all") == 0);
    check("an empty value is not a date", httpParseDate("") == 0);
    check("an unknown month is not a date",
          httpParseDate("Sun, 06 Xxx 1994 08:49:37 GMT") == 0);
    check("a truncated value is not a date",
          httpParseDate("Sun, 06 Nov 1994 08:49") == 0);

    // The point of all of it: a host whose own clock is wrong still admits
    // players, because it verifies against the issuer's time and not its own.
    {
        NetIssuerClock clock;
        check("nothing is claimed before the issuer has been heard from",
              !clock.known() && clock.offset() == 0);

        // The host's clock is five minutes slow. Without this, every ticket
        // would look as though it had not been issued yet.
        const long long hostClock = kIat - 300;
        clock.observe(kIat, hostClock);
        check("the offset is learned", clock.known() && clock.offset() == 300);
        check("issuer time is recovered", clock.now(hostClock) == kIat);

        auto c = goodCheck();
        c.now = clock.now(hostClock + 10);
        NetJoinTicket t;
        check("a host five minutes slow still admits a live ticket",
              netVerifyJoinTicket(kToken, goodKeys(), c, t));

        // And the same host with the clock ignored would have refused it,
        // which is the failure this removes.
        auto naive = goodCheck();
        naive.now = hostClock + 10;
        NetJoinTicket t2;
        check("the same host WITHOUT the issuer clock would have refused it",
              !netVerifyJoinTicket(kToken, goodKeys(), naive, t2));
    }
    {
        // A host an hour fast is the other direction, and just as common.
        NetIssuerClock clock;
        const long long hostClock = kIat + 3600;
        clock.observe(kIat, hostClock);
        auto c = goodCheck();
        c.now = clock.now(hostClock + 10);
        NetJoinTicket t;
        check("a host an hour fast still admits a live ticket",
              netVerifyJoinTicket(kToken, goodKeys(), c, t));
    }
    {
        // A reply with no Date must not be read as "our clock is correct".
        NetIssuerClock clock;
        clock.observe(kIat, kIat - 300);
        clock.observe(0, kIat - 300);
        check("a reply without a date leaves the estimate alone",
              clock.known() && clock.offset() == 300);
    }
    {
        // Learning the issuer's clock must not make expiry meaningless.
        NetIssuerClock clock;
        const long long hostClock = kIat - 300;
        clock.observe(kIat, hostClock);
        auto c = goodCheck();
        c.now = clock.now(hostClock + 1000);      // genuinely long past exp
        NetJoinTicket t;
        check("a genuinely expired ticket is still refused",
              !netVerifyJoinTicket(kToken, goodKeys(), c, t));
    }
}

}  // namespace

int main() {
    printf("join ticket verification\n");
    testKeys();
    testAccepts();
    testRefuses();
    testReplay();
    testIssuerClock();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
