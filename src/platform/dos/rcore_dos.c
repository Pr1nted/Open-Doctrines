/**********************************************************************************************
*
*   rcore_dos - raylib's platform layer for DOS (DJGPP), for Open Doctrines
*
*   PLATFORM: DOS, 32-bit protected mode under a DPMI host (CWSDPMI, DOSBox, Windows 9x)
*       - Screen: a VESA BIOS Extensions 2.0+ linear framebuffer, 32, 24, 16 or 15 bits
*       - Mouse:  the INT 33h mouse driver (CuteMouse, DOSBox's own), wheel if it has one
*       - Keyboard: an IRQ 1 handler reading scan codes from the keyboard controller
*       - Time:   uclock(), the PIT at 1.19 MHz
*
*   HOW IT IS BUILT. Not a raylib platform raylib knows: the game's CMake builds raylib
*   as PLATFORM=Memory (rlsw, no OS windowing) and copies this file over
*   platforms/rcore_memory.c, so rcore.c includes it in that one's place. rcore_memory.c
*   draws into a buffer nobody shows and reads no input; this is the same thing with a
*   screen and hands.
*
*   OFF DOS (any other compiler) it builds against a stand-in screen in memory and no
*   input, so the frame path -- rlsw's output, converted to a VESA pixel format, with the
*   cursor drawn in -- runs on a Linux host in CI, where a DJGPP binary cannot. Set
*   OD_DOS_FB_DUMP=<file.ppm> to have the last frame of the stand-in screen written out,
*   and OD_DOS_FB_BPP=32|24|16|15 to pick the pixel format it pretends to have.
*
*   LICENSE: zlib/libpng, as raylib (this file started as raylib's rcore_memory.c,
*   Copyright (c) 2025-2026 Ramon Santamaria (@raysan5) and contributors).
*
**********************************************************************************************/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(__DJGPP__)
    #include <dpmi.h>               // __dpmi_int, __dpmi_physical_address_mapping, LDT descriptors
    #include <go32.h>               // __tb, _dos_ds, _go32_my_cs, _go32_dpmi_* interrupt vectors
    #include <sys/movedata.h>       // dosmemget, dosmemput, movedata
    #include <sys/farptr.h>         // _farpeekw
    #include <sys/segments.h>       // _my_ds
    #include <pc.h>                 // inportb, outportb
    #include <time.h>               // uclock
    #include <crt0.h>               // _crt0_startup_flags
#else
    #include <time.h>               // clock_gettime
#endif

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
typedef struct {
    unsigned int *pixels;           // rlsw's frame, 8 bits a channel (see SRC_R), screen.width x screen.height
    unsigned char *row;             // one converted frame in the screen's own pixel format

    int width, height;              // the mode's resolution
    int pitch;                      // bytes per scanline on the card
    int bpp;                        // 32, 24, 16 or 15
    int rShift, gShift, bShift;     // field positions of the mode's direct colour
    int rBits, gBits, bBits;

#if defined(__DJGPP__)
    int lfbSelector;                // an LDT selector over the mapped framebuffer
    unsigned long lfbLinear;
    int modeSet;                    // a graphics mode is up and must be undone on exit
    int mouse;                      // an INT 33h driver answered
    int mouseWheel;                 // ...and it reports a wheel (CuteMouse API)
    int keyboardHooked;
    uclock_t timeBase;
#else
    unsigned char *standIn;         // the stand-in "card" a host build draws into
    double timeBase;
#endif
} PlatformData;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
extern CoreData CORE;                   // Global CORE state context

static PlatformData platform = { 0 };   // Platform specific data

//----------------------------------------------------------------------------------
// Module Internal Functions Declaration
//----------------------------------------------------------------------------------
int InitPlatform(void);                 // Initialize platform (graphics, inputs and more)
void ClosePlatform(void);               // Close platform

static void ConvertFrame(void);         // rlsw's RGBA frame -> the card's pixel format, cursor drawn in
static void PresentFrame(void);         // the converted frame -> the card

