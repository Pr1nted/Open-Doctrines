#include "Tunnel.h"

#include <cctype>
#include <chrono>
#include <cstring>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  #include <csignal>
  #include <fcntl.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #define OD_TUNNEL_POSIX 1
#endif

namespace {

/** Ceiling on what a tunnel's chatter may accumulate. */
constexpr size_t kMaxLogBytes = 32 * 1024;

/**
 * The hostname a tunnel announces, pulled out of its output.
 *
 * Matched by SUFFIX rather than by a general URL match. A tunnel prints a lot
 * of things that look like URLs -- documentation links, terms of service, an
 * update notice -- and picking the first one would hand players an address that
 * is not the game. Only a hostname under a domain the provider actually issues
 * counts.
 */
std::string findHostWithSuffix(const std::string& text,
                               const std::vector<std::string>& suffixes) {
    for (const std::string& suffix : suffixes) {
        size_t at = 0;
        while ((at = text.find(suffix, at)) != std::string::npos) {
            // Walk back over the label characters to the start of the hostname.
            size_t start = at;
            while (start > 0) {
                const char c = text[start - 1];
                const bool label = std::isalnum(static_cast<unsigned char>(c)) ||
                                   c == '-' || c == '.';
                if (!label) break;
                start--;
            }
            const size_t end = at + suffix.size();
            std::string host = text.substr(start, end - start);

            // A bare suffix with nothing in front of it is the provider talking
            // about itself, not a tunnel it opened for us.
            if (host.size() > suffix.size() && host[0] != '.' && host[0] != '-')
                return host;
            at = end;
        }
    }
    return {};
}

}  // namespace

const char* tunnelProviderName(TunnelProvider p) {
    switch (p) {
        case TunnelProvider::Cloudflared:  return "Cloudflare Tunnel";
        case TunnelProvider::LocalhostRun: return "localhost.run (over ssh)";
        case TunnelProvider::None:         return "none";
    }
    return "none";
}

bool tunnelProviderWorksUnattended(TunnelProvider p) {
    // Verified by running it: localhost.run accepts the ssh session, prints its
    // welcome, and never announces a hostname without a registered key.
    return p == TunnelProvider::Cloudflared;
}

std::string tunnelProviderHowToGet(TunnelProvider p) {
    switch (p) {
        case TunnelProvider::Cloudflared:
            return "Install cloudflared -- \"brew install cloudflared\" on a Mac, or "
                   "from Cloudflare's downloads page. No account or signup is needed "
                   "for the kind of tunnel this uses.";
        case TunnelProvider::LocalhostRun:
            return "Needs ssh, which every machine has -- but an anonymous tunnel no "
                   "longer gets an address. It only works if you have registered an "
                   "SSH key with localhost.run.";
        case TunnelProvider::None:
            return "Without a tunnel you can still host by forwarding a port on your "
                   "router, or by playing on a local network.";
    }
    return {};
}

std::string tunnelParseAddress(TunnelProvider provider, const std::string& output) {
    std::string host;
    switch (provider) {
        case TunnelProvider::Cloudflared:
            host = findHostWithSuffix(output, {".trycloudflare.com"});
            break;
        case TunnelProvider::LocalhostRun:
            // ONLY .lhr.life. The provider's banner contains links to its own
            // site -- including https://admin.localhost.run/ -- so accepting
            // ".localhost.run" picked their dashboard instead of the tunnel.
            // The address then answered with a perfectly healthy 200 that was
            // not this game, which is a confusing way to fail.
            host = findHostWithSuffix(output, {".lhr.life"});
            break;
        case TunnelProvider::None:
            return {};
    }
    if (host.empty()) return {};

    // Handed back in the form a player pastes. Every one of these terminates
    // TLS at the provider and forwards plaintext to our loopback port, which is
    // exactly what WsServer.h describes and why the server speaks plain ws://.
    return "wss://" + host;
}

// ============================================================ the process ====

#ifdef OD_TUNNEL_POSIX

namespace {

/** Is `name` on PATH? Never an absolute path from anywhere else. */
bool onPath(const char* name) {
    const char* path = getenv("PATH");
    if (!path) return false;
    std::string p(path);
    size_t at = 0;
    while (at <= p.size()) {
        const size_t colon = p.find(':', at);
        const std::string dir = p.substr(at, colon == std::string::npos
                                              ? std::string::npos : colon - at);
        if (!dir.empty()) {
            const std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) return true;
        }
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    return false;
}

}  // namespace

