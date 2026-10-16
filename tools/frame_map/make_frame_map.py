#!/usr/bin/env python3
"""Turn a video into a playable OpenDoctrines map: one square province per
pixel, two countries (BLK and WHT), and a map script that flips province
owners so that each processed turn shows the next frame.

    python3 tools/frame_map/make_frame_map.py my_video.mp4 --size 64x48 --fps 10
    python3 tools/frame_map/make_frame_map.py --demo-video ball.mp4   # make a test clip

Nothing in the engine is changed: the animation is plain OD/MapEngine/3 script
(`set province.ID.owner BLK`, `waitUntil`, `label`/`jump`). Needs ffmpeg for
video input. Standard library only.

HOW THE SCRIPT IS SHAPED
  The map's starting owners ARE frame 0, so turn 0 needs no script work.
  For every later frame the script waits for `map.turn >= var.base + f` and
  sets only the cells that changed since the previous frame. The language has
  no substring or computed-province-id operations (province ids in a reference
  must be literal digits; `{...}` interpolation only works inside `print`), so
  run-length or base-64 decoding inside the script is not possible -- one
  `set` line per changed cell is the most compact form the engine can execute,
  and it is also the cheapest: no loop, so the 10,000-iteration while-cap and
  the 100,000-jump cap never come into play (one jump per video loop).
  Waits are ABSOLUTE turn numbers, not `wait 1 turn`: entry scripts restart
  from the top when a save is loaded, and an absolute wait makes them catch
  up to the right frame in one burst instead of replaying from frame 0
  turn by turn.

THE WORLD
  The game's maps are 8192x4096 (lon/lat maths assumes it), so the grid is
  W*k by H*k image pixels with k the largest whole cell size that fits. Any
  remainder is LAND, given to the game's own "Blocked Territory" sentinel
  (BLC, cid 65535: nobody can enter, colonise or conquer it) as up to four
  strip provinces around the picture. There is no sea anywhere.
  Ids: cell (x, y) is province 1 + y*W + x. Padding strips follow W*H.
"""
import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

MAP_W, MAP_H = 8192, 4096
BLK_CID, WHT_CID, BLC_CID = 1, 2, 65535
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# ── PNG writing (stdlib only) ──────────────────────────────────────────────
def _chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def png_from_rows(width, height, color_type, bit_depth, rows, extra=b""):
    """rows: iterable of raw scanline bytes (no filter byte). Identical
    consecutive rows are written with the Up filter (all zeros), which is what
    makes a 100 MB raster of flat squares compress in a second."""
    comp = zlib.compressobj(6)
    out = []
    prev = None
    for row in rows:
        if row is prev or row == prev:
            out.append(comp.compress(b"\x02" + bytes(len(row))))
        else:
            out.append(comp.compress(b"\x00" + row))
        prev = row
    out.append(comp.flush())
    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + extra
            + _chunk(b"IDAT", b"".join(out)) + _chunk(b"IEND", b""))


