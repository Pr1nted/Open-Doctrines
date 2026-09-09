#include "IrcParse.h"

#include <cctype>

namespace irc {
namespace {

/// Everything after the first " :" is the message, colons and all.
std::string trailingOf(const std::string& line, size_t from) {
    const size_t at = line.find(" :", from);
    if (at == std::string::npos) return {};
    return line.substr(at + 2);
}

}  // namespace

std::string normaliseChannel(const std::string& raw) {
    // Trimmed at the ENDS only. Stripping space everywhere turned "has space"
    // into "hasspace" -- a valid-looking name nobody typed, which would join
    // the wrong channel and read somebody else's chat.
    size_t a = raw.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = raw.find_last_not_of(" \t\r\n");
    std::string c;
    for (size_t i = a; i <= b; ++i) {
        const char ch = raw[i];
        if (ch == '#' && c.empty()) continue;      // "#name" and "name" are the same
        c += (char)std::tolower((unsigned char)ch);
    }
    // Twitch logins are letters, digits and underscore, up to 25.
    if (c.empty() || c.size() > 25) return {};
    for (unsigned char ch : c)
        if (!std::isalnum(ch) && ch != '_') return {};
    return c;
}

std::vector<std::string> anonymousHandshake(const std::string& channel, int nonce) {
    const std::string c = normaliseChannel(channel);
    if (c.empty()) return {};
    // justinfan<digits> is Twitch's anonymous login. The password is ignored
    // for it but the server expects the line, so it is sent and is not a secret.
    const int n = (nonce % 90000) + 10000;
    return {"PASS SCHMOOPIIE",
            "NICK justinfan" + std::to_string(n),
            "JOIN #" + c};
}

Line parseLine(const std::string& raw) {
    Line out;
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) return out;

    // ── PING FIRST, AND IT IS NOT OPTIONAL ──
    //
    // Twitch pings every few minutes and closes the connection when nothing
    // answers. A reader that ignores this works for the length of a test and
    // dies quietly in the middle of a stream.
    if (line.rfind("PING", 0) == 0) {
        out.kind = Line::Kind::Ping;
        const size_t at = line.find(':');
        out.token = (at == std::string::npos) ? "tmi.twitch.tv" : line.substr(at + 1);
        return out;
    }

    // Tags, when the server sends them: "@badge=1;color=#fff :nick!..." -- not
    // asked for here, but a channel may still deliver them and the prefix is
    // what matters, so they are skipped rather than parsed.
    size_t at = 0;
    if (line[0] == '@') {
        at = line.find(' ');
        if (at == std::string::npos) return out;
        ++at;
    }
    if (at >= line.size() || line[at] != ':') return out;

    // ":nick!user@host COMMAND #channel :text"
    const size_t bang = line.find('!', at);
    const size_t space = line.find(' ', at);
    if (space == std::string::npos) return out;

    const std::string command = [&] {
        const size_t next = line.find(' ', space + 1);
        return line.substr(space + 1, (next == std::string::npos ? line.size() : next)
                                          - space - 1);
    }();

    if (command == "PRIVMSG") {
        if (bang == std::string::npos || bang > space) return out;
        out.who = line.substr(at + 1, bang - at - 1);
        for (auto& c : out.who) c = (char)std::tolower((unsigned char)c);
        // The message is everything after " :", so a viewer typing a colon --
        // or an emoticon, or a URL -- keeps it.
        // A PRIVMSG with no " :" carries no words. Not a message, so it never
        // reaches the tally as an empty vote.
        if (line.find(" :", space) == std::string::npos) return out;
        out.text = trailingOf(line, space);
        if (out.who.empty()) return out;
        out.kind = Line::Kind::Message;
        return out;
    }
    // 001 is "welcome", which is how we know the connection is usable.
    if (command == "001") { out.kind = Line::Kind::Welcome; return out; }
    // NOTICE with a login failure: anonymous auth should never fail, but if it
    // does the reader must stop rather than sit there silently reading nothing.
    if (command == "NOTICE") {
        const std::string text = trailingOf(line, space);
        if (text.find("Login authentication failed") != std::string::npos ||
            text.find("Improperly formatted auth") != std::string::npos) {
            out.kind = Line::Kind::Failed;
            out.text = text;
        }
        return out;
    }
    return out;
}

std::vector<Line> parseFrame(const std::string& frame, std::string& carry) {
    std::vector<Line> out;
    std::string buf = carry + frame;
    carry.clear();

    size_t start = 0;
    while (true) {
        const size_t nl = buf.find('\n', start);
        if (nl == std::string::npos) {
            // ── THE HALF LINE IS KEPT ──
            //
            // A frame may end mid-message. Dropping the tail works for months
            // and then a busy channel splits one and a vote goes missing for
            // no reason anybody can reproduce.
            carry = buf.substr(start);
            // Unless it is absurd, in which case it is not a line and keeping
            // it would grow without bound.
            if (carry.size() > 8192) carry.clear();
            break;
        }
        const Line line = parseLine(buf.substr(start, nl - start));
        if (line.kind != Line::Kind::Other) out.push_back(line);
        start = nl + 1;
    }
    return out;
}

}  // namespace irc
