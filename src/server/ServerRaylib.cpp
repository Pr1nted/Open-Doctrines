// raylib's API, for a machine with no graphics card.
//
// WHY THIS FILE EXISTS
//
// A dedicated server has to run on a VPS: no GPU, no display, no X11, often no
// libGL at all. The game cannot do that today. Game::init() calls InitWindow()
// in EVERY mode -- including --simulate -- and raylib's own failure path says
// so in as many words: "The graphics driver did not provide an OpenGL 3.3
// context." A server that refuses to boot on the machines servers actually run
// on is not a server.
//
// The alternative to this file was a second copy of the rules, and that is the
// one outcome worth any amount of work to avoid: the host is the authority, so
// a server whose combat maths drifted from the client's would not be a bug you
// could see, it would be a game that disagreed with itself.
//
// SO THE SERVER COMPILES THE SAME SOURCE AND LINKS THIS INSTEAD OF libraylib.
//
// raylib.h is included for real -- the types, the enums, the prototypes are all
// genuine, so Image, Color and Rectangle mean exactly what they mean in the
// client. Only the implementations are ours. Two consequences worth stating:
//
//   - There is no GL, no GLFW, no windowing library and no audio device linked
//     into the server at all. Not stubbed at runtime: absent from the binary.
//   - The linker is the check. Call a raylib function that is not implemented
//     here and the server fails to LINK, naming it. Nothing can silently reach
//     the graphics layer at runtime, because there is no graphics layer to
//     reach.
//
// WHAT IS REAL AND WHAT IS NOT
//
// Real: everything the simulation reads data through. Image loading and pixel
// access (provinces.png IS the province table -- see Province::colorToId), file
// I/O, directory listing, text formatting, colour maths, time and the RNG.
// These are not graphics; they are how the game reads its own content, and the
// server needs every one of them to be correct.
//
// No-ops: drawing, textures, shaders, windows, input, audio. The server
// compiles Game_Render.cpp and Game_UI.cpp because the simulation calls into
// them, but every draw lands here and returns. That is deliberate: excluding
// those files would mean the server linking a different set of rules than the
// client, which is the drift this file exists to prevent.
//
// THE RNG IS NOT A DETAIL
//
// Turn logic calls rand() directly for combat rolls, and raylib's InitWindow
// normally seeds it. Nothing here calls InitWindow, so the server seeds it
// itself -- see odServerSeedRng(). Without that every server run would resolve
// the same combats identically from a fixed seed, forever.

#include "raylib.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// NOGDI and NOUSER BEFORE windows.h, and they are not optional here.
//
// windows.h pulls in wingdi.h, which declares a function called Rectangle().
// raylib.h -- included above, because this file IS raylib as far as the server
// is concerned -- declares a STRUCT called Rectangle. After windows.h the name
// resolves to the function, so `bool CheckCollisionPointRec(Vector2, Rectangle
// rec)` stops being a declaration and becomes a syntax error, and MSVC reports
// it as `'rec': undeclared identifier` several lines running, which points at
// everything except the cause.
//
// This is why the dedicated server has never once compiled on Windows: the
// error is invisible on macOS and Linux, and the server was only ever built
// inside the platform qualify job, which had already gone red for other
// reasons. It also took OrderValidationTest down with it.
//
// winuser.h is the same trap one step further on -- CloseWindow, ShowCursor,
// LoadImage and DrawText are all names raylib uses and this file defines -- so
// NOUSER goes with it rather than waiting to be discovered separately.
//
// Safe because this file wants exactly two things from windows.h, and both are
// kernel32: GetModuleFileNameW and WideCharToMultiByte.
#define NOGDI
#define NOUSER
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STB_IMAGE_WRITE_STATIC
#include "stb_image.h"
#include "stb_image_write.h"

namespace fs = std::filesystem;

// ─── Clock ──────────────────────────────────────────────────────────

