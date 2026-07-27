#include "ModUpdates.h"
#include "ModManager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::mutex g_infoMutex;

// Everything below treats the response as hostile input, because it is: it
// comes from a server a mod author controls.

bool looksLikeHttpsUrl(const std::string& u) {
    if (u.rfind("https://", 0) != 0) return false;
    if (u.size() > 512) return false;
    // Anything that could end a shell word or start a new command. curl is
    // invoked without a shell below, so this is belt and braces -- but a URL
    // containing these is malformed regardless and not worth fetching.
    for (unsigned char c : u)
        if (c < 0x21 || c > 0x7E || strchr("\"'`\\<>|;&$(){}[]^", c)) return false;
    return true;
}

// Runs curl WITHOUT a shell, so nothing in the URL can be interpreted as a
// command however odd it is. Returns false on any failure.
bool fetch(const std::string& url, std::string& out) {
    out.clear();
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    // popen semantics differ on Windows and there is no subprocess at all under
    // Emscripten. Reporting "unsupported" beats a half-working check.
    (void)url;
    return false;
#else
    int fds[2];
    if (pipe(fds) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        // --max-time bounds a server that accepts and then stalls; --max-filesize
        // bounds one that streams forever. -f makes an HTTP error a failure
        // rather than a body we would then try to parse.
        execlp("curl", "curl", "-fsSL",
               "--max-time", "6",
               "--max-filesize", "65536",
               "--proto", "=https",          // no redirect to http, file, etc.
               "--proto-redir", "=https",    // and none after a redirect either
               "-A", "OpenDoctrines-update-check",
               url.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) {
        out.append(buf, (size_t)n);
        if (out.size() > 65536) break;       // matches --max-filesize
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && !out.empty();
#endif
}

long semverPart(const std::string& v, int idx) {
    std::string core = v.substr(0, v.find_first_of("-+"));
    size_t start = 0;
    for (int i = 0; i < idx; i++) {
        size_t d = core.find('.', start);
        if (d == std::string::npos) return 0;
        start = d + 1;
    }
    return strtol(core.c_str() + start, nullptr, 10);
}

}  // namespace

ModUpdates& ModUpdates::get() {
    static ModUpdates u;
    return u;
}

bool ModUpdates::isNewer(const std::string& have, const std::string& latest) {
    if (latest.empty() || have.empty()) return false;
    for (int i = 0; i < 3; i++) {
        long a = semverPart(have, i), b = semverPart(latest, i);
        if (a != b) return b > a;
    }
    return false;   // equal is not newer
}

bool ModUpdates::parseResponse(const std::string& body, std::string& version,
                               std::string& page) {
    version.clear();
    page.clear();
    if (body.size() > 65536) return false;

    // A deliberately small hand parser rather than the JSON library: this input
    // is untrusted and the shape wanted is two short strings. Anything more
    // elaborate is more attack surface for no benefit.
    auto grab = [&](const char* key, std::string& dst, size_t maxLen) {
        std::string needle = std::string("\"") + key + "\"";
        size_t k = body.find(needle);
        if (k == std::string::npos) return;
        size_t colon = body.find(':', k + needle.size());
        if (colon == std::string::npos) return;
        size_t q1 = body.find('"', colon);
        if (q1 == std::string::npos) return;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 - q1 - 1 > maxLen) return;
        dst = body.substr(q1 + 1, q2 - q1 - 1);
    };
    grab("version", version, 32);
    grab("url", page, 512);

    // The version must actually look like one, or a page that happens to
    // contain the word would be read as an update.
    if (version.empty()) return false;
    bool digit = false;
    for (char c : version) {
        if (c >= '0' && c <= '9') digit = true;
        else if (c != '.' && c != '-' && c != '+' &&
                 !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    }
    if (!digit) return false;
    if (!page.empty() && !looksLikeHttpsUrl(page)) page.clear();
    return true;
}

void ModUpdates::checkAll(const ModManager& mm, bool enabled) {
    // The gate. Nothing above this line has run a request; nothing below it
    // runs one either unless the player asked for this.
    if (!enabled) return;
    if (m_pending.load() > 0) return;          // a round is already in flight

    for (const auto& e : mm.mods()) {
        if (!e.manifestValid) continue;
        const std::string url = e.manifest.updateUrl;
        if (url.empty() || !looksLikeHttpsUrl(url)) continue;

        const std::string id = e.id;
        const std::string have = e.manifest.version;
        m_pending.fetch_add(1);
        std::thread([this, id, have, url]() {
            Info info;
            std::string body;
            if (!fetch(url, body)) {
                info.error = "could not reach the author's update page";
            } else {
                std::string version, page;
                if (!parseResponse(body, version, page)) {
                    info.error = "the author's update page did not report a version";
                } else {
                    info.latest = version;
                    info.page = page.empty() ? url : page;
                    info.newer = isNewer(have, version);
                }
            }
            {
                std::lock_guard<std::mutex> lock(g_infoMutex);
                m_info[id] = std::move(info);
            }
            m_pending.fetch_sub(1);
        }).detach();
    }
}

const ModUpdates::Info* ModUpdates::infoFor(const std::string& modId) const {
    std::lock_guard<std::mutex> lock(g_infoMutex);
    auto it = m_info.find(modId);
    return it == m_info.end() ? nullptr : &it->second;
}

void ModUpdates::clear() {
    std::lock_guard<std::mutex> lock(g_infoMutex);
    m_info.clear();
}
