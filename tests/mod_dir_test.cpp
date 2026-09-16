// What the game will accept from the mod directory, and what it refuses.
//
// A listing is written by a stranger, fetched over the network, drawn on your
// screen, and carries a URL that a button will hand to your browser. So the
// refusals ARE the feature, and the one that matters most is the URL: the
// service refuses anything but https at publish time, and this refuses it again
// at the moment it would be acted on -- because that is the check that has to
// hold even if the service is wrong, or is not the service you think it is.
//
// Build target: ModDirTest. Non-zero exit means a case failed.

#include "net/ModDir.h"

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

std::string doc(const std::string& inner) { return "{\"mods\":[" + inner + "]}"; }

std::string one(const std::string& fields) { return "{" + fields + "}"; }

std::vector<odmoddir::Listing> parse(const std::string& json) {
    std::string why;
    return odmoddir::parse(json, why);
}

const std::string kGood =
    "\"id\":\"com.example.mod\",\"name\":\"Better AI\",\"version\":\"1.2.0\","
    "\"by\":\"Jane\",\"summary\":\"Retunes war scoring.\","
    "\"page\":\"https://example.com/p\",\"side\":\"client\","
    "\"modules\":[\"Core\",\"UI\"],"
    "\"scan\":{\"state\":\"clean\",\"malicious\":0,\"engines\":72}";

}  // namespace

