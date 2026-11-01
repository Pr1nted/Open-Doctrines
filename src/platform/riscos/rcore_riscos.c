/**********************************************************************************************
*
*   rcore_riscos - raylib's platform layer for RISC OS 5, for Open Doctrines
*
*   PLATFORM: RISC OS 5 (Raspberry Pi and other ARMv7 machines), started from the desktop
*       - Screen: a full-screen mode of its own (OS_ScreenMode), 32 bits a pixel, or 16 where
*                 that is all there is; the desktop's mode is put back on the way out
*       - Mouse: OS_Mouse -- Select clicks, Adjust is the right button, Menu the middle one
*       - Keyboard: key state by INKEY scan (OS_Byte 129), typed text from the keyboard
*                   buffer (OS_Byte 145), Escape as a key rather than an escape condition
*
*   A single-tasking, full-screen program, as games on RISC OS usually are: the desktop is
*   paused while it runs and comes back when it quits.
*
*   HOW IT IS BUILT. As src/platform/dos/rcore_dos.c: raylib is built as PLATFORM=Memory
*   (rlsw, no GPU API) and this file is copied over platforms/rcore_memory.c (see OD_RISCOS in
*   CMakeLists.txt). RISC OS has no OpenGL the game can count on; rlsw draws the frame on the
*   CPU and this file only shows it.
*
*   rlsw hands its frame over as BYTES (B, G, R, A when RLSW_FRAMEBUFFER_OUTPUT_BGRA, its
*   default). RISC OS 5's own 32-bit pixel is &00BBGGRR -- bytes R, G, B, 0 -- and its 16-bit
*   one 1:5:5:5 with red in the low bits.
*
*   LICENSE: zlib/libpng, as raylib (this file started as raylib's rcore_memory.c,
*   Copyright (c) 2025-2026 Ramon Santamaria (@raysan5) and contributors).
*
**********************************************************************************************/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <kernel.h>

// The SWIs used here, by number (the X bit is set on each call: errors come back, not up).
#define XBIT                    0x20000
#define SWI_OS_Byte             0x06
#define SWI_OS_Mouse            0x1C
#define SWI_OS_ReadVduVariables 0x31
#define SWI_OS_RemoveCursors    0x36
#define SWI_OS_RestoreCursors   0x37
#define SWI_OS_ReadMonotonicTime 0x42
#define SWI_OS_ScreenMode       0x65

// VDU variables (OS_ReadVduVariables)
#define VDU_XEIG        4
#define VDU_YEIG        5
#define VDU_LINELENGTH  6
#define VDU_LOG2BPP     9
#define VDU_XWINDLIMIT  11
#define VDU_YWINDLIMIT  12
#define VDU_SCREENSTART 148

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
typedef struct {
    unsigned char *frame;           // rlsw's frame, 4 bytes a pixel (see SRC_* below)
    unsigned char *screen;          // screen memory
    int width, height;              // the mode's size in pixels
    int lineLength;                 // bytes per screen line
    int bpp;                        // 16 or 32
    int xeig, yeig;                 // OS units per pixel, as powers of two
    int oldMode;                    // the desktop's mode, put back by ClosePlatform: a number,
    int oldModeBlock[32];           // or (when it is a specifier block) a copy of that block
    bool oldModeKnown;
    bool modeChanged;
    unsigned int timeBase;          // centiseconds (OS_ReadMonotonicTime)
} PlatformData;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
extern CoreData CORE;

static PlatformData platform = { 0 };

int InitPlatform(void);
void ClosePlatform(void);

// rlsw's byte order (see the header above).
#if defined(RLSW_FRAMEBUFFER_OUTPUT_BGRA) && RLSW_FRAMEBUFFER_OUTPUT_BGRA
    #define SRC_R(p) ((p)[2])
    #define SRC_B(p) ((p)[0])
#else
    #define SRC_R(p) ((p)[0])
    #define SRC_B(p) ((p)[2])
#endif
#define SRC_G(p) ((p)[1])

