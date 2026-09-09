#include "DiscordRpc.h"

#include <cstdio>
#include <cerrno>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// No socket in a browser tab, and nothing to connect to either: Discord is a
// desktop application. The whole class becomes a set of no-ops rather than a
// compile error, so callers need no #ifdef of their own.
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#define OD_DISCORD_IPC 1
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace discordrpc {
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

// Opcodes, from Discord's own rpc_connection.cpp.
constexpr uint32_t kOpHandshake = 0;
constexpr uint32_t kOpFrame     = 1;
constexpr uint32_t kOpClose     = 2;

}  // namespace

std::string handshakePayload(const std::string& appId) {
    return "{\"v\":1,\"client_id\":\"" + jsonEscape(appId) + "\"}";
}

std::string activityPayload(const presence::Activity& a, long long pid,
                            long long startedAt, const std::string& nonce) {
    std::string j = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + jsonEscape(nonce) + "\",";
    j += "\"args\":{\"pid\":" + std::to_string(pid) + ",\"activity\":{";
    j += "\"details\":\"" + jsonEscape(a.details) + "\"";
    // An empty field is OMITTED rather than sent empty: Discord renders an
    // empty string as a blank line under the name, which looks like a bug in
    // the game to everybody who sees it.
    if (!a.state.empty()) j += ",\"state\":\"" + jsonEscape(a.state) + "\"";
    // "Since" makes Discord show elapsed time, which is the thing people
    // actually read off a presence.
    if (startedAt > 0)
        j += ",\"timestamps\":{\"start\":" + std::to_string(startedAt) + "}";
    // Assets are omitted entirely when there is no key: an "assets" object with
    // an empty large_image makes Discord show its own placeholder rather than
    // nothing, which looks like the game asked for the wrong picture.
    if (!a.largeImage.empty()) {
        j += ",\"assets\":{\"large_image\":\"" + jsonEscape(a.largeImage) + "\"";
        if (!a.largeText.empty())
            j += ",\"large_text\":\"" + jsonEscape(a.largeText) + "\"";
        j += "}";
    }
    j += "}}}";
    return j;
}

struct Rpc::Impl {
    std::string appId;
    int fd = -1;
    std::string lastSent;
    double nextSendAt = 0.0;
    long long startedAt = 0;
    std::string status;
    double retryAt = 0.0;

#ifdef OD_DISCORD_IPC
    /**
     * Where Discord listens.
     *
     * discord-ipc-0 through discord-ipc-9, under whichever runtime directory
     * this desktop uses -- and under Flatpak/Snap subdirectories, because a
     * Discord installed that way puts its socket one level down and a client
     * that only looks in the obvious place silently never connects.
     */
    static std::vector<std::string> candidatePaths() {
        std::vector<std::string> roots;
        for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
            if (const char* v = std::getenv(var)) {
                std::string r = v;
                while (!r.empty() && r.back() == '/') r.pop_back();
                if (!r.empty()) roots.push_back(r);
            }
        }
        roots.push_back("/tmp");

        std::vector<std::string> out;
        for (const std::string& root : roots) {
            for (const char* sub : {"", "/app/com.discordapp.Discord",
                                    "/snap.discord", "/app/com.discordapp.DiscordCanary"}) {
                for (int i = 0; i < 10; ++i) {
                    out.push_back(root + sub + "/discord-ipc-" + std::to_string(i));
                }
            }
        }
        return out;
    }

    bool writeFrame(uint32_t op, const std::string& payload) {
        if (fd < 0) return false;
        // ── ONE WRITE ──
        //
        // Discord's own documentation is explicit that the header and body must
        // arrive as a single write; separate writes break the connection. So
        // the frame is assembled first and sent once.
        std::vector<char> frame(8 + payload.size());
        const uint32_t len = (uint32_t)payload.size();
        std::memcpy(frame.data(), &op, 4);          // little-endian, and every
        std::memcpy(frame.data() + 4, &len, 4);     // platform this builds for is
        std::memcpy(frame.data() + 8, payload.data(), payload.size());

        size_t sent = 0;
        while (sent < frame.size()) {
            const ssize_t n = ::send(fd, frame.data() + sent, frame.size() - sent,
#ifdef MSG_NOSIGNAL
                                     MSG_NOSIGNAL);
#else
                                     0);
#endif
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    void disconnect() {
        if (fd >= 0) { ::close(fd); fd = -1; }
        lastSent.clear();
        inbox.clear();
    }

    std::string inbox;   // partial frames, between reads

    /**
     * Read whatever Discord has said, and notice when it is a refusal.
     *
     * The first version never read at all. It could therefore not tell a
     * WORKING connection from one Discord had already rejected -- a wrong
     * application id looked exactly like success, for ever, with the presence
     * silently absent. Discord answers a bad client_id with an ERROR dispatch
     * and then closes, so the difference is one read away.
     *
     * Non-blocking: a game must never wait on this.
     */
    void readReplies() {
        if (fd < 0) return;
        char buf[2048];
        for (;;) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) { inbox.append(buf, (size_t)n); continue; }
            if (n == 0) {                       // Discord hung up
                status = "Discord closed the connection.";
                disconnect();
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            disconnect();
            return;
        }

        while (inbox.size() >= 8) {
            uint32_t op = 0, len = 0;
            std::memcpy(&op, inbox.data(), 4);
            std::memcpy(&len, inbox.data() + 4, 4);
            if (len > (1u << 20)) { disconnect(); return; }   // not a frame
            if (inbox.size() < 8 + len) break;
            const std::string body = inbox.substr(8, len);
            inbox.erase(0, 8 + len);

            if (op == kOpClose) {
                // Carries Discord's own words, which are the only useful thing
                // to show: "Invalid Client ID" is a fixable mistake and a
                // generic failure is not.
                status = describeError(body);
                disconnect();
                return;
            }
            const std::string evt = fieldOf(body, "evt");
            if (evt == "ERROR") {
                status = describeError(body);
                continue;
            }
            if (evt == "READY") {
                ready = true;
                status.clear();
            }
        }
    }

    /**
     * The value of a top-level string field, whitespace and all.
     *
     * Written because matching the literal "\"evt\":\"READY\"" depends on
     * Discord emitting compact JSON. It does -- which is why that worked
     * against the real client and failed against a stand-in whose json.dumps
     * put a space after the colon. A matcher that only works because of
     * somebody else's formatting is a matcher waiting to break.
     */
    static std::string fieldOf(const std::string& body, const std::string& key) {
        const std::string needle = "\"" + key + "\"";
        size_t at = body.find(needle);
        if (at == std::string::npos) return {};
        at += needle.size();
        while (at < body.size() && (body[at] == ' ' || body[at] == '\t')) ++at;
        if (at >= body.size() || body[at] != ':') return {};
        ++at;
        while (at < body.size() && (body[at] == ' ' || body[at] == '\t')) ++at;
        if (at >= body.size() || body[at] != '"') return {};
        ++at;
        const size_t end = body.find('"', at);
        if (end == std::string::npos) return {};
        return body.substr(at, end - at);
    }

    static std::string describeError(const std::string& body) {
        // The message field, without a JSON parser on a socket -- the same
        // reasoning as src/net/HttpClient.h.
        const std::string msg = fieldOf(body, "message");
        return msg.empty() ? "Discord refused the connection." : msg;
    }

    bool ready = false;

    bool connect() {
        for (const std::string& path : candidatePaths()) {
            if (path.size() + 1 > sizeof(sockaddr_un::sun_path)) continue;
            const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (s < 0) continue;
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
            if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                // Non-blocking AFTER connecting: a game must never wait on this,
                // and Discord may stop reading at any moment.
                const int flags = ::fcntl(s, F_GETFL, 0);
                if (flags >= 0) ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
                fd = s;
                if (writeFrame(kOpHandshake, handshakePayload(appId))) return true;
                disconnect();
                return false;
            }
            ::close(s);
        }
        return false;
    }
