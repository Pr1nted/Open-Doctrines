#include "ServerConsole.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <thread>

namespace {

constexpr size_t kHistoryLines = 2000;

const char* levelTag(ServerLogLevel l) {
    switch (l) {
        case ServerLogLevel::Debug: return "DEBUG";
        case ServerLogLevel::Info:  return "INFO";
        case ServerLogLevel::Warn:  return "WARN";
        case ServerLogLevel::Error: return "ERROR";
    }
    return "INFO";
}

/** Local wall-clock HH:MM:SS, which is what an operator correlates against. */
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace

bool serverLogLevelFromName(const std::string& name, ServerLogLevel& out) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)tolower(c); });
    if (n == "debug") { out = ServerLogLevel::Debug; return true; }
    if (n == "info")  { out = ServerLogLevel::Info;  return true; }
    if (n == "warn" || n == "warning") { out = ServerLogLevel::Warn; return true; }
    if (n == "error") { out = ServerLogLevel::Error; return true; }
    return false;
}

ServerConsole::ServerConsole() = default;
ServerConsole::~ServerConsole() = default;

void ServerConsole::startReading() {
    if (m_reading.exchange(true)) return;
    // Detached: see the header. A blocking getline cannot be cancelled, so the
    // process must be able to exit while this thread is still sitting in one.
    std::thread([this] {
        std::string line;
        while (std::getline(std::cin, line)) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pending.push_back(line);
            }
            if (m_stopping) break;
        }
        // stdin closed. That is how a server run under a supervisor with no
        // terminal ends up here, and it is NOT a reason to stop: the server
        // should keep running with no console, exactly as it does in UI mode.
    }).detach();
}

void ServerConsole::log(ServerLogLevel level, const std::string& text) {
    if (level < m_level) return;
    const std::string line = "[" + timestamp() + " " + levelTag(level) + "] " + text;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_history.push_back(line);
        while (m_history.size() > kHistoryLines) m_history.pop_front();
    }
    // Unbuffered enough to survive a kill: a server's last line before it died
    // is the one worth having, and a full stdio buffer is where it goes missing.
    std::cout << line << std::endl;
}

void ServerConsole::raw(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_history.push_back(text);
        while (m_history.size() > kHistoryLines) m_history.pop_front();
    }
    std::cout << text << std::endl;
}

std::vector<std::string> ServerConsole::history() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_history.begin(), m_history.end()};
}

void ServerConsole::add(Command c) { m_commands.push_back(std::move(c)); }

bool ServerConsole::poll(std::string& line) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.empty()) return false;
    line = m_pending.front();
    m_pending.pop_front();
    return true;
}

std::vector<std::string> ServerConsole::split(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void ServerConsole::dispatch(const std::string& line) {
    std::vector<std::string> words = split(line);
    if (words.empty()) return;

    const std::string name = words.front();
    words.erase(words.begin());

    for (const Command& c : m_commands) {
        if (c.name != name) continue;
        if ((int)words.size() < c.minArgs ||
            (c.maxArgs >= 0 && (int)words.size() > c.maxArgs)) {
            raw("usage: " + c.usage);
            return;
        }
        c.run(words, *this);
        return;
    }

    // Nearest command by prefix, because the commonest console mistake is a
    // half-remembered name and "unknown command" alone does not help.
    std::string suggestion;
    for (const Command& c : m_commands)
        if (c.name.rfind(name, 0) == 0 || name.rfind(c.name, 0) == 0) {
            suggestion = c.name;
            break;
        }
    if (suggestion.empty())
        raw("unknown command '" + name + "'. Type `help` for the list.");
    else
        raw("unknown command '" + name + "'. Did you mean `" + suggestion + "`?");
}

void ServerConsole::printHelp(const std::string& which) {
    if (!which.empty()) {
        for (const Command& c : m_commands)
            if (c.name == which) {
                raw("  " + c.usage);
                raw("      " + c.help);
                return;
            }
        raw("no command called '" + which + "'.");
        return;
    }

    size_t width = 0;
    for (const Command& c : m_commands) width = std::max(width, c.usage.size());
    raw("Commands:");
    for (const Command& c : m_commands) {
        std::string pad(width - c.usage.size(), ' ');
        raw("  " + c.usage + pad + "   " + c.help);
    }
}
