#!/usr/bin/env python3
"""Build, or check, the PS Vita package (.vpk).

    tools/vita_vpk.py --eboot build/eboot.bin --stage dist/stage --out dist/OpenDoctrines-vita.vpk
    tools/vita_vpk.py --check dist/OpenDoctrines-vita.vpk

A .vpk is a zip that VitaShell installs as an app. What goes in it:

    eboot.bin            the game: a homebrew SELF (CMakeLists.txt, "THE PS VITA")
    sce_sys/param.sfo    title, title id and version, made by VitaSDK's vita-mksfoex
    sce_sys/icon0.png    128x128, the home-screen bubble
    sce_sys/livearea/contents/{bg.png,startup.png,template.xml}
                         the LiveArea page the bubble opens
    data/                the shipped data, exactly what tools/package.py
                         allowlists for every other platform (--stage is its
                         output); the game copies it to the memory card on
                         first start, see src/platform/Vita.h

THE IMAGES ARE MADE HERE, from data/Icon/icon.png and the website's colours,
rather than committed: the Vita takes only 8-bit palette PNGs at fixed sizes,
and a second copy of the icon in that form is a copy that drifts.

--check opens a built .vpk and holds it to the shape above -- the SELF's and
the SFO's magic numbers, the title id and version inside the SFO, the image
sizes and palette mode, the fonts in data/ -- because none of it can be
checked by installing it: there is no Vita on a CI runner.
"""
import argparse
import io
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import odver

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TITLE = "Open Doctrines"
# Four capitals and five digits is the Vita's format. ODOC = Open Doctrines.
TITLE_ID = "ODOC00001"
GROUND = (0x0C, 0x0F, 0x16)
INK = (0xE8, 0xE4, 0xDA)
GOLD = (0xC9, 0xA2, 0x27)

IMAGES = {
    "sce_sys/icon0.png": (128, 128),
    "sce_sys/livearea/contents/bg.png": (840, 500),
    "sce_sys/livearea/contents/startup.png": (280, 158),
}

TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""


def app_ver(v):
    """The SFO's APP_VER is XX.YY. 1.3.2 is 01.32: the minor and patch share
    the second pair, which holds while neither passes 9."""
    return f"{min(v.major, 99):02d}.{min(v.minor, 9)}{min(v.patch, 9)}"


def png(img):
    """8-bit palette PNG bytes, the only kind the LiveArea accepts."""
    out = io.BytesIO()
    img.convert("RGB").quantize(colors=256).save(out, format="PNG", optimize=True)
    return out.getvalue()


