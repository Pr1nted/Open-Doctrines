#!/usr/bin/env python3
"""One fixture mod per capability module, generated from sdk/abi.json.

    python3 tools/gen_capability_mods.py [outdir]     # default build/capmods

WHAT THIS IS FOR

The static side of the SDK is already covered three times over:
ModAbiTest checks the host's capability table against abi.json in both
directions, gen_bindings.py --check keeps every generated binding in step with
it, and check_bindings.py verifies each hand-written binding names every
import and spells its module right.

None of that CALLS anything. A capability can be declared by the host,
described in abi.json, named correctly in eleven bindings, and still answer
wrongly -- or not be reachable at all -- and every one of those checks passes.
That is the "wiring is not execution" gap, and it is the one a modder falls
into first.

So: for each `gearbox:*` module, this emits a C mod that calls EVERY import in
that module once and counts the calls the host came back from. The test that
loads them asserts the count equals the number of imports, which is the thing
worth knowing -- every function in the capability is reachable and the host
survives being asked.

WHAT IT DOES NOT CLAIM. The arguments are benign placeholders: zero handles,
empty strings, zero coordinates. So this proves REACHABILITY and that the host
refuses bad input without falling over. It does not check that the answers are
right -- a function returning the wrong province is indistinguishable here from
one returning the right one. The per-capability tests under tests/ are where
meaning is checked; this is where presence is.

WHY C AND NOT ELEVEN LANGUAGES. Compiling a mod needs that language's wasm
toolchain, and this machine has clang (through emscripten) and nothing else --
no TinyGo, no Zig, no wat2wasm. The other bindings are checked statically and
their prebuilt examples are byte-compared where a toolchain exists. Generating
the same mod for every language is possible and belongs with the CI runners
that have those toolchains; the capability axis is the one that can be covered
here, and it is the one that was empty.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Imports that must not be called by a test that expects to return.
#   abort       -- terminates the instance, by design
#   proc_exit   -- the same, through wasi
# Everything else is expected to tolerate nonsense arguments, which is itself
# part of what this checks.
SKIP = {"abort", "proc_exit"}


def c_names(header):
    """(module-suffix, wire name) -> C identifier, read out of the header.

    NOT "gearbox_" + name. The wire name is what crosses the ABI and the C
    name is what a modder types, and they are deliberately different wherever
    the wire name is generic: gearbox:assets "read" is gearbox_asset_read,
    gearbox:content "add" is gearbox_content_add. Guessing the first rule
    produced a generator that silently skipped the whole Assets capability and
    four fifths of Content, and reported it as those functions being absent
    from the header -- they were there, under the names the header gives them.

    The header states the pair on every declaration, so it is asked rather
    than second-guessed:

        GEARBOX_IMPORT("assets", "read")
        uint32_t gearbox_asset_read(const char* name, ...);
    """
    out = {}
    pat = re.compile(
        r'GEARBOX_IMPORT\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\)\s*'
        r'[\r\n]+[^(;]*?([A-Za-z_][A-Za-z0-9_]*)\s*\(')
    for mod, wire, ident in pat.findall(header):
        out[(mod, wire)] = ident
    return out


def placeholder(p):
    """A benign argument of the right C type.

    Pointers get 0 with a length of 0. The host is documented to refuse an
    out-of-bounds (ptr,len) and log it rather than read it, so this exercises
    the refusal path instead of crashing -- which is the behaviour worth
    having under test anyway.
    """
    # A pointer parameter gets a real null rather than (int32_t)0. Both are
    # zero in wasm32 and clang warns about the second -- ten warnings on the
    # Country fixture alone -- because an integer cast to a pointer is almost
    # always a mistake. Here it is deliberate, so say so in the type system.
    if p.get("role") == "ptr":
        return "(void*)0"
    t = p.get("type", "i32")
    if t == "i64":
        return "(int64_t)0"
    if t == "f64":
        return "(double)0"
    if t == "f32":
        return "(float)0"
    return "(int32_t)0"


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "capmods")
    abi = json.load(open(os.path.join(ROOT, "sdk", "abi.json"), encoding="utf-8"))
    hdr = open(os.path.join(ROOT, "sdk", "gearbox_generated.h"), encoding="utf-8").read()

    names = c_names(hdr)
    print("%d import declarations found in gearbox_generated.h" % len(names))

    by_module = {}
    for imp in abi["imports"]:
        mod = imp.get("module") or ""
        if not mod.startswith("gearbox:"):
            continue          # wasi_snapshot_preview1 is WasiStub's business
        by_module.setdefault(mod, []).append(imp)

    os.makedirs(out, exist_ok=True)
    written, skipped_fns = [], []
    for mod, imps in sorted(by_module.items()):
        slug = re.sub(r"[^a-z0-9]+", "_", mod.replace("gearbox:", ""))
        calls, declared = [], 0
        for imp in sorted(imps, key=lambda i: i["name"]):
            if imp["name"] in SKIP:
                skipped_fns.append(mod + ":" + imp["name"])
                continue
            fn = names.get((mod.replace("gearbox:", ""), imp["name"]))
            # A name in abi.json that the header does not declare is a real
            # gap, but it is gen_bindings.py's to report -- emitting a call to
            # an undeclared function here would fail to compile and blame the
            # wrong file.
            if not fn:
                skipped_fns.append(mod + ":" + imp["name"] + " (not declared in gearbox_generated.h)")
                continue
            args = ", ".join(placeholder(p) for p in imp.get("params", []))
            cast = "(void)" if imp.get("result") else ""
            calls.append("    %s%s(%s); ++g_calls;" % (cast, fn, args))
            declared += 1

        src = os.path.join(out, "cap_%s.c" % slug)
        with open(src, "w", encoding="utf-8") as f:
            f.write("/* GENERATED by tools/gen_capability_mods.py -- do not edit.\n"
                    " *\n"
                    " * Calls every import in %s once.\n"
                    " * Expected call count: %d\n"
                    " */\n" % (mod, declared))
            f.write('#include "gearbox.h"\n\n')
            f.write("static int32_t g_calls = 0;\n\n")
            f.write('GEARBOX_EXPORT("cap_calls")\nint32_t cap_calls(void) { return g_calls; }\n\n')
            f.write('GEARBOX_EXPORT("mod_load")\nint32_t mod_load(void) {\n')
            f.write("\n".join(calls) if calls else "    /* nothing callable */")
            f.write("\n    return 0;\n}\n")
        # The MANIFEST name, which is not the wire module name: the wire says
        # "gearbox:politics.read" and a mod's MANIFEST.json says
        # "Politics.Read". abi.json carries it on each import as `capability`
        # -- null for Core, which needs no grant.
        cap = next((i.get("capability") for i in imps if i.get("capability")), "Core")
        written.append((mod, slug, declared, cap))

    manifest = os.path.join(out, "capabilities.json")
    with open(manifest, "w", encoding="utf-8") as f:
        json.dump({"modules": [{"module": m, "slug": s, "expected_calls": n,
                                "capability": c}
                               for m, s, n, c in written]}, f, indent=2)

    for m, s, n, c in written:
        print("  %-26s cap_%s.c  %-2d call(s)  manifest: %s" % (m, s, n, c))
    print("\n%d capability module(s) -> %s" % (len(written), out))
    if skipped_fns:
        print("skipped %d: %s" % (len(skipped_fns), ", ".join(skipped_fns[:6])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
