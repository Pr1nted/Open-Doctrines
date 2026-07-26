# Interpreter SDKs — Lua, JS/TS, Java/Kotlin, Python

Status after building them. This document previously predicted how the four
would go; most of those predictions were wrong in ways worth recording, so what
follows is what was measured rather than what was expected.

| | Status | Startup cost | Artifact |
|---|---|---|---|
| **Lua** | **shipped** — `sdk/lua` | 350k–400k instructions | 232 KB wasm |
| **JavaScript / TypeScript** | **shipped** — `sdk/js` | under 500k instructions | 702 KB wasm |
| **Java / Kotlin** | **shipped** — `sdk/java` | 10k–20k instructions | 156 KB / 166 KB |
| **Ruby** | **dropped** — see docs/gearbox-languages.md | ~87M measured | ~16 MB stripped |
| **Python** | **shipped** — `sdk/python` (classic interpreter only) | ~130M measured | 43 MB wasm |

Costs are WAMR instruction counts under the host's own configuration
(`FAST_INTERP=1`, no JIT or AOT), obtained by bisecting the instruction limit
until the call stopped trapping.

## The three things the original plan got wrong

**1. Enabling WAMR exception handling is not free — it costs the fast
interpreter.** The plan named `-fwasm-exceptions` the preferred fix for
`setjmp`/`longjmp` and treated `WAMR_BUILD_EXCE_HANDLING=1` as a build-flag
change. It is not. In WAMR 2.4.5 those opcodes are implemented **only in the
classic interpreter**: `core/iwasm/interpreter/wasm_interp_fast.c` answers
`try`/`catch`/`throw` with `wasm_set_exception(module, "unsupported opcode")`
even when the feature is compiled in. Turning it on therefore means
`WAMR_BUILD_FAST_INTERP=0`, slowing every mod in every language to buy error
recovery for one of them. WAMR also implements only the *legacy* proposal,
which current Emscripten does not emit by default.

**Nothing shipped needed it.** Every language here has a way around.

**2. GC was feared and is actually fine.** The plan flagged the
garbage-collection proposal as "a much larger change than exception handling".
It is the opposite: `WASM_ENABLE_GC` **is** implemented in the fast
interpreter. It is still not enabled, because nothing shipped needs it.

**3. TeaVM's WasmGC backend is the wrong target for Java.** The plan called it
"the only realistic route". Its output cannot load here at all: it emits a tag
section (exception handling, see above) *and* 22 imports the host cannot
provide — `teavmJso.*` and `wasm:js-string.*`, because that backend targets a
JavaScript host. TeaVM's **`WEBASSEMBLY_WASI`** target has neither problem and
is what `sdk/java` uses.

## How each one solves setjmp/longjmp

This was correctly identified as the shared blocker. Each language solves it
differently, and only Lua needed the escape hatch the plan described.

- **Lua** — redefines `LUAI_THROW`/`LUAI_TRY` to trap instead of jumping. Lua
  documents these as the hook for exactly this. The cost is real and stated in
  `sdk/lua/README.md`: `pcall` no longer recovers, so a Lua error terminates
  the mod.
- **Java/Kotlin** — never needed it. TeaVM's linear-memory backend implements
  Java exceptions with its own `teavm_catchException`, no proposal required.
- **Ruby** — ships `rb_wasm_setjmp`/`rb_wasm_longjmp` and makes them work with
  **Binaryen's Asyncify** pass, which rewrites the module to unwind and rewind
  its own stack. No wasm EH involved. This is why the prebuilt `ruby.wasm`
  imports nothing outside `wasi_snapshot_preview1`.
- **Python** — does not use `setjmp` at all. `libpython3.12.a` has zero
  undefined `setjmp`/`longjmp` symbols. The plan's claim that CPython is
  blocked here is simply not true.

For a *source* build through Emscripten the blocker is real, and all three
routes fail: the default (`SUPPORT_LONGJMP=emscripten`) imports
`env.invoke_*` and `env._emscripten_throw_longjmp`; `=wasm` needs the EH
proposal; `=0` imports `env.setjmp` and `env.longjmp`. All are outside the ABI,
so the host refuses the mod. A host function cannot implement `longjmp` — it
would have to unwind the wasm stack, which it has no way to do.

## Fuel: mod_load has its own budget now

The plan did not mention fuel, and it was the constraint that nearly killed
both heavy languages. `limits.fuelPerTurn` was applied to *every* exported
call including `mod_load`, and it is capped at 100M — against a measured
startup of 87M for Ruby and 130M for Python.

