# Gearbox in raw WebAssembly text (WAT)

**Status: verified — assembled with wabt 1.0.41 (`wat2wasm`), packed, and loaded by the host. 390 bytes.**

`wat2wasm` and `wasm-tools` are both absent on the machine where this was
written, so `hello.wat` has never been assembled, packed, or loaded. It is
transcribed from [`sdk/abi.json`](../abi.json) and from the host's own loader
(`src/mods/ModRuntime.cpp`), not from a run.

---

## What this is for

Two audiences.

**You want the smallest possible mod.** A hand-written module has no runtime,
no allocator, no startup code and no linker-invented imports. Counting the
sections by hand, `hello.wat` should assemble to well under a kilobyte —
unmeasured, since nothing here could assemble it. Nothing else gets close.

**You are writing a binding for a language the SDK does not cover.** This is
the reference. Everything a binding must get right is visible in
[`hello.wat`](hello.wat) with nothing generating it — the import declarations,
the export declarations, and the fact that a string is an offset and a byte
count. Read that file top to bottom; it is annotated for exactly this purpose
and is longer in comments than in code.

The mod itself is trivial: it logs a line on load, registers a panel, and draws
two lines of text in it. It uses three imports and two exports, which is the
minimum that demonstrates both directions of the ABI.

## Build

```bash
./build.sh          # produces hello-panel-wat.odmod
```

Needs one of:

```bash
brew install wabt           # gives wat2wasm
cargo install wasm-tools    # gives wasm-tools parse
```

Then check it the way the game would:

```bash
odmod-check hello-panel-wat.odmod
odmod-check hello-panel-wat.odmod --revoke UI    # rehearse a revoked capability
```

## Writing a binding for another language

The requirement is small. Your language needs to be able to:

1. **Declare a function import from a named module.** The module name is the
   capability string (`gearbox:core`, `gearbox:ui`, `gearbox:gamestate.read`,
   `gearbox:assets`) and the field name is the function name from
   [`abi.json`](../abi.json). Both are length-prefixed UTF-8 in the binary; the
   colon is an ordinary byte.
2. **Export a function under an exact name.** `mod_load` must come out as
   `mod_load` — no mangling, no leading underscore. Only `mod_load` is
   required.
3. **Put bytes in linear memory and tell you their offset.** That is all a
   string is.

If it can do those three things, it can write a Gearbox mod. Everything else —
allocators, string types, garbage collection — is your language's business and
the host never sees it.

### Signature notation in abi.json

`abi.json` gives each function a `signature` string in WAMR's notation, which
is what the host registers natives with (`src/mods/ModHost.cpp`). Parameters
inside the parentheses, result after:

| Letter | Wasm type | Appears as |
|---|---|---|
| `i` | `i32` | every pointer, every length, every handle, every colour, `bool` |
| `I` | `i64` | `fuel_budget` result, `province_population` result |
| `f` | `f32` | not used by Gearbox 1.0 |
| `F` | `f64` | `country_treasury` result |

So `(iii)` is three `i32` parameters and no result; `()I` is no parameters and
an `i64` result; `(i)F` is one `i32` parameter and an `f64` result.

**Pointers and lengths are both `i32`.** `abi.json` distinguishes them in the
`role` field only to document intent — on the wire they are indistinguishable,
and the host bounds-checks every pointer against your memory on every use. An
out-of-bounds `(ptr, len)` is refused and logged against your mod, not read.

### The five things that actually bite

**Import exactly the gearbox modules and nothing else.** Every import your
module declares is checked at instantiation against the host's table and
against your granted capabilities. Anything unrecognised — a WASI function, an
`env` symbol your linker emitted for an undefined reference, a capability the
user revoked — refuses the load with a diagnostic naming it. There is no
runtime fallback and no way to discover you were denied, because you never
instantiate. This is what makes higher-level toolchains hard: they import a
libc whether you asked for one or not. Check what your toolchain actually
emitted before you blame anything else:

```bash
python3 ../../tools/wasm_imports.py mod.wasm
```

**Export your memory as `memory`.** The desktop backend (WAMR) reaches your
linear memory through its own instance handle, but the web backend reads
`instance.exports.memory` and silently fails every string read without it. The
symptom is a mod that works on desktop and draws nothing in a browser.

**Do not rely on static constructors.** The host calls your exports and nothing
else. WAMR runs `__wasm_call_ctors` after instantiation; the browser backend
runs `new WebAssembly.Instance()` and calls nothing. A wasm `start` function
*is* run by both, so that is the portable place for initialisation — but the
simplest answer is to do your setup in `mod_load`, which is guaranteed to run
first on both.

**Assume MVP plus bulk memory.** The desktop host is WAMR 2.4.5's fast
interpreter with `WAMR_BUILD_BULK_MEMORY=1` and `WAMR_BUILD_SIMD=0`
(`CMakeLists.txt`), so a module containing `v128` will not load on desktop even
though a browser would take it. Anything beyond that — reference types, tail
calls, exception handling, GC — is whatever WAMR 2.4.5 enables by default; the
repo turns none of them on explicitly, so do not count on them. Browsers are
far more permissive than the desktop host, which makes the desktop host the one
to target.

**Keep stack buffers small.** The host gives WAMR a 64 KiB execution stack
(`ModRuntime.cpp`), and most linkers default the linear-memory stack to 64 KiB
as well. A 1 MiB scratch buffer on the stack is not a thing you can have.

### Wire format cheat sheet

Enough to hand-emit a module if you have to. Sections are `id, size, payload`;
all names are length-prefixed UTF-8; all lengths are LEB128.

```
section 1  (type)     function signatures, referenced by index
section 2  (import)   module name, field name, kind 0x00 = func, typeidx
section 3  (function) typeidx per defined function
section 5  (memory)   limits: 0x00 min, or 0x01 min max
section 6  (global)   type, mutability, init expr
section 7  (export)   name, kind (0x00 func, 0x02 memory), index
section 8  (start)    funcidx run at instantiation
section 10 (code)     body per defined function
section 11 (data)     active: 0x00, offset expr, byte vector
```

One import entry, in full:

```
0c 67 65 61 72 62 6f 78 3a 63 6f 72 65    len 12, "gearbox:core"
03 6c 6f 67                               len  3, "log"
00 00                                     kind func, type index 0
```

One export entry:

```
08 6d 6f 64 5f 6c 6f 61 64                len 8, "mod_load"
00 03                                     kind func, function index 3
```

**Imported functions take the low function indices**, in declaration order,
before anything you define. With three imports, your first defined function is
index 3. This is the classic hand-written-wasm bug; in WAT, use symbolic names
(`$log`) and the assembler resolves indices for you.

## Manifest

Same rules as any other mod — see [`docs/modding.md`](../../docs/modding.md).
Two that catch people:

- `id` is lowercase, must contain a dot, and is immutable. It is the trust
  pinning key and the `Storage` namespace.
- `MANIFEST.json` must be the archive's **first** entry.
  `tools/pack_odmod.sh` is what handles that; use it rather than zipping by
  hand.

This mod declares `memoryPages: 2` because `hello.wat` declares one page and
never grows, and `fuelPerTurn: 50000` because drawing two strings costs a few
dozen instructions. Both are ceilings the host clamps to its own maxima, not
reservations — asking for more than you need is not free, it is just less
convincing to a user reading your manifest.
