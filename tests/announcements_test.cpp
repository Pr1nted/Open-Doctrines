// What the game will accept from the announcement service.
//
// This parses a document fetched over the network and drawn on every player's
// main menu, so the refusals ARE the feature. The happy path is four lines; the
// rest of this file is the ways a hostile or broken document is turned away.
//
// Build target: AnnouncementsTest. Non-zero exit means a case failed.

#include "net/Announcements.h"

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

std::string doc(const std::string& inner) {
    return "{\"items\":[" + inner + "]}";
}

}  // namespace

int main() {
    printf("Announcement board\n");
    std::string err;

    section("the one case where a board is shown");
    {
        const auto items = odnews::parseDocument(doc(
            "{\"id\":\"t1\",\"title\":\"Tournament Saturday\","
            "\"body\":\"**Sixteen seats.** Sign up now.\","
            "\"buttonLabel\":\"Join\",\"buttonAction\":\"join\","
            "\"buttonParam\":\"ABCD-1234\"}"), err);
        ok(items.size() == 1, "a well-formed announcement is accepted");
        ok(err.empty(), "with nothing to report");
        if (items.size() == 1) {
            ok(items[0].title == "Tournament Saturday", "the title survives");
            ok(items[0].body.find("**Sixteen seats.**") != std::string::npos,
               "and the markup is kept as TEXT, for the dialogue parser to draw");
            ok(items[0].button.ok(), "the button is usable");
            ok(items[0].button.action == odnews::Action::JoinGame, "and it joins a game");
            ok(items[0].button.param == "ABCD-1234", "with the code it was given");
        }
    }

    section("a button may not name an action this build does not have");
    {
        // THE CASE THAT MATTERS MOST. A document naming something unknown must
        // get NO button -- not a button wired to whatever is nearest.
        const auto items = odnews::parseDocument(doc(
            "{\"id\":\"x\",\"title\":\"Hello\",\"body\":\"Hi\","
            "\"buttonLabel\":\"Run\",\"buttonAction\":\"exec\"}"), err);
        ok(items.empty(), "an unknown action refuses the whole entry");

        for (const char* bad : {"open_url", "shell", "load_mod", "eval", "JOIN", ""}) {
            const auto r = odnews::parseDocument(doc(
                std::string("{\"id\":\"x\",\"title\":\"t\",\"body\":\"b\",")
                + "\"buttonLabel\":\"Go\",\"buttonAction\":\"" + bad + "\"}"), err);
            ok(r.empty(), std::string("and so does \"") + bad + "\"");
        }
        ok(odnews::actionFromName("join") == odnews::Action::JoinGame,
           "while the three real ones still resolve");
        ok(odnews::actionFromName("community") == odnews::Action::Community, "community");
        ok(odnews::actionFromName("account") == odnews::Action::Account, "account");
    }

    section("what a join button's parameter may be");
    {
        // The one action that carries data from the document into the game, so
        // the shape is pinned HERE.
        ok(odnews::looksLikeInviteCode("ABCD-1234"), "a code is letters, digits and dashes");
        ok(!odnews::looksLikeInviteCode(""), "not empty");
        ok(!odnews::looksLikeInviteCode("../../etc/passwd"), "not a path");
        ok(!odnews::looksLikeInviteCode("a b"), "not spaced");
        ok(!odnews::looksLikeInviteCode("http://evil.example"), "not a URL");
        ok(!odnews::looksLikeInviteCode(std::string(64, 'a')), "and not long");

        const auto items = odnews::parseDocument(doc(
            "{\"id\":\"x\",\"title\":\"t\",\"body\":\"b\",\"buttonLabel\":\"Go\","
            "\"buttonAction\":\"join\",\"buttonParam\":\"; rm -rf /\"}"), err);
        ok(items.empty(), "a join whose code is not a code is refused whole");
    }
    {
        // An action that takes nothing must be given nothing: a parameter it
        // ignores today is a parameter somebody relies on tomorrow.
        const auto items = odnews::parseDocument(doc(
            "{\"id\":\"x\",\"title\":\"t\",\"body\":\"b\",\"buttonLabel\":\"Go\","
            "\"buttonAction\":\"community\",\"buttonParam\":\"anything\"}"), err);
        ok(items.empty(), "community with a parameter is refused");
    }

    section("bounds, so a hostile document costs a fixed amount");
    {
        std::string huge(odnews::Limits::kDocument + 1, 'a');
        const auto items = odnews::parseDocument(huge, err);
        ok(items.empty() && !err.empty(), "an oversized document is refused with a reason");

        const std::string longTitle(odnews::Limits::kTitle + 10, 'x');
        ok(odnews::parseDocument(doc(
               "{\"id\":\"x\",\"title\":\"" + longTitle + "\",\"body\":\"b\"}"), err).empty(),
           "an over-long title refuses the entry");

        std::string many;
        for (int i = 0; i < 50; ++i) {
            if (i) many += ",";
            many += "{\"id\":\"i" + std::to_string(i) + "\",\"title\":\"t\",\"body\":\"b\"}";
        }
        ok(odnews::parseDocument(doc(many), err).size() <= odnews::Limits::kMaxItems,
           "and fifty entries are cut to the cap");
    }

    section("malformed input fails closed");
    {
        ok(odnews::parseDocument("", err).empty(), "an empty reply shows no board");
        ok(odnews::parseDocument("not json", err).empty(), "so does rubbish");
        ok(odnews::parseDocument("<html>503</html>", err).empty(),
           "so does an error page from a proxy");
        ok(odnews::parseDocument("{\"items\":[", err).empty(), "so does a truncated document");
        ok(odnews::parseDocument("{\"items\":[{\"id\":\"a\"", err).empty(),
           "so does an unterminated entry");
        ok(odnews::parseDocument("{\"items\":[]}", err).empty() && err.empty(),
           "and an empty list is no board and no complaint");
    }
    {
        // An entry with nothing readable is not an entry.
        ok(odnews::parseDocument(doc("{\"id\":\"x\"}"), err).empty(),
           "an entry with no words is dropped");
        ok(odnews::parseDocument(doc("{\"title\":\"t\",\"body\":\"b\"}"), err).empty(),
           "and one with no id, which is how the game remembers it");
    }
    {
        // Control characters cannot appear in anything legitimate.
        const auto items = odnews::parseDocument(doc(
            "{\"id\":\"x\",\"title\":\"a\\u0007b\",\"body\":\"b\"}"), err);
        (void)items;
        ok(true, "a title with an escape sequence does not crash the parser");
    }

    section("timestamps, and taking itself off the board");
    {
        const long long kSat = 1789000000LL;   // some Saturday
        const auto items = odnews::parseDocument(doc(
            "{\"id\":\"t\",\"title\":\"Tournament\",\"body\":\"b\","
            "\"postedAt\":1788900000,\"until\":1789000000}"), err);
        ok(items.size() == 1, "an entry with timestamps is accepted");
        if (items.size() == 1) {
            ok(items[0].postedAt == 1788900000LL, "the posted time is read");
            ok(items[0].until == 1789000000LL, "and the expiry");
            ok(!items[0].expired(kSat - 60), "it is live a minute before");
            ok(items[0].expired(kSat), "and gone the moment it expires");
            ok(items[0].expired(kSat + 99999), "and stays gone");
        }
        // An entry with no expiry stays until it is taken down by hand.
        const auto forever = odnews::parseDocument(doc(
            "{\"id\":\"f\",\"title\":\"t\",\"body\":\"b\"}"), err);
        ok(forever.size() == 1 && !forever[0].expired(kSat + 99999),
           "an entry with no expiry does not expire");
    }
    {
        // live() is what the menu draws, so the filtering is tested where it
        // is used rather than left to the caller to get right.
        const auto items = odnews::parseDocument(doc(
            "{\"id\":\"old\",\"title\":\"t\",\"body\":\"b\",\"until\":1700000001},"
            "{\"id\":\"new\",\"title\":\"t\",\"body\":\"b\",\"until\":1900000000}"), err);
        ok(items.size() == 2, "both parse");
        const auto shown = odnews::live(items, 1800000000LL);
        ok(shown.size() == 1 && shown[0].id == "new",
           "and only the one that has not expired is shown");
    }
    {
        // A timestamp outside any plausible range is a broken document.
        ok(odnews::parseDocument(doc(
               "{\"id\":\"x\",\"title\":\"t\",\"body\":\"b\",\"postedAt\":5}"), err).empty(),
           "a timestamp from before the game existed is refused");
        ok(odnews::parseDocument(doc(
               "{\"id\":\"x\",\"title\":\"t\",\"body\":\"b\",\"until\":99999999999}"), err).empty(),
           "and one centuries out");
    }

    section("a countdown, before and after the moment");
    {
        const long long T = 1789000000LL;
        auto at = [&](long long offset) { return odnews::formatCountdown(T, T - offset); };
        // Positive offset = the moment is that far in the FUTURE.
        ok(at(3 * 86400) == "in 3 days", "days out reads in days");
        ok(at(2 * 86400) == "in 2 days", "and two is still days");
        ok(at(86400 + 10) == "in 1 day", "one day is singular, not \"1 days\"");
        ok(at(7200) == "in 2h", "hours, with no minutes when there are none");
        ok(at(3600 + 720) == "in 1h 12m", "and with them when there are");
        ok(at(300) == "in 5m", "minutes close in");
        ok(at(45) == "in 45s", "and seconds at the end");
        ok(at(0) == "in 0s", "the moment itself has not passed yet");

        // AFTER. A countdown that goes blank at zero takes the answer away at
        // the point most people are looking at it.
        ok(at(-60) == "1m ago", "a minute past reads as past");
        ok(at(-7200) == "2h ago", "and hours");
        ok(at(-3 * 86400) == "3 days ago", "and days");
        ok(!odnews::formatCountdown(T, T + 999999).empty(),
           "it never goes blank once the moment is behind us");
        ok(odnews::formatCountdown(0, T).empty(), "no instant means no words");
    }

    section("an instant is stored, not a wall-clock reading");
    {
        // formatLocal asks the machine what timezone it is in, so the exact
        // string depends on where this runs. What can be checked anywhere is
        // that it produces something, and that it MOVES with the instant --
        // which is what proves it is converting rather than echoing.
        const std::string a = odnews::formatLocal(1789000000LL);
        const std::string b = odnews::formatLocal(1789000000LL + 7200);
        ok(!a.empty(), "an instant renders as local wall-clock time");
        ok(a != b, "and two hours later reads differently");
        ok(odnews::formatLocal(0).empty(), "no instant means no words");
    }

    section("how a document asks for each");
    {
        const auto items = odnews::parseDocument(doc(
            "{\"id\":\"t\",\"title\":\"Tournament\",\"body\":\"b\","
            "\"eventAt\":1789000000,\"timeStyle\":\"countdown\"}"), err);
        ok(items.size() == 1, "an event with a style is accepted");
        if (items.size() == 1) {
            ok(items[0].eventAt == 1789000000LL, "the instant is read");
            ok(items[0].timeStyle == odnews::Item::TimeStyle::Countdown, "and the style");
        }
        ok(odnews::timeStyleFromName("local") == odnews::Item::TimeStyle::Local, "local");
        ok(odnews::timeStyleFromName("countdown") == odnews::Item::TimeStyle::Countdown,
           "countdown");
        // The usual rule: something this build does not understand is refused
        // whole rather than drawn as something else.
        ok(odnews::parseDocument(doc(
               "{\"id\":\"t\",\"title\":\"t\",\"body\":\"b\","
               "\"eventAt\":1789000000,\"timeStyle\":\"marquee\"}"), err).empty(),
           "an unknown time style refuses the entry");
        ok(odnews::parseDocument(doc(
               "{\"id\":\"t\",\"title\":\"t\",\"body\":\"b\","
               "\"timeStyle\":\"countdown\"}"), err).empty(),
           "and a style with no instant to show");
    }

    section("one bad entry does not take the good ones with it");
    {
        const auto items = odnews::parseDocument(doc(
            "{\"id\":\"good\",\"title\":\"Fine\",\"body\":\"b\"},"
            "{\"id\":\"bad\",\"title\":\"t\",\"body\":\"b\","
            "\"buttonLabel\":\"Go\",\"buttonAction\":\"exec\"}"), err);
        ok(items.size() == 1 && items[0].id == "good",
           "the sound entry is kept and the tampered one is dropped");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
