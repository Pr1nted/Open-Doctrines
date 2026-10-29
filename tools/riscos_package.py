#!/usr/bin/env python3
"""Build, or check, the RISC OS application zip.

    tools/riscos_package.py --elf build/OpenDoctrines --stage dist/stage --out dist/OpenDoctrines-riscos-arm.zip
    tools/riscos_package.py --aif build/RunImage --stage dist/stage --out dist/OpenDoctrines-riscos-arm.zip
    tools/riscos_package.py --check dist/OpenDoctrines-riscos-arm.zip

A RISC OS application is a directory whose name starts with "!"; the Filer
shows it with the icon from its !Sprites, and double-clicking it runs its !Run.
The zip holds one:

    !Doctrines/!Run        Obey file: sets OpenDoctrines$Dir, the memory the
                           game needs (WimpSlot), runs !RunImage
    !Doctrines/!Boot       Obey file: the icon, when the Filer first sees it
    !Doctrines/!Sprites    the icon, 34x34 and a small 17x17, made here from
                           data/Icon/icon.png (32-bit sprites, RISC OS 5)
    !Doctrines/!RunImage   the game, an AIF executable made from the ELF by
                           GCCSDK's elf2aif (here with --elf, or beforehand,
                           where the toolchain is, and handed over with --aif)
    !Doctrines/data/ ...   the shipped data, exactly what tools/package.py
                           allowlists for every other platform (--stage is its
                           output); the game finds it beside its executable

"!Doctrines", not "!OpenDoctrines": a sprite name is at most 12 characters, and
the Filer looks the icon up by the directory's name -- "!opendoctrines" could
never have one.

FILETYPES. RISC OS has no file extensions; a file's type is metadata, and a
zip carries it in the "ARC0" extra field (header id 0x4341) that RISC OS's zip
tools read and write. !Run and !Boot are Obey (&FEB), !Sprites Sprite (&FF9),
!RunImage Absolute (&FF8). Data files go without: UnixLib maps their names, and
unzip on RISC OS turns "icon.png" into "icon/png" as it always does.

--check opens a built zip and holds it to that shape, because there is no
RISC OS machine on a CI runner to try it on.
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
APP = "!Doctrines/"
SPRITE = "!doctrines"
BG = (16, 20, 28)      # the website's dark background

TYPE_OBEY, TYPE_SPRITE, TYPE_ABSOLUTE, TYPE_ELF = 0xFEB, 0xFF9, 0xFF8, 0xE1F
ARC0 = 0x4341

# Memory for the application slot. A world at 1/2 resolution peaks near 230 MB
# (CMakeLists.txt, "RISC OS"); the rest is the frame, the fonts and the AI.
WIMPSLOT_KB = 320 * 1024

RUN = f"""| !Run for Open Doctrines
Set OpenDoctrines$Dir <Obey$Dir>
IconSprites <OpenDoctrines$Dir>.!Sprites
WimpSlot -min {WIMPSLOT_KB}K -max {WIMPSLOT_KB}K
Run <OpenDoctrines$Dir>.!RunImage %*0
"""

BOOT = """| !Boot for Open Doctrines
IconSprites <Obey$Dir>.!Sprites
"""


def arc0_extra(filetype):
    # "ARC0", load address, exec address, attributes, reserved. A typed file's
    # load address is &FFFtttdd: tt t the type, dd the top byte of its 5-byte
    # time stamp (left at 0 here, as is the exec address that holds the rest).
    load = 0xFFF00000 | (filetype << 8)
    body = b"ARC0" + struct.pack("<IIII", load, 0, 0x33, 0)
    return struct.pack("<HH", ARC0, len(body)) + body


def filetype_of(info):
    extra = info.extra
    while len(extra) >= 4:
        hid, size = struct.unpack("<HH", extra[:4])
        body = extra[4:4 + size]
        if hid == ARC0 and body[:4] == b"ARC0" and len(body) >= 8:
            load = struct.unpack("<I", body[4:8])[0]
            if (load >> 20) == 0xFFF:
                return (load >> 8) & 0xFFF
        extra = extra[4 + size:]
    return None


def sprite(name, size):
    """One 32-bit RISC OS 5 sprite (no mask): the icon on the dark background."""
    from PIL import Image
    src = Image.open(os.path.join(ROOT, "data", "Icon", "icon.png")).convert("RGBA")
    img = Image.new("RGBA", (size, size), BG + (255,))
    img.alpha_composite(src.resize((size, size), Image.LANCZOS))
    raw = img.tobytes()      # R, G, B, A
    pixels = b"".join(bytes((raw[i], raw[i + 1], raw[i + 2], 0)) for i in range(0, len(raw), 4))
    # Header: next, name[12], width in words - 1, height - 1, first bit, last bit,
    # image offset, mask offset (= image: no mask), mode word. The mode word is a
    # RISC OS 5 sprite type: bit 0 set, 90 dpi each way, type 6 (32 bits a pixel).
    mode = (6 << 27) | (90 << 14) | (90 << 1) | 1
    header_len = 44
    total = header_len + len(pixels)
    header = struct.pack("<I12siiiiiiI", total, name.encode("latin-1").ljust(12, b"\0"),
                         size - 1, size - 1, 0, 31, header_len, header_len, mode)
    return header + pixels


def sprite_file(sprites):
    # A sprite area as saved to a file: the area's first word (its size) is left
    # out, so offsets count from 4 bytes before the file begins.
    body = b"".join(sprites)
    return struct.pack("<iii", len(sprites), 16, 16 + len(body)) + body


def build(elf, aif, stage, out, elf_image=None):
    version = str(odver.read())
    if elf_image is not None:
        # GCCSDK 10's EABI (arm-riscos-gnueabihf) programs are ELF, which
        # elf2aif does not take ("is not for ARM"); RISC OS runs them as
        # filetype &E1F through GCCSDK's ELF loader.
        with open(elf_image, "rb") as f:
            aif_bytes = f.read()
    elif aif is None:
        with tempfile.TemporaryDirectory() as tmp:
            aif = os.path.join(tmp, "RunImage")
            subprocess.run(["elf2aif", elf, aif], check=True)
            with open(aif, "rb") as f:
                aif_bytes = f.read()
    else:
        with open(aif, "rb") as f:
            aif_bytes = f.read()
    problems = check_elf(aif_bytes) if elf_image is not None else check_aif(aif_bytes)
    if problems:
        print("FAILED: " + problems, file=sys.stderr)
        return 1
    sprites = sprite_file([sprite(SPRITE, 34), sprite("sm" + SPRITE, 17)])

    def typed(z, name, data, filetype):
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.extra = arc0_extra(filetype)
        info.create_system = 13      # Acorn RISC OS
        info.external_attr = 0x33
        z.writestr(info, data)

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        typed(z, APP + "!Run", RUN.encode("latin-1"), TYPE_OBEY)
        typed(z, APP + "!Boot", BOOT.encode("latin-1"), TYPE_OBEY)
        typed(z, APP + "!Sprites", sprites, TYPE_SPRITE)
        typed(z, APP + "!RunImage", aif_bytes, TYPE_ELF if elf_image is not None else TYPE_ABSOLUTE)
        for dirpath, _, files in os.walk(stage):
            for name in sorted(files):
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, stage).replace(os.sep, "/")
                # package.py copies the binary into the stage too; the zip
                # carries it once, as !RunImage.
                if rel in ("OpenDoctrines", "OpenDoctrines.elf", "RunImage"):
                    continue
                z.write(full, APP + rel)
    print(f"wrote {out} ({os.path.getsize(out) // 1024} KB), Open Doctrines {version}")
    return check(out)


def check_elf(b):
    if len(b) < 20 or b[:4] != b"\x7fELF" or b[4] != 1 or b[5] != 1:
        return "!RunImage is not a 32-bit little-endian ELF"
    if struct.unpack("<H", b[18:20])[0] != 40:
        return "!RunImage is an ELF for something other than ARM"
    return ""


def check_aif(b):
    # An AIF image starts with four instructions (BL decompress / BL self-
    # relocate / BL zero-init / BL entry), the first of which may be a NOP
    # (MOV r0, r0) when nothing is compressed. Word 4 is the exit SWI; the
    # header is 128 bytes. A non-executable header here means elf2aif did not.
    if len(b) < 128:
        return "!RunImage is shorter than an AIF header"
    words = struct.unpack("<32I", b[:128])
    entry_bl = words[3]
    if (entry_bl >> 24) != 0xEB:
        return f"!RunImage word 3 is 0x{entry_bl:08x}, not the BL to the entry point an AIF header has"
    if (words[4] & 0x0F000000) != 0x0F000000:
        return f"!RunImage word 4 is 0x{words[4]:08x}, not the exit SWI an AIF header has"
    return ""


def check(path):
    problems = []
    with zipfile.ZipFile(path) as z:
        infos = {i.filename: i for i in z.infolist()}
        image_type = filetype_of(infos[APP + "!RunImage"]) if APP + "!RunImage" in infos else None
        for name, want in (("!Run", TYPE_OBEY), ("!Boot", TYPE_OBEY),
                           ("!Sprites", TYPE_SPRITE),
                           ("!RunImage", TYPE_ELF if image_type == TYPE_ELF else TYPE_ABSOLUTE)):
            info = infos.get(APP + name)
            if info is None:
                problems.append(f"no {APP}{name}")
            elif filetype_of(info) != want:
                problems.append(f"{APP}{name} has filetype {filetype_of(info)!r}, not &{want:03X}")
        if APP + "!RunImage" in infos:
            img = z.read(APP + "!RunImage")
            p = check_elf(img) if image_type == TYPE_ELF else check_aif(img)
            if p:
                problems.append(p)
        if APP + "!Sprites" in infos:
            s = z.read(APP + "!Sprites")
            count, first = struct.unpack("<ii", s[:8])
            name = s[first - 4 + 4:first - 4 + 16].rstrip(b"\0").decode("latin-1")
            if count < 1 or name != SPRITE:
                problems.append(f"!Sprites' first sprite is {name!r}, not {SPRITE!r}: the Filer would show no icon")
        if APP + "!Run" in infos and b"!RunImage" not in z.read(APP + "!Run"):
            problems.append("!Run does not run !RunImage")
        if not any(n.startswith(APP + "data/fonts/") for n in infos):
            problems.append("no data/fonts/: the game would start with no text")
        if APP + "VERSION" not in infos:
            problems.append(f"no {APP}VERSION")
        if any(not n.startswith(APP) for n in infos):
            problems.append("something outside !Doctrines/")
    if problems:
        print("FAILED: " + "; ".join(problems), file=sys.stderr)
        return 1
    print(f"ok    {path}: !Doctrines with !Run, !Boot, !Sprites and an {'ELF' if image_type == TYPE_ELF else 'AIF'} !RunImage, typed; data")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf")
    ap.add_argument("--aif")
    ap.add_argument("--elf-image", help="ship the ELF itself as !RunImage (&E1F)")
    ap.add_argument("--stage")
    ap.add_argument("--out")
    ap.add_argument("--check", metavar="ZIP")
    a = ap.parse_args()
    if a.check:
        return check(a.check)
    if not ((a.elf or a.aif or a.elf_image) and a.stage and a.out):
        ap.error("--elf, --aif or --elf-image, --stage and --out, or --check")
    return build(a.elf, a.aif, a.stage, a.out, a.elf_image)


if __name__ == "__main__":
    sys.exit(main())
