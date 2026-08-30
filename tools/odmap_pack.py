#!/usr/bin/env python3
"""
How a .odmap is written: one encoder, shared by every tool that rewrites one.

WHY

A shipped map was six megabytes, and four and a half of them were political.png
-- a layer the game does not read. Game_Loading.cpp's needed[] list does not
name it; generatePoliticalTexture() draws the board from provinces.png and
countries.json at load, every time. The file existed for the map editor and for
thumbnails, and it is a pure function of data already in the archive, so it is
not carried any more. MapEditor::computePoliticalGradient() is the same
algorithm as the game's and rebuilds it in the one place that wanted it.

land_sea.png was the other half of the waste. It answers one question per pixel
-- land or sea -- and LandSeaMap thresholds it straight back down to that bit on
load, yet it shipped as 32-bit RGBA. Written as an indexed PNG at the bit depth
its palette actually needs, it decodes to byte-identical RGBA and costs less
than half as much. stb_image expands PLTE/tRNS, so nothing on the reading side
changes.

Neither is a quality trade. Every layer here round-trips to the same pixels it
went in as; verify_lossless() is the assertion, and it runs on every write.

WHAT WAS LEFT ON THE TABLE, AND IS NOT ANY MORE

Two things, both about HOW the surviving pixels were deflated rather than about
which pixels survived.

The first is the scanline filter. PNG lets each row be stored as a delta
against its neighbours, and both encoders in use here -- Pillow's optimize=True
and stb_image_write -- pick one per row by the same heuristic, which is tuned
for photographs. A map layer is not a photograph. It is broad regions of one
flat colour, so the UNfiltered bytes are already long runs, and every filter
turns those runs into deltas that deflate then has to encode. On the world
map's province layer the heuristic's choice cost 18%: 656 KB filtered its way,
547 KB with no filter at all. _best_idat now tries each strategy and keeps
whichever is genuinely smaller, rather than trusting either guess.

The second is deflate itself. Zopfli emits an ordinary deflate stream -- miniz,
stb_image, any unzip and any PNG decoder read it with no idea anything is
different -- by spending far longer looking for one. It is another 10-15% on
these layers. It is also minutes rather than seconds, which is why it is an
EFFORT_MAX opt-in taken by the compaction pass and by nothing that runs during
map generation.

Neither touches a pixel, and neither needs a reader to change: filtering and
the deflate stream are internal to a PNG, and the format's own decoder puts
them back. optimize_png() applies both to images this module did not build --
the thumbnail and a couple of hundred flags -- keeping every other chunk of
those files byte for byte, so licence text in a tEXt chunk is not quietly lost
to a size optimisation.

    from odmap_pack import layer_png, optimize_png, read_members, write_odmap
"""

import io
import os
import shutil
import struct
import sys
import tempfile
import zlib
import zipfile

import numpy as np
from PIL import Image

from zopfli_zip import zopfli_deflate

try:
    import zopfli.zlib as _zopfli
except ImportError:                                     # pragma: no cover
    _zopfli = None

Image.MAX_IMAGE_PIXELS = None   # 8192x4096 is over Pillow's decompression-bomb guard

# Layers computed at load rather than stored. Kept out of new archives; still
# read from old ones, which is why nothing needed a format version bump.
DERIVED = ("political.png",)

MAX_PALETTE = 256


def _chunk(tag, data):
    return (len(data).to_bytes(4, "big") + tag + data
            + zlib.crc32(tag + data).to_bytes(4, "big"))


# How hard to press deflate. "fast" is zlib at 9 and is what every tool that
# writes a map during generation uses. "max" adds Zopfli, which emits an
# ordinary deflate stream -- any unzip, any PNG decoder, stb_image and miniz all
# read it unchanged -- by spending far longer searching for one. It is 5-15%
# smaller and minutes rather than seconds, so it belongs in the compaction pass
# (tools/shrink_maps.py) and nowhere else.
EFFORT_FAST = "fast"
EFFORT_MAX = "max"

_warned_no_zopfli = False


