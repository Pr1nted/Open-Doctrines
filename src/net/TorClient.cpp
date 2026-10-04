#include "TorClient.h"

#include "Socks5.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#elif !defined(__EMSCRIPTEN__)
  #include <arpa/inet.h>
  #include <csignal>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace torclient {

namespace {

std::mutex g_mutex;               // one start at a time
std::string g_dataDir;
std::atomic<int> g_port{0};       // the game's own Tor's SOCKS port, once up
std::atomic<int> g_progress{-1};  // bootstrap %, -1 when not starting
std::atomic<bool> g_autoStart{true};
std::string g_lastLine;           // under g_mutex: the last thing Tor said

#if defined(_WIN32)
HANDLE g_proc = nullptr;
const char* kExe = "tor.exe";
const char kSep = '\\';
#elif !defined(__EMSCRIPTEN__)
pid_t g_pid = -1;
const char* kExe = "tor";
const char kSep = '/';
#endif

bool exists(const std::string& p) {
#if defined(_WIN32)
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#elif defined(__EMSCRIPTEN__)
    (void)p;
    return false;
#else
    return access(p.c_str(), X_OK) == 0;
#endif
}

/** Something answering on loopback `port`. */
bool answers(int port) {
#if defined(__EMSCRIPTEN__)
    (void)port;
    return false;
#else
    const auto s = socket(AF_INET, SOCK_STREAM, 0);
    if ((long long)s < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = ::connect(s, (sockaddr*)&a, sizeof a) == 0;
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
    return ok;
#endif
}

int freePort() {
#if defined(__EMSCRIPTEN__)
    return 0;
#else
    const auto s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int port = 0;
    if (bind(s, (sockaddr*)&a, sizeof a) == 0) {
        socklen_t len = sizeof a;
        getsockname(s, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
    }
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
    return port;
#endif
}

/** Read Tor's log as it arrives, keeping the bootstrap percentage. */
void watchOutput(std::string line) {
    const size_t at = line.find("Bootstrapped ");
    if (at != std::string::npos) {
        const int pct = std::atoi(line.c_str() + at + 13);
        if (pct >= 0 && pct <= 100) g_progress.store(pct);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (line.find("[warn]") != std::string::npos || line.find("[err]") != std::string::npos)
        g_lastLine = line;
}

}  // namespace

void setAutoStart(bool on) { g_autoStart.store(on); }

void setDataDir(const std::string& dataDir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dataDir = dataDir;
    if (!g_dataDir.empty() && g_dataDir.back() != '/' && g_dataDir.back() != '\\')
        g_dataDir += kSep;
}

std::string findBinary() {
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        dir = g_dataDir;
    }
    // The release's own copy first: a known version, checked at release time.
    if (!dir.empty() && exists(dir + "tor" + kSep + kExe)) return dir + "tor" + kSep + kExe;
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    // Then wherever an installed one usually is -- including Homebrew's,
    // which a Finder-launched app's PATH does not contain.
    for (const char* p : {"/opt/homebrew/bin/tor", "/usr/local/bin/tor", "/usr/bin/tor",
                          "/usr/sbin/tor"})
        if (exists(p)) return p;
    if (const char* path = std::getenv("PATH")) {
        std::string ps(path);
        size_t at = 0;
        while (at <= ps.size()) {
            const size_t c = ps.find(':', at);
            const std::string d = ps.substr(at, c == std::string::npos ? std::string::npos : c - at);
            if (!d.empty() && exists(d + "/tor")) return d + "/tor";
            if (c == std::string::npos) break;
            at = c + 1;
        }
    }
#endif
    return {};
}

std::string status() {
    const int p = g_progress.load();
    if (p < 0 || p >= 100) return {};
    return "Starting Tor: " + std::to_string(p) + "%";
}

int ensure(std::string& why, int timeoutSec) {
#if defined(__EMSCRIPTEN__)
    (void)timeoutSec;
    why = "a browser cannot connect through Tor";
    return 0;
#else
    // Somebody's Tor already answering is the best answer: a configured port,
    // the tor service, Tor Browser -- or ours, started earlier.
    if (socks5::port() > 0 && answers(socks5::port())) return socks5::port();
    if (g_port.load() > 0 && answers(g_port.load())) return g_port.load();
    for (int p : {9050, 9150})
        if (answers(p)) return p;

    if (!g_autoStart.load()) {
        why = "Tor is not running on this computer. Start the tor service, or open "
              "Tor Browser and leave it open, then try again.";
        return 0;
    }
    static std::mutex startMutex;
    std::lock_guard<std::mutex> starting(startMutex);
    // Another thread may have started it while this one waited.
    if (g_port.load() > 0 && answers(g_port.load())) return g_port.load();

    const std::string bin = findBinary();
    if (bin.empty()) {
        why = "this copy of the game has no Tor, and none is installed. Install Tor "
              "(\"brew install tor\", \"apt install tor\", or Tor Browser) and try again.";
        return 0;
    }
    const int port = freePort();
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        dir = g_dataDir.empty() ? std::string() : g_dataDir + "tor-client";
        g_lastLine.clear();
    }
#if defined(_WIN32)
    if (!dir.empty()) CreateDirectoryA(dir.c_str(), nullptr);
    const std::string owner = std::to_string(GetCurrentProcessId());
#else
    if (!dir.empty()) { ::mkdir(dir.c_str(), 0700); ::chmod(dir.c_str(), 0700); }
    const std::string owner = std::to_string(getpid());
#endif
    std::vector<std::string> args = {
        bin, "--SocksPort", "127.0.0.1:" + std::to_string(port),
        "--ClientOnly", "1",
        // Exit when the game does, however the game ends.
        "--__OwningControllerProcess", owner,
        "--Log", "notice stdout"};
    if (!dir.empty()) { args.push_back("--DataDirectory"); args.push_back(dir); }
    g_progress.store(0);

#if defined(_WIN32)
    std::string cmd;
    for (const auto& a : args) cmd += "\"" + a + "\" ";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<char> line(cmd.begin(), cmd.end());
    line.push_back('\0');
    if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        g_progress.store(-1);
        why = "could not start Tor (" + bin + ")";
        return 0;
    }
    CloseHandle(wr);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    std::thread([rd] {
        char buf[512];
        std::string acc;
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) {
            acc.append(buf, n);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                watchOutput(acc.substr(0, nl));
                acc.erase(0, nl + 1);
            }
        }
        CloseHandle(rd);
    }).detach();
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) { why = "could not start Tor"; g_progress.store(-1); return 0; }
    const pid_t pid = fork();
    if (pid == 0) {
        ::close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        setsid();
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execv(bin.c_str(), av.data());
        _exit(127);
    }
    ::close(pipefd[1]);
    if (pid < 0) { ::close(pipefd[0]); why = "could not start Tor"; g_progress.store(-1); return 0; }
    g_pid = pid;
    std::thread([fd = pipefd[0]] {
        char buf[512];
        std::string acc;
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof buf)) > 0) {
            acc.append(buf, (size_t)n);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                watchOutput(acc.substr(0, nl));
                acc.erase(0, nl + 1);
            }
        }
        ::close(fd);
    }).detach();