//----------------------------------------------------------------------------------
// Keyboard: scan code set 1 -> raylib keys and characters
//----------------------------------------------------------------------------------
// Plain make codes 0x01..0x58.
static const unsigned short dosKeys[0x59] = {
    0, KEY_ESCAPE, KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE, KEY_SIX,                    // 00-07
    KEY_SEVEN, KEY_EIGHT, KEY_NINE, KEY_ZERO, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB,     // 08-0F
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I,                                     // 10-17
    KEY_O, KEY_P, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_ENTER, KEY_LEFT_CONTROL, KEY_A, KEY_S, // 18-1F
    KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON,                             // 20-27
    KEY_APOSTROPHE, KEY_GRAVE, KEY_LEFT_SHIFT, KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V,       // 28-2F
    KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_RIGHT_SHIFT, KEY_KP_MULTIPLY,    // 30-37
    KEY_LEFT_ALT, KEY_SPACE, KEY_CAPS_LOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,             // 38-3F
    KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_NUM_LOCK, KEY_SCROLL_LOCK, KEY_KP_7,           // 40-47
    KEY_KP_8, KEY_KP_9, KEY_KP_SUBTRACT, KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_ADD, KEY_KP_1,    // 48-4F
    KEY_KP_2, KEY_KP_3, KEY_KP_0, KEY_KP_DECIMAL, 0, 0, 0, KEY_F11,                             // 50-57
    KEY_F12                                                                                     // 58
};

// After an E0 prefix: the grey keys, the right-hand modifiers and the keypad's
// Enter and slash.
static int DosExtendedKey(unsigned char code)
{
    switch (code)
    {
        case 0x1C: return KEY_KP_ENTER;
        case 0x1D: return KEY_RIGHT_CONTROL;
        case 0x35: return KEY_KP_DIVIDE;
        case 0x38: return KEY_RIGHT_ALT;
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UP;
        case 0x49: return KEY_PAGE_UP;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x4F: return KEY_END;
        case 0x50: return KEY_DOWN;
        case 0x51: return KEY_PAGE_DOWN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        case 0x5B: return KEY_LEFT_SUPER;
        case 0x5C: return KEY_RIGHT_SUPER;
        case 0x5D: return KEY_KB_MENU;
        default:   return 0;
    }
}

// The character a key types on a US layout. DOS has no keyboard layout the game
// could ask for -- KEYB.COM rewrites the BIOS's translation, which an IRQ 1 handler
// never sees -- so US it is, as every DOS game that reads the controller does.
static int DosKeyChar(int key, int shift, int caps)
{
    if ((key >= KEY_A) && (key <= KEY_Z)) return ((shift != 0) != (caps != 0))? key : key + 32;
    if ((key >= KEY_KP_0) && (key <= KEY_KP_9)) return '0' + (key - KEY_KP_0);

    static const char plain[]   = "0123456789";
    static const char shifted[] = ")!@#$%^&*(";
    if ((key >= KEY_ZERO) && (key <= KEY_NINE)) return shift? shifted[key - KEY_ZERO] : plain[key - KEY_ZERO];

    switch (key)
    {
        case KEY_SPACE:         return ' ';
        case KEY_MINUS:         return shift? '_' : '-';
        case KEY_EQUAL:         return shift? '+' : '=';
        case KEY_LEFT_BRACKET:  return shift? '{' : '[';
        case KEY_RIGHT_BRACKET: return shift? '}' : ']';
        case KEY_BACKSLASH:     return shift? '|' : '\\';
        case KEY_SEMICOLON:     return shift? ':' : ';';
        case KEY_APOSTROPHE:    return shift? '"' : '\'';
        case KEY_GRAVE:         return shift? '~' : '`';
        case KEY_COMMA:         return shift? '<' : ',';
        case KEY_PERIOD:        return shift? '>' : '.';
        case KEY_SLASH:         return shift? '?' : '/';
        case KEY_KP_DECIMAL:    return '.';
        case KEY_KP_DIVIDE:     return '/';
        case KEY_KP_MULTIPLY:   return '*';
        case KEY_KP_SUBTRACT:   return '-';
        case KEY_KP_ADD:        return '+';
        default:                return 0;
    }
}

// One scan code, as the controller sent it. E0 sets the prefix for the next one.
static int dosPrefix = 0;
static int dosCapsLock = 0;

