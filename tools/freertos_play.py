#!/usr/bin/env python3
"""Play a script of commands into the FreeRTOS edition and print what it said.

    tools/freertos_play.py SCRIPT -- COMMAND...

COMMAND is anything with the game's console on its stdin and stdout: the host
build, the POSIX build, or QEMU with -serial stdio. Each line of SCRIPT is
sent only once the game has printed its "> " prompt -- a serial port takes
what arrives before the program has set it up and drops it, which on QEMU's
STM32 model is every character typed before main() has run.

The transcript is normalised so that every board's can be compared with every
other's: carriage returns go, and so does the echo of each command, which a
serial line has and a terminal does not. The banner (it names the board) and
the "hk" lines (uptime, stacks and heap are the board's own) are dropped.
Exit status: the program's, or 2 if it went quiet for --timeout seconds.

    tools/freertos_play.py --raw-in FILE SCRIPT

normalises a transcript captured elsewhere instead of running anything: the
Plan 9 edition's, which plays the script as a file on its standard input
inside a 9front guest (tests/plan9_9front.py), where nothing here can wait on
its prompts. Lines of FILE before the first "> " are dropped as the banner.
"""
import argparse
import os
import select
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--raw", action="store_true", help="print the transcript as it came")
    ap.add_argument("--raw-in", help="normalise this captured output; run nothing")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.command[1:] if a.command[:1] == ["--"] else a.command
    lines = [l.rstrip("\n") for l in open(a.script) if l.strip() and not l.startswith("#")]
    if a.raw_in:
        with open(a.raw_in, "rb") as f:
            sys.stdout.write(normalise(f.read().decode("latin-1").replace("\r", ""), lines))
        return 0

    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    sent = 0
    last = time.time()
    fd = p.stdout.fileno()
    while True:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            out += chunk
            last = time.time()
            if out.endswith(b"> ") and sent < len(lines):
                p.stdin.write((lines[sent] + "\n").encode())
                p.stdin.flush()
                sent += 1
        elif p.poll() is not None:
            break
        elif time.time() - last > a.timeout:
            p.kill()
            sys.stdout.write(out.decode("latin-1"))
            print(f"\n[no output for {a.timeout:.0f} s after {sent} of {len(lines)} commands]")
            return 2
    code = p.wait()
    text = out.decode("latin-1").replace("\r", "")
    if a.raw:
        sys.stdout.write(text)
        return code

    sys.stdout.write(normalise(text, lines))
    return code


def normalise(text, lines):
    # Split at the prompts: block k is what command k-1 printed.
    blocks = text.split("> ")
    keep = []
    for k, block in enumerate(blocks[1:]):
        command = lines[k] if k < len(lines) else ""
        if command and block.startswith(command + "\n"):
            block = block[len(command) + 1:]
        if command.split()[:1] == ["hk"]:
            block = ""
        keep.append(f"> {command}\n{block}")
    return "".join(keep)


if __name__ == "__main__":
    sys.exit(main())
