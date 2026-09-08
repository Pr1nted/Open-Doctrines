#include "RunCurl.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#if !defined(_WIN32)
#include <csignal>
#endif

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace odproc {
namespace {

#if defined(_WIN32)
// One argv entry, quoted the way CommandLineToArgvW will read it back.
//
// CreateProcess takes a single command LINE, not a vector, so the arguments
// have to survive being flattened and re-split. Joining them with spaces --
// which is what this code used to do -- silently breaks the moment any argument
// contains one, and the argument most likely to is the output path: the
// updater writes into %LOCALAPPDATA%, so a player whose Windows account is
// "John Smith" had `-o C:\Users\John Smith\...` split into two arguments and
// the download failed with no explanation.
//
// The rules are Microsoft's own: backslashes are literal EXCEPT in a run
// immediately before a quote, where they are doubled, and an embedded quote is
// escaped with one more backslash.
std::string quoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;

    std::string out = "\"";
    for (size_t i = 0; ; ++i) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == '\\') { ++i; ++slashes; }
        if (i == a.size()) {
            out.append(slashes * 2, '\\');   // before the closing quote
            break;
        }
        if (a[i] == '"') {
            out.append(slashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(slashes, '\\');
            out += a[i];
        }
    }
    out += '"';
    return out;
}
#endif

// The whole of runCurl and runTool. `program` is the executable; args are argv
// entries and are never concatenated into anything a shell would parse.
bool runProgram(const std::string& program,
                const std::vector<std::string>& args,
                long long expectedSize, std::atomic<int>* percent,
                const std::string& progressFile) {
    auto poll = [&]() {
        if (!percent || expectedSize <= 0 || progressFile.empty()) return;
        std::error_code ec;
        auto n = (long long)fs::file_size(progressFile, ec);
        if (!ec) percent->store((int)std::min(100LL, n * 100 / expectedSize));
    };

#if defined(_WIN32)
    std::string cmd = quoteArg(program);
    for (const auto& a : args) cmd += " " + quoteArg(a);

    STARTUPINFOA si{}; si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;                    // no console flash over the game
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    DWORD rc = 1;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 150) == WAIT_OBJECT_0) break;
        poll();
    }
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return rc == 0;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        poll();
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

}  // namespace

bool runCurl(const std::vector<std::string>& args, long long expectedSize,
             std::atomic<int>* percent, const std::string& progressFile) {
#if defined(_WIN32)
    return runProgram("curl.exe", args, expectedSize, percent, progressFile);
#else
    return runProgram("curl", args, expectedSize, percent, progressFile);
#endif
}

bool runTool(const std::string& program, const std::vector<std::string>& args) {
    return runProgram(program, args, 0, nullptr, std::string());
}

long long startDetached(const std::string& program,
                        const std::vector<std::string>& args) {
#if defined(_WIN32)
    std::string cmd = quoteArg(program);
    for (const auto& a : args) cmd += " " + quoteArg(a);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (long long)pi.dwProcessId;
#else
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        // Its own session, so Ctrl-C in the terminal the game was launched from
        // does not also kill the runner, and so the runner cannot pull the game
        // down with it.
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    return (long long)pid;
#endif
}

bool stopDetached(long long pid) {
    if (pid <= 0) return false;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) return false;
    const bool ok = TerminateProcess(h, 0) != 0;
    CloseHandle(h);
    return ok;
#else
    if (kill((pid_t)pid, SIGTERM) != 0) return false;
    // Reaped so it does not sit as a zombie for as long as the game runs.
    for (int i = 0; i < 40; ++i) {
        if (waitpid((pid_t)pid, nullptr, WNOHANG) == (pid_t)pid) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill((pid_t)pid, SIGKILL);
    waitpid((pid_t)pid, nullptr, WNOHANG);
    return true;
#endif
}

bool detachedAlive(long long pid) {
    if (pid <= 0) return false;
#if defined(_WIN32)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    // Reap first: a dead child we started is a zombie until waited for, and a
    // zombie answers kill(0) as though it were alive.
    if (waitpid((pid_t)pid, nullptr, WNOHANG) == (pid_t)pid) return false;
    return kill((pid_t)pid, 0) == 0;
#endif
}

}  // namespace odproc
