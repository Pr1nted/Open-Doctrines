#!/usr/bin/env python3
"""
Give raylib a SetFontDefault(), so the game can point DrawText at a readable
vector font instead of the built-in 10-pixel bitmap.

    python3 tools/patch_raylib_default_font.py <path to rtext.c>

WHY THIS EXISTS

Every ASCII label in the interface is drawn with raylib's DrawText, which has
no font argument: it always uses the file-static `defaultFont` in rtext.c, a
224-glyph 10-pixel bitmap. That is the blocky type players reported as hard to
read (itch/Discord, "can there be different fonts... I have a difficult time
reading"). The game already loads DejaVuSans for its non-ASCII glyphs; it just
had no way to route the 276 DrawText call sites through it, because `defaultFont`
is static and raylib ships no setter for it.

This adds one: `void SetFontDefault(Font font)` assigns the static. The game
then offers a Settings -> Font choice; picking "Readable" calls it with the
DejaVuSans-based atlas and every DrawText sharpens at once, with no change to a
single call site. "Pixel" (the default) never calls it, so the stock bitmap and
the layouts tuned against its metrics are untouched for anyone who does not opt
in.

WHY A SCRIPT, AND WHY IT IS FATAL WHEN IT FAILS

raylib is pinned (CMakeLists.txt's GIT_TAG) and built from source (CMakeLists.txt). Applying the
delta here rather than vendoring a fork keeps it to the handful of lines that
matter and visible in one file -- the same reasoning as patch_raylib_vorbis.py.
If it cannot be applied the build MUST stop: the game declares SetFontDefault
and links against it, so an unpatched rtext.c is not a degraded game, it is a
link error -- but exiting non-zero here names the cause instead of leaving it to
a screen of undefined-symbol output.

Idempotent: running it twice is a no-op, which matters because CMake re-runs
configure far more often than it re-fetches raylib.
"""

import sys

MARKER = "SetFontDefault"

# Anchors: the end of GetFontDefault(), unique in rtext.c and the natural
# place for a sibling setter. Two shapes, because raylib changed it: 5.5
# guarded the body with SUPPORT_DEFAULT_FONT, and 6.x removed that switch (the
# default font is always built) so the body is a bare return. Each anchor gets
# the setter that matches -- an #if on a macro 6.x no longer defines would
# compile to a setter that silently does nothing.
ANCHOR_55 = """Font GetFontDefault()
{
#if defined(SUPPORT_DEFAULT_FONT)
    return defaultFont;
#else
    Font font = { 0 };
    return font;
#endif
}"""
ADDITION_55 = """
// Added by OpenDoctrines (tools/patch_raylib_default_font.py): point DrawText
// at a readable vector font instead of the built-in bitmap. The game declares
// this itself and calls it only when the player picks the readable font.
void SetFontDefault(Font font)
{
#if defined(SUPPORT_DEFAULT_FONT)
    defaultFont = font;
#else
    (void)font;
#endif
}"""
ANCHOR_6 = """Font GetFontDefault()
{
    return defaultFont;
}"""
ADDITION_6 = """
// Added by OpenDoctrines (tools/patch_raylib_default_font.py): point DrawText
// at a readable vector font instead of the built-in bitmap. The game declares
// this itself and calls it only when the player picks the readable font.
void SetFontDefault(Font font)
{
    defaultFont = font;
}"""
ANCHORS = [(ANCHOR_55, ADDITION_55), (ANCHOR_6, ADDITION_6)]


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    path = argv[1]
    try:
        with open(path, "r", encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        print(f"patch_raylib_default_font: cannot read {path}: {e}", file=sys.stderr)
        return 1

    # Already applied? The setter's definition is the tell -- GetFontDefault
    # mentions defaultFont too, so match the function signature, not the name.
    if "void SetFontDefault(Font font)" in src:
        print("patch_raylib_default_font: already applied")
        return 0

    found = [(a, add) for a, add in ANCHORS if src.count(a) == 1]
    if len(found) != 1:
        print(f"patch_raylib_default_font: expected exactly one GetFontDefault "
              f"body (raylib 5.5 or 6.x shape) to anchor on in {path}, found "
              f"{len(found)}. raylib's rtext.c has changed; the patch must be "
              f"re-derived before the build can link.",
              file=sys.stderr)
        return 1

    anchor, addition = found[0]
    src = src.replace(anchor, anchor + addition, 1)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(src)
    import os
    os.replace(tmp, path)
    print("patch_raylib_default_font: added SetFontDefault() to rtext.c")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