struct Tunnel::Impl {
    pid_t pid = -1;
    int   fd = -1;      // read end of the tunnel's combined output
    std::chrono::steady_clock::time_point startedAt{};
};

namespace {
// A tunnel that has said nothing useful by now is not going to. Generous,
// because these connect over the internet and one of them negotiates ssh
// first -- but bounded, because the alternative is a host staring at
// "Opening a tunnel..." for the rest of the evening.
constexpr int kAnnounceTimeoutSeconds = 25;
}  // namespace

Tunnel::Tunnel() : m_impl(std::make_unique<Impl>()) {}
Tunnel::~Tunnel() { stop(); }

bool Tunnel::available() { return true; }

namespace {
/** Set when the game has installed its own copy; searched before PATH. */
std::string g_toolsDir;
}  // namespace

void tunnelSetToolsDir(const std::string& dir) { g_toolsDir = dir; }

/** Where cloudflared is, preferring the game's own copy. Empty if absent. */
std::string tunnelResolveProgram(const char* name) {
    if (!g_toolsDir.empty()) {
        const std::string own = g_toolsDir + "/" + name;
        if (access(own.c_str(), X_OK) == 0) return own;
    }
    return onPath(name) ? std::string(name) : std::string();
}

std::vector<TunnelProvider> tunnelProvidersAvailable() {
    std::vector<TunnelProvider> out;
    // cloudflared first: it needs no account, its hostnames are stable for the
    // life of the tunnel, and it does not depend on somebody's ssh config --
    // and, as of writing, anonymous localhost.run tunnels no longer issue an
    // address at all, so it is the only one that actually works unattended.
    if (!tunnelResolveProgram("cloudflared").empty())
        out.push_back(TunnelProvider::Cloudflared);
    if (onPath("ssh")) out.push_back(TunnelProvider::LocalhostRun);
    return out;
}

bool Tunnel::start(TunnelProvider provider, uint16_t localPort, std::string& error) {
    stop();
    m_provider = provider;
    m_address.clear();
    m_error.clear();
    m_log.clear();

    const std::string port = std::to_string(localPort);
    std::vector<std::string> argv;
    const char* program = nullptr;

    std::string resolved;
    switch (provider) {
        case TunnelProvider::Cloudflared:
            // The game's own copy when there is one, otherwise whatever is on
            // PATH. Either way it is a path this code chose, never one that
            // arrived from a network.
            resolved = tunnelResolveProgram("cloudflared");
            program = resolved.empty() ? "cloudflared" : resolved.c_str();
            argv = {program, "tunnel", "--url", "http://localhost:" + port};
            break;
        case TunnelProvider::LocalhostRun:
            program = "ssh";
            argv = {"ssh", "-R", "80:localhost:" + port,
                    // No shell, no key agent, no interactive prompt: this is a
                    // port forward and nothing else.
                    "-N",
                    "-o", "StrictHostKeyChecking=accept-new",
                    "-o", "ServerAliveInterval=30",
                    "nokey@localhost.run"};
            break;
        case TunnelProvider::None:
            error = "no tunnel provider chosen";
            m_state = State::Failed;
            return false;
    }

    if (provider == TunnelProvider::Cloudflared ? resolved.empty() : !onPath(program)) {
        error = std::string("cloudflared") + " is not installed. " +
                tunnelProviderHowToGet(provider);
        m_error = error;
        m_state = State::Failed;
        return false;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        error = "could not open a pipe to the tunnel";
        m_error = error;
        m_state = State::Failed;
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        error = "could not start the tunnel";
        m_error = error;
        m_state = State::Failed;
        return false;
    }

    if (pid == 0) {
        // Child: both streams to the pipe, because these tools announce the
        // hostname on stderr as often as on stdout.
        ::close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        // Detach from the terminal so ssh cannot try to prompt the user for
        // anything; there is nobody there to answer.
        setsid();

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        // execvp searches PATH for a bare name and uses a path as given, which
        // is exactly the behaviour wanted for both cases above.
        execvp(program, args.data());
        _exit(127);
    }

    ::close(pipefd[1]);
    // Non-blocking, because update() runs on the render thread and must never
    // wait on a program that may say nothing for seconds.
    const int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags != -1) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    m_impl->pid = pid;
    m_impl->fd = pipefd[0];
    m_impl->startedAt = std::chrono::steady_clock::now();
    m_state = State::Starting;
    error.clear();
    return true;
}

