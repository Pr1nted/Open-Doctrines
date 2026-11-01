#!/usr/bin/env python3
"""Zip the FreeRTOS edition's boards, or check a zip.

    tools/freertos_package.py --builds build/freertos --out dist/OpenDoctrines-freertos.zip
    tools/freertos_package.py --check dist/OpenDoctrines-freertos.zip

    OpenDoctrines-freertos/
      mps2-an385.elf       Cortex-M3, for QEMU's mps2-an385
      stm32f405.elf        Cortex-M4F, for QEMU's netduinoplus2 ...
      stm32f405.bin        ... and the same image to flash at 0x08000000
      riscv32-virt.elf     RV32IMAC, for QEMU's virt machine
      README.txt           the line that starts each one
      VERSION

The POSIX build is not in it: it is a host program, and the source builds it
in one make. --check holds a zip to that shape and each image to its CPU,
because nothing on a CI runner flashes a board.
"""
import argparse
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import odver

TOP = "OpenDoctrines-freertos/"
EM_ARM, EM_RISCV = 40, 243
BOARDS = {
    "mps2-an385": (EM_ARM, "qemu-system-arm -M mps2-an385 -nographic -semihosting-config enable=on,target=native -kernel mps2-an385.elf"),
    "stm32f405": (EM_ARM, "qemu-system-arm -M netduinoplus2 -nographic -semihosting-config enable=on,target=native -kernel stm32f405.elf"),
    "riscv32-virt": (EM_RISCV, "qemu-system-riscv32 -M virt -bios none -nographic -kernel riscv32-virt.elf"),
}

README = """Open Doctrines on FreeRTOS {version}

The game's rules on a microcontroller: 1,632 provinces, 183 countries, the
desktop game's combat width, an opponent for every country, played over a
serial line. Type "help" once it starts. Ctrl-A then X leaves QEMU.

{runs}

On a real STM32F405 board: flash stm32f405.bin at 0x08000000 (st-flash write
stm32f405.bin 0x08000000, or any SWD programmer), and connect a 3.3 V serial
adapter to PA9 (TX) and PA10 (RX), 115200 8N1.

Source and the full story: freertos/README.md in the repository.
"""


def elf_machine(b):
    if b[:4] != b"\x7fELF" or b[4] != 1:
        return None
    return struct.unpack("<H", b[18:20])[0]


def build(builds, out):
    version = str(odver.read())
    runs = "\n".join(f"  {name}:\n    {cmd}\n" for name, (_, cmd) in BOARDS.items())
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in BOARDS:
            z.write(os.path.join(builds, name, "opendoctrines.elf"), f"{TOP}{name}.elf")
        z.write(os.path.join(builds, "stm32f405", "opendoctrines.bin"), f"{TOP}stm32f405.bin")
        z.writestr(TOP + "README.txt", README.format(version=version, runs=runs))
        z.writestr(TOP + "VERSION", version + "\n")
    print(f"wrote {out} ({os.path.getsize(out) // 1024} KB), Open Doctrines {version}")
    return check(out)


def check(path):
    problems = []
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        for name, (machine, _) in BOARDS.items():
            elf = f"{TOP}{name}.elf"
            if elf not in names:
                problems.append(f"no {elf}")
            elif elf_machine(z.read(elf)) != machine:
                problems.append(f"{elf} is not a 32-bit ELF for machine {machine}")
        binary = TOP + "stm32f405.bin"
        if binary not in names:
            problems.append(f"no {binary}")
        else:
            b = z.read(binary)
            sp, reset = struct.unpack("<II", b[:8])
            # The vector table: the stack in SRAM, the reset handler in flash, Thumb.
            if not (0x20000000 < sp <= 0x20020000 and 0x08000000 <= reset < 0x08100000 and reset & 1):
                problems.append(f"{binary} does not start with an STM32F405 vector table")
        for need in ("README.txt", "VERSION"):
            if TOP + need not in names:
                problems.append(f"no {TOP}{need}")
    if problems:
        print("FAILED: " + "; ".join(problems), file=sys.stderr)
        return 1
    print(f"ok    {path}: three boards, each for its CPU, and a flashable STM32F405 image")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--builds")
    ap.add_argument("--out")
    ap.add_argument("--check", metavar="ZIP")
    a = ap.parse_args()
    if a.check:
        return check(a.check)
    if not (a.builds and a.out):
        ap.error("--builds and --out, or --check")
    return build(a.builds, a.out)


if __name__ == "__main__":
    sys.exit(main())
