# Gearbox SDK for Rust — v1.0

**Status: verified — built with rustc 1.97.1 (wasm32-unknown-unknown), packed, and loaded by the host. 3,277 bytes.**

The commands below were run on macOS/arm64 with rustc 1.97.1.
The binding is a transcription of [`sdk/abi.json`](../abi.json), cross-checked
against [`sdk/gearbox.h`](../gearbox.h) and the host's own import table in
`src/mods/ModHost.cpp`. If something does not build on your toolchain,
`abi.json` is still the source of truth and this crate is the bug.

---

## What this is

A `no_std` binding over the Gearbox mod ABI: all 18 imports, the five exports,
the env struct, both enums. No allocator, no runtime, no dependencies. The safe
wrappers are one-liners over the raw imports and inline away under LTO, so what
ships is your code and very little else.

Needs a reasonably current toolchain. By inspection — not measurement — the
oldest thing used here is `strip` in `[profile]`, which arrived in Rust 1.59;
const generics, `const`-context `assert!` and `resolver = "2"` are all older
than that.

```
sdk/rust/
├── Cargo.toml                    workspace root + the `gearbox` library
├── .cargo/config.toml            defaults every cargo command here to WASM
├── src/
│   ├── lib.rs                    safe API, Env, handles, ModCell, the macro
│   ├── ffi.rs                    the 18 raw imports, one line each
│   └── fmt.rs                    Buf<N>: formatting without an allocator
├── examples/hello_panel/         a complete mod (mirrors ../examples/hello-panel)
│   ├── Cargo.toml                the cdylib
│   ├── MANIFEST.json
│   └── src/lib.rs
└── build.sh                      cargo build + pack into .odmod
```

## Toolchain

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh   # if you have no rustup
rustup target add wasm32-unknown-unknown
```

`wasm32-unknown-unknown` is a separate download from the toolchain and is not
installed by default. Skipping it produces a link failure several screens long
rather than a clear message, so `build.sh` checks for it up front.

`.cargo/config.toml` sets `build.target` so plain `cargo build` and
`cargo check` from `sdk/rust` are already WASM. This matters more than it
looks: the crate does not compile for a host triple — `core::arch::wasm32` does
not exist there and the `gearbox:*` imports have nothing to link against — so a
`cargo check` that quietly picked your host target would fail for reasons that
have nothing to do with your code. Cargo reads that file from the *working
directory* upward, not from `--manifest-path`, so `cd sdk/rust` first or pass
`--target` yourself.

Use `wasm32-unknown-unknown`, **not** `wasm32-wasi`. WASI expects a set of
`wasi_snapshot_preview1` imports that the host does not provide — no
filesystem, no clock, no stdout — so a WASI module fails to link at load with a
diagnostic about missing imports. That is the sandbox working as designed.

## Build

```bash
cd sdk/rust
./build.sh                        # -> examples/hello_panel/hello-panel-rust.odmod
```

Or by hand, from `sdk/rust`:

```bash
cargo build --release --target wasm32-unknown-unknown -p hello-panel
cp target/wasm32-unknown-unknown/release/hello_panel.wasm examples/hello_panel/mod.wasm
../../tools/pack_odmod.sh examples/hello_panel examples/hello_panel/hello-panel-rust.odmod
```

`pack_odmod.sh` takes a **directory** containing `MANIFEST.json` and `mod.wasm`,
not a list of files. It exists because `MANIFEST.json` must be the archive's
first entry — that is what lets the loader validate your declared limits before
it decompresses anything else — and most zip tools will not give you that
ordering by accident. If you package by hand and see `ManifestNotFirst`, this
is why.

Then load it from the mod menu. To check it the way the game will, first:

```bash
build/odmod-check examples/hello_panel/hello-panel-rust.odmod
build/odmod-check examples/hello_panel/hello-panel-rust.odmod --revoke UI
```

`build.sh` runs both if `build/odmod-check` exists. The second one rehearses a
user turning UI off in **Advanced**, which your mod must survive.

## Starting your own mod

Copy `examples/hello_panel/`, then:

1. Change `id` in `MANIFEST.json`. It is immutable, lowercase, must contain a
   dot, and is both your `Storage` namespace and your trust-pinning key.
   Changing it later orphans your users' settings.
2. Change `name` in `Cargo.toml` and the `[lib] name`, and point
   `gearbox = { path = ... }` at `sdk/rust`.
3. Add yourself to `members` in `sdk/rust/Cargo.toml`, or make your own
   workspace and copy the `[profile.release]` block — see below for why that
   block matters.

## The gotchas that actually bite in Rust

**`#![no_std]`.** There is no `std` on this target that means anything: no
filesystem, no threads, no clock, no stdout. `std` would link but every useful
part of it would be a stub, so the crate is `no_std` and so is your mod. In
practice you lose `String`, `Vec`, `format!`, `HashMap` and `f64::abs`, and you
keep everything in `core`. `fmt::Buf<N>` covers the case that actually comes
up, which is building a string to draw.