//----------------------------------------------------------------------------------
// SWIs
//----------------------------------------------------------------------------------
static _kernel_oserror *Swi(int number, _kernel_swi_regs *regs)
{
    return _kernel_swi(number | XBIT, regs, regs);
}

static int OsByte(int a, int x, int y, int *outX, int *outY)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[0] = a; r.r[1] = x; r.r[2] = y;
    if (Swi(SWI_OS_Byte, &r) != NULL) return -1;
    if (outX) *outX = r.r[1];
    if (outY) *outY = r.r[2];
    return 0;
}

static void ReadVdu(void)
{
    int in[] = { VDU_XEIG, VDU_YEIG, VDU_LINELENGTH, VDU_LOG2BPP, VDU_XWINDLIMIT, VDU_YWINDLIMIT, VDU_SCREENSTART, -1 };
    int out[8] = { 0 };
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[0] = (int)(intptr_t)in;
    r.r[1] = (int)(intptr_t)out;
    Swi(SWI_OS_ReadVduVariables, &r);
    platform.xeig = out[0];
    platform.yeig = out[1];
    platform.lineLength = out[2];
    platform.bpp = 1 << out[3];
    platform.width = out[4] + 1;
    platform.height = out[5] + 1;
    platform.screen = (unsigned char *)(intptr_t)out[6];
}

// A mode by its size, through a mode specifier block (RISC OS 3.5 and later):
// flags 1, width, height, log2 bpp, frame rate -1 (any), then -1 (no mode variables).
static bool SelectMode(int w, int h, int log2bpp)
{
    int block[6] = { 1, w, h, log2bpp, -1, -1 };
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[0] = 0;
    r.r[1] = (int)(intptr_t)block;
    return Swi(SWI_OS_ScreenMode, &r) == NULL;
}

//----------------------------------------------------------------------------------
// The cursor: the hardware pointer is turned off, so it is painted into each frame.
//----------------------------------------------------------------------------------
static const char *roCursor[19] = {
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
            const char c = roCursor[y][x];
            if ((c == ' ') || (cx + x < 0) || (cx + x >= platform.width)) continue;
            unsigned char *p = platform.frame + ((size_t)(cy + y)*platform.width + cx + x)*4;
            const unsigned char v = (c == 'X')? 0 : 255;
            p[0] = p[1] = p[2] = v;
        }
    }
}

static void PresentFrame(void)
{
    OsByte(19, 0, 0, NULL, NULL);                       // wait for the vertical sync
    const unsigned char *src = platform.frame;
    for (int y = 0; y < platform.height; y++)
    {
        unsigned char *row = platform.screen + (size_t)y*platform.lineLength;
        if (platform.bpp == 32)
        {
            uint32_t *d = (uint32_t *)row;
            for (int x = 0; x < platform.width; x++, src += 4)
                d[x] = (uint32_t)SRC_R(src) | ((uint32_t)SRC_G(src) << 8) | ((uint32_t)SRC_B(src) << 16);
        }
        else
        {
            uint16_t *d = (uint16_t *)row;
            for (int x = 0; x < platform.width; x++, src += 4)
                d[x] = (uint16_t)((SRC_R(src) >> 3) | ((SRC_G(src) >> 3) << 5) | ((SRC_B(src) >> 3) << 10));
        }
    }
}