def _deflate(data, effort=EFFORT_FAST):
    """zlib stream, as small as the effort asked for."""
    global _warned_no_zopfli
    if effort == EFFORT_MAX:
        if _zopfli is not None:
            # Past a few megabytes the extra iterations stop paying: on the
            # 100 MB province layer, 15 iterations beat 5 by 242 bytes and cost
            # three more minutes. Spend them where they are still worth it.
            iters = 15 if len(data) < 4_000_000 else 5
            return _zopfli.compress(data, numiterations=iters)
        if not _warned_no_zopfli:
            _warned_no_zopfli = True
            print("  note: zopfli is not installed, falling back to zlib -- maps will be\n"
                  "        a few percent larger. python3 -m pip install zopfli",
                  file=sys.stderr)
    return zlib.compress(data, 9)


def _filtered(rows, bpp, strategy):
    """Scanlines -> PNG-filtered scanlines, each with its filter byte in front.

    Vectorised because it can be: a PNG filter subtracts a prediction made from
    UNfiltered neighbours, so every byte of a row is independent of every other
    byte of the output. Only undoing it is serial.
    """
    h, stride = rows.shape
    out = np.empty((h, stride + 1), dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.uint8)
    for y in range(h):
        line = rows[y]
        if strategy == "minsum":
            best = None
            for ft in range(5):
                enc = _filter_row(line, prev, ft, bpp)
                # The heuristic from the PNG spec: treat the filtered bytes as
                # signed and take the smallest absolute sum, as a stand-in for
                # the entropy deflate is about to have to encode.
                score = int(np.minimum(enc, 256 - enc.astype(np.int16)).sum())
                if best is None or score < best[0]:
                    best = (score, ft, enc)
            out[y, 0], out[y, 1:] = best[1], best[2]
        else:
            out[y, 0] = strategy
            out[y, 1:] = _filter_row(line, prev, strategy, bpp)
        prev = line
    return out.tobytes()


def _filter_row(line, prev, ft, bpp):
    if ft == 0:
        return line
    left = np.zeros_like(line)
    left[bpp:] = line[:-bpp]
    if ft == 1:
        return (line - left).astype(np.uint8)
    if ft == 2:
        return (line - prev).astype(np.uint8)
    if ft == 3:
        return (line - ((left.astype(np.uint16) + prev) >> 1).astype(np.uint8)).astype(np.uint8)
    upleft = np.zeros_like(prev)
    upleft[bpp:] = prev[:-bpp]
    p = left.astype(np.int16) + prev - upleft
    pa, pb, pc = np.abs(p - left), np.abs(p - prev), np.abs(p - upleft)
    pred = np.where((pa <= pb) & (pa <= pc), left, np.where(pb <= pc, prev, upleft))
    return (line - pred.astype(np.uint8)).astype(np.uint8)


# Every filter, plus the spec's per-row heuristic. Searched rather than guessed
# at, because the guesses are wrong here: Pillow's optimize=True picks per row
# and produced a province layer 18% larger than filter 0 flat, and stb_image_write
# picks the same way. Map layers are broad regions of one colour, so the
# unfiltered bytes are already long runs and any filter turns them into deltas
# that deflate then has to encode.
_FILTER_STRATEGIES = (0, 1, 2, 3, 4, "minsum")


def _best_idat(rows, bpp, effort):
    """The smallest deflate stream over any filter strategy, and its bytes.

    Every strategy is tried over the whole image, not over a sample of it. A
    sample was measured and rejected: on flat art it picks the same winner, but
    on the store banners -- gradients, where the choice is genuinely close --
    four bands of 64 rows chose "minsum" where the whole image chooses Sub, and
    paid 3.2% for it. The ranking is not where the time goes anyway; filtering
    and deflating the winner over 134 MB is.
    """
    ranked = sorted(((len(zlib.compress(_filtered(rows, bpp, st), 6)), st)
                     for st in _FILTER_STRATEGIES), key=lambda t: t[0])
    # Ranked at level 6 and finished at the real level: the candidates differ by
    # tens of percent, far more than the gap between deflate levels, so the
    # cheap ranking picks the same winner and costs one expensive pass, not six.
    return _deflate(_filtered(rows, bpp, ranked[0][1]), effort)


