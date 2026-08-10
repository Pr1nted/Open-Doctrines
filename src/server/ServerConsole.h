#pragma once

// The server's console: what it prints, and what you can type at it.
//
// Modelled on what a Minecraft server does, because that is the interface every
// server operator already knows: a timestamped log on stdout, and bare commands
// typed on stdin with no prefix character.
//
// READING STDIN MUST NOT STOP THE GAME
//
// The turn loop has to keep running while nobody is typing, so the read happens
// on its own thread and lands in a queue the loop drains. Doing it any other way
// -- polling stdin from the loop, or select() on a handle that is a pipe on one
// platform and a console on another -- is where this kind of code usually goes
// wrong on Windows.
//
// The thread is DETACHED and the process exits without joining it, deliberately.
// A blocking read on stdin cannot be cancelled portably; joining would mean the
// server hanging on shutdown until somebody pressed Return, which is precisely
// the behaviour that makes people reach for kill -9.
//
// COMMANDS ARE A TABLE, NOT A CHAIN OF IFS
//
// `help` is generated from the same table that dispatches, so a command cannot
// exist without being documented, and the argument counts are checked in one
// place rather than in each handler. Registration happens in Game_Server.cpp,
// which is where the things commands act on actually live.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

enum class ServerLogLevel : uint8_t { Debug = 0, Info, Warn, Error };

bool serverLogLevelFromName(const std::string& name, ServerLogLevel& out);

class ServerConsole {
public:
    ServerConsole();
    ~ServerConsole();
    ServerConsole(const ServerConsole&) = delete;
    ServerConsole& operator=(const ServerConsole&) = delete;

    /** Start reading stdin. Not called by the UI-only mode, which has no stdin. */
    void startReading();

    /** Print at or above the configured level. Timestamped, one line. */
    void log(ServerLogLevel level, const std::string& text);
    void info(const std::string& t)  { log(ServerLogLevel::Info, t); }
    void warn(const std::string& t)  { log(ServerLogLevel::Warn, t); }
    void error(const std::string& t) { log(ServerLogLevel::Error, t); }
    void debug(const std::string& t) { log(ServerLogLevel::Debug, t); }

    /** Print with no timestamp or level, for `help` and `config` tables. */
    void raw(const std::string& text);

    void setLevel(ServerLogLevel l) { m_level = l; }
    ServerLogLevel level() const { return m_level; }

    /**
     * Everything printed so far, newest last, capped.
     *
     * Kept so the UI mode and the Android app can show the same log the console
     * shows without a second logging path. The cap is what stops a server that
     * has been up for a month from holding a month of text.
     */
    std::vector<std::string> history() const;

    // ── commands ──

    struct Command {
        std::string name;
        std::string usage;     // e.g. "kick <player> [reason]"
        std::string help;      // one line
        int minArgs = 0;
        int maxArgs = -1;      // -1 = any number
        /** Args are already split; the rest of the line is joined for `say`. */
        std::function<void(const std::vector<std::string>&, ServerConsole&)> run;
    };

    void add(Command c);

    /** A line as typed. Splits, checks arity, dispatches, or explains. */
    void dispatch(const std::string& line);

    /** Next line typed, if any. False when nobody has typed. */
    bool poll(std::string& line);

    /** Print the generated command list. `help <name>` prints one. */
    void printHelp(const std::string& which = "");

    const std::vector<Command>& commands() const { return m_commands; }

    /** True once `stop` was typed or a signal asked for shutdown. */
    bool stopping() const { return m_stopping; }
    void requestStop() { m_stopping = true; }

    /** Split a command line into words, honouring "quoted phrases". */
    static std::vector<std::string> split(const std::string& line);

private:
    std::vector<Command> m_commands;
    ServerLogLevel m_level = ServerLogLevel::Info;

    mutable std::mutex m_mutex;
    std::deque<std::string> m_pending;    // typed, not yet dispatched
    std::deque<std::string> m_history;    // printed
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_reading{false};
};