static void DosFeedScanCode(unsigned char code)
{
    if ((code == 0xE0) || (code == 0xE1)) { dosPrefix = code; return; }

    const int released = (code & 0x80) != 0;
    const unsigned char make = code & 0x7F;
    int key = 0;

    if (dosPrefix == 0xE1) { dosPrefix = 0; return; }   // Pause/Break: nothing the game binds
    if (dosPrefix == 0xE0)
    {
        dosPrefix = 0;
        if ((make == 0x2A) || (make == 0x36)) return;   // the fake shifts around grey keys
        key = DosExtendedKey(make);
    }
    else if (make < (sizeof(dosKeys)/sizeof(dosKeys[0]))) key = dosKeys[make];

    if ((key <= 0) || (key >= MAX_KEYBOARD_KEYS)) return;

    if (released)
    {
        CORE.Input.Keyboard.currentKeyState[key] = 0;
        return;
    }

    // The controller's own typematic repeat arrives as more make codes.
    if (CORE.Input.Keyboard.currentKeyState[key]) CORE.Input.Keyboard.keyRepeatInFrame[key] = 1;
    else
    {
        CORE.Input.Keyboard.currentKeyState[key] = 1;
        if (CORE.Input.Keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE)
        {
            CORE.Input.Keyboard.keyPressedQueue[CORE.Input.Keyboard.keyPressedQueueCount] = key;
            CORE.Input.Keyboard.keyPressedQueueCount++;
        }
        if (key == KEY_CAPS_LOCK) dosCapsLock = !dosCapsLock;
    }

    const int shift = CORE.Input.Keyboard.currentKeyState[KEY_LEFT_SHIFT] || CORE.Input.Keyboard.currentKeyState[KEY_RIGHT_SHIFT];
    const int ctrl  = CORE.Input.Keyboard.currentKeyState[KEY_LEFT_CONTROL] || CORE.Input.Keyboard.currentKeyState[KEY_RIGHT_CONTROL];
    const int ch = ctrl? 0 : DosKeyChar(key, shift, dosCapsLock);
    if ((ch != 0) && (CORE.Input.Keyboard.charPressedQueueCount < MAX_CHAR_PRESSED_QUEUE))
    {
        CORE.Input.Keyboard.charPressedQueue[CORE.Input.Keyboard.charPressedQueueCount] = ch;
        CORE.Input.Keyboard.charPressedQueueCount++;
    }
}

#if defined(__DJGPP__)
//----------------------------------------------------------------------------------
// Keyboard: the IRQ 1 handler
//----------------------------------------------------------------------------------
// The handler only queues the byte; everything above runs from PollInputEvents, outside
// the interrupt. The queue and the handler are locked so a page fault can never land
// inside the interrupt (CWSDPMI pages to disk).
#define DOS_KEY_QUEUE 256
static volatile unsigned char dosKeyQueue[DOS_KEY_QUEUE];
static volatile unsigned int dosKeyHead = 0, dosKeyTail = 0;
static _go32_dpmi_seginfo dosOldKeyboard, dosNewKeyboard;

static void DosKeyboardIrq(void)
{
    const unsigned char code = inportb(0x60);
    const unsigned int next = (dosKeyHead + 1) & (DOS_KEY_QUEUE - 1);
    if (next != dosKeyTail) { dosKeyQueue[dosKeyHead] = code; dosKeyHead = next; }
    outportb(0x20, 0x20);   // end of interrupt, to the master PIC
}
static void DosKeyboardIrqEnd(void) { }

static int DosHookKeyboard(void)
{
    _go32_dpmi_lock_code((void *)DosKeyboardIrq, (char *)DosKeyboardIrqEnd - (char *)DosKeyboardIrq);
    _go32_dpmi_lock_data((void *)dosKeyQueue, sizeof(dosKeyQueue));
    _go32_dpmi_lock_data((void *)&dosKeyHead, sizeof(dosKeyHead));
    _go32_dpmi_lock_data((void *)&dosKeyTail, sizeof(dosKeyTail));

    _go32_dpmi_get_protected_mode_interrupt_vector(9, &dosOldKeyboard);
    dosNewKeyboard.pm_offset = (unsigned long)DosKeyboardIrq;
    dosNewKeyboard.pm_selector = _go32_my_cs();
    if (_go32_dpmi_allocate_iret_wrapper(&dosNewKeyboard) != 0) return 0;
    _go32_dpmi_set_protected_mode_interrupt_vector(9, &dosNewKeyboard);
    return 1;
}

static void DosUnhookKeyboard(void)
{
    if (!platform.keyboardHooked) return;
    _go32_dpmi_set_protected_mode_interrupt_vector(9, &dosOldKeyboard);
    _go32_dpmi_free_iret_wrapper(&dosNewKeyboard);
    platform.keyboardHooked = 0;
}

//----------------------------------------------------------------------------------
// Screen: VESA BIOS Extensions
//----------------------------------------------------------------------------------
#pragma pack(push, 1)
typedef struct {
    char signature[4];
    uint16_t version;
    uint32_t oemString;
    uint32_t capabilities;
    uint32_t videoModes;            // real-mode far pointer to a 0xFFFF-terminated list
    uint16_t totalMemory;           // in 64 KB blocks
    uint8_t rest[512 - 20];
} VbeInfo;

