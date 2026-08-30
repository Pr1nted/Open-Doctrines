#pragma once

#include <cstdio>
#include <string>
#include <ostream>

/**
 * Logging for the loading path, with no iostream underneath it.
 *
 * WHY. Asyncify unwinds the stack whenever the audio pump yields, which during
 * a map load happens every few thousand provinces. Coming back in, a call into
 * libc++'s iostream formatting -- basic_ostream::__put_num_, which reaches the
 * locale's num_put facet through a virtual call -- traps the module outright:
 *
 *     RuntimeError: null function
 *       at basic_ostream<...>::__put_num_
 *       at Game::updateLoading()
 *       at Object.doRewind
 *
 * No message, no partial load, no clue that it was about maps. The tab keeps
 * running and drops back to the menu, so it reads as "entering a world does
 * nothing" -- which is exactly how it was reported, on a phone and then on
 * every desktop browser too.
 *
 * printf has no virtual dispatch and no locale behind it and survives the same
 * path. This keeps the `<<` spelling so the call sites read as they did, and
 * builds the line in a std::string using to_string rather than a stream.
 *
 * One line per statement: the temporary flushes when the full statement ends,
 * so a chain broken across several source lines still arrives whole.
 */
class LoadLog {
public:
    /// Where finished lines also go, besides stdout.
    ///
    /// The game redirects std::cout into an in-game console with rdbuf. This
    /// does not go through std::cout, so without a hook that console would
    /// simply stop showing anything the moment the logging moved off
    /// iostream. Game::init installs one.
    using Sink = void (*)(const char*);
    static void setSink(Sink s) { sink() = s; }

    ~LoadLog() {
        if (m_s.empty()) return;
        std::fputs(m_s.c_str(), stdout);
        std::fflush(stdout);
        if (sink()) sink()(m_s.c_str());
    }

    LoadLog& operator<<(const char* v)        { m_s += v ? v : "(null)"; return *this; }
    LoadLog& operator<<(const std::string& v) { m_s += v;                return *this; }
    LoadLog& operator<<(char v)               { m_s += v;                return *this; }
    LoadLog& operator<<(bool v)               { m_s += v ? "true" : "false"; return *this; }

    LoadLog& operator<<(int v)                { return num(v); }
    LoadLog& operator<<(unsigned v)           { return num(v); }
    LoadLog& operator<<(long v)               { return num(v); }
    LoadLog& operator<<(unsigned long v)      { return num(v); }
    LoadLog& operator<<(long long v)          { return num(v); }
    LoadLog& operator<<(unsigned long long v) { return num(v); }

    // Trailing zeros trimmed, so "3.0" prints as "3" the way the stream did.
    LoadLog& operator<<(double v) {
        char b[64];
        std::snprintf(b, sizeof b, "%g", v);
        m_s += b;
        return *this;
    }
    LoadLog& operator<<(float v) { return *this << (double)v; }

    LoadLog& operator<<(const void* v) {
        char b[32];
        std::snprintf(b, sizeof b, "%p", v);
        m_s += b;
        return *this;
    }

    /// std::endl and friends. Only the newline matters here; the flush happens
    /// when the statement ends either way.
    LoadLog& operator<<(std::ostream& (*)(std::ostream&)) { m_s += '\n'; return *this; }

private:
    template <typename T>
    LoadLog& num(T v) { m_s += std::to_string(v); return *this; }

    static Sink& sink() { static Sink s = nullptr; return s; }

    std::string m_s;
};