def indexed_png(arr, effort=EFFORT_FAST):
    """RGBA array -> indexed PNG bytes, or None past 256 distinct colours.

    Filter 0 (None) on every row on purpose. The adaptive filters make
    neighbouring BYTES similar, which is what helps a photograph; on packed
    palette indices they turn a run of identical pixels into a run of deltas
    that deflate handles no better.
    """
    if arr.ndim != 3 or arr.shape[2] != 4:
        return None
    h, w = arr.shape[:2]
    flat = arr.reshape(-1, 4)
    key = (flat[:, 0].astype(np.uint32) << 24 | flat[:, 1].astype(np.uint32) << 16
           | flat[:, 2].astype(np.uint32) << 8 | flat[:, 3].astype(np.uint32))
    uniq = np.unique(key)
    if len(uniq) > MAX_PALETTE:
        return None

    bits = 1 if len(uniq) <= 2 else 2 if len(uniq) <= 4 else 4 if len(uniq) <= 16 else 8
    idx = np.searchsorted(uniq, key).astype(np.uint8).reshape(h, w)

    palette = b"".join(bytes(((int(u) >> 24) & 255, (int(u) >> 16) & 255,
                              (int(u) >> 8) & 255)) for u in uniq)
    alpha = bytes(int(u) & 255 for u in uniq)

    if bits == 8:
        rows = idx
    else:
        # MSB first within each byte, which is how the PNG spec packs indices.
        packed = np.unpackbits(idx, axis=1).reshape(h, w, 8)[:, :, 8 - bits:]
        rows = np.packbits(packed.reshape(h, -1), axis=1)
    raw = b"".join(b"\x00" + rows[y].tobytes() for y in range(h))

    out = b"\x89PNG\r\n\x1a\n"
    out += _chunk(b"IHDR", w.to_bytes(4, "big") + h.to_bytes(4, "big")
                  + bytes([bits, 3, 0, 0, 0]))
    out += _chunk(b"PLTE", palette)
    if any(a != 255 for a in alpha):
        # tRNS may stop after the last non-opaque entry; the rest default opaque.
        last = max(i for i, a in enumerate(alpha) if a != 255) + 1
        out += _chunk(b"tRNS", alpha[:last])
    out += _chunk(b"IDAT", _deflate(raw, effort))
    out += _chunk(b"IEND", b"")
    return out


def truecolour_png(arr, effort=EFFORT_FAST):
    """RGB(A) array -> PNG, with the filter strategy searched rather than guessed.

    An all-opaque alpha channel is dropped rather than stored. It carries no
    information -- a truecolour PNG without one decodes to alpha 255 -- and
    keeping it costs a byte a pixel, 33 MB of deflate input on a map layer.

    Written here rather than handed to Pillow because Pillow's optimize=True
    chooses a filter per row by the same heuristic stb_image_write uses, and on
    a province layer that heuristic is simply wrong: the shipped map's IDAT was
    656 KB filtered its way and 547 KB with no filter at all. _best_idat tries
    each and keeps whichever is actually smaller.
    """
    if arr.ndim == 3 and arr.shape[2] == 4 and bool((arr[:, :, 3] == 255).all()):
        arr = arr[:, :, :3]
    if arr.ndim == 2:
        arr = arr[:, :, None]
    h, w, ch = arr.shape
    colour = {1: 0, 2: 4, 3: 2, 4: 6}.get(ch)
    if colour is None:
        return None
    rows = np.ascontiguousarray(arr).reshape(h, w * ch)
    out = b"\x89PNG\r\n\x1a\n"
    out += _chunk(b"IHDR", w.to_bytes(4, "big") + h.to_bytes(4, "big")
                  + bytes([8, colour, 0, 0, 0]))
    out += _chunk(b"IDAT", _best_idat(rows, ch, effort))
    out += _chunk(b"IEND", b"")
    return out


def _as_rgba(arr_or_img):
    if isinstance(arr_or_img, Image.Image):
        return np.array(arr_or_img.convert("RGBA"))
    a = np.asarray(arr_or_img)
    if a.ndim == 2:
        return np.dstack([a, a, a, np.full_like(a, 255)])
    if a.shape[2] == 3:
        return np.dstack([a, np.full(a.shape[:2], 255, np.uint8)])
    return a


def verify_lossless(encoded, original_rgba):
    """Decode what we are about to write and insist it is the same pixels."""
    back = np.array(Image.open(io.BytesIO(encoded)).convert("RGBA"))
    return back.shape == original_rgba.shape and np.array_equal(back, original_rgba)


