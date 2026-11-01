#!/usr/bin/env python3
"""Build, or check, the Nintendo 3DS homebrew zip.

    tools/n3ds_package.py --elf build/OpenDoctrines.elf --stage dist/stage --out dist/OpenDoctrines-3ds-arm.zip
    tools/n3ds_package.py --check dist/OpenDoctrines-3ds-arm.zip

The Homebrew Launcher lists every .3dsx under sd:/3ds/, with the name and icon
carried in the file's SMDH. A player unzips this onto the root of their SD
card, so the zip IS that layout:

    3ds/OpenDoctrines/OpenDoctrines.3dsx   the game (CMakeLists.txt, "THE NINTENDO
                                           3DS"), its SMDH inside: name, description,
                                           the 48x48 icon
    3ds/OpenDoctrines/data/                the shipped data, exactly what
                                           tools/package.py allowlists for every
                                           other platform (--stage is its output);
                                           the game finds it beside its executable,
                                           which the launcher passes as argv[0]
    3ds/OpenDoctrines/READ ME FIRST.txt, VERSION   from package.py too

The .3dsx is made here from the ELF with devkitPro's own tools, smdhtool and
3dsxtool (under $DEVKITPRO/tools/bin), and THE ICON IS MADE HERE from
data/Icon/icon.png rather than committed, for the reason tools/vita_vpk.py
gives: a second copy in another size is a copy that drifts.

--check opens a built zip and holds it to that shape -- the 3DSX header and the
SMDH it points to, the fonts in data/ -- because there is no 3DS on a CI runner
to try it on.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import odver

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = "3ds/OpenDoctrines/"
EXE = APP + "OpenDoctrines.3dsx"
ICON_SIZE = (48, 48)
BG = (16, 20, 28)      # the website's dark background

NAME = "Open Doctrines"
DESCRIPTION = "Grand strategy, 1641 provinces. New 3DS."
AUTHOR = "Open Doctrines contributors"


def tool(name):
    base = os.environ.get("DEVKITPRO", "/opt/devkitpro")
    path = os.path.join(base, "tools", "bin", name)
    return path if os.path.exists(path) else name


def make_icon(path):
    from PIL import Image
    src = Image.open(os.path.join(ROOT, "data", "Icon", "icon.png")).convert("RGBA")
    icon = Image.new("RGBA", ICON_SIZE, BG + (255,))
    mark = src.resize((ICON_SIZE[0] - 4, ICON_SIZE[1] - 4), Image.LANCZOS)
    icon.alpha_composite(mark, (2, 2))
    icon.save(path, "PNG")


def build(elf, stage, out):
    version = str(odver.read())
    with tempfile.TemporaryDirectory() as tmp:
        icon = os.path.join(tmp, "icon.png")
        smdh = os.path.join(tmp, "OpenDoctrines.smdh")
        exe = os.path.join(tmp, "OpenDoctrines.3dsx")
        make_icon(icon)
        subprocess.run([tool("smdhtool"), "--create", NAME, f"{DESCRIPTION} {version}", AUTHOR,
                        icon, smdh], check=True)
        subprocess.run([tool("3dsxtool"), elf, exe, f"--smdh={smdh}"], check=True)
        with open(exe, "rb") as f:
            exe_bytes = f.read()
    problems = check_3dsx(exe_bytes)
    if problems:
        print("FAILED: " + problems, file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(EXE, exe_bytes)
        for dirpath, _, files in os.walk(stage):
            for name in sorted(files):
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, stage).replace(os.sep, "/")
                # package.py copies the binary into the stage too; the zip
                # carries it once, as the .3dsx.
                if rel in ("OpenDoctrines", "OpenDoctrines.elf", "OpenDoctrines.3dsx"):
                    continue
                z.write(full, APP + rel)
    print(f"wrote {out} ({os.path.getsize(out) // 1024} KB), Open Doctrines {version}")
    return check(out)


def check_3dsx(b):
    # The 3DSX header: magic, header size, relocation header size, version,
    # flags, then the code/rodata/data/bss sizes. A header longer than the
    # basic 32 bytes carries the extended one: the SMDH's offset and size.
    if len(b) < 44 or b[:4] != b"3DSX":
        return "not a 3DSX file"
    header_size = struct.unpack("<H", b[4:6])[0]
    code_size = struct.unpack("<I", b[16:20])[0]
    if code_size == 0:
        return "the 3DSX has no code segment"
    if header_size < 44:
        return "the 3DSX has no extended header, so no SMDH: the launcher would show no name or icon"
    smdh_off, smdh_size = struct.unpack("<II", b[32:40])
    if smdh_size != 0x36C0 or smdh_off + smdh_size > len(b) or b[smdh_off:smdh_off + 4] != b"SMDH":
        return f"no SMDH where the 3DSX header says (offset {smdh_off}, size {smdh_size})"
    # The first title (Japanese), then the English one: 0x200 bytes each, the
    # short name first, UTF-16.
    english = b[smdh_off + 8 + 0x200:smdh_off + 8 + 0x200 + 0x80].decode("utf-16-le", "replace").split("\0")[0]
    if english != NAME:
        return f"the SMDH's English name is {english!r}, not {NAME!r}"
    return ""


def check(path):
    problems = []
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        for need in (EXE, APP + "VERSION"):
            if need not in names:
                problems.append(f"no {need}")
        if EXE in names:
            p = check_3dsx(z.read(EXE))
            if p:
                problems.append(p)
        if not any(n.startswith(APP + "data/fonts/") for n in names):
            problems.append("no data/fonts/: the game would start with no text")
        if any(not n.startswith(APP) for n in names):
            problems.append("something outside 3ds/OpenDoctrines/")
    if problems:
        print("FAILED: " + "; ".join(problems), file=sys.stderr)
        return 1
    print(f"ok    {path}: Homebrew Launcher layout, 3DSX header, SMDH (name and icon), data")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf")
    ap.add_argument("--stage")
    ap.add_argument("--out")
    ap.add_argument("--check", metavar="ZIP")
    a = ap.parse_args()
    if a.check:
        return check(a.check)
    if not (a.elf and a.stage and a.out):
        ap.error("--elf, --stage and --out, or --check")
    return build(a.elf, a.stage, a.out)


if __name__ == "__main__":
    sys.exit(main())
