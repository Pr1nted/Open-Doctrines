#include "WebPersist.h"

#include <iostream>

// raylib and OdState are needed by the web BODY of these functions and by
// nothing else -- off the web every one of them is empty. Kept inside the
// guard so this file links into targets that have neither: SaveDeltaTest
// deliberately builds SaveManager.cpp alone, with no window and no map, and
// SaveManager calls odPersistMark().
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include "raylib.h"
#include "../OdState.h"
#endif

namespace {

#ifdef __EMSCRIPTEN__

// The IDBFS mount, and the archive inside it. Deliberately OUTSIDE /data: /data
// is the preload, and mounting anything over it would hide the content the game
// ships with.
const char* kMountDir = "/persist";
const char* kArchive  = "/persist/state.odstate";

bool  g_ready   = false;    // the mount exists and has been read back
bool  g_dirty   = false;    // something changed since the last write
double g_lastWrite = 0.0;

// Long enough that dragging a volume slider does not serialise the player's
// saves on every frame, short enough that a tab closed without warning loses
// little. Every flush point that matters -- a save finishing, leaving a world
// -- calls odPersistFlush() and does not wait for this.
constexpr double kMinWriteInterval = 20.0;

/** Runs the browser's event loop until the pending syncfs reports back. */
void waitForSync() {
    // 15 seconds is not a timeout for a slow disk, it is a backstop for a
    // callback that never fires -- a private-mode tab that refuses IndexedDB,
    // or a quota prompt nobody answers. Coming up with an empty data/ is
    // correct there; hanging on the loading screen forever is not.
    const double deadline = GetTime() + 15.0;
    while (!EM_ASM_INT({ return Module.odPersistDone | 0; })) {
        if (GetTime() > deadline) {
            std::cerr << "[persist] IndexedDB did not answer; "
                         "this session will not be saved" << std::endl;
            return;
        }
        emscripten_sleep(16);
    }
}

#endif  // __EMSCRIPTEN__

}  // namespace

void odPersistInit(const std::string& dataDir) {
#ifndef __EMSCRIPTEN__
    (void)dataDir;
#else
    EM_ASM({
        var dir = UTF8ToString($0);
        Module.odPersistDone = 0;
        try {
            FS.mkdirTree(dir);
            FS.mount(IDBFS, {}, dir);
        } catch (e) {
            // EBUSY means it is already mounted, which is fine and is what a
            // second call looks like. Anything else and there is no store, so
            // say so and let the game come up unpersisted rather than not
            // come up at all.
            if (!e || e.errno !== 16) {
                console.error('[persist] mount failed', e);
                Module.odPersistDone = 1;
                Module.odPersistFailed = 1;
                return;
            }
        }
        // A tab being hidden is the last warning this page reliably gets, and
        // the archive may already be sitting in the mount with its transfer to
        // IndexedDB still in flight. Pushing it again here costs nothing when
        // there is nothing outstanding.
        //
        // syncfs ONLY -- deliberately no call back into wasm. Re-entering the
        // module from an event handler while it may be suspended mid-Asyncify
        // (downloading a scenario, waiting on the account service) is how you
        // corrupt a stack, and the frame loop rewrites the archive within
        // seconds anyway.
        if (!Module.odPersistHooked) {
            Module.odPersistHooked = 1;
            var push = function () { try { FS.syncfs(false, function () {}); } catch (e) {} };
            document.addEventListener('visibilitychange', function () {
                if (document.visibilityState === 'hidden') push();
            });
            window.addEventListener('pagehide', push);
        }

        // true = IndexedDB -> memory. The direction that reads.
        FS.syncfs(true, function (err) {
            if (err) {
                console.error('[persist] load failed', err);
                Module.odPersistFailed = 1;
            }
            Module.odPersistDone = 1;
        });
    }, kMountDir);

    waitForSync();

    if (EM_ASM_INT({ return Module.odPersistFailed | 0; })) return;
    g_ready = true;
    g_lastWrite = GetTime();

    if (!FileExists(kArchive)) {
        std::cout << "[persist] no previous session stored" << std::endl;
        return;
    }

    // Unpacked with the same loader the manual "Load .odstate" uses, over the
    // same directory. Refuses absolute and ".." entries, which matters because
    // IndexedDB is per-origin and another page on this origin could in
    // principle have written here.
    std::string err;
    int count = 0;
    if (OdState::load(dataDir, kArchive, err, &count))
        std::cout << "[persist] restored " << count << " file(s)" << std::endl;
    else
        std::cerr << "[persist] could not restore: " << err << std::endl;
#endif
}

void odPersistMark() {
#ifdef __EMSCRIPTEN__
    g_dirty = true;
#endif
}

void odPersistTick(const std::string& dataDir) {
#ifndef __EMSCRIPTEN__
    (void)dataDir;
#else
    if (!g_ready || !g_dirty) return;
    if (GetTime() - g_lastWrite < kMinWriteInterval) return;
    odPersistFlush(dataDir);
#endif
}

void odPersistFlush(const std::string& dataDir) {
#ifndef __EMSCRIPTEN__
    (void)dataDir;
#else
    if (!g_ready || !g_dirty) return;
    g_dirty = false;
    g_lastWrite = GetTime();

    std::string err;
    int count = 0;
    if (!OdState::save(dataDir, kArchive, err, &count)) {
        std::cerr << "[persist] could not write: " << err << std::endl;
        return;
    }

    // false = memory -> IndexedDB. NOT waited on: the archive is already
    // written into the mount, and the browser will finish the transfer while
    // the game carries on. Blocking here would stall a frame for the size of
    // every save the player owns.
    EM_ASM({
        if (Module.odPersistWriting) { Module.odPersistAgain = 1; return; }
        Module.odPersistWriting = 1;
        var again = function () {
            Module.odPersistWriting = 0;
            if (Module.odPersistAgain) {
                Module.odPersistAgain = 0;
                Module.odPersistWriting = 1;
                FS.syncfs(false, again);
            }
        };
        FS.syncfs(false, function (err) {
            if (err) console.error('[persist] write failed', err);
            again();
        });
    });
#endif
}