typedef struct {
    uint16_t attributes;            // bit 0 supported, bit 4 graphics, bit 7 linear framebuffer
    uint8_t winA, winB;
    uint16_t granularity, winSize, segA, segB;
    uint32_t winFunc;
    uint16_t pitch;                 // bytes per scanline
    uint16_t width, height;
    uint8_t charW, charH, planes, bpp, banks, memoryModel, bankSize, pages, reserved0;
    uint8_t rBits, rShift, gBits, gShift, bBits, bShift, xBits, xShift, directColor;
    uint32_t framebuffer;           // physical address of the linear framebuffer
    uint32_t offscreen;
    uint16_t offscreenSize;
    uint16_t linPitch;              // VBE 3: bytes per scanline in linear modes
    uint8_t rest[256 - 52];
} VbeMode;
#pragma pack(pop)

static int DosVbeModeInfo(int mode, VbeMode *info)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F01;
    r.x.cx = mode;
    r.x.es = __tb >> 4;
    r.x.di = __tb & 0x0F;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return 0;
    dosmemget(__tb, sizeof(VbeMode), info);
    return 1;
}

// How well a mode fits what the game asked for: the closest size not larger than the
// request, then the deepest colour (32 bits converts fastest, then 16).
static long DosModeScore(const VbeMode *m, int wantW, int wantH)
{
    if ((m->width > wantW) || (m->height > wantH)) return -1;
    if ((m->width < 640) || (m->height < 400)) return -1;
    long score = (long)m->width*m->height*8;
    switch (m->bpp) { case 32: score += 4; break; case 16: score += 3; break; case 24: score += 2; break; case 15: score += 1; break; default: return -1; }
    return score;
}

// Pick and set a mode. OD_VESA=<width>x<height> overrides the choice, for a card whose
// best mode looks wrong on its monitor.
static int DosSetVideoMode(int wantW, int wantH)
{
    const char *force = getenv("OD_VESA");
    if (force != NULL) sscanf(force, "%dx%d", &wantW, &wantH);

    VbeInfo info;
    memset(&info, 0, sizeof(info));
    memcpy(info.signature, "VBE2", 4);
    dosmemput(&info, sizeof(info), __tb);

    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F00;
    r.x.es = __tb >> 4;
    r.x.di = __tb & 0x0F;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F)
    {
        TRACELOG(LOG_WARNING, "DOS: no VESA BIOS Extensions (load a VBE driver such as UniVBE)");
        return 0;
    }
    dosmemget(__tb, sizeof(info), &info);
    if (memcmp(info.signature, "VESA", 4) != 0 || info.version < 0x0200)
    {
        TRACELOG(LOG_WARNING, "DOS: VESA %d.%d found, 2.0 or later needed for a linear framebuffer", info.version >> 8, info.version & 0xFF);
        return 0;
    }

    // Copy the mode list out first: it may live in the transfer buffer the mode queries
    // below overwrite.
    unsigned short modes[256];
    int count = 0;
    unsigned long list = ((info.videoModes >> 16) << 4) + (info.videoModes & 0xFFFF);
    while (count < 255)
    {
        unsigned short mode = _farpeekw(_dos_ds, list + count*2);
        if (mode == 0xFFFF) break;
        modes[count++] = mode;
    }

    int best = -1;
    long bestScore = -1;
    VbeMode bestInfo;
    for (int i = 0; i < count; i++)
    {
        VbeMode m;
        if (!DosVbeModeInfo(modes[i], &m)) continue;
        if ((m.attributes & 0x91) != 0x91) continue;            // supported, graphics, linear
        if ((m.memoryModel != 6) && !((m.memoryModel == 4) && (m.bpp > 8))) continue;  // direct colour
        if (m.bpp == 16 && m.gBits == 5) m.bpp = 15;            // some BIOSes call 15-bit modes 16
        long s = DosModeScore(&m, wantW, wantH);
        if (s > bestScore) { bestScore = s; best = modes[i]; bestInfo = m; }
    }
    if (best < 0)
    {
        TRACELOG(LOG_WARNING, "DOS: no linear 15/16/24/32-bit VESA mode of 640x400 or more, up to %ix%i", wantW, wantH);
        return 0;
    }

    platform.width = bestInfo.width;
    platform.height = bestInfo.height;
    platform.bpp = bestInfo.bpp;
    platform.pitch = ((info.version >= 0x0300) && bestInfo.linPitch)? bestInfo.linPitch : bestInfo.pitch;
    platform.rShift = bestInfo.rShift; platform.rBits = bestInfo.rBits;
    platform.gShift = bestInfo.gShift; platform.gBits = bestInfo.gBits;
    platform.bShift = bestInfo.bShift; platform.bBits = bestInfo.bBits;
    if (platform.rBits == 0)   // a VBE 1.2-style mode block: assume the usual layouts
    {
        if (platform.bpp == 16) { platform.rShift = 11; platform.rBits = 5; platform.gShift = 5; platform.gBits = 6; platform.bShift = 0; platform.bBits = 5; }
        else if (platform.bpp == 15) { platform.rShift = 10; platform.rBits = 5; platform.gShift = 5; platform.gBits = 5; platform.bShift = 0; platform.bBits = 5; }
        else { platform.rShift = 16; platform.rBits = 8; platform.gShift = 8; platform.gBits = 8; platform.bShift = 0; platform.bBits = 8; }
    }

    // Map the framebuffer and put a selector over it.
    __dpmi_meminfo map;
    map.address = bestInfo.framebuffer;
    map.size = (unsigned long)platform.pitch*platform.height;
    if (__dpmi_physical_address_mapping(&map) != 0)
    {
        TRACELOG(LOG_WARNING, "DOS: the DPMI host would not map the framebuffer at 0x%08lx", (unsigned long)bestInfo.framebuffer);
        return 0;
    }
    platform.lfbLinear = map.address;
    platform.lfbSelector = __dpmi_allocate_ldt_descriptors(1);
    if (platform.lfbSelector < 0) return 0;
    __dpmi_set_segment_base_address(platform.lfbSelector, platform.lfbLinear);
    __dpmi_set_segment_limit(platform.lfbSelector, (map.size - 1) | 0xFFF);

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F02;
    r.x.bx = best | 0x4000;     // linear framebuffer
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F)
    {
        TRACELOG(LOG_WARNING, "DOS: VESA mode 0x%03x could not be set", best);
        return 0;
    }
    platform.modeSet = 1;

    TRACELOG(LOG_INFO, "DOS: VESA %d.%d, mode 0x%03x: %ix%i, %i bits, pitch %i", info.version >> 8, info.version & 0xFF,
             best, platform.width, platform.height, platform.bpp, platform.pitch);
    return 1;
}

