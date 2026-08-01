#include "HttpClient.h"

#include "NetUrl.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

// ------------------------------------------------------------------ json ----
//
// These run against replies from the network, so each one treats its input as
// hostile: bounded scans, no recursion, and a default return for anything that
// is not exactly the shape expected.

namespace {

// Finds the value that follows `"key":`, and returns the offset of its first
// character.
//
// It must KEEP LOOKING past a match that is not a key. `{"kind":"linked",...}`
// contains the six characters "linked" in quotes as a VALUE, and stopping at
// the first textual match there made a lookup for the `linked` key land on the
// wrong thing entirely -- which is exactly how account linking silently failed
// to update. A quoted string is only a key if a colon follows it.
//
// Still not a parser: it does not track nesting, so it finds a key at any
// depth. Every reply it reads is ours and shallow, and `scopeTo` below narrows
// the search when that is not enough.
size_t findValue(const std::string& json, const std::string& key, size_t from = 0) {
    const std::string needle = "\"" + key + "\"";
    size_t at = json.find(needle, from);
    while (at != std::string::npos) {
        size_t after = at + needle.size();
        while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) after++;
        if (after < json.size() && json[after] == ':') {
            after++;
            while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) after++;
            return after < json.size() ? after : std::string::npos;
        }
        at = json.find(needle, at + 1);   // that was a value, not a key
    }
    return std::string::npos;
}

}  // namespace

size_t httpJsonScope(const std::string& json, const std::string& key) {
    const size_t at = findValue(json, key);
    return at == std::string::npos ? 0 : at;
}

std::string httpJsonString(const std::string& json, const std::string& key,
                           uint32_t maxLen, size_t from) {
    size_t at = findValue(json, key, from);
    if (at == std::string::npos || json[at] != '"') return {};
    at++;

    std::string out;
    while (at < json.size() && out.size() <= maxLen) {
        const char c = json[at];
        if (c == '"') return out;
        if (c != '\\') { out += c; at++; continue; }

        if (at + 1 >= json.size()) return {};
        const char esc = json[at + 1];
        at += 2;
        switch (esc) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '"': out += '"';  break;
            case '\\': out += '\\'; break;
            case '/': out += '/';  break;
            case 'u': {
                if (at + 4 > json.size()) return {};
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    const char h = json[at + i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return {};
                }
                at += 4;
                // Surrogates are dropped rather than paired. Nothing the
                // account API returns needs them -- nicknames are ASCII by
                // policy -- and half-decoding one would produce invalid UTF-8
                // that the font renderer would then have to cope with.
                if (cp >= 0xD800 && cp <= 0xDFFF) break;
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: return {};
        }
    }
    return {};   // unterminated, or past the ceiling
}

long long httpJsonNumber(const std::string& json, const std::string& key,
                         long long fallback, size_t from) {
    const size_t at = findValue(json, key, from);
    if (at == std::string::npos) return fallback;
    if (json[at] != '-' && !std::isdigit(static_cast<unsigned char>(json[at]))) return fallback;
    return std::strtoll(json.c_str() + at, nullptr, 10);
}

bool httpJsonBool(const std::string& json, const std::string& key, bool fallback,
                  size_t from) {
    const size_t at = findValue(json, key, from);
    if (at == std::string::npos) return fallback;
    if (json.compare(at, 4, "true") == 0) return true;
    if (json.compare(at, 5, "false") == 0) return false;
    return fallback;
}

std::vector<std::string> httpJsonStringArray(const std::string& json,
                                             const std::string& key,
                                             size_t maxItems, size_t from) {
    std::vector<std::string> out;
    const size_t at = findValue(json, key, from);
    if (at == std::string::npos || json[at] != '[') return out;
    const size_t end = json.find(']', at);
    if (end == std::string::npos) return out;

    for (size_t i = at + 1; i < end && out.size() < maxItems; ) {
        const size_t open = json.find('"', i);
        if (open == std::string::npos || open > end) break;
        size_t close = open + 1;
        while (close < end && !(json[close] == '"' && json[close - 1] != '\\')) close++;
        if (close >= end) break;
        out.push_back(json.substr(open + 1, close - open - 1));
        i = close + 1;
    }
    return out;
}

