#pragma once

// std::mutex and friends for a C++ library built without threads (DJGPP).
//
// DJGPP's libstdc++ is configured with no thread model (_GLIBCXX_HAS_GTHREADS
// undefined), so <mutex> declares no std::mutex, <condition_variable> no
// std::condition_variable, and std::thread cannot be constructed. The game
// takes locks all over -- around state a worker thread shares on every other
// platform -- and on DOS there is only ever one thread, so a lock that does
// nothing is exactly right. Force-included into every C++ file of the DOS
// build (see OD_DOS in CMakeLists.txt) rather than edited into each one.
//
// What this does NOT give is threads. Nothing here can run work concurrently,
// and std::thread stays unconstructible: the places that start a thread take
// the single-threaded path the web build already has (OD_NO_THREADS, see
// src/util/Async.h). A condition variable wait that is not already satisfied
// would wait forever on a thread that does not exist, so it stops the game
// with a message naming the bug instead of hanging the machine.

#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if !defined(_GLIBCXX_HAS_GTHREADS)

namespace std {

[[noreturn]] inline void od_no_threads_wait() {
    std::fputs("internal error: waited for another thread, and DOS has none\n", stderr);
    std::abort();
}

class mutex {
public:
    constexpr mutex() noexcept = default;
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    void lock() {}
    bool try_lock() { return true; }
    void unlock() {}
};

class recursive_mutex {
public:
    constexpr recursive_mutex() noexcept = default;
    recursive_mutex(const recursive_mutex&) = delete;
    recursive_mutex& operator=(const recursive_mutex&) = delete;
    void lock() {}
    bool try_lock() { return true; }
    void unlock() {}
};

class timed_mutex : public mutex {
public:
    template <class R, class P> bool try_lock_for(const chrono::duration<R, P>&) { return true; }
    template <class C, class D> bool try_lock_until(const chrono::time_point<C, D>&) { return true; }
};

enum class cv_status { no_timeout, timeout };

class condition_variable {
public:
    condition_variable() noexcept = default;
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;
    void notify_one() noexcept {}
    void notify_all() noexcept {}
    template <class L> void wait(L&) { od_no_threads_wait(); }
    template <class L, class P> void wait(L&, P pred) { if (!pred()) od_no_threads_wait(); }
    template <class L, class R, class Pe> cv_status wait_for(L&, const chrono::duration<R, Pe>&) { return cv_status::timeout; }
    template <class L, class R, class Pe, class P> bool wait_for(L&, const chrono::duration<R, Pe>&, P pred) { return pred(); }
    template <class L, class C, class D> cv_status wait_until(L&, const chrono::time_point<C, D>&) { return cv_status::timeout; }
    template <class L, class C, class D, class P> bool wait_until(L&, const chrono::time_point<C, D>&, P pred) { return pred(); }
};

using condition_variable_any = condition_variable;

}  // namespace std

#endif  // !_GLIBCXX_HAS_GTHREADS