static void DosTextMode(void)
{
    if (!platform.modeSet) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);
    platform.modeSet = 0;
}

static void PresentFrame(void)
{
    const int rowBytes = platform.width*((platform.bpp + 7)/8);
    if (platform.pitch == rowBytes)
    {
        movedata(_my_ds(), (unsigned)platform.row, platform.lfbSelector, 0, rowBytes*platform.height);
    }
    else
    {
        for (int y = 0; y < platform.height; y++)
            movedata(_my_ds(), (unsigned)(platform.row + y*rowBytes), platform.lfbSelector, y*platform.pitch, rowBytes);
    }
}

//----------------------------------------------------------------------------------
// Mouse: INT 33h
//----------------------------------------------------------------------------------
static void DosMouseInit(void)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0000;            // reset
    __dpmi_int(0x33, &r);
    if (r.x.ax != 0xFFFF) { TRACELOG(LOG_WARNING, "DOS: no mouse driver (load CTMOUSE or MOUSE.COM)"); return; }
    platform.mouse = 1;

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0007; r.x.cx = 0; r.x.dx = platform.width - 1;     // horizontal range
    __dpmi_int(0x33, &r);
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0008; r.x.cx = 0; r.x.dx = platform.height - 1;    // vertical range
    __dpmi_int(0x33, &r);
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0004; r.x.cx = platform.width/2; r.x.dx = platform.height/2;
    __dpmi_int(0x33, &r);

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0011;            // CuteMouse wheel API: 'WM' and CX bit 0
    __dpmi_int(0x33, &r);
    platform.mouseWheel = (r.x.ax == 0x574D) && (r.x.cx & 1);
    TRACELOG(LOG_INFO, "DOS: mouse driver found%s", platform.mouseWheel? ", with a wheel" : "");
}

