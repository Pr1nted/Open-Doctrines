#!/usr/bin/env python3
"""
Write zip archives whose members are deflated as hard as deflate goes.

    from zopfli_zip import zopfli_deflate
    with zopfli_deflate():
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            ...

The result is an ORDINARY zip. Zopfli emits a normal deflate stream -- every
unzip, miniz, and Python's own zipfile read it without knowing anything is
different -- it just searches far longer for a smaller one. Worth about 1.5% of
a release zip and 3% of a .odmap, which on this project is a megabyte and a
half between them.

Done by swapping the compressor zipfile reaches for, rather than by writing the
archive by hand. Headers, the central directory, the file modes and the
executable bit stay zipfile's job, and the single thing that changes is the
single thing that needed to.

DELIBERATELY DEPENDENCY-LIGHT. tools/package.py runs on release CI, where the
fewer imports the better; this module pulls in nothing but the standard library
and zopfli itself, so packaging never drags numpy and Pillow along behind it.
Without zopfli installed everything still works and the archives are a few
percent larger.
"""

import contextlib
import zipfile
import zlib

try:
    import zopfli.zlib as _zopfli
except ImportError:                                     # pragma: no cover
    _zopfli = None

# Below this ratio, deflate has already found everything there is: the member is
# a PNG, an Ogg, a nested .odmap or the AI model, all compressed already.
# Running Zopfli over those costs minutes and returns nothing.
WORTH_IT = 0.98

# Past a few megabytes the extra iterations stop paying. Measured on the 100 MB
# province layer: 15 iterations beat 5 by 242 bytes and cost three more minutes.
BIG = 4_000_000


def available():
    return _zopfli is not None


class _ZopfliCompressor:
    """Looks like zlib's compressobj to zipfile; buffers, then thinks hard.

    zipfile feeds a member through .compress() and closes it with .flush(),
    counting whatever comes back as the compressed size -- so returning
    everything at the end is fine, and is what lets a whole-member algorithm
    stand in for a streaming one.
    """

    def __init__(self):
        self._buf = bytearray()

    def compress(self, data):
        self._buf += data
        return b""

    def flush(self, *args):
        raw = bytes(self._buf)
        self._buf = bytearray()
        # Raw deflate, no zlib wrapper: a zip member is the bare stream, so the
        # two-byte header and four-byte Adler-32 come off both candidates.
        best = zlib.compress(raw, 9)[2:-4]
        if _zopfli is not None and raw and len(best) < len(raw) * WORTH_IT:
            cand = _zopfli.compress(raw, numiterations=15 if len(raw) < BIG else 5)[2:-4]
            if len(cand) < len(best):
                best = cand
        return best


@contextlib.contextmanager
def zopfli_deflate(enabled=True):
    """Make zipfile's DEFLATED members go through Zopfli for the duration."""
    if not enabled or _zopfli is None:
        yield
        return
    original = zipfile._get_compressor

    def patched(compress_type, compresslevel=None):
        if compress_type == zipfile.ZIP_DEFLATED:
            return _ZopfliCompressor()
        return original(compress_type, compresslevel)

    zipfile._get_compressor = patched
    try:
        yield
    finally:
        zipfile._get_compressor = original
