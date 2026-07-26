# Gearbox SDK — Zig

**Status: verified — built with Zig 0.16.0, packed, and loaded by the host. 1,836 bytes.**

Built and loaded with Zig 0.16.0 on macOS/arm64. Treat every command below as
intent, not as something that has been observed to work, and expect to fix
small things on first build. The *ABI* it binds to is not in doubt —
[`sdk/abi.json`](../abi.json) is machine-checked against the host's own function
table by `tests/mod_abi_test.cpp` — but the Zig spelling of it is untested.

```
sdk/zig/
├── gearbox.zig                    the binding: 18 imports, 5 exports
├── build.zig                      build-system path (version-sensitive)
├── README.md
└── examples/hello-panel/
    ├── main.zig                   port of sdk/examples/hello-panel/mod.c
    ├── MANIFEST.json
    └── build.sh                   plain `zig build-exe` path, recommended
```

## Why Zig

A freestanding Zig mod is the same size as the C one — a couple of kilobytes —
with no libc, no allocator and no startup code, and you get slices, so the
`(ptr, len)` pairs the ABI is built out of stop being something you carry by
hand. That is the whole pitch. If you want the C behaviour with fewer chances
to pass the wrong length, this is it.

## Build

```bash
sdk/zig/examples/hello-panel/build.sh      # -> hello-panel-zig.odmod
```

That is the recommended path: it drives `zig build-exe` directly, and compiler
flags have been far more stable across Zig releases than the build system API.
Set `ZIG=` if `zig` is not on your `PATH`. The script ends by calling
`tools/pack_odmod.sh`, which needs `zip`.

What it runs, unwrapped:

```bash
zig build-exe \
    -target wasm32-freestanding \
    -O ReleaseSmall \
    -fno-entry \
    --export=mod_load \
    --export=mod_unload \
    --export=mod_draw_panel \
    -femit-bin=mod.wasm \
    --dep gearbox \
    -Mroot=main.zig \
    -Mgearbox=../../gearbox.zig
```

The build-system path exists too, and additionally packs:

```bash
cd sdk/zig && zig build pack
```

Check the result before you ship it — this runs the same loader the game does:

```bash
odmod-check sdk/zig/examples/hello-panel/hello-panel-zig.odmod
odmod-check --revoke UI sdk/zig/examples/hello-panel/hello-panel-zig.odmod
```

## The flags, and why each one is there

**`-target wasm32-freestanding`** — not `wasm32-wasi`. The host builds WAMR with
`WAMR_BUILD_LIBC_WASI=0`, so WASI's imports are not linked at all. A module that
imports `fd_write` is refused at load with a diagnostic naming the module. This
is the sandbox, not an oversight, and there is no flag on the host side to
loosen it.

**`-fno-entry`** — there is no `_start`. The host calls `mod_load` and the other
`mod_*` exports directly and never calls an entry point. Without this the linker
looks for a `main` you do not have.

**`--export=<name>`** — names the symbols to keep. Whether `export fn` on its
own survives into a wasm-freestanding executable has varied by Zig release, so
name them explicitly. It is also stricter than `-rdynamic`: misspell an export
and you get a link error now instead of "mod does not export mod_draw_panel"
from the game later. Export exactly the hooks you implement, no more —
`--export=mod_pre_turn` for a function that does not exist will not link.

**`-O ReleaseSmall`** — a mod runs in an interpreter under a fuel budget, so
size and predictability matter more than unrolled speed. It also drops the
safety checks that would otherwise pull panic formatting into a module that is
otherwise 2 KB.

**`--dep gearbox -Mroot=... -Mgearbox=...`** — passes `gearbox.zig` as a named
module. It cannot be a relative `@import("../../gearbox.zig")`: Zig refuses to
import a file outside the root module's directory, and the SDK lives two levels
up from the example. **For your own mod, just copy `gearbox.zig` next to your
source and `@import("gearbox.zig")`** — it is one self-contained file with no
dependencies, and that sidesteps the module CLI entirely.

**Nothing about memory.** wasm-ld should export the linear memory as `memory`
unless you ask it not to, and the web host needs that — it reads
`instance.exports.memory` to copy strings in and out. So do not pass
`--import-memory`: the mod owns its memory, the host does not hand it one. Since
if your Zig version differs from 0.16.0, confirm it once:

```bash
wasm-objdump -x mod.wasm | grep -E 'memory|mod_'
```

which also shows you the import list — exactly what the host checks against your
granted capabilities.

**No `--allow-undefined`.** The C build needs `-Wl,--allow-undefined` because a
plain `extern` declaration in C is just an undefined symbol. Zig's
`extern "gearbox:core" fn log(...)` carries the module name in the declaration,
so it becomes a proper WASM import and there is nothing undefined to allow.