int main() {
    section("an ordinary listing");
    {
        auto got = parse(doc(one(kGood)));
        ok(got.size() == 1, "one listing reads");
        if (got.size() == 1) {
            ok(got[0].name == "Better AI", "the name survives");
            ok(got[0].by == "Jane", "the author survives");
            ok(got[0].page == "https://example.com/p", "an https page is kept");
            ok(got[0].scan == odmoddir::Scan::Clean, "a clean scan reads as clean");
            ok(got[0].engines == 72, "the denominator comes with the count");
            ok(got[0].modules.size() == 2, "the capabilities read");
            ok(!got[0].changesTurns, "UI and Core do not change turns");
        }
    }

    section("the URL, which a button would open");
    {
        // THE ONE THAT MATTERS. If any of these came back openable, the
        // directory would be a way to point somebody's machine at a scheme of
        // the publisher's choosing.
        const char* refused[] = {
            "http://example.com/p",
            "javascript:alert(1)",
            "file:///etc/passwd",
            "HTTPS://example.com/p",           // scheme is matched exactly
            "https://example.com/a b",         // a space
            "https://example.com/`id`",        // shell metacharacters
            "https://example.com/a;b",
            "",
        };
        const std::string base = "\"id\":\"com.example.mod\",\"name\":\"Better AI\"";
        for (const char* u : refused) {
            // ONE "page" key in the object. An earlier version appended a second
            // one after kGood's, and the reader takes the first -- so every case
            // passed the good URL through and the test proved nothing.
            auto got = parse(doc(one(base + ",\"page\":\"" + std::string(u) + "\"")));
            // The listing itself still shows -- it just offers no button.
            const bool cleared = got.size() == 1 && got[0].page.empty();
            ok(cleared, std::string("refused, and the row survives without a link: ")
                        + (*u ? u : "(empty)"));
        }
        ok(!odmoddir::openable("http://example.com"), "openable() refuses plain http");
        ok(odmoddir::openable("https://example.com/x"), "openable() allows https");
    }

    section("a scan result is never rounded up");
    {
        auto unknown = parse(doc(one(
            "\"id\":\"a.b\",\"name\":\"N\",\"scan\":{\"state\":\"unknown\"}")));
        ok(unknown.size() == 1 && unknown[0].scan == odmoddir::Scan::Unknown,
           "unknown stays unknown");

        // A state this build has never heard of must read as "nobody asked",
        // never as clean: an unrecognised word is not a reassurance.
        auto future = parse(doc(one(
            "\"id\":\"a.b\",\"name\":\"N\",\"scan\":{\"state\":\"probably-fine\"}")));
        ok(future.size() == 1 && future[0].scan == odmoddir::Scan::Unscanned,
           "a word we do not know reads as not-checked, not clean");

        auto none = parse(doc(one("\"id\":\"a.b\",\"name\":\"N\"")));
        ok(none.size() == 1 && none[0].scan == odmoddir::Scan::Unscanned,
           "no scan object reads as not-checked");

        auto flagged = parse(doc(one(
            "\"id\":\"a.b\",\"name\":\"N\",\"scan\":{\"state\":\"flagged\","
            "\"malicious\":7,\"suspicious\":2,\"engines\":71}")));
        ok(flagged.size() == 1 && flagged[0].scan == odmoddir::Scan::Flagged
           && flagged[0].flagged == 9,
           "flagged counts malicious and suspicious together");
    }

    section("where it runs");
    {
        auto server = parse(doc(one("\"id\":\"a.b\",\"name\":\"N\",\"side\":\"server\"")));
        ok(server.size() == 1 && server[0].side == odmoddir::Side::Server, "server reads");
        auto both = parse(doc(one("\"id\":\"a.b\",\"name\":\"N\",\"side\":\"both\"")));
        ok(both.size() == 1 && both[0].side == odmoddir::Side::Synchronised,
           "\"both\" is shown as synchronised");
        // An unrecognised side reads as the mildest claim, not the loudest.
        auto odd = parse(doc(one("\"id\":\"a.b\",\"name\":\"N\",\"side\":\"elsewhere\"")));
        ok(odd.size() == 1 && odd[0].side == odmoddir::Side::Client,
           "an unknown side reads as client");
    }

    section("a mod that changes how turns resolve says so");
    {
        for (const char* m : {"GameProcess", "GameState.Write"}) {
            auto got = parse(doc(one("\"id\":\"a.b\",\"name\":\"N\",\"modules\":[\"Core\",\""
                                     + std::string(m) + "\"]")));
            ok(got.size() == 1 && got[0].changesTurns,
               std::string(m) + " marks the listing");
        }
    }

    section("what is dropped whole");
    {
        ok(parse(doc(one("\"name\":\"No id\""))).empty(), "a listing with no id");
        ok(parse(doc(one("\"id\":\"a.b\""))).empty(), "a listing with no name");
        // One bad entry must not cost the good ones.
        auto mixed = parse(doc(one("\"name\":\"No id\"") + "," + one(kGood)));
        ok(mixed.size() == 1, "a broken entry is skipped and the rest still read");
    }

    section("nothing unbounded reaches the screen");
    {
        const std::string huge(4000, 'x');
        // A field past its bound reads as absent, so an over-long REQUIRED field
        // drops the listing rather than drawing a row nobody can read.
        ok(parse(doc(one("\"id\":\"a.b\",\"name\":\"" + huge + "\""))).empty(),
           "an over-long name drops the listing");
        // An optional one just goes missing, and the row survives.
        auto got = parse(doc(one("\"id\":\"a.b\",\"name\":\"N\",\"summary\":\""
                                 + huge + "\"")));
        ok(got.size() == 1, "an over-long summary leaves the listing readable");
        if (got.size() == 1) {
            ok(got[0].summary.empty(), "and the summary is simply absent");
            ok(got[0].name == "N", "the rest of the listing is untouched");
        }
        std::string many;
        for (int i = 0; i < 200; i++) {
            if (i) many += ",";
            many += one("\"id\":\"a.b" + std::to_string(i) + "\",\"name\":\"N\"");
        }
        ok(parse(doc(many)).size() <= odmoddir::Limits::kItems,
           "the list stops at the item cap");
    }

    section("no network is an empty list, not an error");
    {
        ok(parse("").empty(), "an empty body");
        ok(parse("not json at all").empty(), "a body that is not json");
        ok(parse("{\"mods\":\"nope\"}").empty(), "mods that is not a list");
        ok(parse("{\"error\":\"rate_limited\"}").empty(), "a refusal from the service");
        ok(parse("{\"mods\":[").empty(), "a truncated document");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
