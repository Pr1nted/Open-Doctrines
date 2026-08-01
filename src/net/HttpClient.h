#pragma once

// A minimal HTTPS client for the account API.
//
// WHY NOT curl, WHICH THE UPDATER ALREADY SHELLS OUT TO
//
// The updater spawns `curl` for one download and does not care that it costs a
// process. This is different in two ways that matter: it carries a bearer
// token, and it runs while the game is on screen. Putting a credential in a
// subprocess argv makes it visible in `ps` to every other user on the machine,
// and the round trip through a process per poll would be absurd during a login
// that polls every two seconds. mbedTLS is already linked for the WebSocket, so
// this reuses TlsSocket -- and therefore the one certificate-verification
// policy the game has.
//
// WHAT IT IS NOT
//
// Not a general HTTP client. No redirects (an account API that redirects is a
// misconfiguration, and following one would send a bearer token somewhere the
// caller did not name), no cookies, no connection reuse, no compression. One
// request, one connection, one response.
//
// BLOCKING. Every call here blocks the calling thread. AccountClient owns a
// thread for exactly that reason; nothing on the render thread may call this.

#include <cstdint>
#include <string>
#include <vector>

struct HttpResponse {
    int         status = 0;      // 0 when the request never completed
    std::string body;
    /** Empty on success. A sentence for a player, not a diagnostic code. */
    std::string error;

    /**
     * The server's own clock, from the `Date` header, in unix seconds. 0 when
     * absent or unparseable.
     *
     * Here because join tickets expire in two minutes, and a host whose clock
     * is wrong would otherwise reject every player alive with no symptom but
     * "nobody can join". Knowing the issuer's real time removes the cause
     * instead of widening the window. See NetIssuerClock in JoinTicket.h.
     *
     * NOT a trusted time source: it is unauthenticated HTTP metadata, and it is
     * only ever used to move a deadline that a signature already binds.
     */
    long long serverTime = 0;

    bool ok() const { return status >= 200 && status < 300; }
};

/**
 * Parse an HTTP `Date` header (RFC 7231 IMF-fixdate) into unix seconds.
 *
 * 0 when the value is not a date. Exposed for tests -- date parsing is the kind
 * of thing that is quietly wrong for one month of the year.
 */
long long httpParseDate(const std::string& value);

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::string body;              // sent as application/json when non-empty
    std::string bearer;            // Authorization: Bearer <this>, when set
    std::string adminSecret;       // x-od-admin, when set

    // Refused unless the caller opts in, and only http://localhost qualifies
    // even then. A bearer token on a plaintext connection is readable by
    // anyone on the path.
    bool allowInsecure = false;

    /** Ceiling on the response body. A reply larger than this is an error. */
    uint32_t maxResponseBytes = 1024 * 1024;

    int timeoutMs = 20000;
};

/** Performs one request. Never throws; failures arrive in `error`. */
HttpResponse httpRequest(const HttpRequest& request);

/**
 * Called over and over while a request is blocked waiting for a reply.
 *
 * WHY THIS EXISTS
 *
 * On desktop an account job runs on a worker thread and the game carries on
 * drawing. The web build has no threads, so the job runs inline on the frame
 * thread -- and the device sign-in flow polls for up to ten minutes, waiting
 * for a person to finish in another tab. For all that time nothing returned to
 * the frame loop, so the canvas never repainted and the game looked hung. It
 * was not hung, but "not hung" is not a defence when the screen is frozen.
 *
 * The hook lets the waiting code hand a frame back to whoever installed it.
 * Game installs one that paints a "signing in" screen and watches for Esc, so
 * the wait is something the player can see and get out of.
 *
 * It is called from inside the wait, so it MUST NOT start another request, and
 * it must be safe to run when no frame is open -- account jobs begin from
 * update..., before BeginDrawing.
 */
using NetWaitHook = void (*)(double elapsedMs);
void netSetWaitHook(NetWaitHook fn);

/**
 * Runs the installed hook, if any, telling it how long this wait has lasted.
 *
 * The elapsed time is what stops the hook being a nuisance: most requests come
 * back in well under a second, and covering the screen for those would flash a
 * waiting panel over the UI every couple of seconds during the sign-in poll.
 * The hook decides for itself when a wait is long enough to be worth showing.
 */
void netRunWaitHook(double elapsedMs);

// --------------------------------------------------------------- helpers ----
//
// Just enough JSON to read the account API's replies, which are small, flat and
// ours. Deliberately NOT a JSON parser: pulling one string out of a known reply
// does not justify the surface area of a real one on a network-facing path, and
// nlohmann is already in the game for files we produced ourselves.
//
// Every function returns a default when the key is absent or the shape is not
// what was expected, so a malformed reply reads as "no value" rather than
// throwing on a background thread.

std::string httpJsonString(const std::string& json, const std::string& key,
                           uint32_t maxLen = 4096, size_t from = 0);
long long   httpJsonNumber(const std::string& json, const std::string& key,
                           long long fallback = 0, size_t from = 0);
bool        httpJsonBool(const std::string& json, const std::string& key,
                         bool fallback = false, size_t from = 0);

/** Flat array of strings. Bounded, so a hostile reply cannot allocate freely. */
std::vector<std::string> httpJsonStringArray(const std::string& json,
                                             const std::string& key,
                                             size_t maxItems = 16, size_t from = 0);

/**
 * Offset of the value of `key`, for passing as `from` to the readers above.
 *
 * Use it to scope a lookup into a sub-object. `{"kind":"linked","account":{…}}`
 * has keys at two levels, and reading `linked` without scoping to `account`
 * finds the wrong one.
 */
size_t httpJsonScope(const std::string& json, const std::string& key);

/** Escapes a string for embedding in a JSON body. */
std::string httpJsonEscape(const std::string& text);
