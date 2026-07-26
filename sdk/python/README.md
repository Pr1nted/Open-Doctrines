# Python SDK

Write a mod in Python. CPython 3.12 and the whole standard library are compiled
into the module alongside your script, so what ships is still a single
self-contained `.odmod` — but a very large one.

```bash
tools/sdk_toolchains.sh install          # wasi-sdk 20 and libpython
sdk/python/examples/hello-panel/build.sh
```

## Read this first: Python needs a different host build

**A Python mod will not load on the default build of the game.**

```bash
cmake -S . -B build-python -DOD_MODS_FAST_INTERP=OFF
```

CPython is too big for WAMR's *fast* interpreter. The fast interpreter encodes
each function's operand-stack offsets in a signed 16-bit field, and rejects any
module where a function exceeds `INT16_MAX` — CPython's evaluation loop does.
The loader says so plainly:

```
mod.wasm did not load: WASM module load failed: fast interpreter offset overflow
```

This is structural, not a tuning knob. The limits live in
`wasm_loader.c`; the classic interpreter has neither. Binaryen's `--flatten`
pass could in principle lower the stack depth, but on a 34 MB module it ran for
over 40 minutes without completing, so it is not a viable build step.

The cost of the classic interpreter is that **every** mod gets slower, in every
language — which is why it is not the default and why Python is opt-in. The
test suites skip the Python example automatically when run against a fast
build, matching on the loader's message rather than on the mod's name.

Every other SDK here runs on both builds.

## Sizes, measured

| | |
|---|---|
| CPython | 3.12.0, prebuilt for wasm32-wasi |
| wasi-sdk | **20** — see the pin below |
| `mod.wasm` | 43 MB |
| `hello-panel-python.odmod` | 17.8 MB |
| Frozen stdlib | 460 modules, 8.2 MB of marshalled code |

A Python mod is a **megabyte-scale download**. That is the honest trade for
writing mods in Python, and it should be stated to anyone installing one. Lua
is 232 KB and JavaScript 702 KB for the identical mod.

## The wasi-sdk version is pinned, deliberately

`libpython3.12.a` is built against wasi-sdk 20's libc. Linking it with a newer
wasi-sdk produces a module that loads, starts, and then faults with
`out of bounds memory access` partway through CPython's startup — no link
error, no warning. It cost a debugging cycle here; do not "upgrade" the SDK
without rebuilding libpython to match.

## How the standard library gets in

There is no filesystem: every WASI path call is refused by the host. CPython
will not start without `encodings`, so the library cannot be read from disk.

`tools/gen_frozen.py` turns the shipped `python312.zip` into a C table of
**frozen modules**, and `gearbox_py.c` points `PyImport_FrozenModules` at it
before `Py_InitializeFromConfig`. This is a supported CPython mechanism — the
header describes it as "embedding apps may change this pointer to point to
their favorite collection of frozen modules" — and the import machinery
consults it before it ever touches a path.

The zip ships `.pyc` files, so no compiler is involved and no CPython 3.12 is
needed on the build machine: a `.pyc` is a 16-byte header followed by exactly
the marshalled code object the frozen table wants. That matters, because the
host `python3` is 3.14 and its marshal format would not load in 3.12.

The whole stdlib is frozen, so `json`, `re`, `dataclasses`, `collections`,
`itertools`, `math` and the rest all import normally.

## What the script sees

Hooks are module-level functions:

```python
def mod_load():                     return 0    # non-zero refuses the load
def mod_unload():                   pass
def mod_draw_panel(panel, w, h):    pass
def mod_pre_turn(turn):             pass
def mod_post_turn(turn):            pass
```

Everything else is the builtin `gearbox` module:

```python
import gearbox

gearbox.log(gearbox.INFO, "hello")      # TRACE / INFO / WARN / ERROR
gearbox.env()                           # dict: isHeadless, screenW, platform, …
gearbox.abort("unrecoverable")          # does not return
gearbox.fuelBudget()                    # int, or float('inf') when unmetered

gearbox.turnNumber()
gearbox.countryCount()
gearbox.countryAt(i)                    # 0-based; None if out of range
gearbox.countryName(c)                  # str; two-call sizing done for you
gearbox.countryTreasury(c)              # float
gearbox.countryProvinceCount(c)
gearbox.provincePopulation(p)
gearbox.provinceOwner(p)                # None if none

gearbox.panelRegister(title, minW, minH)
gearbox.drawText(panel, x, y, rgba, text)
gearbox.drawRect(panel, x, y, w, h, rgba)
gearbox.button(panel, x, y, w, h, label)    # returns a bool, not 0/1

gearbox.assetSize(name)                 # only with -DGBX_WITH_ASSETS=1
gearbox.assetRead(name)                 # bytes, or None if absent
```

Two departures from the raw C ABI, because matching it exactly would make the
Python worse: **absent entities are `None`**, not `0xFFFFFFFF`, and **`button`
returns a `bool`**. Indices stay 0-based, as both the ABI and Python are.

`print()` is redirected to the host log, since there is no stdout a player sees.

Exceptions behave normally: one raised in a hook is caught at the boundary,
logged with its type, and the mod carries on. Only Lua among these SDKs cannot
do that.

## What is not in the sandbox

`open()` raises, and so does anything under `os` that touches a path. The
frozen stdlib includes those modules — they import fine — but every call
reaches a host that refuses it. There is no network, no subprocess, no clock
that reveals the real date, and no way to learn anything about the machine.

`site` is disabled, `sys.path` is empty by construction, and bytecode writing
is off because there is nowhere to write it.

## Capabilities

Build flags, exactly as in the Lua and JavaScript SDKs, because every wasm
import must resolve whether or not the script calls it:

| Flag | Default | Manifest capability |
|---|---|---|
| `GBX_WITH_UI` | on | `UI` |
| `GBX_WITH_GAMESTATE` | on | `GameState.Read` |
| `GBX_WITH_ASSETS` | **off** | `Assets` |

After building, `tools/wasm_imports.py mod.wasm` prints what you actually
imported; keep `MANIFEST.json` in step with that.

## Adding your own mod

Copy `examples/hello-panel`, replace `main.py`, and change the `MANIFEST.json`
id. `build.sh` regenerates both `script.h` and `frozen_stdlib.h`, so neither can
drift from its source.
