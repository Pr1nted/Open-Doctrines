# Lua SDK

Write a mod in Lua. The script is embedded in a wasm module together with the
Lua interpreter, so the thing you ship is still a single self-contained
`.odmod` — the host does not know or care that there is an interpreter inside.

```bash
sdk/lua/examples/hello-panel/build.sh
```

Built and measured with:

| | |
|---|---|
| Lua | 5.4.7 (fetched by `build.sh`, cached in `sdk/lua/.lua-src/`) |
| Emscripten | 5.0.7-git |
| `mod.wasm` | 232 KB |
| `hello-panel-lua.odmod` | 97 KB |
| `mod_load` cost | between 350k and 400k instructions, measured by bisecting `fuelPerTurn` until the load stopped trapping |

The example declares `fuelPerTurn: 1000000`, roughly 2.5× the measured load
cost. `mod_load` is the expensive call — it creates the interpreter, opens the
libraries and compiles the script. Drawing a panel is far cheaper, but the
budget is applied per call, so it has to cover the worst one.

## Read this before you write a Lua mod

**`pcall` does not recover from errors in this build. A Lua error terminates
the mod.**

It is logged first, through the Core log import, and the host disables the mod
and shows the message — so the failure is reported rather than silent. But a
script cannot catch its own errors, and a Lua library that uses `pcall` for
control flow will take the mod down with it.

The reason is in `gbx_throw.h` in full. Briefly: Lua signals errors by
`longjmp`, which does not work on wasm32. The usual fix is `-fwasm-exceptions`
plus WAMR's exception-handling proposal, but in WAMR 2.4.5 those opcodes are
implemented only in the *classic* interpreter — `wasm_interp_fast.c` raises
"unsupported opcode" for them even when the feature is compiled in. Enabling
exceptions therefore means turning off `WAMR_BUILD_FAST_INTERP`, which would
slow down every mod in every other language to buy error recovery for this one.
So Lua takes the escape hatch its own source offers (`LUAI_THROW`/`LUAI_TRY`)
and traps instead.

If WAMR ever implements exception handling in the fast interpreter, this
becomes a one-file change.

## What the script sees

Hooks are globals. Define the ones you want; the rest are simply absent.

```lua
function mod_load()                      return 0 end   -- non-zero refuses the load
function mod_unload()                    end
function mod_draw_panel(panel, w, h)     end
function mod_pre_turn(turn)              end
function mod_post_turn(turn)             end
```

Everything else lives on the `gearbox` table:

```lua
gearbox.log(gearbox.INFO, "hello")       -- TRACE / INFO / WARN / ERROR
gearbox.env()                            -- table: isHeadless, screenW, platform, …
gearbox.abort("unrecoverable")           -- does not return
gearbox.fuelBudget()                     -- integer, or math.huge when unmetered

gearbox.turnNumber()
gearbox.countryCount()
gearbox.countryAt(i)                     -- 1-BASED, see below. nil if out of range
gearbox.countryName(c)                   -- string; two-call sizing done for you
gearbox.countryTreasury(c)               -- number
gearbox.countryProvinceCount(c)
gearbox.provincePopulation(p)
gearbox.provinceOwner(p)                 -- nil if none

gearbox.panelRegister(title, minW, minH) -- returns a panel handle
gearbox.drawText(panel, x, y, rgba, s)
gearbox.drawRect(panel, x, y, w, h, rgba)
gearbox.button(panel, x, y, w, h, label) -- returns a BOOLEAN, not 0/1

gearbox.assetSize(name)                  -- only with -DGBX_WITH_ASSETS=1
gearbox.assetRead(name)                  -- string, or nil if absent
```

Two places where this deliberately differs from the C ABI, because matching C
exactly would make the Lua wrong:

- **`countryAt` is 1-based.** The ABI is 0-based; Lua is not. Indices are
  converted at the binding.
- **`button` returns a boolean**, not the ABI's 0/1, so `if gearbox.button(…)`
  reads correctly.

`countryName` and `assetRead` do the ABI's two-call sizing internally and hand
back a finished string.

## Capabilities are chosen at build time

Every wasm import has to resolve when the module is instantiated, whether or
not the script ever calls it. So binding a capability here means the mod must
declare it in `MANIFEST.json` and the user must grant it.

Binding all 18 imports unconditionally would make a mod that just draws a panel
demand `Assets` and `GameState.Read` as well. Instead each group is a build
flag in `build.sh`:

| Flag | Default | Manifest capability |
|---|---|---|
| `GBX_WITH_UI` | on | `UI` |
| `GBX_WITH_GAMESTATE` | on | `GameState.Read` |
| `GBX_WITH_ASSETS` | **off** | `Assets` |

Turn a group off and drop the matching entry from `modules`. After building,
`tools/wasm_imports.py mod.wasm` prints what you actually imported — keep the
manifest in step with that, not with what you meant to do.

## What is not in the sandbox

`io`, `os`, `package`/`require` and `debug` are absent. Not merely unregistered
— their `.c` files are never compiled, so no filesystem call is linked into the
module at all. `dofile` and `loadfile` are removed from the base library for
the same reason, and `luaL_loadfilex` is replaced with one that returns an
error saying so.

`print` is redirected to the host log, since there is no stdout anyone reads.

`load` on a **string** still works, and so do `string`, `table`, `math`,
`utf8` and `coroutine`.

The module still imports four WASI functions — `fd_write`, `fd_close`,
`fd_seek` and `clock_time_get` (that last one is `math.randomseed` asking for a
seed). They come from Emscripten's libc, they are covered by the `WasiStub`
capability, and the ones that touch a filesystem are refused by the host rather
than answered. That is why the example's manifest lists `WasiStub`.

## Adding to an existing mod

`gearbox_lua.c` and `gbx_throw.h` are the SDK; `examples/hello-panel/` is a
worked example. Copy the example directory, replace `main.lua`, and edit the
`MANIFEST.json` id. `build.sh` regenerates `script.h` from `main.lua` on every
build, so the script is never out of date with the module.