def layer_png(arr_or_img, effort=EFFORT_FAST):
    """Smallest lossless PNG for a map layer.

    Indexed when the palette fits (land_sea's three colours, most generated
    province layers), truecolour otherwise (the shipped province layers carry
    over a thousand ids). Whichever wins, the bytes are decoded again and
    compared before they are returned -- an encoder that silently altered a
    province id would corrupt a map in a way that only shows up as a rendering
    fault much later.
    """
    rgba = _as_rgba(arr_or_img)

    # WHICH encoding wins is decided at the cheap effort, and the expensive one
    # is then spent on the winner alone. Indexed against truecolour is a
    # difference of tens of percent -- land_sea is 156 KB one way and 314 KB the
    # other -- while deflate effort is a few, so the ranking does not change and
    # the loser does not need to be encoded twice. Zopfli over the 134 MB
    # truecolour form of a layer that is about to be thrown away was three
    # minutes a map, spent on nothing.
    scored = []
    for encode in (indexed_png, truecolour_png):
        data = encode(rgba, EFFORT_FAST)
        if data:
            scored.append((len(data), encode, data))
    scored.sort(key=lambda t: t[0])

    for _, encode, data in scored:
        if not verify_lossless(data, rgba):
            continue
        if effort == EFFORT_FAST:
            return data
        harder = encode(rgba, effort)
        if harder and len(harder) < len(data) and verify_lossless(harder, rgba):
            return harder
        return data
    raise AssertionError("no lossless encoding round-tripped for this layer")


# Colour type -> the Pillow mode whose raw bytes are the PNG's own samples.
# Only at bit depth 8, and only for the types below: anything else is returned
# untouched rather than guessed at.
_MODE_FOR_COLOUR = {0: "L", 2: "RGB", 3: "P", 4: "LA", 6: "RGBA"}
_CHANNELS_FOR_COLOUR = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def optimize_png(data, effort=EFFORT_FAST):
    """Re-encode an existing PNG to the same pixels in fewer bytes.

    For the images an .odmap carries that layer_png does not build -- the
    thumbnail, and a couple of hundred flags -- where the file came from
    somewhere else and was encoded by whatever wrote it. Every chunk except
    IDAT is copied through byte for byte, so the palette, the transparency and
    any licence text in a tEXt chunk survive; only the scanline filtering and
    the deflate stream are chosen again.

    Returns the original bytes unchanged for anything outside what it can
    handle exactly (interlaced, 16-bit, an unknown colour type) or for anything
    it fails to make smaller. Never returns something that decodes differently.
    """
    try:
        chunks = _png_chunks(data)
    except ValueError:
        return data
    header = dict(chunks).get(b"IHDR")
    if header is None or len(header) != 13:
        return data
    w, h, depth, colour, comp, filt, interlace = struct.unpack(">IIBBBBB", header)
    if interlace or comp != 0 or filt != 0 or colour not in _MODE_FOR_COLOUR:
        return data
    if depth != 8 and not (colour == 3 and depth in (1, 2, 4)):
        return data

    try:
        img = Image.open(io.BytesIO(data))
        img.load()
        samples = np.array(img.convert(_MODE_FOR_COLOUR[colour]))
    except Exception:
        return data
    if samples.shape[:2] != (h, w):
        return data
    if samples.ndim == 2:
        samples = samples[:, :, None]
    if samples.shape[2] != _CHANNELS_FOR_COLOUR[colour]:
        return data

    if depth == 8:
        rows = np.ascontiguousarray(samples).reshape(h, w * samples.shape[2])
        bpp = _CHANNELS_FOR_COLOUR[colour]
        idat = _best_idat(rows, bpp, effort)
    else:
        # Sub-byte palette indices, MSB first, as the spec packs them. Pillow
        # hands them back one per byte, so they go back together here.
        idx = samples[:, :, 0].astype(np.uint8)
        if int(idx.max(initial=0)) >= (1 << depth):
            return data
        bits = np.unpackbits(idx, axis=1).reshape(h, w, 8)[:, :, 8 - depth:]
        rows = np.packbits(bits.reshape(h, -1), axis=1)
        # Filter 0 only below eight bits a pixel, and not because searching is
        # slow. A filter under a packed bit depth works on BYTES that each hold
        # several indices, which is why it does not pay here anyway -- and it
        # is the corner of the spec least likely to be exercised in the wild,
        # so it is the corner where a decoder is likeliest to be wrong. The
        # game reads these through stb_image; Pillow agreeing that a file is
        # correct is not evidence that stb does. Nothing is given up: on packed
        # indices the filters lose to no filter, which is the same finding
        # indexed_png() is built on.
        idat = _deflate(_filtered(rows, 1, 0), effort)

    rebuilt = [(t, c) for t, c in chunks if t != b"IDAT"]
    out = b"\x89PNG\r\n\x1a\n"
    for tag, payload in rebuilt:
        if tag == b"IEND":
            out += _chunk(b"IDAT", idat)
        out += _chunk(tag, payload)

    if len(out) >= len(data):
        return data
    # An independent decode of both, compared. This is the whole warrant for
    # calling any of it lossless.
    before = np.array(Image.open(io.BytesIO(data)).convert("RGBA"))
    if not verify_lossless(out, before):
        return data
    return out


