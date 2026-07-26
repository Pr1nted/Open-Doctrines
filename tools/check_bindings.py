#!/usr/bin/env python3
"""Cross-check every SDK binding against sdk/abi.json.

Most of the bindings under sdk/ cannot be compiled here -- there is no Rust,
Zig, TinyGo, C# wasm or SwiftWasm toolchain on this machine. That makes a
transcription error invisible until a modder hits it, and the failure mode is
nasty: a mistyped import module name produces "this host does not provide that
import" at load time, pointing at the modder's code rather than at ours.

This catches what a compiler would not anyway. It checks two things textually:

  1. Every import module string appearing in a binding is one the host actually
     provides. A typo like "gearbox:gamestate" for "gearbox:gamestate.read" is
     caught here.
  2. Every import in abi.json is mentioned somewhere in the binding, so a
     binding that silently omits a function is reported.

It is a lint, not a proof. It cannot tell you the argument order is right.

    python3 tools/check_bindings.py            # check all
    python3 tools/check_bindings.py rust zig   # check some
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.join(ROOT, "sdk")

SOURCE_EXT = {".rs", ".zig", ".go", ".cs", ".swift", ".ts", ".wat", ".h",
              ".c", ".cpp", ".java", ".kt"}

# Directories that are a language binding rather than something else.
SKIP_DIRS = {"node_modules", "target", "bin", "obj", ".git", "examples"}

# Directories that are not expected to mention every import, and why. They are
# still checked for invalid module names and a mod_load export.
PARTIAL = {
    "cpp": "example only, uses sdk/gearbox.h",
    "wat": "hand-written wire-level reference",
    # The Lua binding is C that #includes sdk/gearbox.h, so the import
    # declarations it compiles against are the ones already checked as
    # "c (gearbox.h)" -- repeating them here would test the same text twice.
    # What is Lua-specific is the script-facing gearbox table, and this lint
    # cannot check a table's argument order any more than it can a struct's.
    # ModExamplesTest is what covers that, by rendering the mod.
    "lua": "binds via sdk/gearbox.h",
    # Same shape as Lua: gearbox_qjs.c #includes sdk/gearbox.h, and what is
    # JS-specific is the script-facing `gearbox` object plus its TypeScript
    # declarations in types/gearbox.d.ts.
    "js": "binds via sdk/gearbox.h",
    # Same again: gearbox_py.c includes sdk/gearbox.h, and the
    # script-facing surface is a Python module this lint cannot inspect.
    "python": "binds via sdk/gearbox.h",
}


def sources_for(path):
    out = []
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if os.path.splitext(fn)[1] in SOURCE_EXT:
                out.append(os.path.join(dirpath, fn))
    return out


def main():
    with open(os.path.join(SDK, "abi.json")) as f:
        abi = json.load(f)

    known_modules = {i["module"] for i in abi["imports"]}

    # WasiStub is not part of the surface a language binding wraps. Those imports
    # are emitted by an interpreter's own libc when someone ships CPython or Lua
    # inside a mod -- a Rust or Zig binding declaring fd_write would be wrong.
    # So they are known-good module names, but not required of any binding.
    imports = [(i["module"], i["name"]) for i in abi["imports"]
               if i.get("capability") != "WasiStub"]
    export_names = [e["name"] for e in abi["exports"]]

    wanted = sys.argv[1:]
    langs = []
    for entry in sorted(os.listdir(SDK)):
        p = os.path.join(SDK, entry)
        if not os.path.isdir(p) or entry in SKIP_DIRS:
            continue
        if wanted and entry not in wanted:
            continue
        langs.append((entry, p))

    # The C binding is two headers at the SDK root: gearbox.h carries the prose
    # and the macros, gearbox_generated.h the declarations (generated from
    # abi.json by tools/gen_bindings.py). Both are read as one binding.
    if not wanted or "c" in wanted:
        langs.insert(0, ("c (gearbox.h)", [os.path.join(SDK, "gearbox.h"),
                                           os.path.join(SDK, "gearbox_generated.h")]))

    failures = 0
    for name, path in langs:
        if isinstance(path, list):
            files = path
        else:
            files = [path] if os.path.isfile(path) else sources_for(path)
        if not files:
            continue
        text = ""
        for f in files:
            try:
                with open(f, encoding="utf-8", errors="replace") as fh:
                    text += fh.read() + "\n"
            except OSError:
                pass
        if not text.strip():
            continue

        problems = []

        # 1. every gearbox:* string must name a real module
        for m in sorted(set(re.findall(r"gearbox:[A-Za-z0-9_.]+", text))):
            if m not in known_modules:
                problems.append(f"unknown import module {m!r}")

        # 2. every ABI import should be mentioned -- but only for directories
        # that claim to be a full binding.
        partial = PARTIAL.get(name)
        missing = [f"{m}.{n}" for m, n in imports
                   if f'"{n}"' not in text and f"'{n}'" not in text
                   and not re.search(rf"\b{re.escape(n)}\b", text)]
        if missing and not partial:
            problems.append(f"{len(missing)} import(s) not mentioned: "
                            + ", ".join(missing[:4])
                            + (" …" if len(missing) > 4 else ""))

        # 3. mod_load must appear; it is the only mandatory export
        if not re.search(r"\bmod_load\b", text):
            problems.append("mod_load is never mentioned")

        missing_exports = [e for e in export_names
                           if not re.search(rf"\b{re.escape(e)}\b", text)]

        if problems:
            failures += 1
            print(f"FAIL  {name}")
            for p in problems:
                print(f"        {p}")
        else:
            extra = ""
            if missing_exports:
                extra = f"  (optional exports not mentioned: {', '.join(missing_exports)})"
            if partial:
                print(f"ok    {name}  —  {partial}, "
                      f"{len(imports) - len(missing)}/{len(imports)} imports used")
            else:
                print(f"ok    {name}  —  {len(imports)} imports, "
                      f"{len(export_names) - len(missing_exports)}/{len(export_names)}"
                      f" exports{extra}")

    print()
    if failures:
        print(f"{failures} binding(s) disagree with sdk/abi.json")
        return 1
    print("all bindings reference the ABI consistently")
    print("NOTE: this is a text lint. It does not verify argument order or")
    print("      types, and it does not compile anything.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
