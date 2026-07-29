# Gearbox SDK — Go (TinyGo)

**Status: verified — built with TinyGo 0.41.1, packed, and loaded by the host. 3,089 bytes.**

TinyGo needs `wasm-opt` on PATH or `WASMOPT` set; see Build below.

Getting this to load required a host fix: TinyGo emits `_initialize` (the WASI
reactor convention) and the host was not calling it, so `mod_load` trapped with
`Exception: unreachable`. Fixed in `src/mods/ModRuntime.cpp` and pinned by
`tests/mod_runtime_test.cpp`.

Built with TinyGo 0.41.1 on macOS/arm64. Also checked with the
standard Go toolchain that was on the machine:

- `gofmt -l` is clean and `GOOS=wasip1 GOARCH=wasm go vet ./...` passes, so the
  package parses and type-checks.
- `GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared` produces a module whose
  import section contains `gearbox:core::log`, `gearbox:ui::button` and the
  rest, with exactly the module and field names `abi.json` specifies. The
  `//go:wasmimport gearbox:core log` spelling — colon in the module name and
  all — is therefore right.
- That same module also imports ten `wasi_snapshot_preview1` functions and
  weighs 1.87 MB, which is precisely why you must not build it that way. See
  [Standard Go will not work](#standard-go-will-not-work).

Nothing about the TinyGo path — flag names, target behaviour, output size,
runtime initialisation — has been observed. Expect to fix things on first build.

```
sdk/go/
├── gearbox.go                     the binding: 18 imports, 5 exports
├── go.mod
├── README.md
└── examples/hello-panel/
    ├── main.go                    port of sdk/examples/hello-panel/mod.c
    ├── MANIFEST.json
    └── build.sh
```

## Standard Go will not work

Not "is discouraged" — will not load. There are two GOOS values that emit wasm
and neither produces something this host accepts.

**`GOOS=js`** expects `wasm_exec.js`, the runtime shim that services the
`gojs` import module — scheduling, timers, the syscall bridge. The Gearbox host
provides `gearbox:*` and nothing else, so those imports have nothing to link
against and the mod is refused with a diagnostic naming the missing module.

**`GOOS=wasip1`** emits WASI. Measured on the example in this directory, the
Go runtime pulls in `fd_write`, `proc_exit`, `clock_time_get`, `random_get`,
`poll_oneoff`, `sched_yield`, `args_get`, `args_sizes_get`, `environ_get` and
`environ_sizes_get`. The host builds WAMR with `WAMR_BUILD_LIBC_WASI=0`, so
those imports are not registered at all. That is the sandbox — the reason a mod
cannot open a file is that the capability does not exist in its instance, not
that something checks — and there is no host flag that loosens it. A mod built
this way is refused at load with a diagnostic naming `wasi_snapshot_preview1`.

So: **TinyGo**, `-target=wasm-unknown`, which is the bare wasm32 target with no
WASI and no JS.

## Build

```bash
sdk/go/examples/hello-panel/build.sh      # -> hello-panel-go.odmod
```

Set `TINYGO=` if `tinygo` is not on your `PATH`. The script ends by calling
`tools/pack_odmod.sh`, which needs `python3`. What it runs, unwrapped:

```bash
cd sdk/go
tinygo build -target=wasm-unknown -no-debug -scheduler=none -panic=trap \
    -o examples/hello-panel/mod.wasm ./examples/hello-panel
```

| Flag | Why |
|---|---|
| `-target=wasm-unknown` | Bare wasm32. No WASI, no JS glue — the only target whose import list the host will accept. |
| `-no-debug` | Drops DWARF. A mod ships as a binary and the sections are dead weight inside the archive limits. |
| `-scheduler=none` | No goroutines, no coroutine trampolines. A hook runs to completion or burns its fuel; there is nothing to schedule against. |
| `-panic=trap` | A Go panic becomes an `unreachable`, which the host reports as a trap and disables the mod over. `-panic=print` wants somewhere to print, and on `wasm-unknown` there is nowhere. |

Then check it before you ship it — this runs the same loader the game does:

```bash
odmod-check sdk/go/examples/hello-panel/hello-panel-go.odmod
odmod-check --revoke UI sdk/go/examples/hello-panel/hello-panel-go.odmod
```

## Which TinyGo version, and why it matters

**This code targets TinyGo 0.35.0 or newer**, because that is the release that
added `//go:wasmexport`. It matches the directive Go itself adopted in 1.24, and
it is what `examples/hello-panel/main.go` uses:

```go
//go:wasmexport mod_load
func modLoad() int32 { ... }
```

**On older TinyGo, use `//export` instead**, which is what the toolchain
supported before the `//go:wasmexport` directive existed:

```go
//export mod_load
func modLoad() int32 { ... }
```

Both spell the same thing: keep the function in the module under the name the
host looks up. The export names are fixed by the ABI (`mod_load`, `mod_unload`,
`mod_pre_turn`, `mod_post_turn`, `mod_draw_panel`); the Go function names are
yours.

This is the most version-sensitive thing in the directory and the version on
this machine could not be checked, so treat 0.35.0 as "what the code was written
for", not as a verified floor. If `tinygo build` rejects the directive, that is
the switch to flip. If it instead complains about building a `package main`
without a `_start`, try adding `-buildmode=c-shared`.

`//go:wasmimport` is older and has been stable for longer, but it too has moved:
which parameter types are legal has been tightened over releases. The binding
sticks to `uint32`, `int32`, `int64`, `uint64`, `float64` and `unsafe.Pointer`,
which is the intersection that has been accepted throughout.

`unsafe.StringData` and `unsafe.SliceData` need Go 1.20 semantics, hence the
`go 1.21` line in `go.mod`.

## Known risk: runtime initialisation

A Gearbox mod has no entry point. The host instantiates the module and then
calls `mod_load` directly; it never calls `_start` or `_initialize`, because
`abi.json` does not define one.

The standard-Go build measured above exports `_initialize` alongside the three
`mod_*` functions. That is the WASI reactor convention: package-level
initialisers and heap setup run there, and everything else assumes it already
ran. **If TinyGo does the same on `wasm-unknown`, a Go mod's runtime is never
initialised**, and anything depending on that — the GC heap, any package
variable with a non-constant initialiser — is in an undefined state by the time
your first hook runs. This has not been observed either way for TinyGo and is
the single thing most likely to bite.

Check yours:

```bash
wasm-objdump -x mod.wasm | grep -iE '_initialize|_start'
```

`build.sh` prints the import and export sections for you when `wasm-objdump` is
available.

What to do about it:

1. **Write the mod so it does not care.** Every package-level variable in the
   example is its zero value, and nothing allocates after `mod_load`. Zero
   values live in `.bss` and are established by instantiation itself, not by
   code, so there is nothing to initialise. This is the mitigation the example
   ships with, and it is good practice regardless — see the next section.
2. If your mod genuinely needs the Go runtime up, the clean fix is one line in
   the host: call `_initialize` after instantiation when the module exports it.
   That is deliberately out of scope for this directory.

Zig has no equivalent problem, which is worth knowing when choosing: a
freestanding Zig module has no runtime to start.

## Heap, GC and fuel

TinyGo ships a runtime. That is the trade against C or Zig, and it is not small:

| | Typical hello-world |
|---|---|
| C / Zig, freestanding | ~2 KB |
| TinyGo | ~50 KB (per `docs/gearbox-sdk.md`; not measured here) |
| Standard Go, wasip1 | 1.87 MB, and refused at load anyway (measured) |

Size is the least of it. The costs that actually bite:

**Fuel.** You get `limits.fuelPerTurn` instructions per hook call, and TinyGo's
generated code does more per source statement than C's — bounds checks,
interface dispatch, nil checks, GC bookkeeping. The example's manifest asks for
400,000 against the C version's 200,000 for exactly the same behaviour. That is
a guess, not a measurement; if a hook is terminated mid-call, this is the first
number to raise.

**Allocation.** `mod_draw_panel` runs once per frame per visible panel. Anything
that allocates there produces garbage at frame rate inside a fuel budget, and
you pay twice — once to allocate, once when the collector scans. The example
allocates nothing after `mod_load`: `gLine` and `gName` are package-level
arrays, reused every frame, and the binding gives you a non-allocating form of
every call that returns variable-length data:

```go
var buf [64]byte
name := gearbox.CountryNameInto(c, buf[:])   // []byte, no allocation
s := gearbox.CountryName(c)                  // string, allocates twice
```

Use the `Into` forms in hooks and the convenient ones in `mod_load`.

**Collector choice.** `build.sh` leaves `-gc` at the target's default. If you do
allocate: `-gc=leaking` is the fastest and never collects, so the heap only
grows until it hits `limits.memoryPages` and traps the mod; `-gc=conservative`
collects but scans, and the scan is charged to whichever hook happened to
trigger it, which makes fuel use spiky. Neither is a good answer for a mod that
allocates per frame. Not allocating per frame is.

## Using the binding

```go
import gearbox "github.com/Pr1nted/Open-Doctrines/sdk/go"
```

The example is inside the same module as the package, so this resolves locally
and no network access is involved. For a mod of your own, either vendor
`gearbox.go` into your module or add a `replace` directive pointing at this
directory — there is no published module to `go get`.

Everything outside the `raw*` functions takes Go strings and slices; the
`(ptr, len)` split of the wire ABI stops at that line. Empty strings and slices
are passed as a nil pointer with length 0, because the pointer behind a
zero-length Go slice is not guaranteed to be anything and the host
bounds-checks what it is handed.

**You only import what you call.** The measured build emitted twelve
`gearbox:*` imports out of the eighteen the binding declares — `abort`,
`fuel_budget`, `province_population`, `province_owner` and both `gearbox:assets`
functions were dead-stripped because the example never calls them. This matters:
an import that survives must be declared in `MANIFEST.json` and granted by the
user, or the load is refused. If `odmod-check` complains about a capability you
thought you were not using, `wasm-objdump -x` will show you which call kept it
alive.

**Handles die with the hook.** `Country` and `Province` are opaque uint32s valid
only for the call that produced them. Caching one across turns gives you a stale
or reassigned entity. `Invalid` is `0xFFFFFFFF`, not zero — a zero handle is a
real one.

**Two-call sizing.** `CountryNameInto` and `AssetReadInto` return a sub-slice of
the buffer you passed, already clipped to what was written. `CountryNameLen` and
`AssetSize` are the sizing half. Truncation is not an error.

**Float conversion is undefined out of range.** Converting a float64 outside
uint64's range is undefined in Go — no panic, just a wrong number. The example
clamps the treasury before converting, written as `!(t < 1e18)` so NaN, which
loses every comparison, clamps too.

If something here disagrees with `sdk/abi.json`, `abi.json` wins and this is the
bug. See [`docs/gearbox-sdk.md`](../../docs/gearbox-sdk.md) for the ABI itself —
capabilities, fuel, the headless rule, and the manifest.
