#include "Async.h"

#include <deque>
#include <mutex>

#if !defined(__EMSCRIPTEN__) && !defined(OD_NO_THREADS)
#include <thread>
#endif

namespace odasync {
namespace {

#if defined(__EMSCRIPTEN__) || defined(OD_NO_THREADS)
// Single-threaded (the browser, and DOS): no lock is needed, and taking one would be a claim about
// concurrency that is not true here.
std::deque<std::function<void()>> g_queue;
int g_running = 0;
#else
std::mutex g_lock;
int g_live = 0;
#endif

}  // namespace

void run(std::function<void()> fn) {
#if defined(__EMSCRIPTEN__) || defined(OD_NO_THREADS)
    g_queue.push_back(std::move(fn));
#else
    {
        std::lock_guard<std::mutex> g(g_lock);
        ++g_live;
    }
    std::thread([fn = std::move(fn)]() {
        fn();
        std::lock_guard<std::mutex> g(g_lock);
        --g_live;
    }).detach();
#endif
}

bool pump() {
#if defined(__EMSCRIPTEN__) || defined(OD_NO_THREADS)
    if (g_queue.empty()) return false;
    // Taken off the queue BEFORE running: the job blocks for as long as the
    // request takes, and ASYNCIFY resumes the loop underneath it, so a job left
    // on the queue while it runs would be started again by the next pump.
    auto fn = std::move(g_queue.front());
    g_queue.pop_front();
    ++g_running;
    fn();
    --g_running;
    return true;
#else
    return false;
#endif
}

int outstanding() {
#if defined(__EMSCRIPTEN__) || defined(OD_NO_THREADS)
    return (int)g_queue.size() + g_running;
#else
    std::lock_guard<std::mutex> g(g_lock);
    return g_live;
#endif
}

}  // namespace odasync