def _png_chunks(data):
    """[(tag, payload)] in file order. Raises on anything that is not a PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    out, p = [], 8
    while p + 8 <= len(data):
        length = struct.unpack(">I", data[p:p + 4])[0]
        tag = data[p + 4:p + 8]
        if p + 12 + length > len(data):
            raise ValueError("truncated chunk")
        out.append((tag, data[p + 8:p + 8 + length]))
        p += 12 + length
    if not out or out[-1][0] != b"IEND":
        raise ValueError("no IEND")
    return out


def province_ids(members):
    """provinces.png -> the province id painted in each pixel, as uint32."""
    pv = np.array(Image.open(io.BytesIO(members["provinces.png"])).convert("RGBA"))
    return (pv[:, :, 0].astype(np.uint32) << 16
            | pv[:, :, 1].astype(np.uint32) << 8
            | pv[:, :, 2].astype(np.uint32))


def political_layer(path):
    """The full-resolution political raster for a map, as an RGB image.

    Read from the archive when one is stored -- maps written before the layer
    became derived still carry theirs -- and drawn from province ownership when
    not. Callers that want a picture of who owns what (the banner, thumbnails)
    go through here instead of assuming the file is present.
    """
    import json

    members, _ = read_members(path)
    if "political.png" in members:
        return Image.open(io.BytesIO(members["political.png"])).convert("RGB")
    # Imported inside the function on purpose: fill_water_speckle imports this
    # module for its writer, so at module scope the two would not load in
    # either order.
    from fill_water_speckle import build_political
    return build_political(province_ids(members),
                           json.loads(members["provinces.json"]),
                           json.loads(members["countries.json"])).convert("RGB")


def _umask():
    """The process umask, read without leaving it changed."""
    current = os.umask(0)
    os.umask(current)
    return current


def read_members(path):
    """(members, dirs) for an .odmap: name -> bytes, plus its directory entries."""
    with zipfile.ZipFile(path) as z:
        members = {i.filename: z.read(i.filename) for i in z.infolist() if not i.is_dir()}
        dirs = [i.filename for i in z.infolist() if i.is_dir()]
    return members, dirs


def write_odmap(path, members, dirs=(), effort=EFFORT_FAST):
    """Rewrite an .odmap in place, atomically, dropping the derived layers.

    Through a temp file in the same directory and a move, so an interrupted run
    leaves the previous map intact rather than a half-written archive that the
    game will try to load.
    """
    members = {k: v for k, v in members.items() if k not in DERIVED}
    fd, tmp = tempfile.mkstemp(suffix=".odmap", dir=os.path.dirname(path) or ".")
    os.close(fd)
    try:
        # The archive's own deflate, not just its members'. The PNGs inside are
        # already compressed and skipped, but the JSON is not: the province
        # tables, the minority breakdown and the policy list are 800 KB of
        # deflate output across the shipped maps, and Zopfli takes 3% off it for
        # an archive every reader still opens as an ordinary zip.
        with zopfli_deflate(effort == EFFORT_MAX):
            with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
                for d in dirs:
                    z.writestr(d, b"")
                for name, data in members.items():
                    z.writestr(name, data)
        # mkstemp creates 0600, and moving it into place carries that with it --
        # so re-encoding a map quietly made it unreadable to anyone but the user
        # who ran the tool. These are shipped world files. Give them the mode the
        # umask would have given a plainly-created file, which is what every
        # other writer here produces.
        os.chmod(tmp, 0o666 & ~_umask())
        shutil.move(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