//----------------------------------------------------------------------------------
// Keyboard: INKEY numbers (negative, as BBC BASIC writes them) -> raylib keys
//----------------------------------------------------------------------------------
static const struct { int inkey; int key; } roKeys[] = {
    { -66, KEY_A }, { -101, KEY_B }, { -83, KEY_C }, { -51, KEY_D }, { -35, KEY_E }, { -68, KEY_F },
    { -84, KEY_G }, { -85, KEY_H }, { -38, KEY_I }, { -70, KEY_J }, { -71, KEY_K }, { -87, KEY_L },
    { -102, KEY_M }, { -86, KEY_N }, { -55, KEY_O }, { -56, KEY_P }, { -17, KEY_Q }, { -52, KEY_R },
    { -82, KEY_S }, { -36, KEY_T }, { -54, KEY_U }, { -100, KEY_V }, { -34, KEY_W }, { -67, KEY_X },
    { -69, KEY_Y }, { -98, KEY_Z },
    { -40, KEY_ZERO }, { -49, KEY_ONE }, { -50, KEY_TWO }, { -18, KEY_THREE }, { -19, KEY_FOUR },
    { -20, KEY_FIVE }, { -53, KEY_SIX }, { -37, KEY_SEVEN }, { -22, KEY_EIGHT }, { -39, KEY_NINE },
    { -114, KEY_F1 }, { -115, KEY_F2 }, { -116, KEY_F3 }, { -21, KEY_F4 }, { -117, KEY_F5 }, { -118, KEY_F6 },
    { -23, KEY_F7 }, { -119, KEY_F8 }, { -120, KEY_F9 }, { -31, KEY_F10 }, { -29, KEY_F11 }, { -30, KEY_F12 },
    { -113, KEY_ESCAPE }, { -74, KEY_ENTER }, { -61, KEY_KP_ENTER }, { -48, KEY_BACKSPACE }, { -97, KEY_TAB },
    { -99, KEY_SPACE }, { -90, KEY_DELETE }, { -62, KEY_INSERT }, { -63, KEY_HOME }, { -106, KEY_END },
    { -64, KEY_PAGE_UP }, { -79, KEY_PAGE_DOWN },
    { -58, KEY_UP }, { -42, KEY_DOWN }, { -26, KEY_LEFT }, { -122, KEY_RIGHT },
    { -4, KEY_LEFT_SHIFT }, { -7, KEY_RIGHT_SHIFT }, { -5, KEY_LEFT_CONTROL }, { -8, KEY_RIGHT_CONTROL },
    { -6, KEY_LEFT_ALT }, { -9, KEY_RIGHT_ALT },
    { -24, KEY_MINUS }, { -94, KEY_EQUAL }, { -57, KEY_LEFT_BRACKET }, { -89, KEY_RIGHT_BRACKET },
    { -88, KEY_SEMICOLON }, { -80, KEY_APOSTROPHE }, { -103, KEY_COMMA }, { -104, KEY_PERIOD },
    { -105, KEY_SLASH }, { -121, KEY_BACKSLASH }, { -46, KEY_GRAVE },
};

static bool InkeyDown(int inkey)
{
    int x = 0, y = 0;
    if (OsByte(129, inkey & 0xFF, 0xFF, &x, &y) != 0) return false;
    return (x == 0xFF) && (y == 0xFF);
}

