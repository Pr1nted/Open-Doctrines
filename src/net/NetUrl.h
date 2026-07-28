#pragma once

// The one URL parser, shared by the WebSocket transport and the HTTPS client.
//
// Deliberately strict, and deliberately NOT a general URL parser: it accepts
// exactly the shapes this game connects to and refuses everything else rather
// than making a guess. A URL a parser is wrong about is a connection to
// somewhere unintended, which is the whole of the risk here.
//
// One copy because two would be two sets of injection checks, and the second
// one is always the one nobody reviewed.

#include <cstdint>
#include <string>

struct NetUrl {
    bool        secure = true;   // wss / https
    std::string host;
    uint16_t    port = 443;
    std::string path = "/";      // includes any query string

    // Accepts ws://, wss://, http:// and https://. `secure` records which,
    // and the caller decides whether plaintext is acceptable -- that is a
    // policy question, not a parsing one.
    static bool parse(const std::string& url, NetUrl& out);
};