**You must define a panic handler.** A `no_std` artifact needs exactly one
`#[panic_handler]` and will not link without it. The `gearbox` crate cannot
provide it: `#[panic_handler]` is a global item, and one inside an `rlib` would
fix the choice for every mod that ever links the crate. So it is a macro you
invoke once at the top of your own crate:

```rust
gearbox::abort_on_panic!("my-mod panicked");
```

That routes panics to `gearbox_abort`, which disables the mod and shows the
message. Better than the alternative, which is a bare WASM trap with nothing
attached to it. The message is a fixed string on purpose — rendering the real
panic message pulls in `core::fmt`'s argument machinery for a path that only
runs when your mod is already broken.

**Better still: have no panics.** Every `slice[i]`, `unwrap`, and debug-build
arithmetic overflow is a branch into the panic machinery, and each one is
binary size for a path you do not want taken. `get(..).unwrap_or(&[])` instead
of `[..]` is not paranoia here, it is what keeps the panic path out of the
module. This crate has none of its own.

**`#[no_mangle]` on every export.** The host looks up `mod_load` by that exact
name. Rust mangles symbol names by default and the mangled name contains a hash
of the crate metadata, so without `#[no_mangle]` the host reports
`mod does not export mod_load` and refuses the load. There is no partial
success mode.

**`extern "C"`, not the default ABI.** Rust's own `extern "Rust"` calling
convention is explicitly unstable — the same compiler at a different version is
allowed to change it. `extern "C"` is the convention the host actually calls
with. On wasm32 the two happen to agree for simple integer arguments today,
which is exactly the kind of thing that works until it does not.

**`crate-type = ["cdylib"]`.** Only a cdylib gets an export section. Build your
mod as an `rlib` and you get a `.wasm` with no exports and the same
`mod does not export mod_load`. The `gearbox` crate itself is an rlib, which is
correct — a library has no `mod_load` to export.

**Strings are `(ptr, len)`, never null-terminated.** This is the one place the
ABI is nicer in Rust than in C: `&str` *is* a pointer and a length, so the safe
wrappers take `&str` and pass `s.as_ptr(), s.len() as u32` with nothing in
between. Do not go looking for `CString`; a trailing `\0` would just be one
more byte the host draws. Coming back the other way, the host writes raw bytes
into a buffer you own — see two-call sizing below.

**Two-call sizing, and truncation is not failure.** `country_name` and
`asset_read` write at most `cap` bytes and return the *full* length:

```rust
let need = c.name_len();                       // sizing call, writes nothing
let mut buf = [0u8; 64];
let fill = gearbox::country_name_into(c, &mut buf);
if fill.truncated() { /* fill.written is still good data */ }
```

`gearbox::country_name(c, &mut buf) -> &str` wraps both calls when you just
want the string. It cuts the result back to a UTF-8 character boundary, because
the host truncates by bytes and does not know where a codepoint ends — without
that step a long name with a non-ASCII character in the wrong place gives you
half a character and a replacement glyph.

**`mod_load` returning non-zero refuses the load.** Return `0` to be accepted.
Any other value and the mod does not run and the number is shown to the user.
It is not a status code the host interprets, so do not return a count or a
handle from it by accident. Reserve it for "I genuinely cannot run" — not for
"UI was revoked", which you should degrade through.

**Your default stack is 1 MiB, and `memoryPages` has to cover it.** `wasm-ld`
reserves a 1 MiB shadow stack unless told otherwise, so a Rust mod's *initial*
memory is around 17 pages before it does anything. The C example declares
`"memoryPages": 16` and is fine; declaring 16 here would fail instantiation,
because the limit binds exactly — you can grow to it and no further. This
example asks for 32. If you want the C example's footprint, shrink the stack:

```toml
# .cargo/config.toml
[target.wasm32-unknown-unknown]
rustflags = ["-C", "link-arg=-zstack-size=65536"]
```

Then check it actually still runs — 64 KiB of stack is not much if you put
large arrays in locals, and a WASM stack overflow is a trap, not a message.
This is not set by default here for that reason.

**Profiles only apply from the workspace root.** `[profile.release]` in a
workspace *member* is ignored, with a warning. The settings that matter for
size live in `sdk/rust/Cargo.toml`: `opt-level = "z"`, `lto = true`,
`codegen-units = 1`, `panic = "abort"`, `strip = true`. A debug build is
substantially larger and executes many more instructions per call, which comes
straight out of your fuel budget — always ship `--release`.

**`mod_draw_panel` runs every frame.** Re-issue every draw call each time; the
command list is cleared between frames. There is no retained scene, and nothing
you drew last frame survives.

**Handles do not survive the hook.** `Country` and `Province` are opaque `u32`
and Rust cannot help you here — there is no lifetime on the wire to attach to
them. Storing one in a `static` and using it next turn gives you a stale or
reassigned entity, silently. Keep an index or a name, and re-resolve.

