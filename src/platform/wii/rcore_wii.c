/**********************************************************************************************
*
*   rcore_wii - raylib's platform layer for the Nintendo Wii (libogc), for Open Doctrines
*
*   PLATFORM: Wii homebrew, started from the Homebrew Channel
*       - Screen: the video interface's external framebuffer (YUY2), 640x480 (NTSC/EURGB60)
*                 or 640x528 (PAL), double-buffered
*       - Pointer: the Wii Remote's IR sensor, as the mouse
*       - Buttons: A click, B right-click, + / - zoom (wheel), the d-pad as arrow keys,
*                  1 as Escape, 2 as Enter, HOME quits to the loader
*       - Keyboard: a USB keyboard if one is plugged in (libogc's wiikeyboard), for text
*       - Storage: the SD card (libfat), where the game, its data and saves live
*
*   HOW IT IS BUILT. As src/platform/dos/rcore_dos.c: raylib is built as PLATFORM=Memory
*   (rlsw, no GPU API) and this file is copied over platforms/rcore_memory.c (see OD_WII in
*   CMakeLists.txt). The Wii's GPU (GX) is not OpenGL, and rlsw draws the frame on the CPU;
*   this file only shows it.
*
*   BIG-ENDIAN. The Wii's CPU is a PowerPC, and rlsw hands its frame over as BYTES (B, G, R,
*   A when RLSW_FRAMEBUFFER_OUTPUT_BGRA, its default), so the frame is read a byte at a time
*   here, never as 32-bit words -- which would put the channels in the other order.
*
*   LICENSE: zlib/libpng, as raylib (this file started as raylib's rcore_memory.c,
*   Copyright (c) 2025-2026 Ramon Santamaria (@raysan5) and contributors).
*
**********************************************************************************************/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <gccore.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/keyboard.h>
#include <fat.h>
#include <ogc/lwp_watchdog.h>

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
typedef struct {
    unsigned char *frame;           // rlsw's frame, 4 bytes a pixel (see SRC_* below)
    GXRModeObj *mode;
    void *xfb[2];                   // the two external framebuffers, uncached addresses
    int current;                    // which one is being drawn into
    int width, height;              // the framebuffer's size
    int keyboard;                   // a USB keyboard driver is up
    u64 timeBase;
} PlatformData;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
extern CoreData CORE;

static PlatformData platform = { 0 };

int InitPlatform(void);
void ClosePlatform(void);

// rlsw's byte order (see BIG-ENDIAN above).
#if defined(RLSW_FRAMEBUFFER_OUTPUT_BGRA) && RLSW_FRAMEBUFFER_OUTPUT_BGRA
    #define SRC_R(p) ((p)[2])
    #define SRC_B(p) ((p)[0])
#else
    #define SRC_R(p) ((p)[0])
    #define SRC_B(p) ((p)[2])
#endif
#define SRC_G(p) ((p)[1])

//----------------------------------------------------------------------------------
// The cursor: the video interface draws none, so it is painted into each frame.
//----------------------------------------------------------------------------------
static const char *wiiCursor[19] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X..........X", "X......XXXXX", "X...X..X    ", "X..XX..X    ",
    "X.X  X..X   ", "XX   X..X   ", "      X..X  ", "       XX   "
};

static void PaintCursor(void)
{
    if (CORE.Input.Mouse.cursorHidden) return;
    const int cx = (int)CORE.Input.Mouse.currentPosition.x, cy = (int)CORE.Input.Mouse.currentPosition.y;
    for (int y = 0; y < 19; y++)
    {
        if ((cy + y < 0) || (cy + y >= platform.height)) continue;
        for (int x = 0; x < 12; x++)
        {
            const char c = wiiCursor[y][x];
            if ((c == ' ') || (cx + x < 0) || (cx + x >= platform.width)) continue;
            unsigned char *p = platform.frame + ((size_t)(cy + y)*platform.width + cx + x)*4;
            const unsigned char v = (c == 'X')? 0 : 255;
            p[0] = p[1] = p[2] = v;
        }
    }
}

