/**********************************************************************************************
*
*   rcore_3ds - raylib's platform layer for the Nintendo 3DS (libctru), for Open Doctrines
*
*   PLATFORM: 3DS homebrew (.3dsx), started from the Homebrew Launcher
*       - The game draws a 640x480 frame, which no 3DS screen is. So it is shown twice:
*           top screen     the whole frame at half size (320x240, centred), with a box
*                          where the bottom screen is looking
*           bottom screen  a 320x240 window of it at full size, around the cursor -- the
*                          one that can be read and touched
*       - Pointer: the stylus on the bottom screen points (and clicks) where it touches;
*                  the Circle Pad moves the cursor, and the window follows it
*       - Buttons: A click, B right-click, L / R zoom (wheel), the D-pad as arrow keys,
*                  START as Escape, X as Enter, SELECT as Tab, Y opens the system
*                  keyboard for text
*       - Storage: the SD card, where the game, its data and saves live
*
*   HOW IT IS BUILT. As src/platform/wii/rcore_wii.c: raylib is built as PLATFORM=Memory
*   (rlsw, no GPU API) and this file is copied over platforms/rcore_memory.c (see OD_3DS in
*   CMakeLists.txt). The 3DS's GPU (PICA200) is not OpenGL; rlsw draws the frame on the CPU
*   and this file only shows it.
*
*   rlsw hands its frame over as BYTES (B, G, R, A when RLSW_FRAMEBUFFER_OUTPUT_BGRA, its
*   default); the 3DS's framebuffers are BGR8, three bytes a pixel, and stored rotated --
*   each COLUMN of the screen is a run of 240 pixels, bottom to top.
*
*   LICENSE: zlib/libpng, as raylib (this file started as raylib's rcore_memory.c,
*   Copyright (c) 2025-2026 Ramon Santamaria (@raysan5) and contributors).
*
**********************************************************************************************/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// libctru names its buttons KEY_A, KEY_UP and so on, as enumerators -- and raylib.h, already
// included by rcore.c above this file, has enumerators of those names. libctru's are renamed
// on the way in; below this block KEY_A is raylib's again, and the buttons are CTR_KEY_A.
#define KEY_A     CTR_KEY_A
#define KEY_B     CTR_KEY_B
#define KEY_X     CTR_KEY_X
#define KEY_Y     CTR_KEY_Y
#define KEY_L     CTR_KEY_L
#define KEY_R     CTR_KEY_R
#define KEY_UP    CTR_KEY_UP
#define KEY_DOWN  CTR_KEY_DOWN
#define KEY_LEFT  CTR_KEY_LEFT
#define KEY_RIGHT CTR_KEY_RIGHT
#include <3ds.h>
#undef KEY_A
#undef KEY_B
#undef KEY_X
#undef KEY_Y
#undef KEY_L
#undef KEY_R
#undef KEY_UP
#undef KEY_DOWN
#undef KEY_LEFT
#undef KEY_RIGHT

//----------------------------------------------------------------------------------
// Memory and stack. The Homebrew Launcher gives a .3dsx the application's memory;
// libctru keeps 32 MB of it as the GPU's linear heap unless told otherwise, and this
// build never touches the GPU -- the two screens' framebuffers are all that live there.
// And a 32 KB main stack, libctru's default, is a microcontroller's; the game's is
// the size it has everywhere else.
//----------------------------------------------------------------------------------
u32 __ctru_linear_heap_size = 4*1024*1024;
u32 __stacksize__ = 1024*1024;

#define FRAME_W 640
#define FRAME_H 480
#define TOP_W 400
#define BOT_W 320
#define SCR_H 240

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
typedef struct {
    unsigned char *frame;           // rlsw's frame, 4 bytes a pixel (see SRC_* below)
    int viewX, viewY;               // the bottom screen's window into the frame
    float cursorX, cursorY;         // where the Circle Pad has put the cursor
    bool touching;
    bool newModel;                  // a New 3DS (804 MHz, 124 MB for applications)
    u64 tickBase;
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
// The cursor: the screens draw none, so it is painted into each frame.
//----------------------------------------------------------------------------------
static const char *ctrCursor[19] = {
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
        if ((cy + y < 0) || (cy + y >= FRAME_H)) continue;
        for (int x = 0; x < 12; x++)
        {
            const char c = ctrCursor[y][x];
            if ((c == ' ') || (cx + x < 0) || (cx + x >= FRAME_W)) continue;
            unsigned char *p = platform.frame + ((size_t)(cy + y)*FRAME_W + cx + x)*4;
            const unsigned char v = (c == 'X')? 0 : 255;
            p[0] = p[1] = p[2] = v;
        }
    }
}