# ── frames ─────────────────────────────────────────────────────────────────
def read_video_frames(path, w, h, fps, max_frames, threshold, invert, start, duration):
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found on PATH")
    vf = []
    if fps:
        vf.append(f"fps={fps}")
    # Exact grid size; aspect is the caller's choice (--size). area = box filter.
    vf.append(f"scale={w}:{h}:flags=area")
    vf.append("format=gray")
    cmd = ["ffmpeg", "-v", "error"]
    if start:
        cmd += ["-ss", str(start)]
    cmd += ["-i", path]
    if duration:
        cmd += ["-t", str(duration)]
    cmd += ["-an", "-vf", ",".join(vf), "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    if max_frames:
        cmd[cmd.index("-an"):cmd.index("-an")] = ["-frames:v", str(max_frames)]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = w * h
    frames = []
    while True:
        buf = proc.stdout.read(n)
        if len(buf) < n:
            break
        # 1 = black (BLK), 0 = white (WHT)
        f = bytes(1 if (v < threshold) != invert else 0 for v in buf)
        frames.append(f)
    proc.wait()
    if proc.returncode != 0 and not frames:
        sys.exit("ffmpeg failed to decode " + path)
    return frames


def demo_frames(w, h, count):
    """A white ball bouncing over black, plus a sweeping bar, plus a few
    all-black / all-white frames (so a country owning nothing is exercised)."""
    frames = []
    r = max(2.0, min(w, h) / 6)
    x, y, vx, vy = r + 1, r + 1, w / 23.0, h / 17.0
    for i in range(count):
        cells = bytearray(b"\x01" * (w * h))
        if i % 50 in (25,):
            frames.append(bytes(cells))            # all black
            continue
        if i % 50 in (26,):
            frames.append(bytes(w * h))            # all white
            continue
        bar = int(i * 1.5) % w
        for yy in range(h):
            for xx in range(w):
                if (xx - x) ** 2 + (yy - y) ** 2 <= r * r or xx == bar:
                    cells[yy * w + xx] = 0
        frames.append(bytes(cells))
        x += vx; y += vy
        if x < r or x > w - 1 - r: vx = -vx; x += 2 * vx
        if y < r or y > h - 1 - r: vy = -vy; y += 2 * vy
    return frames


def write_demo_video(path, w, h, count, fps):
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found on PATH")
    frames = demo_frames(w, h, count)
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "gray",
           "-s", f"{w}x{h}", "-r", str(fps), "-i", "-", "-pix_fmt", "yuv420p",
           "-vf", "scale=iw*8:ih*8:flags=neighbor", "-c:v", "libx264", "-crf", "12", path]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for f in frames:
        p.stdin.write(bytes(0 if v else 255 for v in f))
    p.stdin.close()
    p.wait()
    print(f"wrote demo video {path} ({count} frames, {w * 8}x{h * 8}, {fps} fps)")