//----------------------------------------------------------------------------------
// The frame, as YUY2: two pixels to a 32-bit word, Y0 Cb Y1 Cr (ITU-R BT.601).
//----------------------------------------------------------------------------------
static void PresentFrame(void)
{
    uint32_t *out = (uint32_t *)platform.xfb[platform.current];
    const unsigned char *src = platform.frame;
    const int pairs = platform.width/2;

    for (int y = 0; y < platform.height; y++)
    {
        for (int x = 0; x < pairs; x++, src += 8)
        {
            const int r0 = SRC_R(src), g0 = SRC_G(src), b0 = SRC_B(src);
            const int r1 = SRC_R(src + 4), g1 = SRC_G(src + 4), b1 = SRC_B(src + 4);
            const int y0 = ((66*r0 + 129*g0 + 25*b0 + 128) >> 8) + 16;
            const int y1 = ((66*r1 + 129*g1 + 25*b1 + 128) >> 8) + 16;
            const int r = (r0 + r1) >> 1, g = (g0 + g1) >> 1, b = (b0 + b1) >> 1;
            const int cb = ((-38*r - 74*g + 112*b + 128) >> 8) + 128;
            const int cr = ((112*r - 94*g - 18*b + 128) >> 8) + 128;
            *out++ = ((uint32_t)y0 << 24) | ((uint32_t)cb << 16) | ((uint32_t)y1 << 8) | (uint32_t)cr;
        }
    }

    VIDEO_SetNextFramebuffer(platform.xfb[platform.current]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    platform.current ^= 1;
}

//----------------------------------------------------------------------------------
// Keyboard: USB HID usage codes -> raylib keys
//----------------------------------------------------------------------------------
static int WiiUsbKey(int code)
{
    if ((code >= 0x04) && (code <= 0x1D)) return KEY_A + (code - 0x04);
    if ((code >= 0x1E) && (code <= 0x26)) return KEY_ONE + (code - 0x1E);
    if (code == 0x27) return KEY_ZERO;
    if ((code >= 0x3A) && (code <= 0x45)) return KEY_F1 + (code - 0x3A);
    switch (code)
    {
        case 0x28: return KEY_ENTER;
        case 0x29: return KEY_ESCAPE;
        case 0x2A: return KEY_BACKSPACE;
        case 0x2B: return KEY_TAB;
        case 0x2C: return KEY_SPACE;
        case 0x2D: return KEY_MINUS;
        case 0x2E: return KEY_EQUAL;
        case 0x2F: return KEY_LEFT_BRACKET;
        case 0x30: return KEY_RIGHT_BRACKET;
        case 0x31: return KEY_BACKSLASH;
        case 0x33: return KEY_SEMICOLON;
        case 0x34: return KEY_APOSTROPHE;
        case 0x35: return KEY_GRAVE;
        case 0x36: return KEY_COMMA;
        case 0x37: return KEY_PERIOD;
        case 0x38: return KEY_SLASH;
        case 0x49: return KEY_INSERT;
        case 0x4A: return KEY_HOME;
        case 0x4B: return KEY_PAGE_UP;
        case 0x4C: return KEY_DELETE;
        case 0x4D: return KEY_END;
        case 0x4E: return KEY_PAGE_DOWN;
        case 0x4F: return KEY_RIGHT;
        case 0x50: return KEY_LEFT;
        case 0x51: return KEY_DOWN;
        case 0x52: return KEY_UP;
        case 0xE0: return KEY_LEFT_CONTROL;
        case 0xE1: return KEY_LEFT_SHIFT;
        case 0xE2: return KEY_LEFT_ALT;
        case 0xE4: return KEY_RIGHT_CONTROL;
        case 0xE5: return KEY_RIGHT_SHIFT;
        case 0xE6: return KEY_RIGHT_ALT;
        default:   return 0;
    }
}

static void KeyDown(int key)
{
    if ((key <= 0) || (key >= MAX_KEYBOARD_KEYS)) return;
    if (CORE.Input.Keyboard.currentKeyState[key]) { CORE.Input.Keyboard.keyRepeatInFrame[key] = 1; return; }
    CORE.Input.Keyboard.currentKeyState[key] = 1;
    if (CORE.Input.Keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
        CORE.Input.Keyboard.keyPressedQueue[CORE.Input.Keyboard.keyPressedQueueCount++] = key;
}

static void KeyUp(int key)
{
    if ((key > 0) && (key < MAX_KEYBOARD_KEYS)) CORE.Input.Keyboard.currentKeyState[key] = 0;
}

static void KeyChar(int ch)
{
    if ((ch >= 32) && (ch != 127) && (CORE.Input.Keyboard.charPressedQueueCount < MAX_CHAR_PRESSED_QUEUE))
        CORE.Input.Keyboard.charPressedQueue[CORE.Input.Keyboard.charPressedQueueCount++] = ch;
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Window and Graphics Device
//----------------------------------------------------------------------------------
bool WindowShouldClose(void)
{
    if (CORE.Window.ready) return CORE.Window.shouldClose;
    else return true;
}

// The whole television is the window.
void ToggleFullscreen(void) { }
void ToggleBorderlessWindowed(void) { }
void MaximizeWindow(void) { }
void MinimizeWindow(void) { }
void RestoreWindow(void) { }
void SetWindowState(unsigned int flags) { CORE.Window.flags |= (flags & FLAG_FULLSCREEN_MODE); }
void ClearWindowState(unsigned int flags) { (void)flags; }
void SetWindowIcon(Image image) { (void)image; }
void SetWindowIcons(Image *images, int count) { (void)images; (void)count; }
void SetWindowTitle(const char *title) { CORE.Window.title = title; }
void SetWindowPosition(int x, int y) { (void)x; (void)y; }
void SetWindowMonitor(int monitor) { (void)monitor; }
void SetWindowMinSize(int width, int height) { CORE.Window.screenMin.width = width; CORE.Window.screenMin.height = height; }
void SetWindowMaxSize(int width, int height) { CORE.Window.screenMax.width = width; CORE.Window.screenMax.height = height; }
void SetWindowSize(int width, int height) { (void)width; (void)height; }
void SetWindowOpacity(float opacity) { (void)opacity; }
void SetWindowFocused(void) { }
void *GetWindowHandle(void) { return NULL; }

int GetMonitorCount(void) { return 1; }
int GetCurrentMonitor(void) { return 0; }
Vector2 GetMonitorPosition(int monitor) { (void)monitor; return (Vector2){ 0, 0 }; }
int GetMonitorWidth(int monitor) { (void)monitor; return platform.width; }
int GetMonitorHeight(int monitor) { (void)monitor; return platform.height; }
int GetMonitorPhysicalWidth(int monitor) { (void)monitor; return 0; }
int GetMonitorPhysicalHeight(int monitor) { (void)monitor; return 0; }
int GetMonitorRefreshRate(int monitor) { (void)monitor; return 60; }
const char *GetMonitorName(int monitor) { (void)monitor; return "TV"; }
Vector2 GetWindowPosition(void) { return (Vector2){ 0, 0 }; }
Vector2 GetWindowScaleDPI(void) { return (Vector2){ 1.0f, 1.0f }; }

void SetClipboardText(const char *text) { (void)text; }
const char *GetClipboardText(void) { return NULL; }
Image GetClipboardImage(void) { Image image = { 0 }; return image; }

void ShowCursor(void) { CORE.Input.Mouse.cursorHidden = false; }
void HideCursor(void) { CORE.Input.Mouse.cursorHidden = true; }
void EnableCursor(void) { CORE.Input.Mouse.cursorHidden = false; CORE.Input.Mouse.cursorLocked = false; }
void DisableCursor(void) { CORE.Input.Mouse.cursorHidden = true; CORE.Input.Mouse.cursorLocked = true; }

void SwapScreenBuffer(void)
{
    rlCopyFramebuffer(0, 0, CORE.Window.render.width, CORE.Window.render.height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, platform.frame);
    PaintCursor();
    PresentFrame();
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Misc
//----------------------------------------------------------------------------------
double GetTime(void)
{
    return (double)ticks_to_microsecs(gettime() - platform.timeBase)*1e-6;
}

// No browser on a Wii. The game shows the address instead.
void OpenURL(const char *url)
{
    TRACELOG(LOG_INFO, "SYSTEM: no browser on this platform: %s", url);
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Inputs
//----------------------------------------------------------------------------------
int SetGamepadMappings(const char *mappings) { (void)mappings; return 0; }
void SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration)
{
    (void)gamepad; (void)leftMotor; (void)rightMotor; (void)duration;
}

void SetMousePosition(int x, int y)
{
    CORE.Input.Mouse.currentPosition = (Vector2){ (float)x, (float)y };
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
}

void SetMouseCursor(int cursor) { CORE.Input.Mouse.cursor = cursor; }
const char *GetKeyName(int key) { (void)key; return ""; }

// The Remote's buttons, as keys and mouse buttons the game already understands.
static void WiiButtons(u32 held, u32 down)
{
    CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_LEFT] = (held & WPAD_BUTTON_A)? 1 : 0;
    CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_RIGHT] = (held & WPAD_BUTTON_B)? 1 : 0;
    if (down & WPAD_BUTTON_PLUS) CORE.Input.Mouse.currentWheelMove.y += 1.0f;
    if (down & WPAD_BUTTON_MINUS) CORE.Input.Mouse.currentWheelMove.y -= 1.0f;

    static const struct { u32 button; int key; } map[] = {
        { WPAD_BUTTON_UP, KEY_UP }, { WPAD_BUTTON_DOWN, KEY_DOWN },
        { WPAD_BUTTON_LEFT, KEY_LEFT }, { WPAD_BUTTON_RIGHT, KEY_RIGHT },
        { WPAD_BUTTON_1, KEY_ESCAPE }, { WPAD_BUTTON_2, KEY_ENTER },
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++)
    {
        if (held & map[i].button) KeyDown(map[i].key);
        else if (!platform.keyboard || !CORE.Input.Keyboard.currentKeyState[map[i].key]) KeyUp(map[i].key);
    }
    if (down & WPAD_BUTTON_HOME) exit(0);     // back to the Homebrew Channel
}

void PollInputEvents(void)
{
#if SUPPORT_GESTURES_SYSTEM
    UpdateGestures();
#endif
    CORE.Input.Keyboard.keyPressedQueueCount = 0;
    CORE.Input.Keyboard.charPressedQueueCount = 0;
    CORE.Input.Mouse.currentWheelMove.x = 0;
    CORE.Input.Mouse.currentWheelMove.y = 0;
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
    CORE.Input.Gamepad.lastButtonPressed = 0;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.Input.Touch.previousTouchState[i] = CORE.Input.Touch.currentTouchState[i];
    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++)
    {
        CORE.Input.Keyboard.previousKeyState[i] = CORE.Input.Keyboard.currentKeyState[i];
        CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;
    }
    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) CORE.Input.Mouse.previousButtonState[i] = CORE.Input.Mouse.currentButtonState[i];

    WPAD_ScanPads();
    WPADData *wd = WPAD_Data(0);
    if ((wd != NULL) && wd->ir.valid)
    {
        CORE.Input.Mouse.currentPosition.x = wd->ir.x;
        CORE.Input.Mouse.currentPosition.y = wd->ir.y;
        CORE.Input.Touch.position[0] = CORE.Input.Mouse.currentPosition;
    }
    WiiButtons(WPAD_ButtonsHeld(0), WPAD_ButtonsDown(0));

    if (platform.keyboard)
    {
        keyboard_event ev;
        while (KEYBOARD_GetEvent(&ev) > 0)
        {
            if (ev.type == KEYBOARD_PRESSED) { KeyDown(WiiUsbKey(ev.keycode)); KeyChar(ev.symbol); }
            else if (ev.type == KEYBOARD_RELEASED) KeyUp(WiiUsbKey(ev.keycode));
        }
    }

    if ((CORE.Input.Keyboard.exitKey != KEY_NULL) && CORE.Input.Keyboard.currentKeyState[CORE.Input.Keyboard.exitKey] &&
        !CORE.Input.Keyboard.previousKeyState[CORE.Input.Keyboard.exitKey]) CORE.Window.shouldClose = true;
}