//----------------------------------------------------------------------------------
// The two screens
//----------------------------------------------------------------------------------

// A screen pixel (x across, y down) in a 3DS framebuffer: column-major, bottom to top.
static inline u8 *ScreenPixel(u8 *fb, int x, int y)
{
    return fb + ((size_t)x*SCR_H + (SCR_H - 1 - y))*3;
}

static void PresentTop(void)
{
    u16 fw = 0, fh = 0;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
    const int ox = (TOP_W - FRAME_W/2)/2;       // 40: the frame is 4:3, the screen 5:3
    memset(fb, 0, (size_t)TOP_W*SCR_H*3);

    // Half size, each screen pixel the average of a 2x2 block.
    for (int y = 0; y < SCR_H; y++)
    {
        const unsigned char *r0 = platform.frame + (size_t)(2*y)*FRAME_W*4;
        const unsigned char *r1 = r0 + FRAME_W*4;
        for (int x = 0; x < FRAME_W/2; x++, r0 += 8, r1 += 8)
        {
            u8 *d = ScreenPixel(fb, ox + x, y);
            d[0] = (u8)((SRC_B(r0) + SRC_B(r0 + 4) + SRC_B(r1) + SRC_B(r1 + 4) + 2) >> 2);
            d[1] = (u8)((SRC_G(r0) + SRC_G(r0 + 4) + SRC_G(r1) + SRC_G(r1 + 4) + 2) >> 2);
            d[2] = (u8)((SRC_R(r0) + SRC_R(r0 + 4) + SRC_R(r1) + SRC_R(r1 + 4) + 2) >> 2);
        }
    }

    // Where the bottom screen is looking.
    const int bx0 = ox + platform.viewX/2, by0 = platform.viewY/2;
    const int bx1 = bx0 + BOT_W/2 - 1, by1 = by0 + SCR_H/2 - 1;
    for (int x = bx0; x <= bx1; x++)
    {
        u8 *a = ScreenPixel(fb, x, by0), *b = ScreenPixel(fb, x, by1);
        a[0] = b[0] = 40; a[1] = b[1] = 200; a[2] = b[2] = 255;
    }
    for (int y = by0; y <= by1; y++)
    {
        u8 *a = ScreenPixel(fb, bx0, y), *b = ScreenPixel(fb, bx1, y);
        a[0] = b[0] = 40; a[1] = b[1] = 200; a[2] = b[2] = 255;
    }
}

static void PresentBottom(void)
{
    u16 fw = 0, fh = 0;
    u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fw, &fh);
    for (int y = 0; y < SCR_H; y++)
    {
        const unsigned char *src = platform.frame + ((size_t)(platform.viewY + y)*FRAME_W + platform.viewX)*4;
        for (int x = 0; x < BOT_W; x++, src += 4)
        {
            u8 *d = ScreenPixel(fb, x, y);
            d[0] = SRC_B(src); d[1] = SRC_G(src); d[2] = SRC_R(src);
        }
    }
}

// The bottom screen's window keeps the cursor inside it, with a margin, and never leaves
// the frame.
static void FollowCursor(void)
{
    const int margin = 48;
    const int cx = (int)CORE.Input.Mouse.currentPosition.x, cy = (int)CORE.Input.Mouse.currentPosition.y;
    if (cx < platform.viewX + margin) platform.viewX = cx - margin;
    if (cx > platform.viewX + BOT_W - margin) platform.viewX = cx - BOT_W + margin;
    if (cy < platform.viewY + margin) platform.viewY = cy - margin;
    if (cy > platform.viewY + SCR_H - margin) platform.viewY = cy - SCR_H + margin;
    if (platform.viewX < 0) platform.viewX = 0;
    if (platform.viewY < 0) platform.viewY = 0;
    if (platform.viewX > FRAME_W - BOT_W) platform.viewX = FRAME_W - BOT_W;
    if (platform.viewY > FRAME_H - SCR_H) platform.viewY = FRAME_H - SCR_H;
}

