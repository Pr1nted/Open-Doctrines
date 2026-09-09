// Reading Twitch chat: everything that arrives, and everything that half does.
//
// Build target: IrcParseTest. Non-zero exit means a case failed.

#include "stream/IrcParse.h"

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
}  // namespace

int main() {
    printf("Twitch chat lines\n");

    section("an ordinary message");
    {
        const irc::Line l = irc::parseLine(
            ":anna!anna@anna.tmi.twitch.tv PRIVMSG #streamer :war");
        ok(l.kind == irc::Line::Kind::Message, "is a message");
        ok(l.who == "anna", "from a named viewer");
        ok(l.text == "war", "with what they said");
    }
    {
        // The message keeps its own colons: emoticons, times and URLs all have
        // them, and cutting at the wrong one truncates a vote.
        const irc::Line l = irc::parseLine(
            ":bob!bob@bob.tmi.twitch.tv PRIVMSG #s :starts at 20:00 lol :)");
        ok(l.text == "starts at 20:00 lol :)", "colons inside the message survive");
    }
    {
        const irc::Line l = irc::parseLine(
            ":ANNA!anna@anna.tmi.twitch.tv PRIVMSG #s :WAR");
        ok(l.who == "anna", "the login is lowercased, so one viewer is one key");
        ok(l.text == "WAR", "but what they typed is left exactly as typed");
    }
    {
        // Tags are not asked for, but a channel may send them anyway.
        const irc::Line l = irc::parseLine(
            "@badge-info=;color=#FF0000;display-name=Anna "
            ":anna!anna@anna.tmi.twitch.tv PRIVMSG #s :2");
        ok(l.kind == irc::Line::Kind::Message && l.who == "anna" && l.text == "2",
           "a tagged line parses the same");
    }

    section("the PING that keeps the connection alive");
    {
        // Twitch pings every few minutes and hangs up when nothing answers. A
        // reader that ignores this works for the length of a test and dies
        // quietly in the middle of a stream.
        const irc::Line l = irc::parseLine("PING :tmi.twitch.tv");
        ok(l.kind == irc::Line::Kind::Ping, "a ping is recognised");
        ok(l.token == "tmi.twitch.tv", "with the token to send back");
    }

    section("several lines in one frame, and half of one");
    {
        std::string carry;
        const auto lines = irc::parseFrame(
            ":a!a@a PRIVMSG #s :1\r\n"
            ":b!b@b PRIVMSG #s :2\r\n"
            "PING :tmi.twitch.tv\r\n", carry);
        ok(lines.size() == 3, "three lines in one frame are three lines");
        ok(lines[0].who == "a" && lines[1].who == "b", "in order");
        ok(lines[2].kind == irc::Line::Kind::Ping, "including the ping");
        ok(carry.empty(), "and nothing is left over");
    }
    {
        // THE ONE THAT BITES MONTHS LATER. A busy channel splits a line across
        // frames; dropping the tail loses one vote in a thousand, silently.
        std::string carry;
        auto first = irc::parseFrame(":a!a@a PRIVMSG #s :hello\r\n:b!b@b PRIV", carry);
        ok(first.size() == 1, "the whole line in the first frame is delivered");
        ok(!carry.empty(), "and the half line is kept");
        const auto second = irc::parseFrame("MSG #s :war\r\n", carry);
        ok(second.size() == 1 && second[0].who == "b" && second[0].text == "war",
           "then completed by the next frame");
        ok(carry.empty(), "with nothing left over");
    }
    {
        // A stream of bytes that never contains a newline must not grow for
        // ever.
        std::string carry;
        irc::parseFrame(std::string(9000, 'x'), carry);
        ok(carry.empty(), "an absurd partial line is dropped rather than hoarded");
    }

    section("joining anonymously, with no account at all");
    {
        const auto lines = irc::anonymousHandshake("Streamer", 42);
        ok(lines.size() == 3, "three lines open a session");
        ok(lines[0].rfind("PASS", 0) == 0, "a pass the server ignores");
        ok(lines[1].rfind("NICK justinfan", 0) == 0,
           "the anonymous login, so no token is needed anywhere");
        ok(lines[2] == "JOIN #streamer", "and the channel, lowercased");
        ok(irc::anonymousHandshake("", 1).empty(), "no channel, no handshake");
    }

    section("channel names");
    {
        ok(irc::normaliseChannel("#Streamer") == "streamer", "a hash is optional");
        ok(irc::normaliseChannel("  Streamer ") == "streamer", "space is ignored");
        ok(irc::normaliseChannel("a_1") == "a_1", "underscores and digits are fine");
        // Anything else is somebody typing the wrong thing into the box, and a
        // JOIN built from it would be a malformed line on the wire.
        ok(irc::normaliseChannel("has space").empty(), "a space inside is not a name");
        ok(irc::normaliseChannel("bad/name").empty(), "nor a slash");
        ok(irc::normaliseChannel(std::string(30, 'a')).empty(), "nor 30 characters");
        ok(irc::normaliseChannel("").empty(), "nor nothing");
    }

    section("what is not a message");
    {
        ok(irc::parseLine(":tmi.twitch.tv 001 justinfan1 :Welcome").kind
               == irc::Line::Kind::Welcome, "the welcome is recognised");
        ok(irc::parseLine(":tmi.twitch.tv NOTICE * :Login authentication failed").kind
               == irc::Line::Kind::Failed, "and a login failure, so the reader can stop");
        ok(irc::parseLine(":a!a@a JOIN #s").kind == irc::Line::Kind::Other,
           "a join is not a message");
        ok(irc::parseLine("").kind == irc::Line::Kind::Other, "nor is nothing");
        ok(irc::parseLine("garbage").kind == irc::Line::Kind::Other, "nor rubbish");
        ok(irc::parseLine(":a!a@a PRIVMSG #s").kind == irc::Line::Kind::Other,
           "nor a PRIVMSG with no text");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