#endif

    // Ready when Tor says it has finished connecting -- the port answers well
    // before that, and a connection attempted then just waits inside Tor.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    while (std::chrono::steady_clock::now() < until) {
        if (g_progress.load() >= 100 && answers(port)) {
            g_port.store(port);
            socks5::setPort(port);
            return port;
        }
#if defined(_WIN32)
        if (WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) break;
#else
        int st = 0;
        if (waitpid(g_pid, &st, WNOHANG) == g_pid) { g_pid = -1; break; }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::string last;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        last = g_lastLine;
    }
    const int pct = g_progress.load();
    g_progress.store(-1);
    shutdown();
    why = "Tor could not connect (" + std::to_string(pct < 0 ? 0 : pct) +
          "% after " + std::to_string(timeoutSec) + "s). This network may block Tor." +
          (last.empty() ? std::string() : " Tor said: " + last);
    return 0;
#endif
}

void shutdown() {
#if defined(_WIN32)
    if (g_proc) { TerminateProcess(g_proc, 0); CloseHandle(g_proc); g_proc = nullptr; }
#elif !defined(__EMSCRIPTEN__)
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        int st = 0;
        for (int i = 0; i < 50 && waitpid(g_pid, &st, WNOHANG) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (waitpid(g_pid, &st, WNOHANG) == 0) { kill(g_pid, SIGKILL); waitpid(g_pid, &st, 0); }
        g_pid = -1;
    }
#endif
    g_port.store(0);
}

}  // namespace torclient