//----------------------------------------------------------------------------------
// Keys
//----------------------------------------------------------------------------------
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

// UTF-8 from the system keyboard, one code point at a time into raylib's queue.
static void QueueText(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p && (CORE.Input.Keyboard.charPressedQueueCount < MAX_CHAR_PRESSED_QUEUE))
    {
        int cp = 0, n = 0;
        if (*p < 0x80) { cp = *p; n = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; n = 4; }
        else { p++; continue; }
        for (int i = 1; i < n; i++)
        {
            if ((p[i] & 0xC0) != 0x80) { n = i; cp = -1; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += n;
        if (cp >= 32) CORE.Input.Keyboard.charPressedQueue[CORE.Input.Keyboard.charPressedQueueCount++] = cp;
    }
}

// Y: the system's own keyboard, which takes over both screens until it is closed. What was
// typed arrives as characters, as if from a keyboard, into whatever field has focus.
static void SystemKeyboard(void)
{
    static char text[256];
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)sizeof(text) - 1);
    swkbdSetHintText(&kb, "Open Doctrines");
    text[0] = '\0';
    if (swkbdInputText(&kb, text, sizeof(text)) == SWKBD_BUTTON_CONFIRM) QueueText(text);
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Window and Graphics Device
//----------------------------------------------------------------------------------
bool WindowShouldClose(void)
{
    if (CORE.Window.ready) return CORE.Window.shouldClose;
    else return true;
}

// The screens are the window.
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
int GetMonitorWidth(int monitor) { (void)monitor; return FRAME_W; }
int GetMonitorHeight(int monitor) { (void)monitor; return FRAME_H; }
int GetMonitorPhysicalWidth(int monitor) { (void)monitor; return 0; }
int GetMonitorPhysicalHeight(int monitor) { (void)monitor; return 0; }
int GetMonitorRefreshRate(int monitor) { (void)monitor; return 60; }
const char *GetMonitorName(int monitor) { (void)monitor; return "3DS"; }
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
    PresentTop();
    PresentBottom();
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Misc
//----------------------------------------------------------------------------------
double GetTime(void)
{
    return (double)(svcGetSystemTick() - platform.tickBase)/(double)SYSCLOCK_ARM11;
}

// No browser the game can hand an address to. The game shows the address instead.
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
    platform.cursorX = (float)x;
    platform.cursorY = (float)y;
}

void SetMouseCursor(int cursor) { CORE.Input.Mouse.cursor = cursor; }
const char *GetKeyName(int key) { (void)key; return ""; }

