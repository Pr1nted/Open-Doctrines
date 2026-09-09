#include "DiscordRpc.h"

#include <cstdio>
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
    }

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
std::string Rpc::status() const { return m_impl->status; }

void Rpc::update(const presence::Activity& activity, double now) {
    if (m_impl->appId.empty()) return;

    if (m_impl->fd < 0) {
        // Discord not running is the ordinary case, not a failure. Retried
        // slowly and silently: somebody who opens Discord halfway through a
        // session should get their presence, and somebody who never does should
        // never see a word about it.
        if (now < m_impl->retryAt) return;
        m_impl->retryAt = now + 30.0;
        if (!m_impl->connect()) return;
        m_impl->startedAt = (long long)::time(nullptr);
    }

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