static void DosMousePoll(void)
{
    if (!platform.mouse) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0003;
    __dpmi_int(0x33, &r);

    CORE.Input.Mouse.currentPosition.x = (float)(short)r.x.cx;
    CORE.Input.Mouse.currentPosition.y = (float)(short)r.x.dx;
    CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_LEFT]   = (r.x.bx & 1)? 1 : 0;
    CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_RIGHT]  = (r.x.bx & 2)? 1 : 0;
    CORE.Input.Mouse.currentButtonState[MOUSE_BUTTON_MIDDLE] = (r.x.bx & 4)? 1 : 0;
    if (platform.mouseWheel)
    {
        // BH: signed wheel movement since the last call, positive towards the user.
        signed char wheel = (signed char)r.h.bh;
        CORE.Input.Mouse.currentWheelMove.y = -(float)wheel;
    }
    CORE.Input.Touch.position[0] = CORE.Input.Mouse.currentPosition;
}

static void DosMouseSet(int x, int y)
{
    if (!platform.mouse) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0004; r.x.cx = x; r.x.dx = y;
    __dpmi_int(0x33, &r);
}

#else   // !__DJGPP__: the stand-in screen

static void PresentFrame(void)
{
    // Nothing to show it on. The last frame stays in standIn for OD_DOS_FB_DUMP.
}

#endif

//----------------------------------------------------------------------------------
// Frame conversion, shared by DOS and the stand-in
//----------------------------------------------------------------------------------
// The cursor: DOS draws none in a VESA mode, so it is painted into each frame. A
// 12x19 arrow, 'X' outline and '.' fill.
static const char *dosCursor[19] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X..........X", "X......XXXXX", "X...X..X    ", "X..XX..X    ",
    "X.X  X..X   ", "XX   X..X   ", "      X..X  ", "       XX   "
};

// rlsw hands its frame over as BGRA bytes (RLSW_FRAMEBUFFER_OUTPUT_BGRA, on by
// default: it is built to fill an SDL window surface). rcore.c sets
// SW_GL_FRAMEBUFFER_COPY_BGRA false for this platform, but that is the macro's
// old name and rlsw no longer reads it, so the frame is BGRA here too -- read as
// a little-endian word, 0xAARRGGBB, which is the common VESA 32-bit layout as it
// stands.
#if defined(RLSW_FRAMEBUFFER_OUTPUT_BGRA) && RLSW_FRAMEBUFFER_OUTPUT_BGRA
    #define SRC_R(p) (((p) >> 16) & 0xFF)
    #define SRC_B(p) ((p) & 0xFF)
#else
    #define SRC_R(p) ((p) & 0xFF)
    #define SRC_B(p) (((p) >> 16) & 0xFF)
#endif
#define SRC_G(p) (((p) >> 8) & 0xFF)

static inline uint32_t PackPixel(uint32_t src)
{
    const uint32_t r = SRC_R(src), g = SRC_G(src), b = SRC_B(src);
    return ((r >> (8 - platform.rBits)) << platform.rShift) |
           ((g >> (8 - platform.gBits)) << platform.gShift) |
           ((b >> (8 - platform.bBits)) << platform.bShift);
}

static void ConvertFrame(void)
{
    const int w = platform.width, h = platform.height;
    const int bytes = (platform.bpp + 7)/8;
    const unsigned int *src = platform.pixels;
    unsigned char *dst = platform.row;

    // Top row first. The usual 32-bit mode (8:8:8 at 16, 8 and 0) takes each
    // pixel as it comes, with red and blue put where the card wants them.
    if ((bytes == 4) && (platform.rShift == 16) && (platform.gShift == 8) && (platform.bShift == 0))
    {
        uint32_t *out = (uint32_t *)dst;
        for (int i = 0; i < w*h; i++)
        {
            const uint32_t p = src[i];
            out[i] = (SRC_R(p) << 16) | (SRC_G(p) << 8) | SRC_B(p);
        }
    }
    else
    {
        for (int i = 0; i < w*h; i++)
        {
            const uint32_t v = PackPixel(src[i]);
            unsigned char *o = dst + i*bytes;
            o[0] = v & 0xFF;
            if (bytes > 1) o[1] = (v >> 8) & 0xFF;
            if (bytes > 2) o[2] = (v >> 16) & 0xFF;
            if (bytes > 3) o[3] = 0;
        }
    }

    if (CORE.Input.Mouse.cursorHidden) return;
    const uint32_t black = PackPixel(0xFF000000u), white = PackPixel(0xFFFFFFFFu);
    const int cx = (int)CORE.Input.Mouse.currentPosition.x, cy = (int)CORE.Input.Mouse.currentPosition.y;
    for (int y = 0; y < 19; y++)
    {
        if ((cy + y < 0) || (cy + y >= h)) continue;
        for (int x = 0; x < 12; x++)
        {
            const char c = dosCursor[y][x];
            if ((c == ' ') || (cx + x < 0) || (cx + x >= w)) continue;
            const uint32_t v = (c == 'X')? black : white;
            unsigned char *o = dst + ((cy + y)*w + cx + x)*bytes;
            for (int k = 0; k < bytes; k++) o[k] = (v >> (8*k)) & 0xFF;
        }
    }
}

