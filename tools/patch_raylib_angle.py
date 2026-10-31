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

2. WHICH BACKEND. ANGLE's backend is a GLFW *init* hint, so it must be set
   before glfwInit(), which raylib calls inside InitWindow() -- the game has no
   earlier moment to reach. The hint is read from OD_ANGLE_BACKEND
   (metal, vulkan, opengl, d3d11, d3d9; unset or anything else lets ANGLE
   choose). An environment variable rather than an API because raylib has no
   hook here, and the game sets it from its config before InitWindow().

FATAL WHEN IT FAILS, for the reason the other raylib patches are: an OD_ANGLE
build that cannot find ANGLE does not degrade, it fails to open a window.

Idempotent: running it twice is a no-op.
"""

import os
import sys

MARKER = "OD_ANGLE_BACKEND"

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

INIT_OLD = '''    // Initialize GLFW internal global state
    int result = glfwInit();'''
INIT_NEW = '''    // OpenDoctrines (tools/patch_raylib_angle.py): pick ANGLE's backend.
    // Only an ES build reaches EGL, so this is inert anywhere else.
    {
        const char *b = getenv("OD_ANGLE_BACKEND");
        int t = GLFW_ANGLE_PLATFORM_TYPE_NONE;
        if (b != NULL)
        {
            if (strcmp(b, "metal") == 0) t = GLFW_ANGLE_PLATFORM_TYPE_METAL;
            else if (strcmp(b, "vulkan") == 0) t = GLFW_ANGLE_PLATFORM_TYPE_VULKAN;
            else if (strcmp(b, "opengl") == 0) t = GLFW_ANGLE_PLATFORM_TYPE_OPENGL;
            else if (strcmp(b, "d3d11") == 0) t = GLFW_ANGLE_PLATFORM_TYPE_D3D11;
            else if (strcmp(b, "d3d9") == 0) t = GLFW_ANGLE_PLATFORM_TYPE_D3D9;
        }
        glfwInitHint(GLFW_ANGLE_PLATFORM_TYPE, t);
    }
    // Initialize GLFW internal global state
    int result = glfwInit();'''

EDITS = [
    ("src/external/glfw/src/egl_context.c", EGL_OLD, EGL_NEW),
    ("src/external/glfw/src/egl_context.c", GLES_OLD, GLES_NEW),
    ("src/platforms/rcore_desktop_glfw.c", INIT_OLD, INIT_NEW),
]


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    root = argv[1]
    texts = {}
    for rel, _, _ in EDITS:
        path = os.path.join(root, rel)
        if path not in texts:
            try:
                with open(path, "r", encoding="utf-8") as f:
                    texts[path] = f.read()
            except OSError as e:
                print(f"patch_raylib_angle: cannot read {path}: {e}", file=sys.stderr)
                return 1

    glfw = os.path.join(root, "src/platforms/rcore_desktop_glfw.c")
    if MARKER in texts[glfw]:
        print("patch_raylib_angle: already applied")
        return 0

    for rel, old, new in EDITS:
        path = os.path.join(root, rel)
        if texts[path].count(old) != 1:
            print(f"patch_raylib_angle: expected exactly one anchor in {rel}, found "
                  f"{texts[path].count(old)}. raylib or its bundled GLFW has changed; "
                  f"the patch must be re-derived before an OD_ANGLE build can open a window.",
                  file=sys.stderr)
            return 1
        texts[path] = texts[path].replace(old, new, 1)

    for path, src in texts.items():
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(src)
        os.replace(tmp, path)
    print("patch_raylib_angle: ANGLE lookup beside the game, backend from OD_ANGLE_BACKEND")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