std::string httpJsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// ------------------------------------------------------------------ date ----

long long httpParseDate(const std::string& value) {
    // IMF-fixdate, the only form RFC 7231 requires a sender to produce:
    //     Sun, 06 Nov 1994 08:49:37 GMT
    // The obsolete RFC 850 and asctime forms are not handled; nothing we talk
    // to emits them, and guessing at a two-digit year is worse than reporting
    // no time at all.
    if (value.size() < 29) return 0;

    static const char* kMonths[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    auto digits = [&](size_t at, size_t n, long long& out) {
        out = 0;
        for (size_t i = 0; i < n; i++) {
            const char c = value[at + i];
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
        }
        return true;
    };

    // "Sun, " -- the weekday is redundant with the date, so it is skipped
    // rather than checked. A sender that disagrees with itself about which day
    // it is should not stop a player joining.
    const size_t at = value.find(", ");
    if (at == std::string::npos || at + 2 + 20 > value.size()) return 0;
    const size_t p = at + 2;

    long long day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (!digits(p, 2, day)) return 0;
    if (value[p + 2] != ' ') return 0;

    int month = -1;
    for (int i = 0; i < 12; i++)
        if (value.compare(p + 3, 3, kMonths[i]) == 0) { month = i + 1; break; }
    if (month < 0) return 0;

    if (value[p + 6] != ' ') return 0;
    if (!digits(p + 7, 4, year)) return 0;
    if (value[p + 11] != ' ') return 0;
    if (!digits(p + 12, 2, hour)) return 0;
    if (value[p + 14] != ':') return 0;
    if (!digits(p + 15, 2, minute)) return 0;
    if (value[p + 17] != ':') return 0;
    if (!digits(p + 18, 2, second)) return 0;

    if (day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return 0;
    if (year < 1970 || year > 9999) return 0;

    // Days from the civil date, by Howard Hinnant's algorithm. Used instead of
    // timegm(), which is not portable, or mktime(), which would apply the
    // player's local timezone to a value that is explicitly GMT -- an error
    // that would look correct in one timezone and be hours out in another.
    long long y = year;
    const unsigned m = static_cast<unsigned>(month);
    const unsigned d = static_cast<unsigned>(day);
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + static_cast<long long>(doe) - 719468;

    return days * 86400 + hour * 3600 + minute * 60 + second;
}

// --------------------------------------------------------------- request ----

// Whoever wants a frame while the network is waiting. See NetWaitHook.
static NetWaitHook g_waitHook = nullptr;
void netSetWaitHook(NetWaitHook fn) { g_waitHook = fn; }
void netRunWaitHook(double elapsedMs) { if (g_waitHook) g_waitHook(elapsedMs); }

// Shared by every backend. The rules these encode -- never send a credential in
// clear, never let one smuggle a header -- are properties of the request, not
// of the transport, so a second backend must not get its own version of them.
namespace {

HttpResponse failure(const std::string& text) {
    HttpResponse r;
    r.error = text;
    return r;
}

bool isLocalhost(const std::string& host) {
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

// Header values go verbatim into the request. A newline in one would let a
// caller inject a header of its own -- and one of these values is a bearer
// token that arrives from a file.
bool headerSafe(const std::string& value) {
    for (unsigned char c : value) if (c < 0x20 || c == 0x7F) return false;
    return true;
}

// The scheme/credential check both backends must pass before anything is sent.
// Returns an error sentence, or empty when the request may proceed.
std::string refuseUnsafeRequest(const HttpRequest& request, const NetUrl& url) {
    if (!url.secure) {
        if (!request.allowInsecure || !isLocalhost(url.host)) {
            // A bearer token on a plaintext connection is readable by anyone on
            // the path. The localhost carve-out exists for `wrangler dev` and is
            // deliberately not reachable for any other host.
            return "refusing to send account credentials over an "
                   "unencrypted connection";
        }
    }
    if (!headerSafe(request.bearer) || !headerSafe(request.adminSecret))
        return "that credential contains characters that cannot be sent";
    return "";
}

}  // namespace

#if !defined(__EMSCRIPTEN__) && defined(OD_ENABLE_NET)

#include "TlsSocket.h"

#include <chrono>

// (failure/isLocalhost/headerSafe moved above the backend split -- the web
//  backend enforces the same rules and there must not be two copies of them.)

HttpResponse httpRequest(const HttpRequest& request) {
    NetUrl url;
    if (!NetUrl::parse(request.url, url)) return failure("that is not a usable address");

    if (!url.secure) {
        if (!request.allowInsecure || !isLocalhost(url.host)) {
            // A bearer token on a plaintext connection is readable by anyone
            // on the path. The localhost carve-out exists for `wrangler dev`
            // and is deliberately not reachable for any other host.
            return failure("refusing to send account credentials over an "
                           "unencrypted connection");
        }
    }
    if (!headerSafe(request.bearer) || !headerSafe(request.adminSecret)) {
        return failure("that credential contains characters that cannot be sent");
    }

    TlsSocket sock;
    std::string err;
    if (!sock.open(url.host, url.port, url.secure, err)) return failure(err);

    const bool defaultPort = (url.secure && url.port == 443) ||
                             (!url.secure && url.port == 80);
    std::string head =
        request.method + " " + url.path + " HTTP/1.1\r\n"
        "Host: " + url.host + (defaultPort ? "" : ":" + std::to_string(url.port)) + "\r\n"
        "User-Agent: OpenDoctrines\r\n"
        "Accept: application/json\r\n"
        // No keep-alive: one request per connection means there is no state to
        // get wrong between them, and the account API is not a hot path.
        "Connection: close\r\n";

    if (!request.bearer.empty())      head += "Authorization: Bearer " + request.bearer + "\r\n";
    if (!request.adminSecret.empty()) head += "x-od-admin: " + request.adminSecret + "\r\n";
    if (!request.body.empty()) {
        head += "Content-Type: application/json\r\n";
        head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
    }
    head += "\r\n";

    if (!sock.writeAll(reinterpret_cast<const uint8_t*>(head.data()), head.size()) ||
        (!request.body.empty() &&
         !sock.writeAll(reinterpret_cast<const uint8_t*>(request.body.data()),
                        request.body.size()))) {
        return failure("the connection closed while sending the request");
    }

    std::string raw;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(request.timeoutMs);
    // Headers plus body, with a ceiling so a server that never stops talking
    // cannot make this grow without bound.
    const size_t cap = request.maxResponseBytes + 64 * 1024;

    for (;;) {
        if (std::chrono::steady_clock::now() > deadline) {
            return failure("the server did not reply in time");
        }
        uint8_t buf[4096];
        const int rc = sock.read(buf, sizeof(buf));
        if (rc == TlsSocket::kRetry) continue;
        if (rc == TlsSocket::kClosed) break;
        if (rc < 0) {
            // A truncated reply is not usable, but if the headers and a
            // complete body already arrived, the close is just the server
            // hanging up after "Connection: close" and that is fine.
            if (raw.find("\r\n\r\n") == std::string::npos) {
                return failure("the connection was lost");
            }
            break;
        }
        raw.append(reinterpret_cast<char*>(buf), static_cast<size_t>(rc));
        if (raw.size() > cap) return failure("the server sent an oversized reply");
    }

    const size_t headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string::npos) return failure("the server sent a malformed reply");

    HttpResponse response;
    if (raw.compare(0, 5, "HTTP/") != 0) return failure("the server sent a malformed reply");
    const size_t statusAt = raw.find(' ');
    if (statusAt == std::string::npos) return failure("the server sent a malformed reply");
    response.status = static_cast<int>(std::strtol(raw.c_str() + statusAt + 1, nullptr, 10));

    std::string body = raw.substr(headEnd + 4);

    // Chunked is the one transfer encoding worth handling: Workers use it for
    // anything it has not buffered, so it is not an exotic case.
    std::string lowerHead = raw.substr(0, headEnd);
    for (char& c : lowerHead) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // The server's clock, so a host with a wrong one can still admit players.
    if (const size_t dateAt = lowerHead.find("\r\ndate:"); dateAt != std::string::npos) {
        size_t from = dateAt + 7;
        while (from < headEnd && raw[from] == ' ') from++;
        const size_t eol = raw.find("\r\n", from);
        if (eol != std::string::npos && eol <= headEnd)
            response.serverTime = httpParseDate(raw.substr(from, eol - from));
    }
    if (lowerHead.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t at = 0;
        while (at < body.size()) {
            const size_t eol = body.find("\r\n", at);
            if (eol == std::string::npos) break;
            const long long size = std::strtoll(body.substr(at, eol - at).c_str(), nullptr, 16);
            if (size <= 0) break;                       // 0 terminates; <0 is malformed
            at = eol + 2;
            if (at + static_cast<size_t>(size) > body.size()) break;   // truncated
            decoded.append(body, at, static_cast<size_t>(size));
            if (decoded.size() > request.maxResponseBytes) {
                return failure("the server sent an oversized reply");
            }
            at += static_cast<size_t>(size) + 2;        // skip the trailing CRLF
        }
        body.swap(decoded);
    }

    if (body.size() > request.maxResponseBytes) return failure("the server sent an oversized reply");
    response.body = std::move(body);
    return response;
}

#elif defined(__EMSCRIPTEN__)

// The browser build reaches the account API through the Fetch API, which
// handles TLS, DNS and redirects for us. Two things differ from the desktop
// path and both change how failures read:
//
//   CORS is the BROWSER's rule, not ours. The account service has to send
//   Access-Control-Allow-Origin, and has to allow Authorization and
//   Content-Type on the preflight, or the request never leaves the tab. The
//   browser deliberately keeps the reason vague, so a refusal here looks
//   exactly like an unreachable server -- which is why the message below says
//   so instead of guessing.
//
//   httpRequest() is synchronous by signature and fetch is not. ASYNCIFY is
//   what bridges that: the wait loop unwinds to the browser and resumes when a
//   callback has run. It is also why this must never be called from a context
//   that cannot be unwound.

#include <emscripten/emscripten.h>   // emscripten_sleep, emscripten_get_now
#include <emscripten/fetch.h>

namespace {

struct FetchState {
    bool         done      = false;
    bool         abandoned = false;   // the caller gave up; the callback owns this now
    HttpResponse res;
};

// Both callbacks end here. `f` is closed last, and the state is freed only when
// the caller has already walked away -- otherwise the caller frees it.
void finishFetch(emscripten_fetch_t* f, FetchState* st) {
    st->done = true;
    const bool orphan = st->abandoned;
    emscripten_fetch_close(f);
    if (orphan) delete st;            // nobody is waiting for this any more
}

void onFetchSucceeded(emscripten_fetch_t* f) {
    auto* st = static_cast<FetchState*>(f->userData);
    st->res.status = f->status;
    if (f->data && f->numBytes > 0)
        st->res.body.assign(f->data, f->data + f->numBytes);

    // The issuer's clock, for the same reason the desktop backend reads it:
    // join tickets expire in two minutes and a wrong local clock rejects every
    // player with no symptom but "nobody can join". See NetIssuerClock.
    const size_t len = emscripten_fetch_get_response_headers_length(f);
    if (len > 0) {
        std::string headers(len + 1, '\0');
        emscripten_fetch_get_response_headers(f, headers.data(), len + 1);
        for (size_t i = 0; i < headers.size();) {
            size_t eol = headers.find('\n', i);
            if (eol == std::string::npos) eol = headers.size();
            std::string line = headers.substr(i, eol - i);
            if (line.size() > 5) {
                std::string name = line.substr(0, 5);
                for (auto& c : name) c = (char)tolower((unsigned char)c);
                if (name == "date:") {
                    size_t v = line.find(':');
                    std::string value = line.substr(v + 1);
                    while (!value.empty() && (value.front() == ' ')) value.erase(value.begin());
                    while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
                        value.pop_back();
                    st->res.serverTime = httpParseDate(value);
                }
            }
            i = eol + 1;
        }
    }
    finishFetch(f, st);
}

void onFetchFailed(emscripten_fetch_t* f) {
    auto* st = static_cast<FetchState*>(f->userData);
    st->res.status = f->status;
    // status 0 means the request never reached a server -- offline, DNS, or a
    // CORS refusal, which the browser will not distinguish for us.
    st->res.error = (f->status == 0)
        ? "could not reach the account service from the browser (it may be "
          "offline, or may not allow requests from this page)"
        : "the account service replied with an error";
    finishFetch(f, st);
}

}  // namespace

HttpResponse httpRequest(const HttpRequest& request) {
    NetUrl url;
    if (!NetUrl::parse(request.url, url)) return failure("that is not a usable address");
    const std::string refusal = refuseUnsafeRequest(request, url);
    if (!refusal.empty()) return failure(refusal);

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    snprintf(attr.requestMethod, sizeof(attr.requestMethod), "%s", request.method.c_str());
    attr.attributes   = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.timeoutMSecs = (unsigned long)request.timeoutMs;
    attr.onsuccess    = onFetchSucceeded;
    attr.onerror      = onFetchFailed;

    // Header array and body must outlive the call to emscripten_fetch(), which
    // returns before the request is sent. Both live until the wait below ends.
    std::vector<const char*> headers;
    headers.push_back("Accept");
    headers.push_back("application/json");
    if (!request.body.empty()) {
        headers.push_back("Content-Type");
        headers.push_back("application/json");
    }
    std::string bearerValue;
    if (!request.bearer.empty()) {
        bearerValue = "Bearer " + request.bearer;
        headers.push_back("Authorization");
        headers.push_back(bearerValue.c_str());
    }
    if (!request.adminSecret.empty()) {
        headers.push_back("x-od-admin");
        headers.push_back(request.adminSecret.c_str());
    }
    headers.push_back(nullptr);
    attr.requestHeaders = headers.data();

    if (!request.body.empty()) {
        attr.requestData     = request.body.data();
        attr.requestDataSize = request.body.size();
    }

    // Heap-allocated, because if the wait gives up the callback still owns a
    // pointer to it. Writing a reply into a stack frame that has returned is
    // the one way this could corrupt memory rather than merely fail.
    auto* st = new FetchState();
    attr.userData = st;

    emscripten_fetch(&attr, request.url.c_str());

    // fetch's own timeout should fire first; this is the backstop for the case
    // where it does not, and it deliberately does not free the state.
    const double startedMs = emscripten_get_now();
    const double deadline = startedMs + (double)request.timeoutMs + 2000.0;
    while (!st->done) {
        if (emscripten_get_now() > deadline) {
            st->abandoned = true;      // the callback will free it, whenever it runs
            return failure("the account service did not reply in time");
        }
        // A frame for whoever is watching, then back to the browser. Without
        // the hook this loop kept the tab alive and the canvas frozen.
        netRunWaitHook(emscripten_get_now() - startedMs);
        emscripten_sleep(10);          // unwinds to the browser, resumes here
    }

    HttpResponse res = std::move(st->res);
    delete st;

    if (res.error.empty() && res.body.size() > request.maxResponseBytes)
        return failure("the server sent an oversized reply");
    return res;
}

#else

HttpResponse httpRequest(const HttpRequest&) {
    HttpResponse r;
    r.error = "this build of OpenDoctrines was compiled without networking "
              "(-DOD_ENABLE_NET=OFF)";
    return r;
}

#endif
