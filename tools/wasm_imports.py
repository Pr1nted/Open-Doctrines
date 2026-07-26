#!/usr/bin/env python3
"""List the imports and exports of a .wasm or .odmod file.

The first thing to run when a mod is refused with "this host does not provide
that import": every import must start with "gearbox:". Anything else is your
toolchain adding something you did not ask for.

    python3 tools/wasm_imports.py mod.wasm
    python3 tools/wasm_imports.py mymod.odmod
"""

import sys
import zipfile

KINDS = {0: "func", 1: "table", 2: "memory", 3: "global"}


def uleb(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7F) << s
        if not x & 0x80:
            return r, i
        s += 7


def name(b, i):
    n, i = uleb(b, i)
    return b[i:i + n].decode("utf-8", "replace"), i + n


def walk(data):
    imports, exports = [], []
    i = 8
    while i < len(data):
        sid = data[i]
        i += 1
        size, i = uleb(data, i)
        end = i + size
        if sid == 2:
            count, j = uleb(data, i)
            for _ in range(count):
                m, j = name(data, j)
                f, j = name(data, j)
                k = data[j]
                j += 1
                imports.append((KINDS.get(k, str(k)), m, f))
                if k == 0:
                    _, j = uleb(data, j)
                elif k == 3:
                    j += 2
                else:
                    flags, j = uleb(data, j)
                    _, j = uleb(data, j)
                    if flags & 1:
                        _, j = uleb(data, j)
        elif sid == 7:
            count, j = uleb(data, i)
            for _ in range(count):
                n, j = name(data, j)
                k = data[j]
                j += 1
                _, j = uleb(data, j)
                exports.append((KINDS.get(k, str(k)), n))
        i = end
    return imports, exports


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]

    if path.endswith(".odmod"):
        with zipfile.ZipFile(path) as z:
            data = z.read("mod.wasm")
    else:
        with open(path, "rb") as f:
            data = f.read()

    if data[:4] != b"\0asm":
        print(f"{path}: not a WebAssembly binary")
        return 1

    imports, exports = walk(data)
    # wasi_snapshot_preview1 is legitimate IF the mod declares the WasiStub
    # capability -- that is how an interpreter-in-a-mod (Python, Ruby, Lua,
    # Java) boots. Everything else outside gearbox: is refused.
    ALLOWED = ("gearbox:", "wasi_snapshot_preview1")
    bad = [x for x in imports if not x[1].startswith(ALLOWED)]
    wasi = [x for x in imports if x[1] == "wasi_snapshot_preview1"]

    print(f"imports ({len(imports)}):")
    for kind, m, f in imports:
        flag = ("  <-- needs the WasiStub capability" if m == "wasi_snapshot_preview1"
                else "  <-- NOT PART OF THE GEARBOX ABI" if not m.startswith("gearbox:") else "")
        print(f"  {kind:7} {m}.{f}{flag}")

    print(f"\nexports ({len(exports)}):")
    for kind, n in exports:
        print(f"  {kind:7} {n}")

    if bad:
        print(f"\n{len(bad)} import(s) outside the Gearbox ABI. The host will refuse")
        print("this mod. See docs/gearbox-troubleshooting.md section 7.")
        return 1

    if not any(n == "mod_load" for _, n in exports):
        print("\nno mod_load export — the host cannot start this mod.")
        return 1

    if wasi:
        print(f"\n{len(wasi)} WASI import(s): your MANIFEST.json must list \"WasiStub\"")
        print("in \"modules\", and the user must grant it. Fewer is better --")
        print("each one is surface the sandbox would rather not have.")
    print("\nOK: all imports are permitted, mod_load is exported.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
