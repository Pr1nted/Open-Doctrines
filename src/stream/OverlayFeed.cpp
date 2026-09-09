#include "OverlayFeed.h"

#include "StreamSafe.h"

#include <cctype>

namespace overlay {
namespace {

std::string jsonEscape(const std::string& v) {
    std::string out;
    for (char c : v) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c >= 0x20) out += c;
        }
    }
    return out;
}

}  // namespace

bool safeToShow(const std::string& value) {
    // A path with a home directory in it carries a real name. This is the same
    // check stream-safe mode uses, applied here unconditionally.
    if (streamsafe::looksSensitive(value)) return false;
    // An account id is 32+ hex characters and nothing else. Nothing the overlay
    // legitimately shows looks like that, so the shape is enough.
    if (value.size() >= 32) {
        bool allHex = true;
        for (unsigned char c : value)
            if (!std::isxdigit(c)) { allHex = false; break; }
        if (allHex) return false;
    }
    return true;
}

std::string toText(const Feed& feed) {
    std::string out;
    for (const Fact& f : feed.facts) {
        if (!safeToShow(f.value)) continue;
        out += f.key;
        out += ": ";
        out += f.value;
        out += "\n";
    }
    return out;
}

std::string toJson(const Feed& feed) {
    std::string out = "{";
    bool first = true;
    for (const Fact& f : feed.facts) {
        if (!safeToShow(f.value)) continue;
        if (!first) out += ",";
        first = false;
        out += "\"" + jsonEscape(f.key) + "\":\"" + jsonEscape(f.value) + "\"";
    }
    out += "}";
    return out;
}

}  // namespace overlay
