#!/usr/bin/env python3
"""The mod registry's capability list agrees with sdk/abi.json.

    tools/check_registry_modules.py

WHY THIS EXISTS

net/src/mods/registry.ts validates a listing's `modules` against KNOWN_MODULES,
written by hand. It fell eighteen names behind: ten entries while the ABI
defined twenty-eight, so the directory refused every mod declaring anything
added since the list was typed -- Economy, Country, Military, Politics,
Research, Content, Scripts, Net, Render, Audio, and Neural.Decide.

Neural.Decide is the capability Gearbox 1.3 exists for. The first mod built on
it could not be listed, and the only thing the author saw was

    Unknown capability module: Economy.Read

which names the wrong module and says nothing about the list being stale. The
mod loaded in the real runtime the whole time, because the ENGINE knew every
one of those names. Only the directory did not.

A hand-kept copy of a list that something else owns goes stale; it is only a
question of when somebody notices. This is how it gets noticed.

WasiStub is excluded on purpose: it is the libc shim a module links against,
not a capability a listing declares.
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ABI = os.path.join(ROOT, "sdk", "abi.json")
REGISTRY = os.path.join(ROOT, "net", "src", "mods", "registry.ts")

NOT_A_CAPABILITY = {"WasiStub"}


def abi_modules():
    with open(ABI, encoding="utf-8") as f:
        abi = json.load(f)
    mods = abi.get("modules")
    if isinstance(mods, dict):
        names = set(mods)
    elif isinstance(mods, list):
        names = {m.get("name", str(m)) if isinstance(m, dict) else str(m) for m in mods}
    else:
        print(f"FAIL  {ABI} has no usable 'modules'")
        sys.exit(1)
    return names - NOT_A_CAPABILITY


def registry_modules():
    with open(REGISTRY, encoding="utf-8") as f:
        src = f.read()
    m = re.search(r"const KNOWN_MODULES\s*=\s*\[(.*?)\];", src, re.S)
    if not m:
        print(f"FAIL  no KNOWN_MODULES array in {REGISTRY}")
        sys.exit(1)
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def main():
    want, have = abi_modules(), registry_modules()
    missing, extra = sorted(want - have), sorted(have - want)

    if not missing and not extra:
        print(f"ok    the mod registry knows all {len(want)} capability modules "
              f"sdk/abi.json defines")
        return 0

    for name in missing:
        print(f"FAIL  sdk/abi.json defines {name}, and the registry would refuse "
              f"a listing that declares it")
    for name in extra:
        print(f"FAIL  the registry accepts {name}, which sdk/abi.json does not "
              f"define")
    print(f"\n{len(missing) + len(extra)} problem(s). Update KNOWN_MODULES in "
          f"net/src/mods/registry.ts to match sdk/abi.json.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
