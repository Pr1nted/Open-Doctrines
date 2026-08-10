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

    from odmap_pack import layer_png, read_members, write_odmap, DERIVED
"""

import io
import os
import shutil
import tempfile
import zlib
import zipfile

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None   # 8192x4096 is over Pillow's decompression-bomb guard

# Layers computed at load rather than stored. Kept out of new archives; still
# read from old ones, which is why nothing needed a format version bump.
DERIVED = ("political.png",)

MAX_PALETTE = 256


def _chunk(tag, data):
    return (len(data).to_bytes(4, "big") + tag + data
            + zlib.crc32(tag + data).to_bytes(4, "big"))


def indexed_png(arr, level=9):
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
    out += _chunk(b"IDAT", zlib.compress(raw, level))
    out += _chunk(b"IEND", b"")
    return out


def truecolour_png(arr):
    """RGB(A) array -> PNG with Pillow choosing a filter per row, deflate 9.

    An all-opaque alpha channel is dropped rather than stored. It carries no
    information -- a truecolour PNG without one decodes to alpha 255 -- and
    keeping it costs a byte a pixel, 33 MB of deflate input on a map layer.
    """
    if arr.ndim == 3 and arr.shape[2] == 4 and bool((arr[:, :, 3] == 255).all()):
        arr = arr[:, :, :3]
    buf = io.BytesIO()
    Image.fromarray(arr).save(buf, format="PNG", optimize=True, compress_level=9)
    return buf.getvalue()


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


def layer_png(arr_or_img):
    """Smallest lossless PNG for a map layer.

    Indexed when the palette fits (land_sea's three colours, most generated
    province layers), adaptive-filtered truecolour otherwise (the shipped
    province layers carry over a thousand ids). Whichever wins, the bytes are
    decoded again and compared before they are returned -- an encoder that
    silently altered a province id would corrupt a map in a way that only
    shows up as a rendering fault much later.
    """
    rgba = _as_rgba(arr_or_img)
    candidates = [c for c in (indexed_png(rgba), truecolour_png(rgba)) if c]
    for data in sorted(candidates, key=len):
        if verify_lossless(data, rgba):
            return data
    raise AssertionError("no lossless encoding round-tripped for this layer")


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


def read_members(path):
    """(members, dirs) for an .odmap: name -> bytes, plus its directory entries."""
    with zipfile.ZipFile(path) as z:
        members = {i.filename: z.read(i.filename) for i in z.infolist() if not i.is_dir()}
        dirs = [i.filename for i in z.infolist() if i.is_dir()]
    return members, dirs


def write_odmap(path, members, dirs=()):
    """Rewrite an .odmap in place, atomically, dropping the derived layers.

    Through a temp file in the same directory and a move, so an interrupted run
    leaves the previous map intact rather than a half-written archive that the
    game will try to load.
    """
    members = {k: v for k, v in members.items() if k not in DERIVED}
    fd, tmp = tempfile.mkstemp(suffix=".odmap", dir=os.path.dirname(path) or ".")
    os.close(fd)
    try:
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for d in dirs:
                z.writestr(d, b"")
            for name, data in members.items():
                z.writestr(name, data)
        shutil.move(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
