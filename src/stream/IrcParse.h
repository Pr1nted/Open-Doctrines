#pragma once

// Reading Twitch chat, which is IRC wearing a WebSocket.
//
// ── WHY THIS IS A FILE OF ITS OWN ──
//
// Connecting is five lines and the game already has a WebSocket. The part with
// edge cases is what arrives: several IRC lines in one frame, a PING that must
// be answered or the server hangs up, display names with colons in the message
// after them, and tags in front of the prefix. Every one of those is pure
// string work over text from a stranger, so it is separated from the socket and
// tested exhaustively -- see tests/irc_parse_test.cpp.
//
// ── ANONYMOUS, DELIBERATELY ──
//
// Twitch lets anyone read a channel with no token at all: connect, say you are
// `justinfan<digits>`, join. So chat-plays-the-country needs NO account, no
// OAuth, and no credential stored anywhere -- which also means it cannot post,
// cannot moderate, and cannot act as the streamer. Read-only by construction is
// a much better place to be than read-only by promise.

#include <string>
#include <vector>

namespace irc {

/** One thing that arrived on the wire. */
struct Line {
    enum class Kind { Other = 0, Message, Ping, Welcome, Failed };
    Kind        kind = Kind::Other;
    std::string who;      ///< the sender's login, lowercase
    std::string text;     ///< what they said
    std::string token;    ///< PING's token, to send back
};

/**
 * Split a frame into lines and parse each.
 *
 * A frame may carry several lines, or half of one -- the trailing partial is
 * returned in `carry` and must be prepended to the next frame. Losing that is
 * the classic version of this bug: it works for months and then a busy channel
 * splits a message and one vote in a thousand goes missing.
 */
std::vector<Line> parseFrame(const std::string& frame, std::string& carry);

/** Parse exactly one line. Exposed for the tests and for parseFrame. */
Line parseLine(const std::string& line);

/// The lines that open an anonymous, read-only session on `channel`.
std::vector<std::string> anonymousHandshake(const std::string& channel, int nonce);

/// Channel names are lowercase and have no leading '#'. Empty when unusable.
std::string normaliseChannel(const std::string& raw);

}  // namespace irc