# ── map ────────────────────────────────────────────────────────────────────
def layout(w, h):
    k = min(MAP_W // w, MAP_H // h)
    if k < 1:
        sys.exit(f"grid {w}x{h} does not fit an {MAP_W}x{MAP_H} map")
    gw, gh = w * k, h * k
    x0, y0 = (MAP_W - gw) // 2, (MAP_H - gh) // 2
    pads = []   # (x, y, w, h) land strips owned by BLC
    if x0 > 0: pads.append((0, 0, x0, MAP_H))
    if MAP_W - x0 - gw > 0: pads.append((x0 + gw, 0, MAP_W - x0 - gw, MAP_H))
    if y0 > 0: pads.append((x0, 0, gw, y0))
    if MAP_H - y0 - gh > 0: pads.append((x0, y0 + gh, gw, MAP_H - y0 - gh))
    return k, x0, y0, pads


def province_png(w, h, k, x0, y0, pads):
    pad_ids = [w * h + 1 + i for i in range(len(pads))]

    def rgb(pid):
        return bytes(((pid >> 16) & 255, (pid >> 8) & 255, pid & 255))

    cache = {}

    def row_at(yy):
        key = []
        for (px, py, pw, ph), pid in zip(pads, pad_ids):
            if py <= yy < py + ph:
                key.append(pid)
        gy = (yy - y0) // k if y0 <= yy < y0 + h * k else -1
        key = (gy, tuple(key))
        if key in cache:
            return cache[key]
        row = bytearray(MAP_W * 3)
        for (px, py, pw, ph), pid in zip(pads, pad_ids):
            if py <= yy < py + ph:
                row[px * 3:(px + pw) * 3] = rgb(pid) * pw
        if gy >= 0:
            for gx in range(w):
                pid = 1 + gy * w + gx
                sx = x0 + gx * k
                row[sx * 3:(sx + k) * 3] = rgb(pid) * k
        cache[key] = bytes(row)
        return cache[key]

    return png_from_rows(MAP_W, MAP_H, 2, 8, (row_at(yy) for yy in range(MAP_H))), pad_ids


def land_png():
    # Indexed 1-bit, palette white, opaque: every pixel is land.
    row = b"\x00" * (MAP_W // 8)
    plte = _chunk(b"PLTE", b"\xff\xff\xff\x00\x00\x00")
    return png_from_rows(MAP_W, MAP_H, 3, 1, (row for _ in range(MAP_H)), plte)


def hexrgb(s):
    s = s.lstrip("#")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def thumb_png(frame0, w, h, k, x0, y0, colors):
    tw, th = 512, 256
    sx, sy = MAP_W / tw, MAP_H / th
    blk, wht, pad = (bytes(hexrgb(c)) for c in colors)
    rows = []
    for ty in range(th):
        my = int((ty + 0.5) * sy)
        row = bytearray()
        for tx in range(tw):
            mx = int((tx + 0.5) * sx)
            gx, gy = (mx - x0) // k, (my - y0) // k
            if mx >= x0 and my >= y0 and gx < w and gy < h:
                row += blk if frame0[gy * w + gx] else wht
            else:
                row += pad
        rows.append(bytes(row))
    return png_from_rows(tw, th, 2, 8, rows)


def flag_svg(color):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="240" height="120" '
            f'viewBox="0 0 240 120"><rect width="240" height="120" fill="{color}"/>'
            f'<rect x="1" y="1" width="238" height="118" fill="none" stroke="#808080" '
            f'stroke-width="2"/></svg>')


def build_script(frames, w, h, loop, probes, header_note):
    """One entry script. Returns (text, expected) where expected[f] is the
    number of BLK cells after frame f (for verification)."""
    iso = {1: "BLK", 0: "WHT"}
    L = ["#OD/MapEngine/3",
         "# Frame Map: generated by tools/frame_map/make_frame_map.py",
         "# " + header_note,
         "set rules.rebellions false",
         "set var.base 0",
         'print "[FRAMEMAP] start turn {map.turn}"',
         "label loop"]
    n = len(frames)
    probe_ids = []
    if probes:
        step = max(1, (w * h) // probes)
        probe_ids = list(range(1, w * h + 1, step))[:probes]

    def frame_block(f, prev, cur):
        changed = [i for i in range(w * h) if prev[i] != cur[i]]
        L.append(f"waitUntil map.turn >= var.base + {f}")
        for i in changed:
            L.append(f"set province.{i + 1}.owner {iso[cur[i]]}")
        msg = f"[FRAMEMAP] frame {f % n} turn {{map.turn}} flipped {len(changed)} blk {{country.BLK.province_count}}"
        if probe_ids:
            msg += " probes " + "".join("{province.%d.owner}" % p for p in probe_ids)
        L.append(f'print "{msg}"')
        return len(changed)

    flips = []
    for f in range(1, n):
        flips.append(frame_block(f, frames[f - 1], frames[f]))
    if loop and n > 1:
        flips.append(frame_block(n, frames[n - 1], frames[0]))
        L.append(f"set var.base = var.base + {n}")
        L.append("jump loop")
    L.append("")
    return "\n".join(L), flips, probe_ids


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video", nargs="?", help="input video (anything ffmpeg reads)")
    ap.add_argument("--size", default="64x48", help="grid WxH in provinces (default 64x48)")
    ap.add_argument("--fps", type=float, default=0, help="resample to this many frames/s (default: source rate)")
    ap.add_argument("--max-frames", type=int, default=0, help="stop after N frames")
    ap.add_argument("--start", type=float, default=0, help="seek to this many seconds first")
    ap.add_argument("--duration", type=float, default=0, help="only this many seconds")
    ap.add_argument("--threshold", type=int, default=128, help="luma below this is black (0-255)")
    ap.add_argument("--invert", action="store_true", help="swap black and white")
    ap.add_argument("--no-loop", action="store_true", help="stop on the last frame instead of looping")
    ap.add_argument("--name", default="Frame Map", help="map name shown in the browser")
    ap.add_argument("--black", default="#000000", help="BLK country colour")
    ap.add_argument("--white", default="#ffffff", help="WHT country colour")
    ap.add_argument("--pad-color", default="#808080", help="colour of the blocked padding land")
    ap.add_argument("--country-names", default="Black,White", help="display names, comma separated")
    ap.add_argument("--probes", type=int, default=0, help="debug: print the owner of N evenly spaced cells each frame")
    ap.add_argument("--demo", type=int, default=0, help="use N frames of the built-in bouncing-ball animation instead of a video")
    ap.add_argument("--demo-video", metavar="OUT.mp4", help="write the bouncing-ball animation as a video file and exit")
    ap.add_argument("-o", "--out", help="output .odmap (default: data/custom_maps/<name>.odmap)")
    a = ap.parse_args()

    w, h = (int(v) for v in a.size.lower().split("x"))
    if w < 1 or h < 1:
        sys.exit("--size must be positive")
    if a.demo_video:
        write_demo_video(a.demo_video, w, h, a.max_frames or 120, a.fps or 10)
        return

    if a.demo:
        frames = demo_frames(w, h, a.demo)
        src = f"built-in demo, {a.demo} frames"
    elif a.video:
        frames = read_video_frames(a.video, w, h, a.fps, a.max_frames, a.threshold,
                                   a.invert, a.start, a.duration)
        src = f"{os.path.basename(a.video)}, {len(frames)} frames at {a.fps or 'source'} fps"
    else:
        ap.error("give a video file, --demo N or --demo-video OUT")
    if not frames:
        sys.exit("no frames decoded")

    k, x0, y0, pads = layout(w, h)
    if w * h + len(pads) > 65000:
        sys.exit("too many provinces (save format stores ids as uint16); use a smaller --size")

    script, flips, probe_ids = build_script(frames, w, h, not a.no_loop, a.probes,
                                            f"{w}x{h} cells, {src}; cell (x,y) = province 1+y*{w}+x")
    prov_png, pad_ids = province_png(w, h, k, x0, y0, pads)

    f0 = frames[0]
    names = (a.country_names.split(",") + ["", ""])[:2]
    provinces, population, resources, compass, minorities = {}, {}, {}, {}, {}
    for i in range(w * h):
        pid = i + 1
        cid = BLK_CID if f0[i] else WHT_CID
        provinces[str(pid)] = {"id": pid, "name": "", "country_id": cid,
                               "iso_a3": "BLK" if f0[i] else "WHT", "color": "#%06x" % pid}
    for pid in pad_ids:
        provinces[str(pid)] = {"id": pid, "name": "", "country_id": BLC_CID,
                               "iso_a3": "BLC", "color": "#%06x" % pid}
    for pid in list(range(1, w * h + 1)):
        population[str(pid)] = 1000
        resources[str(pid)] = {
            "oil": {"a": 0.0, "b": 0.0}, "gold": {"a": 0.0, "b": 0.0},
            "rubber": {"a": 0.0, "b": 0.0}, "gemstones": {"a": 0.0, "b": 0.0},
            "metal": {"a": 0.0, "b": 0.0},
            "industry": {"level": 0, "income": 0.0, "specialization": "Metal",
                         "resourceIncome": 0.0, "popIncome": 0.0, "popModifier": 1.0,
                         "fortification": 0},
            "fortification": 0}
        compass[str(pid)] = {"left": 0, "auth": 0}
        minorities[str(pid)] = [{"n": "Pixel", "p": 100.0}]
    for pid in pad_ids:
        population[str(pid)] = 0

    def country(cid, iso, name, color):
        return {"id": cid, "iso_a3": iso, "name": name, "color": color,
                "flag_actual": {"type": "hstripes_3", "colors": [color, color, color],
                                "image": f"flags/{iso}.svg"},
                "flag_censored": {"type": "hstripes_3", "colors": [color, color, color],
                                  "censored": True},
                "treasury": 0.0}
    countries = {"1": country(BLK_CID, "BLK", names[0], a.black),
                 "2": country(WHT_CID, "WHT", names[1], a.white),
                 str(BLC_CID): {"id": BLC_CID, "iso_a3": "BLC", "name": "Blocked Territory",
                                "color": a.pad_color, "treasury": 0.0,
                                "flag_actual": {"type": "hstripes_3", "colors": [a.pad_color] * 3},
                                "flag_censored": {"type": "hstripes_3", "colors": [a.pad_color] * 3,
                                                  "censored": True}}}

    def j(o):
        return json.dumps(o, separators=(",", ":")).encode()

    policies = b"{}"
    tut = os.path.join(ROOT, "data", "STDmaps", "tutorial.odmap")
    if os.path.exists(tut):
        with zipfile.ZipFile(tut) as z:
            policies = z.read("policies.json")

    members = {
        "metadata.json": j({"name": a.name,
                            "description": f"A {w}x{h} video, one province per pixel; every turn is the next frame. {src}.",
                            "author": "tools/frame_map", "map_date": "January 2000 AD",
                            "license": "CC-BY-4.0", "has_scripts": True}),
        "provinces.png": prov_png,
        "land_sea.png": land_png(),
        "thumb.png": thumb_png(f0, w, h, k, x0, y0, (a.black, a.white, a.pad_color)),
        "provinces.json": j(provinces),
        "countries.json": j(countries),
        "population.json": j(population),
        "resources.json": j(resources),
        "political_compass.json": j(compass),
        "country_compass.json": j({"BLK": {"left": 0, "auth": 0}, "WHT": {"left": 0, "auth": 0}}),
        "minorities.json": j(minorities),
        "minority_colors.json": j({"Pixel": [128, 128, 128]}),
        "starting_policies.json": j({"starting_policies": {}}),
        "starting_minority_policies.json": j({}),
        # Allied, so neither AI has a reason to take the other's pixels.
        "relations.json": j({"BLK": {"WHT": "alliance"}, "WHT": {"BLK": "alliance"}}),
        "claims.json": j({}),
        "ports.json": j({}),
        "armies.json": j({}),
        "ships.json": j([]),
        "policies.json": policies,
        "flags/BLK.svg": flag_svg(a.black).encode(),
        "flags/WHT.svg": flag_svg(a.white).encode(),
        "scripts/frames.txt": script.encode(),
    }
    out = a.out or os.path.join(ROOT, "data", "custom_maps",
                                "".join(c if c.isalnum() else "_" for c in a.name).strip("_") + ".odmap")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members.items():
            # PNGs are already deflated
            z.writestr(name, data, zipfile.ZIP_STORED if name.endswith(".png") else zipfile.ZIP_DEFLATED)
        z.writestr("scripts/", b"")
    os.replace(tmp, out)

    # Sidecar for verification (only with --probes, so a map dropped into
    # data/custom_maps does not drag a JSON file along): expected BLK count
    # and probe owners per frame, to compare against the [FRAMEMAP] prints.
    if not a.probes:
        print(f"wrote {out}  ({os.path.getsize(out) / 1e6:.2f} MB)")
        print(f"  grid {w}x{h}, cell {k}px, {len(frames)} frames, {sum(flips)} flips, "
              f"script {len(script) / 1e6:.2f} MB")
        return
    exp = {"size": [w, h], "frames": len(frames), "loop": not a.no_loop,
           "blk": [sum(f) for f in frames], "flips": [0] + flips,
           "probes": probe_ids,
           "probe_owners": [["BLK" if f[p - 1] else "WHT" for p in probe_ids] for f in frames]}
    with open(os.path.splitext(out)[0] + ".expected.json", "w") as fh:
        json.dump(exp, fh)

    print(f"wrote {out}  ({os.path.getsize(out) / 1e6:.2f} MB)")
    print(f"  grid {w}x{h}, cell {k}px, picture at ({x0},{y0}) {w * k}x{h * k}, "
          f"{len(pads)} blocked padding strip(s)")
    print(f"  {len(frames)} frames, {sum(flips)} cell flips, script {len(script) / 1e6:.2f} MB "
          f"({len(script.splitlines())} lines)")


if __name__ == "__main__":
    main()
