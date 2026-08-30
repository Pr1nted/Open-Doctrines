#!/usr/bin/env python3
"""
Re-deflate the AI model's ODAZ container as hard as deflate goes.

    python3 tools/shrink_model.py                    # data/ai/model.bin, in place
    python3 tools/shrink_model.py path/to/model.bin
    python3 tools/shrink_model.py --check            # report only, write nothing

WHY

data/ai/model.bin is an ODAZ container: the ODAI bytes with the four byte
positions of every float separated into their own streams, then deflated (see
src/ai/ModelBlob.h). The game writes it with miniz, which is the right choice
there -- saveModel() runs every twenty turns during training and cannot afford
to think for a minute about it.

Shipping is the other case. The file is written once and downloaded by
everybody, so it is worth spending the minute: Zopfli finds a smaller stream of
exactly the same kind, and miniz inflates it without knowing the difference.
Worth about 115 KB.

NOT A DIFFERENT FORMAT AND NOT A DIFFERENT MODEL. The container, the header and
the deinterleave are untouched; only the deflate stream inside is replaced, and
every weight is bit for bit what it was. That is checked rather than claimed --
the result is unpacked again and compared with the input before anything is
written.

The transform itself was measured and left alone. Splitting the floats by FIELD
instead of by byte (one sign plane, one exponent plane, three mantissa planes)
comes out 8 KB LARGER, and delta-coding the exponent plane larger still: both
break the byte alignment the LZ matcher works on. The byte deinterleave the
container already uses is the best of the three.

Needs the zopfli package (python3 -m pip install zopfli); without it there is
nothing to do and the file is left alone.
"""

import argparse
import os
import struct
import sys
import zlib

try:
    import zopfli.zlib as _zopfli
except ImportError:                                     # pragma: no cover
    _zopfli = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = os.path.join(ROOT, "data", "ai", "model.bin")

MAGIC = b"ODAZ"
HEADER = 14          # magic(4) + format(1) + reserved(1) + raw length(8, LE)


def repack(data):
    """(new_bytes, payload_length). Returns None when there is nothing to do.

    What the container holds is ALREADY the deinterleaved bytes -- the transform
    happens before the deflate, so inflating the payload gives the transformed
    form back, not the plain ODAI. Re-applying the deinterleave here produced a
    double-transformed buffer that compressed WORSE than what was on disk, which
    the "already as small as deflate goes" guard then reported as success. The
    only thing to do at this level is deflate the very same bytes harder.
    """
    if data[:4] != MAGIC or len(data) < HEADER:
        return None, 0
    raw_len = struct.unpack("<Q", data[6:HEADER])[0]
    body = zlib.decompress(data[HEADER:])
    if len(body) != raw_len:
        raise ValueError("container length disagrees with its payload")
    if _zopfli is None:
        return None, raw_len

    # numiterations is low because the payload is megabytes; past a handful the
    # extra passes buy bytes, not kilobytes. See tools/zopfli_zip.py.
    squeezed = _zopfli.compress(body, numiterations=5)
    if len(squeezed) >= len(data) - HEADER:
        return None, raw_len

    out = data[:HEADER] + squeezed
    # The gate: inflate what we are about to write and insist it is the same
    # bytes. This file is the only copy of every hour of training in it.
    if zlib.decompress(out[HEADER:]) != body:
        raise ValueError("the re-deflated payload did not round-trip")
    return out, raw_len


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("model", nargs="?", default=DEFAULT)
    ap.add_argument("--check", action="store_true",
                    help="report what would be saved and write nothing")
    args = ap.parse_args()

    if not os.path.isfile(args.model):
        print(f"no such file: {args.model}", file=sys.stderr)
        return 1
    if _zopfli is None:
        print("zopfli is not installed, so the model is left as it is:\n"
              "  python3 -m pip install zopfli", file=sys.stderr)
        return 0

    with open(args.model, "rb") as f:
        data = f.read()
    if data[:4] != MAGIC:
        print(f"{args.model} is not an ODAZ container -- pack it first with "
              f"the ModelPack tool.", file=sys.stderr)
        return 1

    out, raw_len = repack(data)
    if out is None:
        print(f"  {os.path.relpath(args.model, ROOT)}: already as small as "
              f"deflate goes ({len(data)} bytes)")
        return 0

    verb = "would shrink" if args.check else "shrank"
    print(f"  {os.path.relpath(args.model, ROOT)}: {len(data)} -> {len(out)} bytes "
          f"({len(out) / len(data):.1%} of it, {len(data) - len(out)} saved), "
          f"{raw_len} plain, {verb}")
    if args.check:
        return 0

    tmp = args.model + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, args.model)
    return 0


if __name__ == "__main__":
    sys.exit(main())
