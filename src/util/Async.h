#pragma once

// Work that must not block the frame, on a platform that may have no threads.
//
// ── THE PROBLEM THIS EXISTS FOR ──
//
// Every language-model request in this game was written as
// `std::thread(...).detach()`. That is right on desktop and IMPOSSIBLE in the
// browser: the web build is single-threaded on purpose (see the ASYNCIFY note
// in CMakeLists.txt), so constructing a std::thread there does not degrade --
// it aborts the module. Nothing stopped a player enabling the module on web,
// so the first letter they wrote would have killed the tab.
//
// ── HOW THE BROWSER DOES IT INSTEAD ──
//
// ASYNCIFY. A synchronous-looking call unwinds the whole wasm stack, hands the
// thread back to the browser, and resumes when the reply arrives -- which is
// how the account client already works. So the browser needs no thread; it
// needs the work to run somewhere it can be unwound from.
//
// That "somewhere" matters. Unwinding is cheap near the top of the loop and
// expensive deep inside it, and the unwind buffer is finite. So on web the work
// is QUEUED and run from the main loop by pump(), where the stack is shallow --
// rather than inline from wherever it was requested, which may be several
// frames down inside a turn resolution.
//
// On desktop this is exactly the detached thread it replaces, and pump() does
// nothing.

#include <functional>

namespace odasync {

/**
 * Run `fn` without blocking the caller.
 *
 * Desktop: a detached thread, started immediately.
 * Browser: queued, and run by the next pump().
 *
 * `fn` must own everything it touches. It runs on another thread on desktop and
 * at an unspecified later moment in the browser, so capturing anything the
 * caller will free -- or any Game state -- is a bug on both.
 */
void run(std::function<void()> fn);

/**
 * Run one queued job, if there is one. Returns true if it ran something.
 *
 * Called from the main loop, and from any bounded wait that is expecting one of
 * these to finish -- on web nothing else will ever run them, so a wait that did
 * not pump would be a wait for something that cannot happen.
 *
 * A no-op on desktop, where the jobs are already running.
 */
bool pump();

/// How many jobs are queued or running. For bounded waits and diagnostics.
int outstanding();

}  // namespace odasync