static void ScanKeys(void)
{
    for (size_t i = 0; i < sizeof(roKeys)/sizeof(roKeys[0]); i++)
    {
        const int key = roKeys[i].key;
        if (InkeyDown(roKeys[i].inkey))
        {
            if (CORE.Input.Keyboard.currentKeyState[key]) CORE.Input.Keyboard.keyRepeatInFrame[key] = 1;
            else
            {
                CORE.Input.Keyboard.currentKeyState[key] = 1;
                if (CORE.Input.Keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
                    CORE.Input.Keyboard.keyPressedQueue[CORE.Input.Keyboard.keyPressedQueueCount++] = key;
            }
        }
        else CORE.Input.Keyboard.currentKeyState[key] = 0;
    }
}

// Typed characters from the keyboard buffer (buffer 0), in RISC OS's Latin-1 alphabet,
// which is Unicode's first 256 code points. Codes the cursor and function keys put in the
// buffer (&80-&FF outside Latin-1's printable range) and control codes are dropped: those
// keys arrive through the scan above.
static void ReadTypedText(void)
{
    for (int i = 0; i < 64; i++)
    {
        _kernel_swi_regs r;
        int carry = 1;
        memset(&r, 0, sizeof(r));
        r.r[0] = 145; r.r[1] = 0;
        if ((_kernel_swi_c(SWI_OS_Byte | XBIT, &r, &r, &carry) != NULL) || carry) break;
        const int ch = r.r[2] & 0xFF;
        if (((ch >= 32) && (ch < 127)) || (ch >= 0xA0))
        {
            if (CORE.Input.Keyboard.charPressedQueueCount < MAX_CHAR_PRESSED_QUEUE)
                CORE.Input.Keyboard.charPressedQueue[CORE.Input.Keyboard.charPressedQueueCount++] = ch;
        }
    }
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Window and Graphics Device
//----------------------------------------------------------------------------------
bool WindowShouldClose(void)
{
    if (CORE.Window.ready) return CORE.Window.shouldClose;
    else return true;
}

// The whole screen is the window.
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
const char *GetMonitorName(int monitor) { (void)monitor; return "RISC OS"; }
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
static unsigned int Centiseconds(void)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    Swi(SWI_OS_ReadMonotonicTime, &r);
    return (unsigned int)r.r[0];
}

// Centiseconds are what RISC OS counts in; good enough to pace frames by.
double GetTime(void)
{
    return (double)(Centiseconds() - platform.timeBase)/100.0;
}

// The game shows the address instead: a browser would need the desktop, which is paused.
void OpenURL(const char *url)
{
    TRACELOG(LOG_INFO, "SYSTEM: no browser while the game has the screen: %s", url);
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Inputs
//----------------------------------------------------------------------------------
int SetGamepadMappings(const char *mappings) { (void)mappings; return 0; }
void SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration)
{
    (void)gamepad; (void)leftMotor; (void)rightMotor; (void)duration;
}

// The pointer is RISC OS's: moving it means moving the mouse's own position, in OS units
// with the origin at the bottom left (OS_Word 21, 3 is the pointer-position call).
void SetMousePosition(int x, int y)
{
    CORE.Input.Mouse.currentPosition = (Vector2){ (float)x, (float)y };
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
    const int ox = x << platform.xeig, oy = (platform.height - 1 - y) << platform.yeig;
    unsigned char block[5] = { 3, (unsigned char)(ox & 0xFF), (unsigned char)((ox >> 8) & 0xFF),
                               (unsigned char)(oy & 0xFF), (unsigned char)((oy >> 8) & 0xFF) };
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[0] = 21;
    r.r[1] = (int)(intptr_t)block;
    Swi(0x07, &r);                                      // OS_Word
}

void SetMouseCursor(int cursor) { CORE.Input.Mouse.cursor = cursor; }
const char *GetKeyName(int key) { (void)key; return ""; }

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

    // The mouse, in OS units from the bottom left; buttons: 4 Select, 2 Menu, 1 Adjust.
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    if (Swi(SWI_OS_Mouse, &r) == NULL)
    {
        int x = r.r[0] >> platform.xeig;
        int y = platform.height - 1 - (r.r[1] >> platform.yeig);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= platform.width) x = platform.width - 1;
        if (y >= platform.height) y = platform.height - 1;
        CORE.Input.Mouse.currentPosition = (Vector2){ (float)x, (float)y };
        CORE.Input.Touch.position[0] = CORE.Input.Mouse.currentPosition;
        CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_LEFT] = (r.r[2] & 4)? 1 : 0;
        CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_MIDDLE] = (r.r[2] & 2)? 1 : 0;
        CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_RIGHT] = (r.r[2] & 1)? 1 : 0;
    }

    ScanKeys();
    ReadTypedText();

    // No scroll wheel outside the desktop: Page Up and Page Down zoom as it would.
    if (CORE.Input.Keyboard.currentKeyState[KEY_PAGE_UP] && !CORE.Input.Keyboard.previousKeyState[KEY_PAGE_UP])
        CORE.Input.Mouse.currentWheelMove.y += 1.0f;
    if (CORE.Input.Keyboard.currentKeyState[KEY_PAGE_DOWN] && !CORE.Input.Keyboard.previousKeyState[KEY_PAGE_DOWN])
        CORE.Input.Mouse.currentWheelMove.y -= 1.0f;

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
        TRACELOG(LOG_FATAL, "PLATFORM: the RISC OS build needs the software renderer (GRAPHICS_API_OPENGL_SOFTWARE)");
        return -1;
    }

    // The desktop's mode, to put back (OS_ScreenMode 1: the current mode, as a number or a
    // pointer to a specifier block -- either is what OS_ScreenMode 0 takes back).
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[0] = 1;
    if (Swi(SWI_OS_ScreenMode, &r) == NULL)
    {
        platform.oldMode = r.r[1];
        platform.oldModeKnown = true;
        // A value of 256 or more is the address of the OS's own specifier block, which can
        // change under us: keep a copy. Flags, width, height, log2 bpp, frame rate, then
        // (variable, value) pairs ending in -1.
        if ((unsigned int)r.r[1] >= 256)
        {
            const int *block = (const int *)(intptr_t)r.r[1];
            int n = 5;
            while ((n < 31) && (block[n] != -1)) n += 2;
            memcpy(platform.oldModeBlock, block, (size_t)n*sizeof(int));
            platform.oldModeBlock[n] = -1;
            platform.oldMode = (int)(intptr_t)platform.oldModeBlock;
        }
    }

    // The size asked for (the game asks for its window size), then the usual ones, at
    // 32 bits a pixel and then 16. OD_RISCOS_MODE=WxH picks one.
    int want[6][2] = { { CORE.Window.screen.width, CORE.Window.screen.height },
                       { 1280, 720 }, { 1280, 1024 }, { 1024, 768 }, { 800, 600 }, { 640, 480 } };
    const char *env = getenv("OD_RISCOS_MODE");
    if (env != NULL) sscanf(env, "%dx%d", &want[0][0], &want[0][1]);
    for (int depth = 5; (depth >= 4) && !platform.modeChanged; depth--)
        for (int i = 0; (i < 6) && !platform.modeChanged; i++)
            if ((want[i][0] > 0) && (want[i][1] > 0) && SelectMode(want[i][0], want[i][1], depth)) platform.modeChanged = true;
    if (!platform.modeChanged)
    {
        TRACELOG(LOG_FATAL, "PLATFORM: no 16- or 32-bit screen mode could be selected");
        return -1;
    }
    ReadVdu();
    if (((platform.bpp != 32) && (platform.bpp != 16)) || (platform.screen == NULL))
    {
        TRACELOG(LOG_FATAL, "PLATFORM: the mode selected is %i bits a pixel; 16 or 32 are needed", platform.bpp);
        ClosePlatform();
        return -1;
    }

    // No text cursor, no pointer (it is painted), Escape a key rather than a condition.
    memset(&r, 0, sizeof(r));
    Swi(SWI_OS_RemoveCursors, &r);
    OsByte(106, 0, 0, NULL, NULL);
    OsByte(229, 1, 0, NULL, NULL);

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
        ClosePlatform();
        return -1;
    }

    platform.timeBase = Centiseconds();
    CORE.Window.ready = true;
    SetMousePosition(platform.width/2, platform.height/2);
    InitTimer();
    CORE.Storage.basePath = GetWorkingDirectory();
    atexit(ClosePlatform);

    TRACELOG(LOG_INFO, "PLATFORM: RISC OS: %ix%i, %i bits a pixel", platform.width, platform.height, platform.bpp);
    return 0;
}

// Also run at exit (atexit above), so the desktop gets its mode back however the game ends.
void ClosePlatform(void)
{
    static bool closed = false;
    if (closed) return;
    closed = true;

    RL_FREE(platform.frame);
    platform.frame = NULL;
    OsByte(229, 0, 0, NULL, NULL);
    OsByte(106, 1, 0, NULL, NULL);
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    Swi(SWI_OS_RestoreCursors, &r);
    if (platform.modeChanged && platform.oldModeKnown)
    {
        memset(&r, 0, sizeof(r));
        r.r[0] = 0;
        r.r[1] = platform.oldMode;
        Swi(SWI_OS_ScreenMode, &r);
    }
}

// EOF