//----------------------------------------------------------------------------------
// Module Internal Functions Definition
//----------------------------------------------------------------------------------
int InitPlatform(void)
{
    if (rlGetVersion() != RL_OPENGL_SOFTWARE)
    {
        TRACELOG(LOG_FATAL, "PLATFORM: the Wii build needs the software renderer (GRAPHICS_API_OPENGL_SOFTWARE)");
        return -1;
    }

    VIDEO_Init();
    platform.mode = VIDEO_GetPreferredMode(NULL);
    platform.width = platform.mode->fbWidth;
    platform.height = platform.mode->xfbHeight;
    for (int i = 0; i < 2; i++)
    {
        platform.xfb[i] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(platform.mode));
        VIDEO_ClearFrameBuffer(platform.mode, platform.xfb[i], COLOR_BLACK);
    }
    VIDEO_Configure(platform.mode);
    VIDEO_SetNextFramebuffer(platform.xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (platform.mode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    platform.current = 1;

    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, platform.width, platform.height);
    platform.keyboard = (KEYBOARD_Init(NULL) >= 0);

    // The SD card, and the game's folder on it as the working directory: the
    // Homebrew Channel starts us with argv[0] = sd:/apps/<folder>/boot.dol, and
    // the game finds data/ beside its executable (see odApplicationDirectory).
    if (!fatInitDefault()) TRACELOG(LOG_WARNING, "WII: no SD card could be mounted");

    CORE.Window.screen.width = platform.width;
    CORE.Window.screen.height = platform.height;
    CORE.Window.display.width = platform.width;
    CORE.Window.display.height = platform.height;
    CORE.Window.render.width = platform.width;
    CORE.Window.render.height = platform.height;
    CORE.Window.currentFbo.width = platform.width;
    CORE.Window.currentFbo.height = platform.height;
    CORE.Window.flags |= FLAG_FULLSCREEN_MODE;

    platform.frame = (unsigned char *)RL_CALLOC((size_t)platform.width*platform.height, 4);
    if (platform.frame == NULL)
    {
        TRACELOG(LOG_FATAL, "PLATFORM: out of memory for a %ix%i frame", platform.width, platform.height);
        return -1;
    }

    platform.timeBase = gettime();
    CORE.Window.ready = true;
    CORE.Input.Mouse.currentPosition = (Vector2){ platform.width/2.0f, platform.height/2.0f };
    InitTimer();
    CORE.Storage.basePath = GetWorkingDirectory();

    TRACELOG(LOG_INFO, "PLATFORM: WII: %ix%i, USB keyboard %s", platform.width, platform.height, platform.keyboard? "ready" : "absent");
    return 0;
}

void ClosePlatform(void)
{
    if (platform.keyboard) KEYBOARD_Deinit();
    WPAD_Shutdown();
    RL_FREE(platform.frame);
    platform.frame = NULL;
}

// EOF
