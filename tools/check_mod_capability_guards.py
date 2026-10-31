#!/usr/bin/env python3
"""Every mod host native that reaches the game re-checks its capability.

    tools/check_mod_capability_guards.py

WHY THIS EXISTS

WAMR registers host natives GLOBALLY, keyed by import module name, so a
per-instance capability grant cannot be expressed at the WAMR layer. The grant
is therefore enforced at INSTANTIATION -- a mod importing something it was not
granted never links (see ModRuntime::instantiate). That is the load-time gate.

The call-time check in each native -- `MOD_GUARD(bit, fail)`, `ED_GUARD(fail)`,
or an inline `mi->has(MODULE_X)` -- is the defence in depth behind it: if a
future import-resolution bug (or a WAMR behaviour change) ever let an un-granted
module reach a function, the function still refuses. A security audit flagged
two gaps here -- the write side could not be verified by reading alone, and the
ABI 1.0 reads (gs_*) and the fog reads skipped the call-time check entirely,
relying on the load-time gate the 1.1+ functions do not trust by itself.

So verify it mechanically, and keep it verified. Any native that calls
`g_modGame->` -- read or write -- must also hold a capability guard. A new native
that forgets one fails here rather than shipping a hole that only the
instantiation gate happens to cover today.

The guard tokens below are the three shapes in the tree: the MOD_GUARD and
ED_GUARD macros (which resolve `mi` and check `has(bit)`), and an inline
`mi->has(MODULE_...)` for the few natives written before the macros existed.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_CPP = os.path.join(ROOT, "src", "mods", "ModHost.cpp")

GUARD_TOKENS = ("MOD_GUARD(", "ED_GUARD(", "mi->has(MODULE_")


def strip_cxx(src: str) -> str:
    """Blank out comments and string/char literals so brace matching and token
    scanning never trip over text inside them. Newlines are kept so reported
    line numbers stay accurate."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        two = src[i:i + 2]
        if two == "//":
            while i < n and src[i] != "\n":
                i += 1
        elif two == "/*":
            i += 2
            while i < n and src[i:i + 2] != "*/":
                if src[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if src[i] == "\n":
                    out.append("\n")
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def functions(src: str):
    """Yield (name, body_text, line_no) for every `<ret> name(ExecEnv e, ...) {`
    definition, with body_text brace-matched."""
    for m in re.finditer(r"([A-Za-z_]\w*)\s*\(\s*ExecEnv\s+e\b[^;{)]*\)\s*\{", src):
        name = m.group(1)
        brace = src.index("{", m.start())
        depth, i, n = 0, brace, len(src)
        while i < n:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        yield name, src[brace:i + 1], src.count("\n", 0, m.start()) + 1


def main() -> int:
    with open(HOST_CPP, encoding="utf-8") as f:
        src = strip_cxx(f.read())

    checked = 0
    unguarded = []
    for name, body, line in functions(src):
        reached = sorted({m.group(1) for m in
                          re.finditer(r"g_modGame->\s*([A-Za-z_]\w*)", body)})
        if not reached:
            continue
        checked += 1
        if not any(tok in body for tok in GUARD_TOKENS):
            unguarded.append((name, line, reached))

    if unguarded:
        for name, line, reached in unguarded:
            shown = ", ".join(reached[:4]) + (", ..." if len(reached) > 4 else "")
            print(f"FAIL  {name} (ModHost.cpp:{line}) reaches the game "
                  f"({shown}) with no call-time capability guard")
        print(f"\n{len(unguarded)} native(s) missing a guard. Begin each with "
              f"MOD_GUARD(MODULE_X, fail) / ED_GUARD(fail), or an inline "
              f"`if (!mi || !mi->has(MODULE_X) || !g_modGame) return fail;`.")
        return 1

    print(f"ok    all {checked} mod natives that reach the game re-check a "
          f"capability at call time")
    return 0


if __name__ == "__main__":
    sys.exit(main())