void Tunnel::update() {
    if (m_state != State::Starting && m_state != State::Up) return;
    if (m_impl->fd < 0) return;

    for (;;) {
        char buf[1024];
        const ssize_t rc = ::read(m_impl->fd, buf, sizeof(buf));
        if (rc > 0) {
            if (m_log.size() < kMaxLogBytes)
                m_log.append(buf, static_cast<size_t>(rc));
            continue;
        }
        break;
    }

    if (m_state == State::Starting) {
        const std::string found = tunnelParseAddress(m_provider, m_log);
        if (!found.empty()) {
            m_address = found;
            m_state = State::Up;
        }
    }

    // Connected, chatty, and still no address. localhost.run does exactly this
    // for an anonymous connection: it accepts the ssh session, prints its
    // banner, and never issues a hostname -- a free tunnel now needs a
    // registered key. Waiting forever on that is indistinguishable from a hang,
    // so it is called what it is.
    if (m_state == State::Starting) {
        const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_impl->startedAt).count();
        if (waited > kAnnounceTimeoutSeconds) {
            m_error = std::string(tunnelProviderName(m_provider)) +
                      " connected but never gave out an address.";
            if (m_provider == TunnelProvider::LocalhostRun) {
                m_error += " Anonymous localhost.run tunnels now need a registered "
                           "SSH key. Installing cloudflared is the simpler fix: "
                           "\"brew install cloudflared\", then host again.";
            }
            m_state = State::Failed;
            stop();
            m_state = State::Failed;   // stop() would otherwise reset it to Idle
            return;
        }
    }

    // Has it died? A tunnel that exits before announcing anything is a failure
    // the host needs to see, not a silent absence of an address.
    int status = 0;
    const pid_t r = waitpid(m_impl->pid, &status, WNOHANG);
    if (r == m_impl->pid) {
        m_impl->pid = -1;
        if (m_state != State::Up) {
            m_error = m_address.empty()
                ? "The tunnel stopped before it gave an address. " +
                      tunnelProviderHowToGet(m_provider)
                : m_error;
            m_state = State::Failed;
        } else {
            m_error = "The tunnel closed. Players can no longer reach this game.";
            m_state = State::Failed;
        }
    }
}

void Tunnel::stop() {
    if (m_impl && m_impl->pid > 0) {
        kill(m_impl->pid, SIGTERM);
        // Reaped rather than left behind: an orphaned tunnel would keep a port
        // published after the game that opened it has gone.
        int status = 0;
        for (int i = 0; i < 50; i++) {
            if (waitpid(m_impl->pid, &status, WNOHANG) == m_impl->pid) break;
            usleep(20000);
        }
        if (waitpid(m_impl->pid, &status, WNOHANG) == 0) {
            kill(m_impl->pid, SIGKILL);
            waitpid(m_impl->pid, &status, 0);
        }
        m_impl->pid = -1;
    }
    if (m_impl && m_impl->fd >= 0) { ::close(m_impl->fd); m_impl->fd = -1; }
    if (m_state == State::Starting || m_state == State::Up) m_state = State::Idle;
    m_address.clear();
}

#else   // Windows, or the web build

struct Tunnel::Impl {};

Tunnel::Tunnel() : m_impl(std::make_unique<Impl>()) {}
Tunnel::~Tunnel() = default;

bool Tunnel::available() { return false; }

std::vector<TunnelProvider> tunnelProvidersAvailable() { return {}; }

// Declared in Tunnel.h without a platform guard, so they have to EXIST on
// every platform even where they can do nothing -- and Game_Multiplayer.cpp
// calls tunnelSetToolsDir() unconditionally, three times. Without these the
// Windows and web builds compiled every translation unit and then failed at
// link with an undefined symbol, which is why the failure was invisible to
// anyone building on macOS or Linux.
//
// They are no-ops rather than errors: setting where a program lives is
// harmless on a platform that will not run one, and Tunnel::start() already
// says why it cannot help. Refusing here would move that explanation to a
// place with no player in front of it.
void tunnelSetToolsDir(const std::string&) {}

std::string tunnelResolveProgram(const char*) { return {}; }

bool Tunnel::start(TunnelProvider, uint16_t, std::string& error) {
    error = "this build cannot start a tunnel for you; run one alongside the game "
            "and give players the address it prints";
    m_error = error;
    m_state = State::Failed;
    return false;
}

void Tunnel::update() {}
void Tunnel::stop() {}

#endif
