#!/usr/bin/env python3
"""The map-scripting surface, written down so it cannot grow quietly.

    python3 tools/script_surface.py --check     # fails if the source moved
    python3 tools/script_surface.py --update    # accept the new surface
    python3 tools/script_surface.py             # print it

WHY THIS EXISTS. The scripting language has at least four readers, and only one
of them is the engine:

  * docs/scripting.md, which is what a mapmaker has;
  * the map editor's lint, which refuses a statement the declared version does
    not have (ScriptEngine::statementMinVersion);
  * dragoman's src/Scripts.cpp, which translates OD scripts to and from Greater
    Diplomacy 5 `scripted_events` -- a separate MIT repository that cannot
    import this one, so nothing makes it notice;
  * and any mod built against the Gearbox ABI.

Adding a statement to ScriptEngine.cpp updates exactly one of those. The
others go stale silently and stay stale: when this check was first written the
engine was on version 5 and dragoman's translator still emitted
`#OD/MapEngine/2`, knowing nothing of `wait`, `morph`, `terrain` or `fog` --
three versions of drift that no test had ever mentioned.

So the surface is extracted from the source, committed as JSON, and compared.
It is a TRIPWIRE, not a parser: it does not care what a statement means, only
that the set of them is the set that was last agreed. When it fails, the fix is
to update the readers that need it and then --update, not to --update first.
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tools", "script_surface.json")

# Who has to be told when each part of the surface changes. Printed with the
# failure, because "something changed" without "and here is who cares" is how
# a check gets --updated instead of acted on.
READERS = {
    "statements": [
        "docs/scripting.md -- the statement list under ## Commands",
        "ScriptEngine::statementMinVersion -- or the editor lint will allow it in a v1 file",
        "dragoman src/Scripts.cpp -- the OD <-> GD5 translator",
    ],
    "foreach_sources": [
        "docs/scripting.md -- ### foreach / next",
        "dragoman src/Scripts.cpp",
    ],
    "functions": ["docs/scripting.md -- ## Expressions"],
    "ref_roots": ["docs/scripting.md -- ## Variables & References"],
    "engine_version": [
        "docs/scripting.md -- the '## Version: N' heading and the header blurb",
        "dragoman src/Scripts.cpp -- the version it WRITES into generated scripts",
    ],
    "dialects": [
        "docs/scripting.md -- ## Foreign dialects",
        "tests/script_dialect_test.cpp -- every mapping a dialect claims",
        "the dialect's own source of truth, if it has one outside this repo",
    ],
}


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as fh:
        return fh.read()


def surface():
    engine = read("src", "ScriptEngine.cpp")
    header = read("src", "ScriptEngine.h")
    expr = read("src", "script", "Expr.cpp")

    m = re.search(r"ENGINE_VERSION\s*=\s*(\d+)", header)
    if not m:
        sys.exit("could not find ENGINE_VERSION in src/ScriptEngine.h")
    version = int(m.group(1))

    # `if (kw == "x")`, `kw == "x" ||`, and the bare `bt[0] == "x"` block
    # terminators the block scanner matches on.
    statements = set(re.findall(r'\bkw\s*==\s*"([a-z_]+)"', engine))
    statements |= set(re.findall(r'\bbt\[0\]\s*==\s*"([a-z_]+)"', engine))
    statements |= set(re.findall(r'\bt\[0\]\s*==\s*"([a-z_]+)"', engine))

    # The version gate's own tables, as the authority on which version a
    # statement needs. A statement in `statements` and in no table is v1.
    gates = {}
    for ver in range(2, 10):
        tbl = re.search(r"kV%d\[\]\s*=\s*\{(.*?)\};" % ver, engine, re.S)
        if not tbl:
            continue
        for kw in re.findall(r'"([a-z_]+)"', tbl.group(1)):
            gates[kw] = ver

    # `foreach <what> in ...`: the loop variables the dispatch accepts. Scoped
    # to the foreach handler, because `tokens[1]` means something different in
    # every other statement -- unscoped, this picked up "land" from `terrain
    # land|sea` and called it a kind of loop.
    fe = engine[engine.index('if (kw == "foreach")'):]
    fe = fe[:fe.index('if (kw == "array" || kw == "list")')]
    sources = set(re.findall(r'\btokens\[1\]\s*(?:!=|==)\s*"([a-z_]+)"', fe))

    functions = set(re.findall(r'\bf\s*==\s*"([a-z_]+)"', expr))

    # The foreign dialects, from the registry table in Dialects.cpp. A dialect
    # is a translator into native script text (never an evaluator), so what
    # matters here is only that one was added, removed or revised -- its
    # mappings are the dialect test's business.
    dia = read("src", "script", "Dialects.cpp")
    tbl = re.search(r"kAll\s*=\s*\{(.*?)\n    \};", dia, re.S)
    dialects = {}
    if tbl:
        for name, ver in re.findall(r'\{\s*"([a-z0-9_]+)"\s*,\s*(\d+)', tbl.group(1)):
            dialects[name] = int(ver)

    # A reference root is whatever resolveRef/setRef dispatch on.
    roots = set(re.findall(r'ref\.rfind\("([a-z_]+)\.",\s*0\)', engine))
    roots |= set(re.findall(r'\bdots\[0\]\s*==\s*"([a-z_]+)"', engine))
    roots |= set(re.findall(r'head\.rfind\("([a-z_]+)\.",\s*0\)', engine))

    # `wait` and `waitUntil` are recognised by isWaitLine/isWaitTurnsLine
    # rather than by a `kw ==` in the dispatch, so the scan above misses them;
    # the version table knows about `wait`. Union it in so a statement cannot
    # be in the gate and absent from the list.
    statements |= set(gates)
    statements.add("waitUntil")
    statements.add("include")        # spliced by preprocess(), not dispatched

    return {
        "engine_version": version,
        "statements": sorted(statements),
        "statement_min_version": dict(sorted(gates.items())),
        "foreach_sources": sorted(sources),
        "functions": sorted(functions),
        "ref_roots": sorted(roots),
        "dialects": dict(sorted(dialects.items())),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true", help="fail if the source has moved")
    g.add_argument("--update", action="store_true", help="accept the current surface")
    args = ap.parse_args()

    now = surface()

    if args.update:
        with open(MANIFEST, "w", encoding="utf-8") as fh:
            json.dump(now, fh, indent=2)
            fh.write("\n")
        print(f"tools/script_surface.json updated: engine version "
              f"{now['engine_version']}, {len(now['statements'])} statements")
        return 0

    if not args.check:
        json.dump(now, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0

    if not os.path.exists(MANIFEST):
        print("  FAIL  tools/script_surface.json is missing; "
              "run tools/script_surface.py --update")
        return 1
    with open(MANIFEST, encoding="utf-8") as fh:
        was = json.load(fh)

    drift = []
    for key in ("statements", "foreach_sources", "functions", "ref_roots"):
        gained = sorted(set(now.get(key, [])) - set(was.get(key, [])))
        lost = sorted(set(was.get(key, [])) - set(now.get(key, [])))
        if gained or lost:
            drift.append((key, gained, lost))
    if now["engine_version"] != was.get("engine_version"):
        drift.append(("engine_version",
                      [str(now["engine_version"])], [str(was.get("engine_version"))]))
    if now["dialects"] != was.get("dialects", {}):
        a, b = was.get("dialects", {}), now["dialects"]
        changed = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
        drift.append(("dialects",
                      [f"{k} revision {b[k]}" for k in changed if k in b],
                      [f"{k} (was revision {a[k]})" for k in changed if k not in b]))
    if now["statement_min_version"] != was.get("statement_min_version"):
        a, b = was.get("statement_min_version", {}), now["statement_min_version"]
        changed = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
        drift.append(("statement_min_version", [f"{k}={b.get(k, 1)}" for k in changed], []))

    if not drift:
        print(f"  the scripting surface is as recorded (engine version "
              f"{now['engine_version']}, {len(now['statements'])} statements)")
        return 0

    print("  FAIL  the map-scripting surface has moved:")
    for key, gained, lost in drift:
        if gained:
            print(f"          {key}: + {', '.join(gained)}")
        if lost:
            print(f"          {key}: - {', '.join(lost)}")
        for who in READERS.get(key, []):
            print(f"            tell: {who}")
    print()
    print("        Update those, then run: python3 tools/script_surface.py --update")
    return 1


if __name__ == "__main__":
    sys.exit(main())