**Mutable statics.** Rust statics must be `Sync`, and `Cell` is not.
`ModCell<T>` is a `Cell` that asserts `Sync`, which is sound here because a mod
instance is single-threaded: the host drives it from one thread and never
re-enters a hook. It is restricted to `T: Copy` and get/set so no reference
into it can be live across a call. The alternative is `static mut` and an
`unsafe` block at every use, for the same guarantee and a lint that grows teeth
in edition 2024.

**Edition 2021, on purpose.** Edition 2024 makes `#[no_mangle]` require
`#[unsafe(no_mangle)]` and makes `extern` blocks require `unsafe extern`. Both
crates here are edition 2021 so the familiar spelling stays valid. If you move
your own mod to 2024, expect to add those two keywords.

**`memcpy` is provided for you.** Bare-metal Rust targets often need a `mem`
intrinsics crate; `wasm32-unknown-unknown` ships `compiler_builtins` with them
enabled, so `no_std` works out of the box here. Worth knowing because the
failure mode elsewhere — undefined symbol `memcpy` at link time — sends people
down a long detour.

## Import map

Every ABI import, its raw form, and the safe wrapper. `ffi` is a literal
transcription; the wrappers are in the crate root.

| ABI import | `ffi::` | safe |
|---|---|---|
| `core/log` | `log` | `log(Level, &str)`, `trace`/`info`/`warn`/`error` |
| `core/env` | `env` | `Env::get()` |
| `core/abort` | `abort` | `abort(&str) -> !` |
| `core/fuel_budget` | `fuel_budget` | `fuel_budget()`, `is_unmetered()` |
| `gamestate.read/turn_number` | `turn_number` | `turn_number()` |
| `gamestate.read/country_count` | `country_count` | `country_count()` |
| `gamestate.read/country_at` | `country_at` | `country_at(u32) -> Option<Country>`, `countries()` |
| `gamestate.read/country_name` | `country_name` | `country_name`, `country_name_into`, `country_name_len` |
| `gamestate.read/country_treasury` | `country_treasury` | `country_treasury`, `Country::treasury` |
| `gamestate.read/country_province_count` | `country_province_count` | `country_province_count`, `Country::province_count` |
| `gamestate.read/province_population` | `province_population` | `province_population`, `Province::population` |
| `gamestate.read/province_owner` | `province_owner` | `province_owner -> Option<Country>` |
| `ui/panel_register` | `panel_register` | `panel_register(&str, u32, u32) -> Option<Panel>` |
| `ui/draw_rect` | `draw_rect` | `Panel::rect` |
| `ui/draw_text` | `draw_text` | `Panel::text` |
| `ui/button` | `button` | `Panel::button -> bool` |
| `assets/size` | `asset_size` | `asset_size(&str) -> usize` |
| `assets/read` | `asset_read` | `asset_read(&str, &mut [u8]) -> Fill` |

Exports are yours to write; `gearbox::exports` has the five prototypes to copy,
and `examples/hello_panel/src/lib.rs` implements three of them.

## Known-uncertain points

Written without a compiler, so these are the places to look first if it does
not build:

- **`core::arch::wasm32::unreachable()`** in `abort`. Used to give `abort` the
  `!` type and to trap on the web backend, where the host's `abort` records the
  message and *returns* instead of raising an exception. Wrapped in
  `unsafe` with `#[allow(unused_unsafe)]` so it compiles whether or not the
  function is `unsafe` in your `core`. If the path has moved, `loop {}` also
  types as `!` — but it hangs the tab on web rather than trapping.
- **The workspace member under `examples/`.** Cargo treats `examples/` as a
  target directory; `autoexamples = false` in the root manifest is there to
  stop it scanning. If cargo objects anyway, move `examples/hello_panel` to
  `sdk/rust/hello-panel/` and update `members` and `build.sh`.
- **Borrowck in `country_name`.** It returns a `&str` borrowed from the `&mut
  [u8]` you passed in, after an earlier reborrow for the fill. It is written as
  two statements so the first borrow ends before the returned one starts, which
  should satisfy NLL — but this is the shape of code that sometimes needs a
  rewrite.
- **`memoryPages: 32`.** Chosen to clear a 1 MiB default shadow stack plus data
  with room to spare. It has not been measured against a real build; if
  instantiation is refused for memory, raise it, and if you want it lower,
  shrink the stack as shown above.
- **`&::core::panic::PanicInfo` in `abort_on_panic!`** is written without an
  explicit lifetime argument. That relies on lifetime elision in a path, which
  is allowed but linted under `elided_lifetimes_in_paths` (allow by default).
  Written this way on purpose — `PanicInfo<'_>` would be more explicit but
  breaks if the lifetime parameter is ever removed from `core`.
- **Size and fuel claims** throughout this file are reasoning from how the
  toolchain works, not numbers off a build. Measure before you rely on any of
  them.