namespace {

std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

double secondsSinceStart() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

/**
 * The frame step the server reports.
 *
 * Fixed rather than measured. GetFrameTime() feeds animation timers the server
 * never draws, and a real elapsed time here would make those timers run at
 * whatever speed the machine happened to manage -- including, on a fast idle
 * loop, fast enough to overflow the counters they drive. A constant keeps every
 * such timer well-defined and identical on every server.
 */
constexpr float kFrameStep = 1.0f / 60.0f;

/** Bytes per pixel for the formats the game actually asks for. */
int pixelBytes(int format) {
    switch (format) {
        case PIXELFORMAT_UNCOMPRESSED_GRAYSCALE:   return 1;
        case PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA:  return 2;
        case PIXELFORMAT_UNCOMPRESSED_R8G8B8:      return 3;
        case PIXELFORMAT_UNCOMPRESSED_R8G8B8A8:    return 4;
        default:                                   return 0;
    }
}

Color pixelAt(const Image& img, int x, int y) {
    Color c{0, 0, 0, 255};
    const int bpp = pixelBytes(img.format);
    if (!img.data || bpp == 0) return c;
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return c;
    const unsigned char* p = (const unsigned char*)img.data + ((size_t)y * img.width + x) * bpp;
    switch (bpp) {
        case 1: c = {p[0], p[0], p[0], 255}; break;
        case 2: c = {p[0], p[0], p[0], p[1]}; break;
        case 3: c = {p[0], p[1], p[2], 255}; break;
        default: c = {p[0], p[1], p[2], p[3]}; break;
    }
    return c;
}

}  // namespace

/**
 * Seed the C RNG the way InitWindow would have.
 *
 * Called once by the server's main(). See the header note: turn logic rolls
 * combat with rand(), and nothing else in a server process ever seeds it.
 */
extern "C" void odServerSeedRng(unsigned int seed) { srand(seed); }

// ─── Images ─────────────────────────────────────────────────────────
//
// Always decoded to RGBA8. Every caller in the game formats to
// PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 immediately and then reads (Color*)data,
// so producing anything else would only add a conversion nobody wants.

Image LoadImageFromMemory(const char* fileType, const unsigned char* fileData, int dataSize) {
    (void)fileType;
    Image img{};
    if (!fileData || dataSize <= 0) return img;
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load_from_memory(fileData, dataSize, &w, &h, &ch, 4);
    if (!px) return img;
    img.data = px;
    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return img;
}

Image LoadImage(const char* fileName) {
    Image img{};
    if (!fileName) return img;
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(fileName, &w, &h, &ch, 4);
    if (!px) return img;
    img.data = px;
    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return img;
}

void UnloadImage(Image image) { free(image.data); }

void ImageFormat(Image* image, int newFormat) {
    if (!image || !image->data) return;
    const int from = pixelBytes(image->format), to = pixelBytes(newFormat);
    if (from == 0 || to == 0 || from == to) {
        if (from && to) image->format = newFormat;
        return;
    }
    const size_t count = (size_t)image->width * image->height;
    auto* out = (unsigned char*)malloc(count * to);
    if (!out) return;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* s = (const unsigned char*)image->data + i * from;
        unsigned char r = s[0], g = s[0], b = s[0], a = 255;
        if (from == 2) { a = s[1]; }
        else if (from >= 3) { g = s[1]; b = s[2]; if (from == 4) a = s[3]; }
        unsigned char* d = out + i * to;
        if (to == 1) { d[0] = (unsigned char)((r * 30 + g * 59 + b * 11) / 100); }
        else if (to == 2) { d[0] = (unsigned char)((r * 30 + g * 59 + b * 11) / 100); d[1] = a; }
        else { d[0] = r; d[1] = g; d[2] = b; if (to == 4) d[3] = a; }
    }
    free(image->data);
    image->data = out;
    image->format = newFormat;
}

Image ImageCopy(Image image) {
    Image out = image;
    const int bpp = pixelBytes(image.format);
    out.data = nullptr;
    if (!image.data || bpp == 0) return out;
    const size_t bytes = (size_t)image.width * image.height * bpp;
    out.data = malloc(bytes);
    if (out.data) memcpy(out.data, image.data, bytes);
    return out;
}

void ImageResizeNN(Image* image, int newWidth, int newHeight) {
    if (!image || !image->data || newWidth <= 0 || newHeight <= 0) return;
    const int bpp = pixelBytes(image->format);
    if (!bpp) return;
    auto* out = (unsigned char*)malloc((size_t)newWidth * newHeight * bpp);
    if (!out) return;
    for (int y = 0; y < newHeight; ++y) {
        const int sy = (int)((long long)y * image->height / newHeight);
        for (int x = 0; x < newWidth; ++x) {
            const int sx = (int)((long long)x * image->width / newWidth);
            memcpy(out + ((size_t)y * newWidth + x) * bpp,
                   (unsigned char*)image->data + ((size_t)sy * image->width + sx) * bpp, bpp);
        }
    }
    free(image->data);
    image->data = out;
    image->width = newWidth;
    image->height = newHeight;
}

