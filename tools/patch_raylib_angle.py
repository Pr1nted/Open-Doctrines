#!/usr/bin/env python3
"""
Let an OD_ANGLE build find ANGLE beside the game and choose its backend.

    python3 tools/patch_raylib_angle.py <raylib source dir>

WHY THIS EXISTS

OD_ANGLE (experimental, CMakeLists.txt) builds raylib for OpenGL ES 2.0 and
runs it on ANGLE, which translates GLES to Metal on macOS and to Direct3D 11 or
Vulkan on Windows. raylib already asks GLFW for an EGL context when it is built
for ES, and GLFW 3.4 already knows ANGLE. Two things are missing:

1. WHERE THE LIBRARY IS. GLFW dlopens EGL and GLES by leaf name
   ("libEGL.dylib"), which on macOS searches DYLD_LIBRARY_PATH, the current
   directory and /usr/lib -- never the folder the game is in, nor an app
   bundle's Frameworks. On Windows LoadLibrary already looks beside the .exe,
   so only the Apple names change: @executable_path first, then
   @executable_path/../Frameworks, then the leaf name as before.

2. WHICH BACKEND. OD_ANGLE_BACKEND (metal, vulkan, opengl, d3d11, d3d9, or
   swiftshader -- ANGLE's Vulkan backend on its CPU device) is applied where
   GLFW opens the EGL display. GLFW's own ANGLE hint is not enough: on macOS it
   knows only Metal and OpenGL, and a backend it does not know becomes ANGLE's
   default without a word, so a fallback chain would believe it had Vulkan
   while running Metal. Here an unavailable backend fails at eglInitialize,
   InitWindow() returns without a window, and the game tries the next one
   (Game.cpp, odAngleBackends()). An environment variable because raylib has
   no hook this deep; the game sets it before each InitWindow().

3. F12 READ THE WRONG BUFFER. raylib's built-in screenshot key captured after
   SwapScreenBuffer(), when the back buffer is undefined. Desktop GL on macOS
   keeps the pixels; ANGLE's Metal backend does not, so every F12 shot of the
   map was black. It now captures just before the swap.

FATAL WHEN IT FAILS, for the reason the other raylib patches are: an OD_ANGLE
build that cannot find ANGLE does not degrade, it fails to open a window.

Idempotent: running it twice is a no-op.
"""

import os
import sys

EGL_OLD = '''#elif defined(__APPLE__)
        "libEGL.dylib",'''
EGL_NEW = '''#elif defined(__APPLE__)
        // OpenDoctrines (tools/patch_raylib_angle.py): ANGLE ships beside the game.
        "@executable_path/libEGL.dylib",
        "@executable_path/../Frameworks/libEGL.dylib",
        "libEGL.dylib",'''

GLES_OLD = '''#elif defined(__APPLE__)
            "libGLESv2.dylib",'''
GLES_NEW = '''#elif defined(__APPLE__)
            "@executable_path/libGLESv2.dylib",
            "@executable_path/../Frameworks/libGLESv2.dylib",
            "libGLESv2.dylib",'''

DISPLAY_OLD = '''    _glfw.egl.platform = _glfw.platform.getEGLPlatform(&attribs);
    if (_glfw.egl.platform)'''
DISPLAY_NEW = '''    _glfw.egl.platform = _glfw.platform.getEGLPlatform(&attribs);
    // OpenDoctrines (tools/patch_raylib_angle.py): OD_ANGLE_BACKEND names ANGLE's
    // backend. Set here rather than through GLFW's ANGLE hint, which knows only
    // some backends per platform and quietly gives ANGLE's default for the rest
    // -- a fallback chain must see a missing backend FAIL, at eglInitialize,
    // not get another one under its name. The platform's own pairs (X11's
    // native-platform type) are kept; the type and device are replaced.
    {
        const char *odb = getenv("OD_ANGLE_BACKEND");
        EGLint odType = 0, odDevice = 0;
        if (odb != NULL)
        {
            if (strcmp(odb, "metal") == 0) odType = 0x3489;             // TYPE_METAL
            else if (strcmp(odb, "vulkan") == 0) odType = 0x3450;       // TYPE_VULKAN
            else if (strcmp(odb, "swiftshader") == 0) { odType = 0x3450; odDevice = 0x3487; }  // + DEVICE_SWIFTSHADER
            else if (strcmp(odb, "opengl") == 0) odType = 0x320d;       // TYPE_OPENGL
            else if (strcmp(odb, "d3d11") == 0) odType = 0x3208;        // TYPE_D3D11
            else if (strcmp(odb, "d3d9") == 0) odType = 0x3207;         // TYPE_D3D9
        }
        if (odType != 0)
        {
            if (!_glfw.egl.ANGLE_platform_angle)
            {
                _glfwInputError(GLFW_API_UNAVAILABLE, "EGL: OD_ANGLE_BACKEND=%s but this EGL is not ANGLE", odb);
                _glfw_free(attribs);
                _glfwTerminateEGL();
                return GLFW_FALSE;
            }
            int n = 0, k = 0;
            while (attribs != NULL && attribs[n] != EGL_NONE) n += 2;
            EGLint *own = _glfw_calloc(n + 5, sizeof(EGLint));
            for (int i = 0; i < n; i += 2)
            {
                if (attribs[i] == 0x3203 || attribs[i] == 0x3209) continue;   // TYPE, DEVICE_TYPE
                own[k++] = attribs[i];
                own[k++] = attribs[i + 1];
            }
            own[k++] = 0x3203;   // EGL_PLATFORM_ANGLE_TYPE_ANGLE
            own[k++] = odType;
            if (odDevice != 0)
            {
                own[k++] = 0x3209;   // EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE
                own[k++] = odDevice;
            }
            own[k] = EGL_NONE;
            _glfw_free(attribs);
            attribs = own;
            _glfw.egl.platform = EGL_PLATFORM_ANGLE_ANGLE;
        }
    }
    if (_glfw.egl.platform)'''