// The buttons, as keys and mouse buttons the game already understands.
static void CtrButtons(u32 held, u32 down)
{
    CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_LEFT] = ((held & CTR_KEY_A) || platform.touching)? 1 : 0;
    CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_RIGHT] = (held & CTR_KEY_B)? 1 : 0;
    if (down & CTR_KEY_R) CORE.Input.Mouse.currentWheelMove.y += 1.0f;
    if (down & CTR_KEY_L) CORE.Input.Mouse.currentWheelMove.y -= 1.0f;

    static const struct { u32 button; int key; } map[] = {
        { KEY_DUP, KEY_UP }, { KEY_DDOWN, KEY_DOWN }, { KEY_DLEFT, KEY_LEFT }, { KEY_DRIGHT, KEY_RIGHT },
        { KEY_START, KEY_ESCAPE }, { CTR_KEY_X, KEY_ENTER }, { KEY_SELECT, KEY_TAB },
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++)
    {
        if (held & map[i].button) KeyDown(map[i].key);
        else KeyUp(map[i].key);
    }
    if (down & CTR_KEY_Y) SystemKeyboard();
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

    // The HOME menu and the power button come through here; false means close.
    if (!aptMainLoop()) { CORE.Window.shouldClose = true; return; }

    hidScanInput();
    const u32 held = hidKeysHeld(), down = hidKeysDown();

    // The Circle Pad moves the cursor, faster the further it is pushed (it reads
    // about +/-156 at the rim; under 16 is the stick at rest).
    circlePosition pad;
    hidCircleRead(&pad);
    const float dx = (pad.dx > 16 || pad.dx < -16)? pad.dx/156.0f : 0.0f;
    const float dy = (pad.dy > 16 || pad.dy < -16)? -pad.dy/156.0f : 0.0f;
    platform.cursorX += dx*fabsf(dx)*10.0f;
    platform.cursorY += dy*fabsf(dy)*10.0f;

    // The stylus points where it touches: the bottom screen IS that part of the frame.
    platform.touching = (held & KEY_TOUCH) != 0;
    if (platform.touching)
    {
        touchPosition touch;
        hidTouchRead(&touch);
        platform.cursorX = (float)(platform.viewX + touch.px);
        platform.cursorY = (float)(platform.viewY + touch.py);
    }
    if (platform.cursorX < 0) platform.cursorX = 0;
    if (platform.cursorY < 0) platform.cursorY = 0;
    if (platform.cursorX > FRAME_W - 1) platform.cursorX = FRAME_W - 1;
    if (platform.cursorY > FRAME_H - 1) platform.cursorY = FRAME_H - 1;
    CORE.Input.Mouse.currentPosition = (Vector2){ platform.cursorX, platform.cursorY };
    CORE.Input.Touch.position[0] = CORE.Input.Mouse.currentPosition;
    CORE.Input.Touch.currentTouchState[0] = platform.touching? 1 : 0;
    CORE.Input.Touch.pointCount = platform.touching? 1 : 0;
    // The window moves with the Circle Pad only: under the stylus it stays put, or what
    // was touched would slide away from it.
    if (!platform.touching) FollowCursor();

    CtrButtons(held, down);

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
        TRACELOG(LOG_FATAL, "PLATFORM: the 3DS build needs the software renderer (GRAPHICS_API_OPENGL_SOFTWARE)");
        return -1;
    }

    // The New 3DS's 804 MHz and its L2 cache; a no-op on the original model.
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    platform.newModel = isNew;
    osSetSpeedupEnable(true);

    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);

    CORE.Window.screen.width = FRAME_W;
    CORE.Window.screen.height = FRAME_H;
    CORE.Window.display.width = FRAME_W;
    CORE.Window.display.height = FRAME_H;
    CORE.Window.render.width = FRAME_W;
    CORE.Window.render.height = FRAME_H;
    CORE.Window.currentFbo.width = FRAME_W;
    CORE.Window.currentFbo.height = FRAME_H;
    CORE.Window.flags |= FLAG_FULLSCREEN_MODE;

    platform.frame = (unsigned char *)RL_CALLOC((size_t)FRAME_W*FRAME_H, 4);
    if (platform.frame == NULL)
    {
        TRACELOG(LOG_FATAL, "PLATFORM: out of memory for a %ix%i frame", FRAME_W, FRAME_H);
        return -1;
    }

    // The SD card is mounted by libctru before main; the Homebrew Launcher starts us with
    // argv[0] = sdmc:/3ds/OpenDoctrines/OpenDoctrines.3dsx, and the game finds data/
    // beside its executable (see odApplicationDirectory).
    platform.tickBase = svcGetSystemTick();
    CORE.Window.ready = true;
    SetMousePosition(FRAME_W/2, FRAME_H/2);
    FollowCursor();
    InitTimer();
    CORE.Storage.basePath = GetWorkingDirectory();

    TRACELOG(LOG_INFO, "PLATFORM: 3DS: %ix%i shown on both screens, %s model", FRAME_W, FRAME_H, platform.newModel? "New" : "original");
    return 0;
}

void ClosePlatform(void)
{
    RL_FREE(platform.frame);
    platform.frame = NULL;
    gfxExit();
}

// EOF