#else
    bool writeFrame(uint32_t, const std::string&) { return false; }
    void disconnect() {}
    bool connect() { return false; }
#endif
};

Rpc::Rpc() : m_impl(std::make_unique<Impl>()) {}
Rpc::~Rpc() { if (m_impl) m_impl->disconnect(); }

void Rpc::configure(const std::string& appId) {
    if (m_impl->appId == appId) return;
    m_impl->disconnect();
    m_impl->appId = appId;
    m_impl->retryAt = 0.0;
    m_impl->status.clear();
}

bool Rpc::connected() const { return m_impl->fd >= 0; }
bool Rpc::ready() const { return m_impl->ready; }
std::string Rpc::status() const { return m_impl->status; }

void Rpc::update(const presence::Activity& activity, double now) {
    if (m_impl->appId.empty()) return;

    m_impl->readReplies();
    if (m_impl->fd < 0) {
        // Discord not running is the ordinary case, not a failure. Retried
        // slowly and silently: somebody who opens Discord halfway through a
        // session should get their presence, and somebody who never does should
        // never see a word about it.
        if (now < m_impl->retryAt) return;
        m_impl->retryAt = now + 30.0;
        if (!m_impl->connect()) return;
        m_impl->ready = false;
        m_impl->startedAt = (long long)::time(nullptr);
    }

    // ── NOTHING BEFORE READY ──
    //
    // connect() only SENDS the handshake; Discord answers a moment later. The
    // first version fell straight through and wrote SET_ACTIVITY in the same
    // call -- before the handshake had been answered. Discord discards an
    // activity sent then, and the dedup below meant it was never sent again:
    // the connection was healthy, `ready` went true, and the presence was
    // simply never there.
    //
    // Found against the real client, not the stand-in -- the stand-in never
    // replied READY, so this path did not exist in the test.
    if (!m_impl->ready) return;

    const std::string payload =
        activityPayload(activity, (long long)::getpid(), m_impl->startedAt, "od-presence");
    if (payload == m_impl->lastSent) return;
    // Discord drops updates sent too fast rather than erroring, which would
    // leave a stale line on screen. Held to a gap, then the NEWEST state is
    // sent -- not whichever one happened to be next in a queue.
    if (now < m_impl->nextSendAt) return;

    if (!m_impl->writeFrame(kOpFrame, payload)) {
        m_impl->disconnect();
        m_impl->retryAt = now + 30.0;
        return;
    }
    m_impl->lastSent = payload;
    // ── FOUR SECONDS, NOT FIFTEEN ──
    //
    // Discord's limit on SET_ACTIVITY is five updates per twenty seconds, so
    // four is the smallest safe gap. Fifteen was picked as "obviously safe" and
    // was measured swallowing real state changes: a run that went menu -> world
    // -> map editor published only the menu, because every later change fell
    // inside the window. A presence that lags fifteen seconds behind the player
    // is describing a game they have already left.
    m_impl->nextSendAt = now + 4.0;
}

void Rpc::clear() {
    if (m_impl->fd >= 0) {
        m_impl->writeFrame(kOpClose, "{}");
        m_impl->disconnect();
    }
}

}  // namespace discordrpc
