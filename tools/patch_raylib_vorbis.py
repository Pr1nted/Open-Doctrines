#!/usr/bin/env python3
"""
Teach raylib's stb_vorbis that a codebook with no entries is not an error.

    python3 tools/patch_raylib_vorbis.py <path to stb_vorbis.c>

WHY THIS EXISTS

The shipped Ogg files are run through OptiVorbis, which rebuilds each Vorbis
stream's entropy coding for the audio actually present. It is lossless by
construction and worth 9% of the soundtrack -- 4.18 MB off the download, more
than everything else in the release put together.

stb_vorbis, which is what raylib decodes Ogg with, cannot open the results. It
reports VORBIS_outofmem, which is why this looked for a long time like a format
incompatibility and was written off as one. It is not. OptiVorbis emits a
codebook with `entries == 0` -- legal Vorbis, accepted by libvorbis and by
ffmpeg -- and stb_vorbis allocates its tables like this:

    static void *setup_malloc(vorb *f, int sz) { ... return sz ? malloc(sz) : NULL; }

A zero-size request returns NULL BY DESIGN, and two callers read that NULL as
an allocation failure. So the decoder rejects a valid file on the grounds that
it ran out of memory while asking for nothing.

The fix is to check the two allocations only when something was actually
requested. It is two lines, it changes no behaviour for any file that worked
before, and it is a bug worth reporting upstream to stb.

WHY A SCRIPT, AND WHY IT IS FATAL WHEN IT FAILS

raylib is pinned at 5.5 and built from source (see CMakeLists.txt, which stops
using a system copy precisely so this patch is guaranteed to be in the binary).
Applying it here rather than vendoring a whole fork keeps the delta to the two
lines that matter and visible in one file.

If it cannot be applied, the build MUST stop. An unpatched decoder against
optimised audio is not a degraded game, it is a game with no music and no sound
effects at all, and nothing else in the build would notice. Exiting non-zero is
what makes that impossible.

Idempotent: running it twice is a no-op, which matters because CMake re-runs
configure far more often than it re-fetches raylib.
"""

import os
import sys

# (what must be there, what it becomes). Both are the same shape: an allocation
# sized from c->entries, and a NULL check that has to allow the zero case.
# Each hunk carries the ALLOCATION line as well as the check, because the check
# alone is not unique: `if (!c->codewords)` appears twice, once in the
# non-sparse branch sized from c->entries and once in the sparse branch sized
# from c->sorted_entries. Only the first can be asked for zero -- the sparse one
# already sits inside `if (c->sorted_entries)` -- and patching the wrong one
# would suppress a real allocation failure. The two-line anchor picks the right
# one and would stop matching if raylib ever restructured either branch.
PATCHES = [
    ("      if (!lengths) return error(f, VORBIS_outofmem);",
     "      if (c->entries && !lengths) return error(f, VORBIS_outofmem);"),
    ("         c->codewords = (uint32 *) setup_malloc(f, sizeof(c->codewords[0]) * c->entries);\n"
     "         if (!c->codewords)                  return error(f, VORBIS_outofmem);",
     "         c->codewords = (uint32 *) setup_malloc(f, sizeof(c->codewords[0]) * c->entries);\n"
     "         if (c->entries && !c->codewords)     return error(f, VORBIS_outofmem);"),
]


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    path = argv[1]
    if not os.path.isfile(path):
        print(f"patch_raylib_vorbis: no such file: {path}", file=sys.stderr)
        return 1

    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    applied = already = 0
    for want, become in PATCHES:
        if become in src:
            already += 1
            continue
        n = src.count(want)
        if n != 1:
            print(f"patch_raylib_vorbis: expected exactly one occurrence of\n"
                  f"    {want.strip()}\n"
                  f"  in {path}, found {n}. raylib's stb_vorbis has moved; the patch "
                  f"must be re-derived before the optimised audio can ship.",
                  file=sys.stderr)
            return 1
        src = src.replace(want, become, 1)
        applied += 1

    if applied:
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(src)
        os.replace(tmp, path)
        print(f"patch_raylib_vorbis: applied {applied} hunk(s) to {os.path.basename(path)}")
    else:
        print(f"patch_raylib_vorbis: already applied ({already} hunk(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