`mod_load` and `_initialize` now draw on `limits.loadFuel` instead, which
defaults to 500M and needs no declaration. See `ModHostCaps::kDefaultLoadFuel`
and the note in `docs/modding.md` for the trade this makes. The per-turn budget
is unchanged, and `tests/mod_runtime_test.cpp` ("load fuel") pins both halves:
`mod_load` succeeds with `fuelPerTurn: 1`, and an ordinary hook still does not.

## WasiStub is now 44 functions

15 was enough for `tests/mods/wasitest.c`. A real runtime wants far more —
Ruby imports 37 and CPython 43 — and **every import must resolve at
instantiation whether or not it is ever called**, so a missing one refuses the
load rather than failing at the call.

29 were added. Two do something: `clock_res_get` reports a fixed coarse
resolution, `sched_yield` succeeds. The other 27 refuse. The rule the plan set
down still holds and is worth restating: **anything that would let a mod
observe or touch a filesystem is refused, never stubbed with a plausible
answer.** A fake successful `path_open` is far worse than `ENOTCAPABLE`,
because the interpreter then believes it has a file. The socket calls refuse
unconditionally for the same reason.

## Ruby — dropped

Dropped after establishing a working route but before the last step. The
evidence, the two traps it hides, and what finishing it would cost are in
[gearbox-languages.md](gearbox-languages.md#why-ruby-was-dropped) so the
decision can be revisited without repeating the work.

## Python — shipped, on the classic interpreter only

It works: frozen stdlib, generator expressions, string formatting, byte-identical
panel output. Getting there turned up three host bugs and one hard limit.

**The limit.** CPython will not load under WAMR's *fast* interpreter. That
interpreter encodes each function's operand-stack offsets in a signed 16-bit
field and rejects any module exceeding `INT16_MAX`; CPython's evaluation loop
does. It is structural, not a tuning knob, and it is why `OD_MODS_FAST_INTERP`
now exists as a build option. Binaryen `--flatten` could in principle lower the
depth, but ran over 40 minutes on the 34 MB module without finishing, so it is
not a build step anyone would accept. Both test suites skip the Python example
on a fast build, matching the loader's own message rather than the mod's name.

**Three host bugs, all fixed, all live in the default build.** These were real
defects rather than Python-specific concessions:

1. **`fd_prestat_get` returned `ENOTCAPABLE`.** wasi-libc scans descriptors
   from fd 3 looking for preopens, treats `EBADF` as "that is all of them", and
   calls `_Exit(EX_OSERR)` on any other error. `EX_OSERR` is **71** — the
   mystery `exit(71)` that killed CPython *and* Ruby before either interpreter
   started. EBADF is also the honest answer: there is no such descriptor to
   deny. This one finding retroactively explains the Ruby failure that was
   attributed to its standard library.
2. **`fd_fdstat_get` and `fd_filestat_get` refused fds 0-2** while `fd_write`
   accepted them. CPython asks what stdout *is* before wrapping it, so it died
   with "can't initialize sys standard streams". They now describe all three as
   contentless character devices, which is what they are.
3. **A 64 KiB wasm stack.** Enough for a hand-written C mod, not for anything
   that recurses while parsing. CPython needs more than 512 KiB and works at
   1 MiB; it is now 2 MiB.

**One trap worth recording.** `libpython3.12.a` is built against wasi-sdk 20.
Linking it with wasi-sdk 33 gives a module that loads, runs, and then faults
with `out of bounds memory access` inside startup -- no link error, no warning.
The SDK version is pinned in `build.sh` for that reason.

**How the stdlib gets in.** `PyImport_FrozenModules`, which CPython documents
for exactly this. `sdk/python/tools/gen_frozen.py` turns the shipped
`python312.zip` into a C table; the zip holds `.pyc` files, so the marshalled
code objects can be lifted straight out and no CPython 3.12 is needed on the
build machine.

## Definition of done, per language

Unchanged, and `ModExamplesTest` still enforces it:

1. Builds to a `.odmod` via a `build.sh` in `sdk/<lang>/`.
2. `odmod-check` loads it and `mod_load` returns 0.
3. Revoking a declared capability refuses the load.
4. `mod_draw_panel` renders **byte-identical output to the other languages**:
   `Turn 42 / Countries: 3 / Grand Duchy of Testphalia / Provinces: 7 /
   Treasury: 1234 / Next country`.
5. A simulated click advances the mod's own state.
6. `README.md` states the toolchain version it was built with and the module
   size, and does not claim anything untested.

Lua, JavaScript, TypeScript, Java, Kotlin and Python meet all six. Anything short of 4
and 5 is "it links", which is not the same as working.