F12_OLD = '''#if !SUPPORT_CUSTOM_FRAME_CONTROL
    SwapScreenBuffer();                  // Copy back buffer to front buffer (screen)
'''
F12_NEW = '''#if SUPPORT_SCREEN_CAPTURE
    // OpenDoctrines (tools/patch_raylib_angle.py): BEFORE the swap. After it the
    // back buffer is undefined, and ANGLE's Metal backend hands back a fresh,
    // black drawable -- desktop GL only happened to keep the pixels.
    if (IsKeyPressed(KEY_F12))
    {
        TakeScreenshot(TextFormat("screenshot%03i.png", screenshotCounter));
        screenshotCounter++;
    }
#endif

#if !SUPPORT_CUSTOM_FRAME_CONTROL
    SwapScreenBuffer();                  // Copy back buffer to front buffer (screen)
'''
F12_LATE = '''#if SUPPORT_SCREEN_CAPTURE
    if (IsKeyPressed(KEY_F12))
    {
        TakeScreenshot(TextFormat("screenshot%03i.png", screenshotCounter));
        screenshotCounter++;
    }
#endif

    CORE.Time.frameCounter++;'''
F12_LATE_NEW = '''    CORE.Time.frameCounter++;'''

# (file, anchor, replacement, the text that shows the edit is already in).
# Checked per edit rather than once, so a raylib patched by an earlier version
# of this script still picks up the edits added since.
F12_DONE = "BEFORE the swap"
EDITS = [
    ("src/external/glfw/src/egl_context.c", EGL_OLD, EGL_NEW, "@executable_path/libEGL.dylib"),
    ("src/external/glfw/src/egl_context.c", GLES_OLD, GLES_NEW, "@executable_path/libGLESv2.dylib"),
    ("src/external/glfw/src/egl_context.c", DISPLAY_OLD, DISPLAY_NEW, "OD_ANGLE_BACKEND"),
    # Order matters: drop the late capture before adding the early one, or the
    # second anchor would match the block just inserted.
    ("src/rcore.c", F12_LATE, F12_LATE_NEW, F12_DONE),
    ("src/rcore.c", F12_OLD, F12_NEW, F12_DONE),
]


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    root = argv[1]
    texts = {}
    for rel, _, _, _ in EDITS:
        path = os.path.join(root, rel)
        if path not in texts:
            try:
                with open(path, "r", encoding="utf-8") as f:
                    texts[path] = f.read()
            except OSError as e:
                print(f"patch_raylib_angle: cannot read {path}: {e}", file=sys.stderr)
                return 1

    applied = 0
    for rel, old, new, done in EDITS:
        path = os.path.join(root, rel)
        if done in texts[path]:
            continue
        if texts[path].count(old) != 1:
            print(f"patch_raylib_angle: expected exactly one anchor in {rel}, found "
                  f"{texts[path].count(old)}. raylib or its bundled GLFW has changed; "
                  f"the patch must be re-derived before an OD_ANGLE build can open a window.",
                  file=sys.stderr)
            return 1
        texts[path] = texts[path].replace(old, new, 1)
        applied += 1

    if applied == 0:
        print("patch_raylib_angle: already applied")
        return 0
    for path, src in texts.items():
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(src)
        os.replace(tmp, path)
    print(f"patch_raylib_angle: applied {applied} edit(s): ANGLE lookup, OD_ANGLE_BACKEND, F12 before the swap")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