// Box-filtered rather than nearest, which is what raylib does and what the
// thumbnail path expects; a nearest-neighbour "resize" would alias every
// coastline it touched.
void ImageResize(Image* image, int newWidth, int newHeight) {
    if (!image || !image->data || newWidth <= 0 || newHeight <= 0) return;
    ImageFormat(image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const int ow = image->width, oh = image->height;
    auto* out = (unsigned char*)malloc((size_t)newWidth * newHeight * 4);
    if (!out) return;
    for (int y = 0; y < newHeight; ++y) {
        const int y0 = (int)((long long)y * oh / newHeight);
        const int y1 = (int)(((long long)y + 1) * oh / newHeight) > y0
                           ? (int)(((long long)y + 1) * oh / newHeight) : y0 + 1;
        for (int x = 0; x < newWidth; ++x) {
            const int x0 = (int)((long long)x * ow / newWidth);
            const int x1 = (int)(((long long)x + 1) * ow / newWidth) > x0
                               ? (int)(((long long)x + 1) * ow / newWidth) : x0 + 1;
            unsigned long long acc[4] = {0, 0, 0, 0};
            unsigned long long n = 0;
            for (int sy = y0; sy < y1 && sy < oh; ++sy)
                for (int sx = x0; sx < x1 && sx < ow; ++sx) {
                    const unsigned char* p =
                        (unsigned char*)image->data + ((size_t)sy * ow + sx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                    ++n;
                }
            unsigned char* d = out + ((size_t)y * newWidth + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = (unsigned char)(n ? acc[c] / n : 0);
        }
    }
    free(image->data);
    image->data = out;
    image->width = newWidth;
    image->height = newHeight;
}

Image GenImageColor(int width, int height, Color color) {
    Image img{};
    if (width <= 0 || height <= 0) return img;
    const size_t count = (size_t)width * height;
    auto* px = (unsigned char*)malloc(count * 4);
    if (!px) return img;
    for (size_t i = 0; i < count; ++i) {
        px[i * 4 + 0] = color.r; px[i * 4 + 1] = color.g;
        px[i * 4 + 2] = color.b; px[i * 4 + 3] = color.a;
    }
    img.data = px;
    img.width = width;
    img.height = height;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return img;
}

Color GetImageColor(Image image, int x, int y) { return pixelAt(image, x, y); }

Color* LoadImageColors(Image image) {
    const size_t count = (size_t)image.width * image.height;
    if (!image.data || count == 0) return nullptr;
    auto* out = (Color*)malloc(count * sizeof(Color));
    if (!out) return nullptr;
    for (size_t i = 0; i < count; ++i)
        out[i] = pixelAt(image, (int)(i % image.width), (int)(i / image.width));
    return out;
}

void UnloadImageColors(Color* colors) { free(colors); }

void ImageDrawPixel(Image* dst, int posX, int posY, Color color) {
    if (!dst || !dst->data) return;
    const int bpp = pixelBytes(dst->format);
    if (bpp < 3 || posX < 0 || posY < 0 || posX >= dst->width || posY >= dst->height) return;
    unsigned char* p = (unsigned char*)dst->data + ((size_t)posY * dst->width + posX) * bpp;
    p[0] = color.r; p[1] = color.g; p[2] = color.b;
    if (bpp == 4) p[3] = color.a;
}

void ImageDrawRectangle(Image* dst, int posX, int posY, int width, int height, Color color) {
    for (int y = posY; y < posY + height; ++y)
        for (int x = posX; x < posX + width; ++x) ImageDrawPixel(dst, x, y, color);
}

bool ExportImage(Image image, const char* fileName) {
    if (!image.data || !fileName) return false;
    Image rgba = ImageCopy(image);
    ImageFormat(&rgba, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const int ok = stbi_write_png(fileName, rgba.width, rgba.height, 4, rgba.data, rgba.width * 4);
    free(rgba.data);
    return ok != 0;
}

// ─── Files ──────────────────────────────────────────────────────────
//
// Plain stdio and std::filesystem. The Android asset path raylib carries is not
// reproduced: a dedicated server has an ordinary filesystem even on Android,
// where it runs from app storage rather than out of the APK.

unsigned char* LoadFileData(const char* fileName, int* dataSize) {
    if (dataSize) *dataSize = 0;
    if (!fileName) return nullptr;
    FILE* f = fopen(fileName, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return nullptr; }
    auto* buf = (unsigned char*)malloc((size_t)size ? (size_t)size : 1);
    if (!buf) { fclose(f); return nullptr; }
    const size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (dataSize) *dataSize = (int)got;
    return buf;
}

void UnloadFileData(unsigned char* data) { free(data); }

bool SaveFileData(const char* fileName, void* data, int dataSize) {
    if (!fileName || dataSize < 0) return false;
    FILE* f = fopen(fileName, "wb");
    if (!f) return false;
    const size_t put = dataSize ? fwrite(data, 1, (size_t)dataSize, f) : 0;
    fclose(f);
    return put == (size_t)dataSize;
}

char* LoadFileText(const char* fileName) {
    int size = 0;
    unsigned char* raw = LoadFileData(fileName, &size);
    if (!raw) return nullptr;
    auto* text = (char*)realloc(raw, (size_t)size + 1);
    if (!text) { free(raw); return nullptr; }
    text[size] = '\0';
    return text;
}

void UnloadFileText(char* text) { free(text); }

bool FileExists(const char* fileName) {
    std::error_code ec;
    return fileName && fs::exists(fileName, ec) && !fs::is_directory(fileName, ec);
}

bool DirectoryExists(const char* dirPath) {
    std::error_code ec;
    return dirPath && fs::is_directory(dirPath, ec);
}

bool IsPathFile(const char* path) { return FileExists(path); }

/**
 * Directory listing, for the save, map and mod browsers.
 *
 * The returned paths are owned by this call and freed by UnloadDirectoryFiles,
 * matching raylib's contract -- the browsers hold them across frames.
 */
FilePathList LoadDirectoryFiles(const char* dirPath) {
    FilePathList list{};
    std::error_code ec;
    if (!dirPath || !fs::is_directory(dirPath, ec)) return list;
    std::vector<std::string> found;
    for (const auto& e : fs::directory_iterator(dirPath, ec)) found.push_back(e.path().string());
    if (found.empty()) return list;
    list.capacity = (unsigned int)found.size();
    list.count = (unsigned int)found.size();
    list.paths = (char**)malloc(found.size() * sizeof(char*));
    if (!list.paths) { list.capacity = list.count = 0; return list; }
    for (size_t i = 0; i < found.size(); ++i) {
        list.paths[i] = (char*)malloc(found[i].size() + 1);
        if (list.paths[i]) memcpy(list.paths[i], found[i].c_str(), found[i].size() + 1);
    }
    return list;
}

void UnloadDirectoryFiles(FilePathList files) {
    for (unsigned int i = 0; i < files.count; ++i) free(files.paths[i]);
    free(files.paths);
}

/**
 * Where this executable lives, with a trailing separator.
 *
 * Real, not a stub, because the data directory is found relative to it -- the
 * server probes for <exe>/data and <exe>/../data exactly as Game::init() does
 * on the client. Returning nothing here made the server unable to find any map
 * at all, reported as "no map called '1914'", which reads like a missing file
 * rather than a missing path.
 */
const char* GetApplicationDirectory(void) {
    static char dir[4096];
    if (dir[0]) return dir;

    std::string exe;
#if defined(_WIN32)
    {
        wchar_t buf[4096];
        const DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
        if (n > 0) {
            const int need = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
            exe.resize((size_t)need);
            WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, exe.data(), need, nullptr, nullptr);
        }
    }
#elif defined(__APPLE__)
    {
        char buf[4096];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0) exe = buf;
    }
#else
    {
        char buf[4096];
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = 0; exe = buf; }
    }
#endif

    std::error_code ec;
    std::string parent = exe.empty() ? fs::current_path(ec).string()
                                     : fs::path(exe).parent_path().string();
    if (parent.empty() || (parent.back() != '/' && parent.back() != '\\')) parent += '/';
    snprintf(dir, sizeof(dir), "%s", parent.c_str());
    return dir;
}

// ─── Text ───────────────────────────────────────────────────────────

/**
 * raylib's TextFormat, ring buffer and all.
 *
 * The rotation is not incidental: callers pass several TextFormat results to
 * one function (`DrawText(TextFormat(...), ...)` beside another), and a single
 * static buffer would have the second overwrite the first. Same buffer count
 * and size as raylib so behaviour matches the client exactly.
 */
const char* TextFormat(const char* text, ...) {
    static constexpr int kBuffers = 4;
    static constexpr int kLength = 1024;
    static char buffers[kBuffers][kLength]{};
    static int index = 0;

    char* out = buffers[index];
    memset(out, 0, kLength);
    va_list args;
    va_start(args, text);
    vsnprintf(out, kLength, text, args);
    va_end(args);
    index = (index + 1) % kBuffers;
    return out;
}

/**
 * Text width, estimated rather than measured.
 *
 * There is no font here and nothing is drawn, but the value is not free to be
 * nonsense: layout code divides by it and centres on it, so a zero would put a
 * division by zero in a code path the server does execute. raylib's default
 * font is fixed-width at roughly half its point size, which is what this is.
 */
int MeasureText(const char* text, int fontSize) {
    if (!text || !*text) return 0;
    return (int)(strlen(text) * (size_t)(fontSize > 0 ? fontSize : 10)) / 2;
}

Vector2 MeasureTextEx(Font font, const char* text, float fontSize, float spacing) {
    (void)font;
    const float w = (float)MeasureText(text, (int)fontSize) + spacing * (text ? strlen(text) : 0);
    return Vector2{w, fontSize};
}

// ─── Colour ─────────────────────────────────────────────────────────

Color ColorAlpha(Color color, float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return Color{color.r, color.g, color.b, (unsigned char)(255.0f * alpha)};
}

Color Fade(Color color, float alpha) { return ColorAlpha(color, alpha); }

Color GetColor(unsigned int hexValue) {
    return Color{(unsigned char)(hexValue >> 24), (unsigned char)((hexValue >> 16) & 0xFF),
                 (unsigned char)((hexValue >> 8) & 0xFF), (unsigned char)(hexValue & 0xFF)};
}

int ColorToInt(Color color) {
    return (int)(((unsigned int)color.r << 24) | ((unsigned int)color.g << 16) |
                 ((unsigned int)color.b << 8) | (unsigned int)color.a);
}

// ─── Collision ──────────────────────────────────────────────────────
//
// Real, not stubbed. These are pure geometry, the UI code that calls them runs
// on the server, and a hit test that always answered "yes" would have the
// server behaving as though something were permanently under a cursor.

bool CheckCollisionPointRec(Vector2 point, Rectangle rec) {
    return point.x >= rec.x && point.x < rec.x + rec.width &&
           point.y >= rec.y && point.y < rec.y + rec.height;
}

bool CheckCollisionPointCircle(Vector2 point, Vector2 center, float radius) {
    const float dx = point.x - center.x, dy = point.y - center.y;
    return dx * dx + dy * dy <= radius * radius;
}

bool CheckCollisionRecs(Rectangle rec1, Rectangle rec2) {
    return rec1.x < rec2.x + rec2.width && rec1.x + rec1.width > rec2.x &&
           rec1.y < rec2.y + rec2.height && rec1.y + rec1.height > rec2.y;
}

// ─── Time and randomness ────────────────────────────────────────────

double GetTime(void) { return secondsSinceStart(); }
float  GetFrameTime(void) { return kFrameStep; }
int    GetFPS(void) { return 60; }
void   SetTargetFPS(int) {}
void   WaitTime(double seconds) {
    if (seconds > 0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

void SetRandomSeed(unsigned int seed) { srand(seed); }

int GetRandomValue(int min, int max) {
    if (min > max) { const int t = min; min = max; max = t; }
    return min + (int)(rand() % (unsigned)(max - min + 1));
}

// ─── Window queries the simulation reads ────────────────────────────
//
// A server has no window, but code that runs on it still asks how big the
// screen is to lay panels out. A fixed, plausible size keeps that arithmetic
// well-defined; nothing is ever drawn into it.

int  GetScreenWidth(void)  { return 1920; }
int  GetScreenHeight(void) { return 1080; }
int  GetRenderWidth(void)  { return 1920; }
int  GetRenderHeight(void) { return 1080; }
bool IsWindowReady(void)   { return false; }
bool IsWindowResized(void) { return false; }
bool IsWindowFullscreen(void) { return false; }
bool IsWindowMinimized(void) { return false; }
bool IsWindowFocused(void) { return true; }

/**
 * True the moment the server has been told to stop.
 *
 * The game's loops are written as `while (!WindowShouldClose())`, so this is
 * how a headless run ends. odServerRequestStop() is what the console's `stop`
 * command and the signal handler call.
 */
namespace {
bool g_stopRequested = false;
}
extern "C" void odServerRequestStop(void) { g_stopRequested = true; }
extern "C" bool odServerStopRequested(void) { return g_stopRequested; }

bool WindowShouldClose(void) { return g_stopRequested; }

// ─── rlgl ───────────────────────────────────────────────────────────
//
// UiScale.h scales the whole UI by pushing a matrix (see rlgl.h). That is the
// only rlgl this project uses, and on a server it is three no-ops: there is no
// matrix stack because there is no renderer to hold one.
//
// Declared here rather than by including rlgl.h, which pulls in the GL loader.

extern "C" {
void rlPushMatrix(void);
void rlPopMatrix(void);
void rlScalef(float x, float y, float z);
}

void rlPushMatrix(void) {}
void rlPopMatrix(void) {}
void rlScalef(float, float, float) {}
