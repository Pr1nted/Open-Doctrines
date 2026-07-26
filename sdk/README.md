# Gearbox SDK

Everything needed to write a mod for OpenDoctrines.

**Start here:** [docs/gearbox-sdk.md](../docs/gearbox-sdk.md) — the guide.
**Reference:** [docs/gearbox-abi.md](../docs/gearbox-abi.md) — every import and export.
**When it breaks:** [docs/gearbox-troubleshooting.md](../docs/gearbox-troubleshooting.md) — every rejection message and its fix.
**Your language:** [docs/gearbox-languages.md](../docs/gearbox-languages.md) — what works today.

## The ABI is the SDK

A mod is a WebAssembly module that imports 18 host functions and exports up to
5 of its own. That is the entire contract. Each "SDK" here is a transcription of
it into one language's syntax — a few hundred lines, no runtime, no framework.

The transcriptions are kept honest by a single machine-readable definition:

```
host capability table  <--[ModAbiTest]-->  sdk/abi.json  --[gen_abi_docs.py]-->  docs/gearbox-abi.md
                                                 |
                                                 +-- every sdk/<language>/ binding
```

[`abi.json`](abi.json) is the source of truth. `tests/mod_abi_test.cpp` fails the
build if the host and `abi.json` disagree in either direction — a host function
missing from `abi.json` would be absent from every SDK, and an `abi.json` entry
the host lacks would fail to link for every modder. Neither can slip through.

## Layout

```
sdk/
├── abi.json                  the ABI, machine-readable — source of truth
├── gearbox.h                 C and C++ binding (extern "C")
├── examples/hello-panel/     the reference example, in C
├── cpp/                      C++ binding notes + example
├── assemblyscript/           AssemblyScript binding + example
├── rust/                     Rust binding + example
├── zig/                      Zig binding + example
├── go/                       TinyGo binding + example
└── wat/                      raw WebAssembly text — the wire-level reference
```

Every language directory has a `README.md` stating plainly whether the binding
was actually compiled, a `build.sh`, and a hello-world mod that does the same
thing in each language: a panel showing the turn number, the country count, and
one country's name, provinces and treasury.

**All seven shipped SDKs are verified** — C, C++, Rust, Zig, TinyGo,
AssemblyScript and raw WAT have each been compiled, packed, loaded by the host,
and driven through their full draw path with output compared across languages.

C# and Swift were dropped; see [gearbox-languages.md](../docs/gearbox-languages.md).

Testing them was worth it: TinyGo exposed a real host bug (`_initialize` was
never called, so the Go runtime started uninitialised and trapped), which C and
C++ had masked. See [gearbox-languages.md](../docs/gearbox-languages.md).

`tools/check_bindings.py` cross-checks every binding against `abi.json` for
invalid import module names and missing functions. It is a lint, not a compiler.

## Quick start

```bash
# C
sdk/examples/hello-panel/build.sh

# C++
sdk/cpp/build.sh

# AssemblyScript (installs its toolchain on first run)
sdk/assemblyscript/build.sh
```

Each produces a `.odmod`. Check it before shipping:

```bash
cmake --build build --target OdmodCheck
build/odmod-check sdk/cpp/hello-panel-cpp.odmod
```

Then load it from the game's **Mod Menu → Add from computer**, or drop the file
onto the window.

## Tools

| Tool | What it does |
|---|---|
| `build/odmod-check <file>` | Validates an archive under the same limits the game uses, then instantiates it and calls `mod_load`. `--revoke UI` rehearses a user turning a capability off; `--no-run` checks the archive only. |
| `tools/pack_odmod.sh <dir> [out]` | Packs a directory into a `.odmod`, with `MANIFEST.json` first as the loader requires. |
| `tools/wasm_imports.py <file>` | Lists a module's imports and exports and flags anything outside the ABI. The first thing to run when a mod is refused. |
| `tools/gen_abi_docs.py` | Regenerates `docs/gearbox-abi.md` from `abi.json`. |

## What a mod can do today

Read the world, draw a panel, run code on turn boundaries, read its own bundled
assets. **It cannot change the game state** — `GameState.Write`, `Neural`,
`Diplomacy`, `Map` and `Storage` are declared in the ABI but not implemented,
and a mod requesting one is refused at load rather than running without it.

See [docs/modding.md](../docs/modding.md) for the container format, the security
model, and what the host guarantees.
