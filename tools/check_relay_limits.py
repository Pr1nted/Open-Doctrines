#!/usr/bin/env python3
"""Do the game and the relay agree on what the relay will carry?

The relay drops an oversized frame SILENTLY -- no error to the sender, nothing
to the other end (net/src/lobby/LobbyDO.ts). So the game checks the same
numbers before it sends, and those numbers live in two files in two languages.
When they drift, nothing fails: a relayed host's snapshot between the two
limits is simply never delivered, and the joining player waits forever for a
world nobody will send again.

The Worker is the authority -- it is the thing doing the dropping -- so a
mismatch is reported as the C++ side being wrong.

Usage:  python3 tools/check_relay_limits.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKER = os.path.join(ROOT, "net", "src", "lobby", "LobbyDO.ts")
HEADER = os.path.join(ROOT, "src", "net", "NetProtocol.h")

# name in the Worker -> name in the header
PAIRS = [("MAX_FROM_HOST", "kNetRelayMaxFromHost"),
         ("MAX_FROM_CLIENT", "kNetRelayMaxFromClient")]


def worker_value(text, name):
    m = re.search(r"const\s+" + name + r"\s*=\s*([0-9*\s]+);", text)
    if not m:
        return None
    return eval(m.group(1), {"__builtins__": {}})      # digits and * only


def header_value(text, name):
    m = re.search(r"k" + name[1:] + r"\s*=\s*([0-9a-zA-Z*\s]+);", text)
    if not m:
        return None
    expr = m.group(1).replace("u", "")
    return eval(expr, {"__builtins__": {}})


def main():
    if not os.path.exists(WORKER):
        print("skip: no Worker source here, so there is nothing to compare")
        return 0
    worker = open(WORKER, encoding="utf-8").read()
    header = open(HEADER, encoding="utf-8").read()

    bad = 0
    for ts_name, cpp_name in PAIRS:
        want = worker_value(worker, ts_name)
        got = header_value(header, cpp_name)
        if want is None:
            print(f"FAILED: {ts_name} is not in {os.path.relpath(WORKER, ROOT)} any more")
            bad += 1
            continue
        if got is None:
            print(f"FAILED: {cpp_name} is not in {os.path.relpath(HEADER, ROOT)}")
            bad += 1
            continue
        if want != got:
            print(f"FAILED: the relay carries {want} bytes for {ts_name}, "
                  f"but the game checks {got} ({cpp_name}). The Worker is the "
                  f"authority; change the header.")
            bad += 1
        else:
            print(f"ok    {cpp_name} == {ts_name} == {want} bytes")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
