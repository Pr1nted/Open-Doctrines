#include "DevLink.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "DevShared.h"
#include "rlgl.h"

// THIS FILE IS THE ONE PLACE THE SHIM MUST NOT REACH. DevInput.h is force-
// included into every translation unit of the game, including this one, and
// with the macros in place `return IsKeyDown(key);` below would call itself
// forever. Undefined here, after the includes, so the fall-through calls the
// real raylib functions.
#undef IsKeyDown
#undef IsKeyPressed
#undef IsKeyReleased
#undef IsMouseButtonDown
#undef IsMouseButtonPressed
#undef IsMouseButtonReleased
#undef GetMousePosition
#undef GetMouseWheelMove
#undef GetCharPressed

namespace {

const char* devEnv(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : nullptr;
}

/**
 * The mapping, shared by the frame going out and the input coming back.
 *
 * File scope rather than inside either namespace because both halves of this
 * file need it and neither should own it. Opened once; a failure is
 * remembered, so a missing directory does not mean an open() on every frame
 * for the rest of the session.
 */
void* sharedMapping() {
    static void* base = nullptr;
    static bool tried = false;
    if (tried) return base;
    tried = true;

    const char* path = devEnv("OD_DEV_VIEW");
    if (path == nullptr) return nullptr;

    const int fd = ::open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return nullptr;
    // Sized exactly, every time: a file left over from a build with different
    // dimensions would otherwise be mapped short and read off its end.
    if (::ftruncate(fd, static_cast<off_t>(devshared::kTotalBytes)) != 0) {
        ::close(fd);
        return nullptr;
    }
    void* p = ::mmap(nullptr, devshared::kTotalBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return nullptr;

    auto* head = static_cast<devshared::FrameHeader*>(p);
    std::memcpy(head->magic, devshared::kFrameMagic, 8);
    head->width = devshared::kViewWidth;
    head->height = devshared::kViewHeight;
    base = p;
    return base;
}

}  // namespace

namespace devlink {
namespace {

double now() { return GetTime(); }

/// Thirty a second. Twelve was the first guess and it was wrong in a way that
/// only shows up once you try to USE the view: at twelve, a pointer moved in
/// the editor lands up to eighty milliseconds late, which reads as the game
/// being broken rather than the view being slow.
constexpr double kViewInterval = 1.0 / 30.0;

}  // namespace

bool watchEnabled() {
    static const bool on = devEnv("OD_DEV_WATCH") != nullptr;
    return on;
}

bool viewEnabled() {
    static const bool on = devEnv("OD_DEV_VIEW") != nullptr;
    return on;
}

bool modsChanged(const std::string& modsDir) {
    if (!watchEnabled()) return false;

    static double nextPoll = 0.0;
    if (now() < nextPoll) return false;
    nextPoll = now() + 0.25;

    // The directory's own mtime is not enough: on some filesystems it does not
    // move when a file inside it is rewritten in place, which is exactly what
    // installing a rebuilt mod does. So every .odmod is stat'ed and the
    // answer is the combination -- names, sizes and times together, which
    // also catches a file being added or removed.
    std::string fingerprint;
    fingerprint.reserve(256);

    FilePathList files = LoadDirectoryFiles(modsDir.c_str());
    for (unsigned i = 0; i < files.count; ++i) {
        const char* path = files.paths[i];
        if (!IsFileExtension(path, ".odmod")) continue;
        struct stat st {};
        if (::stat(path, &st) != 0) continue;
        fingerprint += GetFileName(path);
        fingerprint += ':';
        fingerprint += std::to_string(static_cast<long long>(st.st_mtime));
        fingerprint += ':';
        fingerprint += std::to_string(static_cast<long long>(st.st_size));
        fingerprint += ';';
    }
    UnloadDirectoryFiles(files);

    static std::string previous;
    static bool seeded = false;
    if (!seeded) {
        // The first poll establishes what "unchanged" looks like. Without
        // this the game would reload once on startup for no reason.
        seeded = true;
        previous = fingerprint;
        return false;
    }
    if (fingerprint == previous) return false;
    previous = fingerprint;
    return true;
}

bool focusRequested() {
    if (!viewEnabled()) return false;
    const char* path = devEnv("OD_DEV_VIEW");
    if (path == nullptr) return false;

    static double nextPoll = 0.0;
    if (now() < nextPoll) return false;
    nextPoll = now() + 0.2;

    // A file beside the frame, whose modification time is the whole message.
    // Nothing is read out of it, so there is no format to get wrong.
    const std::string ask = std::string(path) + ".focus";
    struct stat st {};
    if (::stat(ask.c_str(), &st) != 0) return false;

    static long long previous = 0;
    static bool seeded = false;
    const long long when = static_cast<long long>(st.st_mtime);
    if (!seeded) {
        seeded = true;
        previous = when;
        return false;
    }
    if (when == previous) return false;
    previous = when;
    return true;
}

void publishFrame() {
    if (!viewEnabled()) return;

    static double nextFrame = 0.0;
    if (now() < nextFrame) return;
    nextFrame = now() + kViewInterval;

    void* base = sharedMapping();
    if (base == nullptr) return;

    // FLUSH FIRST. raylib accumulates draw calls in a vertex batch and only
    // sends them to the framebuffer at EndDrawing, so reading the screen
    // before that reads whatever was there when the batch last went out --
    // which, this early in a frame, is the background colour and nothing
    // else. The first version of this published a perfectly valid black
    // rectangle twelve times a second.
    rlDrawRenderBatchActive();

    Image shot = LoadImageFromScreen();
    if (shot.data == nullptr) return;

    // NEAREST NEIGHBOUR, not the bicubic one. ImageResize on two million
    // pixels is tens of milliseconds of the GAME's frame, every frame it
    // publishes -- the view was smooth and the game was not. At a sixth of
    // the size nobody can tell which filter ran.
    ImageResizeNN(&shot, devshared::kViewWidth, devshared::kViewHeight);

    auto* head = static_cast<devshared::FrameHeader*>(base);
    auto* pixels = static_cast<unsigned char*>(base) + devshared::kPixelOffset;
    head->screenWidth = static_cast<unsigned>(GetScreenWidth());
    head->screenHeight = static_cast<unsigned>(GetScreenHeight());

    // A seqlock. Odd means "being written"; a reader that sees the same even
    // number either side of its copy read a whole frame. Cheaper than any
    // lock and, more to the point, a reader that dies mid-read cannot wedge
    // the game.
    head->sequence += 1;  // now odd
    __sync_synchronize();
    std::memcpy(pixels, shot.data, devshared::kPixelBytes);
    __sync_synchronize();
    head->sequence += 1;  // even again

    UnloadImage(shot);
}

}  // namespace devlink

// ---------------------------------------------------------------- input ----

namespace devinput {
namespace {

struct State {
    bool driving = false;
    int mouseX = 0;
    int mouseY = 0;
    unsigned buttons = 0;
    unsigned previousButtons = 0;
    float wheel = 0.0f;
    std::vector<int> keys;
    std::vector<int> previousKeys;
    std::vector<int> chars;
    unsigned lastSequence = 0;
    /// Frames since the editor last said anything. It stops sending when the
    /// pointer leaves its view, and without this the game would be stuck with
    /// whatever was held down at that moment.
    int quiet = 0;
};

State& state() {
    static State s;
    return s;
}

bool holds(const std::vector<int>& v, int key) {
    return std::find(v.begin(), v.end(), key) != v.end();
}

}  // namespace

void poll() {
    if (!devlink::viewEnabled()) return;
    State& s = state();

    s.previousButtons = s.buttons;
    s.previousKeys = s.keys;
    s.chars.clear();

    void* base = sharedMapping();
    if (base == nullptr) {
        s.driving = false;
        return;
    }

    auto* in = reinterpret_cast<devshared::InputBlock*>(static_cast<unsigned char*>(base) +
                                                       devshared::kInputOffset);
    if (in->magic != devshared::kInputMagic) {
        s.driving = false;
        return;
    }

    if (in->sequence == s.lastSequence) {
        // Nothing new. After a few frames of silence the editor has stopped
        // driving -- released, not held.
        if (++s.quiet > 6) {
            s.driving = false;
            s.buttons = 0;
            s.keys.clear();
            s.wheel = 0.0f;
        }
        return;
    }
    s.lastSequence = in->sequence;
    s.quiet = 0;

    s.driving = in->driving != 0;
    s.mouseX = in->mouseX;
    s.mouseY = in->mouseY;
    s.buttons = in->buttons;
    s.wheel = in->wheel;

    s.keys.clear();
    const unsigned keyCount = std::min<unsigned>(in->keyCount, devshared::kMaxKeys);
    for (unsigned i = 0; i < keyCount; ++i) s.keys.push_back(in->keys[i]);

    const unsigned charCount = std::min<unsigned>(in->charCount, devshared::kMaxChars);
    for (unsigned i = 0; i < charCount; ++i) s.chars.push_back(in->chars[i]);

    if (!s.driving) {
        s.buttons = 0;
        s.keys.clear();
        s.wheel = 0.0f;
    }
}

bool driving() { return devlink::viewEnabled() && state().driving; }

namespace {
/// A development aid for the development aid. OD_DEV_VIEW_DEBUG=1 makes the
/// shim say, once a second, what it thinks it is being told and how often it
/// is being asked -- which is the difference between "the editor is not
/// sending" and "the game is not asking".
void trace() {
    static const bool on = std::getenv("OD_DEV_VIEW_DEBUG") != nullptr;
    if (!on) return;
    static double next = 0.0;
    static long asks = 0;
    ++asks;
    const double t = GetTime();
    if (t < next) return;
    next = t + 1.0;
    const State& s = state();
    std::fprintf(stderr,
                 "[devinput] driving=%d seq=%u mouse=%d,%d buttons=%u keys=%zu asks=%ld\n",
                 s.driving ? 1 : 0, s.lastSequence, s.mouseX, s.mouseY, s.buttons,
                 s.keys.size(), asks);
    std::fflush(stderr);
    asks = 0;
}
}  // namespace

// Every one of these falls through to raylib when the editor is not driving,
// so a build with the shim compiled in behaves exactly like one without it
// until something on the other end starts sending.

bool keyDown(int key) {
    if (driving() && holds(state().keys, key)) return true;
    return IsKeyDown(key);
}

bool keyPressed(int key) {
    const State& s = state();
    if (driving() && holds(s.keys, key) && !holds(s.previousKeys, key)) return true;
    return IsKeyPressed(key);
}

bool keyReleased(int key) {
    const State& s = state();
    if (driving() && !holds(s.keys, key) && holds(s.previousKeys, key)) return true;
    return IsKeyReleased(key);
}

bool mouseDown(int button) {
    if (driving() && (state().buttons & (1u << button)) != 0u) return true;
    return IsMouseButtonDown(button);
}

bool mousePressed(int button) {
    const State& s = state();
    const unsigned bit = 1u << button;
    if (driving() && (s.buttons & bit) != 0u && (s.previousButtons & bit) == 0u) return true;
    return IsMouseButtonPressed(button);
}

bool mouseReleased(int button) {
    const State& s = state();
    const unsigned bit = 1u << button;
    if (driving() && (s.buttons & bit) == 0u && (s.previousButtons & bit) != 0u) return true;
    return IsMouseButtonReleased(button);
}

Vector2 mousePosition() {
    trace();
    // REPLACED, not merged: two cursors cannot both be where the mouse is, and
    // while the editor is driving, its pointer is the one being aimed with.
    if (driving()) {
        return Vector2{static_cast<float>(state().mouseX), static_cast<float>(state().mouseY)};
    }
    return GetMousePosition();
}

float wheelMove() {
    if (driving() && state().wheel != 0.0f) return state().wheel;
    return GetMouseWheelMove();
}

int charPressed() {
    State& s = state();
    if (driving() && !s.chars.empty()) {
        const int c = s.chars.front();
        s.chars.erase(s.chars.begin());
        return c;
    }
    return GetCharPressed();
}

}  // namespace devinput