#if !defined(__DJGPP__)
// The stand-in's pixel format, as a card would report it.
static void StandInFormat(int bpp)
{
    platform.bpp = bpp;
    switch (bpp)
    {
        case 16: platform.rShift = 11; platform.rBits = 5; platform.gShift = 5; platform.gBits = 6; platform.bShift = 0; platform.bBits = 5; break;
        case 15: platform.rShift = 10; platform.rBits = 5; platform.gShift = 5; platform.gBits = 5; platform.bShift = 0; platform.bBits = 5; break;
        default: platform.rShift = 16; platform.rBits = 8; platform.gShift = 8; platform.gBits = 8; platform.bShift = 0; platform.bBits = 8; break;
    }
}

// Unpack the stand-in screen back to RGB and write it as a PPM: what a VESA card
// would have shown.
static void StandInDump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return;
    const int bytes = (platform.bpp + 7)/8;
    fprintf(f, "P6\n%d %d\n255\n", platform.width, platform.height);
    for (int i = 0; i < platform.width*platform.height; i++)
    {
        const unsigned char *o = platform.row + i*bytes;
        uint32_t v = 0;
        for (int k = 0; k < bytes; k++) v |= (uint32_t)o[k] << (8*k);
        unsigned char rgb[3];
        const int sh[3] = { platform.rShift, platform.gShift, platform.bShift };
        const int bt[3] = { platform.rBits, platform.gBits, platform.bBits };
        for (int c = 0; c < 3; c++)
        {
            uint32_t x = (v >> sh[c]) & ((1u << bt[c]) - 1);
            rgb[c] = (unsigned char)((x << (8 - bt[c])) | (x >> (2*bt[c] - 8 > 0? 2*bt[c] - 8 : 0)));
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
#endif

//----------------------------------------------------------------------------------
// Module Functions Definition: Window and Graphics Device
//----------------------------------------------------------------------------------

bool WindowShouldClose(void)
{
    if (CORE.Window.ready) return CORE.Window.shouldClose;
    else return true;
}

// The whole screen is the window: there is nothing to toggle, move or resize.
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

// One monitor, the size of the mode.
int GetMonitorCount(void) { return 1; }
int GetCurrentMonitor(void) { return 0; }
Vector2 GetMonitorPosition(int monitor) { (void)monitor; return (Vector2){ 0, 0 }; }
int GetMonitorWidth(int monitor) { (void)monitor; return platform.width; }
int GetMonitorHeight(int monitor) { (void)monitor; return platform.height; }
int GetMonitorPhysicalWidth(int monitor) { (void)monitor; return 0; }
int GetMonitorPhysicalHeight(int monitor) { (void)monitor; return 0; }
int GetMonitorRefreshRate(int monitor) { (void)monitor; return 60; }
const char *GetMonitorName(int monitor) { (void)monitor; return "VESA"; }
Vector2 GetWindowPosition(void) { return (Vector2){ 0, 0 }; }
Vector2 GetWindowScaleDPI(void) { return (Vector2){ 1.0f, 1.0f }; }

// No clipboard in DOS. Copying within the game still works through its own buffer.
void SetClipboardText(const char *text) { (void)text; }
const char *GetClipboardText(void) { return NULL; }
Image GetClipboardImage(void) { Image image = { 0 }; return image; }

void ShowCursor(void) { CORE.Input.Mouse.cursorHidden = false; }
void HideCursor(void) { CORE.Input.Mouse.cursorHidden = true; }
void EnableCursor(void)
{
    SetMousePosition(CORE.Window.screen.width/2, CORE.Window.screen.height/2);
    CORE.Input.Mouse.cursorHidden = false;
    CORE.Input.Mouse.cursorLocked = false;
}
void DisableCursor(void)
{
    SetMousePosition(CORE.Window.screen.width/2, CORE.Window.screen.height/2);
    CORE.Input.Mouse.cursorHidden = true;
    CORE.Input.Mouse.cursorLocked = true;
}

// Swap back buffer with front buffer (screen drawing)
void SwapScreenBuffer(void)
{
    rlCopyFramebuffer(0, 0, CORE.Window.render.width, CORE.Window.render.height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, platform.pixels);
    ConvertFrame();
    PresentFrame();
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Misc
//----------------------------------------------------------------------------------

double GetTime(void)
{
#if defined(__DJGPP__)
    return (double)(uclock() - platform.timeBase)/(double)UCLOCKS_PER_SEC;
#else
    struct timespec ts = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec*1e-9 - platform.timeBase;
#endif
}

// No browser to open. The game shows the address instead (see OpenURL's callers).
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
#if defined(__DJGPP__)
    DosMouseSet(x, y);
#endif
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

#if defined(__DJGPP__)
    while (dosKeyTail != dosKeyHead)
    {
        const unsigned char code = dosKeyQueue[dosKeyTail];
        dosKeyTail = (dosKeyTail + 1) & (DOS_KEY_QUEUE - 1);
        DosFeedScanCode(code);
    }
    DosMousePoll();
#endif

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
        TRACELOG(LOG_FATAL, "PLATFORM: DOS needs the software renderer (GRAPHICS_API_OPENGL_SOFTWARE)");
        return -1;
    }

#if defined(__DJGPP__)
    if (!DosSetVideoMode(CORE.Window.screen.width, CORE.Window.screen.height))
    {
        DosTextMode();
        TRACELOG(LOG_WARNING, "PLATFORM: no usable VESA graphics mode");
        return -1;
    }
    atexit(ClosePlatform);      // a crash or exit() must still leave the screen in text mode
#else
    platform.width = CORE.Window.screen.width;
    platform.height = CORE.Window.screen.height;
    const char *bppEnv = getenv("OD_DOS_FB_BPP");
    StandInFormat(bppEnv? atoi(bppEnv) : 32);
    platform.pitch = platform.width*((platform.bpp + 7)/8);
    atexit(ClosePlatform);      // so OD_DOS_FB_DUMP gets the last frame however the game exits
#endif

    // The game asked for a window; it gets the mode, which may be smaller.
    CORE.Window.screen.width = platform.width;
    CORE.Window.screen.height = platform.height;
    CORE.Window.display.width = platform.width;
    CORE.Window.display.height = platform.height;
    CORE.Window.render.width = platform.width;
    CORE.Window.render.height = platform.height;
    CORE.Window.currentFbo.width = platform.width;
    CORE.Window.currentFbo.height = platform.height;
    CORE.Window.flags |= FLAG_FULLSCREEN_MODE;

    platform.pixels = (unsigned int *)RL_CALLOC((size_t)platform.width*platform.height, sizeof(unsigned int));
    platform.row = (unsigned char *)RL_CALLOC((size_t)platform.width*platform.height, 4);
    if ((platform.pixels == NULL) || (platform.row == NULL))
    {
        TRACELOG(LOG_FATAL, "PLATFORM: out of memory for a %ix%i frame", platform.width, platform.height);
        return -1;
    }

#if defined(__DJGPP__)
    DosMouseInit();
    platform.keyboardHooked = DosHookKeyboard();
    if (!platform.keyboardHooked) TRACELOG(LOG_WARNING, "DOS: could not hook the keyboard interrupt");
    platform.timeBase = uclock();
#else
    platform.timeBase = 0.0;
    platform.timeBase = GetTime();
#endif

    CORE.Window.ready = true;
    CORE.Input.Mouse.currentPosition = (Vector2){ platform.width/2.0f, platform.height/2.0f };

    InitTimer();
    CORE.Storage.basePath = GetWorkingDirectory();

    TRACELOG(LOG_INFO, "PLATFORM: DOS: %ix%i, %i-bit colour", platform.width, platform.height, platform.bpp);
    return 0;
}

void ClosePlatform(void)
{
#if defined(__DJGPP__)
    DosUnhookKeyboard();
    DosTextMode();
    if (platform.lfbSelector > 0) { __dpmi_free_ldt_descriptor(platform.lfbSelector); platform.lfbSelector = 0; }
#else
    const char *dump = getenv("OD_DOS_FB_DUMP");
    if ((dump != NULL) && (platform.row != NULL)) StandInDump(dump);
#endif
    RL_FREE(platform.pixels);
    RL_FREE(platform.row);
    platform.pixels = NULL;
    platform.row = NULL;
}

// EOF