## Gotchas

**Freestanding means freestanding.** No libc, no OS, no allocator. `std` is
importable and plenty of it is pure computation — `std.mem`, `std.math`,
`std.fmt.bufPrint` — but anything touching files, time, threads or
`std.heap.page_allocator` has nothing underneath it here. If you need dynamic
memory, carve a `[N]u8` out of your own module and hand it to
`std.heap.FixedBufferAllocator`; your memory ceiling is `limits.memoryPages` in
the manifest, so a growing allocator will trap the mod rather than misbehave
quietly. The example needs none of this.

**There is no runtime to start.** A Gearbox mod has no entry point: the host
instantiates the module and calls `mod_load` directly, and `abi.json` defines no
`_start` or `_initialize` for it to call first. A freestanding Zig module does
not need one — globals with constant initialisers live in the data or bss
section and are established by instantiation itself, with no code involved. Keep
it that way: if you find yourself wanting work done "before `mod_load`", there
is nowhere to put it. (This is the one place Zig is meaningfully simpler than
the Go SDK next door, where the language ships a runtime that expects to be
initialised.)

**Slices in, `(ptr, len)` on the wire.** Everything in `gearbox.zig` outside the
`raw` namespace takes and returns slices. `raw.*` is the literal wire ABI, one
declaration per entry in `abi.json`, and is there for when you need it — not
because you should normally call it.

**Empty slices are passed as a null pointer.** The host treats a zero length as
the empty string without touching memory either way, but `""`.ptr in Zig is a
dangling-though-aligned pointer, and handing that to a host that bounds-checks
pointers is a bad habit to build.

**Two-call sizing.** `countryName` and `assetRead` return *a slice of the buffer
you passed*, already clipped to what was written. `countryNameLen` / `assetSize`
are the sizing half. A name longer than your buffer is truncated, which is not
an error:

```zig
var buf: [64]u8 = undefined;
const name = gb.countryName(c, &buf);   // []u8, never longer than buf
```

**You only import what you call.** Zig analyses declarations lazily and wasm-ld
keeps only referenced undefined symbols, so `@import`ing `gearbox.zig` does not
put `gearbox:assets` in your module. That matters: an import you never use would
still have to be declared in `MANIFEST.json` and granted by the user, or the
load is refused.

**Handles die with the hook.** `Country` and `Province` are opaque u32s valid
only for the call that produced them. Caching one across turns gives you a stale
or reassigned entity. `INVALID` is `0xFFFFFFFF`, not zero — a zero handle is a
real one.

**`@intFromFloat` traps on out-of-range input** in safety-checked builds, where
C would silently give you garbage. The example clamps the treasury before
converting, which also handles NaN, because failing every comparison means
`!(t < 1e18)` is true. Worth copying: a mod disabled over a cosmetic label is a
bad trade.

**`error` is a keyword,** so `LogLevel.ERROR` from the ABI is spelled
`LogLevel.err`. The numeric value is 3 either way.

**Panic handlers.** The example defines none and relies on Zig's default, which
in a freestanding `ReleaseSmall` build is a trap. If you want your own, note
that its required signature changed in 0.14 (`pub const panic =
std.debug.FullPanic(...)` replacing the old free function). A trap surfaces to
the user as the mod being disabled with a diagnostic, which is survivable but
worse than returning non-zero from `mod_load`.

## Version sensitivity

Zig's language and build API both move, and this file cannot tell you which
version you have. Written against **Zig 0.15.x**; the parts most likely to need
adjusting, roughly in order:

| Thing | Note |
|---|---|
| `build.zig` | `addExecutable(.{ .root_module = ... })` is 0.15-era. 0.14 and older take `.root_source_file`/`.target`/`.optimize` inline — the alternative is in a comment in `build.zig`. This is why `build.sh` is the recommended path. |
| `--dep` / `-M` module CLI | Introduced in 0.12. On older Zig use `--pkg-begin gearbox ... --pkg-end`, or just copy `gearbox.zig` next to your source. |
| `@intCast`, `@intFromEnum`, `@intFromFloat` | Single-argument forms, 0.12+. Pre-0.11 spellings took the type as the first argument (`@intCast(u32, x)`) and had different names. |
| `@memcpy` | Takes two slices of equal length as of 0.11. |
| `mod.entry = .disabled` | 0.12+. Older releases used `-fno-entry` only. |
| `panic` handler signature | Changed in 0.14. The example avoids the question by not defining one. |

If something here disagrees with `sdk/abi.json`, `abi.json` wins and this is the
bug. See [`docs/gearbox-sdk.md`](../../docs/gearbox-sdk.md) for the ABI itself —
capabilities, fuel, the headless rule, and the manifest.