def make_images():
    from PIL import Image, ImageDraw, ImageFont
    icon = Image.open(os.path.join(ROOT, "data", "Icon", "icon.png")).convert("RGBA")
    font_path = os.path.join(ROOT, "data", "fonts", "DejaVuSans.ttf")

    def on_ground(size, icon_px, icon_xy, title_px=0, title_xy=None, lines=(TITLE,)):
        img = Image.new("RGBA", size, GROUND + (255,))
        ic = icon.resize((icon_px, icon_px), Image.LANCZOS)
        img.alpha_composite(ic, icon_xy)
        if title_px:
            d = ImageDraw.Draw(img)
            f = ImageFont.truetype(font_path, title_px)
            x, y = title_xy
            for line in lines:
                d.text((x, y), line, font=f, fill=INK)
                y += int(title_px * 1.25)
            # A gold rule under the title, as on the website's headings.
            w = max(d.textlength(line, font=f) for line in lines)
            y += int(title_px * 0.2)
            d.rectangle([x, y, x + int(w * 0.35), y + max(2, title_px // 16)], fill=GOLD)
        return img

    return {
        "sce_sys/icon0.png": png(on_ground((128, 128), 112, (8, 8))),
        "sce_sys/livearea/contents/bg.png":
            png(on_ground((840, 500), 300, (60, 100), 48, (410, 205))),
        "sce_sys/livearea/contents/startup.png":
            png(on_ground((280, 158), 120, (19, 19), 21, (152, 44), ("Open", "Doctrines"))),
    }


def make_sfo(version, mksfoex):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "param.sfo")
        # ATTRIBUTE2=12 asks for the larger memory budget homebrew games use;
        # the heap the game takes (src/platform/Vita.cpp) needs it.
        subprocess.run([mksfoex, "-s", f"TITLE_ID={TITLE_ID}", "-s", f"APP_VER={app_ver(version)}",
                        "-d", "ATTRIBUTE2=12", TITLE, out], check=True)
        with open(out, "rb") as f:
            return f.read()


def sfo_fields(data):
    """Read an SFO's key/value table. Enough of the format to check it."""
    magic, _ver, key_off, val_off, count = struct.unpack_from("<4sIIII", data, 0)
    if magic != b"\x00PSF":
        raise ValueError("not an SFO")
    fields = {}
    for i in range(count):
        k_off, fmt, length, _maxlen, d_off = struct.unpack_from("<HHIII", data, 20 + 16 * i)
        key = data[key_off + k_off:data.index(b"\x00", key_off + k_off)].decode()
        raw = data[val_off + d_off:val_off + d_off + length]
        fields[key] = raw.rstrip(b"\x00").decode("utf-8", "replace") if fmt != 0x0404 \
            else struct.unpack("<I", raw)[0]
    return fields


def build(args):
    version = odver.read()
    mksfoex = args.mksfoex or os.path.join(os.environ.get("VITASDK", ""), "bin", "vita-mksfoex")
    data_dir = os.path.join(args.stage, "data")
    if not os.path.isdir(data_dir):
        print(f"FAILED: {data_dir} is not there; run tools/package.py first", file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(args.eboot, "eboot.bin")
        z.writestr("sce_sys/param.sfo", make_sfo(version, mksfoex))
        for name, blob in make_images().items():
            z.writestr(name, blob)
        z.writestr("sce_sys/livearea/contents/template.xml", TEMPLATE)
        for dirpath, _, files in os.walk(data_dir):
            for f in sorted(files):
                full = os.path.join(dirpath, f)
                z.write(full, os.path.relpath(full, args.stage).replace(os.sep, "/"))
    print(f"{args.out}: {os.path.getsize(args.out) / 1048576:.1f} MB, "
          f"{TITLE_ID} {app_ver(version)}")
    return check(args.out)


def check(path):
    from PIL import Image
    problems = []
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        need = {"eboot.bin", "sce_sys/param.sfo", "sce_sys/livearea/contents/template.xml"} | set(IMAGES)
        for n in sorted(need - names):
            problems.append(f"missing {n}")
        if "eboot.bin" in names and z.read("eboot.bin")[:4] != b"SCE\x00":
            problems.append("eboot.bin is not a SELF (no SCE magic)")
        if "sce_sys/param.sfo" in names:
            try:
                f = sfo_fields(z.read("sce_sys/param.sfo"))
                if f.get("TITLE_ID") != TITLE_ID:
                    problems.append(f"param.sfo TITLE_ID is {f.get('TITLE_ID')!r}")
                want = app_ver(odver.read())
                if f.get("APP_VER") != want:
                    problems.append(f"param.sfo APP_VER is {f.get('APP_VER')!r}, not {want}")
                if f.get("TITLE") != TITLE:
                    problems.append(f"param.sfo TITLE is {f.get('TITLE')!r}")
            except (ValueError, struct.error) as e:
                problems.append(f"param.sfo unreadable: {e}")
        for name, size in IMAGES.items():
            if name not in names:
                continue
            im = Image.open(io.BytesIO(z.read(name)))
            if im.size != size:
                problems.append(f"{name} is {im.size}, not {size}")
            if im.mode != "P":
                problems.append(f"{name} is mode {im.mode}; the Vita takes 8-bit palette PNGs")
        if not any(n.startswith("data/fonts/") and n.endswith(".ttf") for n in names):
            problems.append("no fonts under data/fonts")
        if any(n.startswith("data/config.json") for n in names):
            problems.append("data/config.json is local state and must not ship")
    if problems:
        print(f"{path}: NOT a valid Vita package")
        for p in problems:
            print("  " + p)
        return 1
    print(f"{path}: a valid Vita package ({len(names)} files)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", metavar="VPK", help="check a built .vpk and exit")
    ap.add_argument("--eboot", help="eboot.bin from the build")
    ap.add_argument("--stage", help="tools/package.py output (holds data/)")
    ap.add_argument("--out", help="the .vpk to write")
    ap.add_argument("--mksfoex", help="vita-mksfoex (default: $VITASDK/bin/vita-mksfoex)")
    args = ap.parse_args()
    if args.check:
        return check(args.check)
    if not (args.eboot and args.stage and args.out):
        ap.error("--eboot, --stage and --out are needed to build")
    return build(args)


if __name__ == "__main__":
    sys.exit(main())
