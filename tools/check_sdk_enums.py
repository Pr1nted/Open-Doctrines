#!/usr/bin/env python3
"""The hand-written SDK header's enums against sdk/abi.json.

WHY THIS EXISTS

Most of sdk/ is generated from abi.json and so cannot drift. sdk/gearbox.h is
not: it is written by hand, because it carries the prose a modder reads, and it
mirrors four of the ABI's enums as C constants. A mirror nobody checks is a
mirror that goes wrong silently -- and the failure is the worst kind, because a
mod compiled against a stale constant passes the wrong index to a call that
accepts it. The host would apply the wrong effect, or refuse a target that
exists, and the mod author would have no way to see why.

Run with --check in tests/run_all.sh. Exits non-zero on any disagreement.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# abi.json enum -> the prefix its C constants carry in sdk/gearbox.h. Only the
# enums the header actually mirrors; the rest are documented in abi.json alone
# and a mod passes a literal, so there is nothing to disagree.
MIRRORED = {
    "net_role": "GEARBOX_NET_",
    "disclosure_field": "GEARBOX_DISCLOSURE_",
    "shader_target": "GEARBOX_SHADER_TARGET_",
    "shader_effect": "GEARBOX_SHADER_EFFECT_",
}


def main() -> int:
    with open(os.path.join(ROOT, "sdk", "abi.json")) as f:
        abi = json.load(f)
    with open(os.path.join(ROOT, "sdk", "gearbox.h")) as f:
        header = f.read()

    # NAME = 0 or NAME = 0,  -- the comment after it is prose and is ignored.
    found = dict(re.findall(r"\b(GEARBOX_[A-Z0-9_]+)\s*=\s*(\d+)", header))
    bad = []
    checked = 0

    for enum, prefix in MIRRORED.items():
        want = {k: v for k, v in abi["enums"].get(enum, {}).items()
                if not k.startswith("$")}
        if not want:
            bad.append(f"abi.json has no enum {enum!r}, but gearbox.h mirrors it")
            continue
        for name, value in want.items():
            const = prefix + name.upper()
            checked += 1
            if const not in found:
                bad.append(f"{enum}.{name} = {value} is not in sdk/gearbox.h "
                           f"(expected {const})")
            elif int(found[const]) != value:
                bad.append(f"{const} is {found[const]} in sdk/gearbox.h, "
                           f"{value} in abi.json")
        # And the other direction: a constant the header carries that the ABI
        # dropped would compile and mean nothing.
        for const in found:
            if not const.startswith(prefix):
                continue
            tail = const[len(prefix):]
            if not any(tail == n.upper() for n in want):
                bad.append(f"sdk/gearbox.h carries {const}, which abi.json's "
                           f"{enum} does not list")

    for line in bad:
        print(f"  FAIL  {line}")
    print(f"{checked} mirrored enum values checked, {len(bad)} wrong")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
