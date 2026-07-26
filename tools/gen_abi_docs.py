#!/usr/bin/env python3
"""Generate docs/gearbox-abi.md from sdk/abi.json.

The ABI reference is generated rather than written so it cannot drift from the
machine-readable definition, which tests/mod_abi_test.cpp in turn pins to the
host's real capability table. The chain is:

    host capability table  <--(ModAbiTest)-->  sdk/abi.json  --(this)-->  docs

Run from the repo root:  python3 tools/gen_abi_docs.py
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ABI = os.path.join(ROOT, "sdk", "abi.json")
OUT = os.path.join(ROOT, "docs", "gearbox-abi.md")

WIRE = {"i32": "i32", "i64": "i64", "f32": "f32", "f64": "f64"}


def wire(t):
    return WIRE.get(t, t)


def role_note(p):
    r = p.get("role", "")
    if r == "ptr":
        return "pointer into your linear memory"
    if r == "len":
        return "byte length"
    if r == "color":
        return "`0xRRGGBBAA`"
    if r == "bool":
        return "0 or 1"
    if r.startswith("handle:"):
        return f"opaque {r.split(':', 1)[1]} handle"
    if r.startswith("enum:"):
        return f"see [{r.split(':', 1)[1]}](#enums)"
    return ""


def wat_signature(imp):
    params = " ".join(wire(p["type"]) for p in imp["params"])
    res = imp.get("result")
    parts = [f'(import "{imp["module"]}" "{imp["name"]}" (func $x']
    if params:
        parts.append(f"(param {params})")
    if res:
        parts.append(f'(result {wire(res["type"])})')
    return " ".join(parts) + "))"


def main():
    with open(ABI) as f:
        abi = json.load(f)

    o = []
    w = o.append

    w("<!-- GENERATED FILE - do not edit by hand.")
    w("     Source: sdk/abi.json   Generator: tools/gen_abi_docs.py")
    w("     Regenerate with: python3 tools/gen_abi_docs.py -->")
    w("")
    w(f"# Gearbox ABI Reference — v{abi['gearbox']}")
    w("")
    w("The complete wire contract between a mod and the host. Every SDK under")
    w("`sdk/` is a transcription of this; if an SDK disagrees with this page, the")
    w("SDK is wrong.")
    w("")
    w("This page is generated from [`sdk/abi.json`](../sdk/abi.json), which")
    w("`tests/mod_abi_test.cpp` checks against the host's real capability table in")
    w("both directions. So the chain host → abi.json → this page is verified at")
    w("build time, not maintained by hand.")
    w("")
    w("- **Imports** are functions the host provides and your mod calls.")
    w("- **Exports** are functions your mod provides and the host calls.")
    w("- Imports live in WASM modules named after the capability that grants them.")
    w("  You only receive imports for capabilities you declared in `MANIFEST.json`")
    w("  **and** the user granted. Importing anything else refuses the load.")
    w("")
    w("Strings are `(ptr, len)` pairs of UTF-8 bytes in your linear memory. They")
    w("are **not** null-terminated. The host never keeps a pointer into your")
    w("memory after a call returns, and you must not keep one of the host's.")
    w("")

    # ---------------- capability modules ----------------
    w("## Capability modules")
    w("")
    w("| Module | Import namespace | Grants | Revocable | Status |")
    w("|---|---|---|---|---|")
    for m in abi["modules"]:
        ns = f"`{m['import']}`" if m["import"] else "—"
        w("| `{}` | {} | {} | {} | {} |".format(
            m["name"], ns, m["grants"],
            "yes" if m["revocable"] else "**no, always granted**",
            "implemented" if m["implemented"] else "**not implemented**"))
    w("")
    w("Requesting a module marked *not implemented* means the imports do not")
    w("exist, so your mod is **refused at load** with a diagnostic naming the")
    w("import. That is deliberate: running you without a capability you believe")
    w("you hold would be worse than refusing.")
    w("")
    w("`GameProcess` grants no imports. It gates whether the host *calls* your")
    w("`mod_pre_turn` / `mod_post_turn` exports.")
    w("")

    # ---------------- imports ----------------
    w("## Imports")
    w("")
    by_module = {}
    for imp in abi["imports"]:
        by_module.setdefault(imp["module"], []).append(imp)

    for mod, entries in by_module.items():
        cap = entries[0].get("capability")
        w(f"### `{mod}`")
        w("")
        w(f"Requires the **{cap or 'Core'}** capability."
          + (" Always granted; cannot be revoked." if not cap else ""))
        w("")
        for imp in entries:
            w(f"#### `{imp['name']}`")
            w("")
            w("```wat")
            w(wat_signature(imp))
            w("```")
            w("")
            if imp["params"]:
                w("| Parameter | Wire type | Meaning |")
                w("|---|---|---|")
                for p in imp["params"]:
                    w("| `{}` | `{}` | {} |".format(
                        p["name"], wire(p["type"]), role_note(p) or "—"))
                w("")
            res = imp.get("result")
            if res:
                note = role_note(res)
                w(f"**Returns** `{wire(res['type'])}`"
                  + (f" — {note}." if note else "."))
                w("")
            else:
                w("**Returns** nothing.")
                w("")
            if imp.get("noreturn"):
                w("**Does not return.** The call traps out of your mod.")
                w("")
            w(imp["doc"])
            w("")

    # ---------------- exports ----------------
    w("## Exports")
    w("")
    w("Only `mod_load` is mandatory. A missing optional export is simply not")
    w("called — it is not an error.")
    w("")
    for e in abi["exports"]:
        w(f"### `{e['name']}`")
        w("")
        params = " ".join(wire(p["type"]) for p in e["params"])
        res = e.get("result")
        sig = f'(func (export "{e["name"]}")'
        if params:
            sig += f" (param {params})"
        if res:
            sig += f' (result {wire(res["type"])})'
        sig += " ...)"
        w("```wat")
        w(sig)
        w("```")
        w("")
        if e["params"]:
            w("| Parameter | Wire type | Meaning |")
            w("|---|---|---|")
            for p in e["params"]:
                w("| `{}` | `{}` | {} |".format(
                    p["name"], wire(p["type"]), role_note(p) or "—"))
            w("")
        w("**Required:** {}.  **Capability:** {}".format(
            "yes" if e.get("required") else "no",
            f"`{e['capability']}`" if e.get("capability") else "none"))
        w("")
        w(e["doc"])
        w("")

    # ---------------- env struct ----------------
    env = abi["env_struct"]
    w("## The `env` struct")
    w("")
    w(f"`{env['size_bytes']}` bytes on wasm32. **Layout is part of the ABI:**")
    w("fields are only ever appended, never reordered or resized.")
    w("")
    w("Write your own struct size into `size` *before* calling `env`. The host")
    w("writes at most that many bytes, so a mod built against an older, smaller")
    w("struct is safe against a newer host that has appended fields.")
    w("")
    w("| Offset | Field | Type | Meaning |")
    w("|---|---|---|---|")
    for f in env["fields"]:
        w("| {} | `{}` | `{}` | {} |".format(
            f["offset"], f["name"], f["type"], f.get("doc", "—")))
    w("")

    # ---------------- enums / constants ----------------
    w("## Enums")
    w("")
    for name, vals in abi["enums"].items():
        w(f"**`{name}`** — " + ", ".join(f"`{k}` = {v}" for k, v in vals.items()))
        w("")
    w("## Constants")
    w("")
    for k, v in abi["constants"].items():
        w(f"- `{k}` = `{v}`")
    w("")

    # ---------------- writing your own binding ----------------
    w("## Writing a binding for a language we do not ship")
    w("")
    w("If your language compiles to wasm32 and can declare imports with an")
    w("explicit module name, you can bind to this in an afternoon. You need three")
    w("things:")
    w("")
    w("1. **Declare the imports.** Whatever your language's syntax is for")
    w("   \"external function in wasm module X named Y\". The module name contains")
    w("   a colon (`gearbox:core`), which some toolchains handle awkwardly — check")
    w("   that early.")
    w("2. **Export `mod_load`** with the exact name, returning `i32`.")
    w("3. **Get a pointer and a length out of a string.** Everything else follows.")
    w("")
    w("At the wire level there is nothing else to it. The raw WAT in")
    w("[`sdk/wat/`](../sdk/wat) shows exactly what the bytes look like, and")
    w("`sdk/abi.json` is machine-readable if you would rather generate the")
    w("binding than write it.")
    w("")
    w("Two things that trip people up:")
    w("")
    w("- **Do not import anything else.** A toolchain that emits `env.abort`,")
    w("  `wasi_snapshot_preview1.fd_write`, or similar will be refused at load —")
    w("  the diagnostic names the offending import. Freestanding/no-std flags are")
    w("  usually what you need.")
    w("- **Your allocator, if any, is yours.** The host never allocates in your")
    w("  memory and never frees anything you pass it.")
    w("")

    with open(OUT, "w") as f:
        f.write("\n".join(o) + "\n")
    print(f"wrote {os.path.relpath(OUT, ROOT)} ({len(o)} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
