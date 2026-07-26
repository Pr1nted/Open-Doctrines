# JavaScript / TypeScript SDK

Write a mod in JavaScript or TypeScript. The script is embedded in a wasm
module together with the QuickJS engine, so what ships is a single
self-contained `.odmod` — the host neither knows nor cares that there is a JS
engine inside.

```bash
sdk/js/examples/hello-panel/build.sh       # JavaScript
sdk/js/examples/hello-panel-ts/build.sh    # TypeScript
```

JavaScript needs only Emscripten. TypeScript additionally needs `tsc`, which
`tools/sdk_toolchains.sh install` puts in `.toolchains/`.

Built and measured with:

| | |
|---|---|
| QuickJS | 2025-04-26 (fetched by `build_mod.sh`, cached in `sdk/js/.quickjs-src/`) |
| TypeScript | 7.0.2 |
| Emscripten | 5.0.7-git |
| `mod.wasm` | 702 KB |
| `.odmod` | 268 KB |
| `mod_draw_panel` cost | between 50k and 80k instructions |
| `mod_load` cost | under 500k instructions |

Both examples declare `fuelPerTurn: 200000`, the same as the C example, and
leave `loadFuel` to the host.

## try/catch works, and that is not a given

Worth saying plainly, because the Lua SDK cannot manage it: **a JavaScript
error does not kill the mod.**

QuickJS signals exceptions with a sentinel return value rather than `longjmp`,
so it needs none of the error-handling surgery `sdk/lua/gbx_throw.h` describes.
`try`/`catch`/`finally` behave normally, and an exception that escapes a hook is
caught at the boundary, logged with its stack, and the mod carries on.

That makes JS the most forgiving of the interpreted SDKs. It is also the reason
QuickJS was chosen over engines that rely on `setjmp`.

## What the script sees

Hooks are globals. Define the ones you want; the rest are simply absent.

```js
function mod_load()                     { return 0; }   // non-zero refuses the load
function mod_unload()                   { }
function mod_draw_panel(panel, w, h)    { }
function mod_pre_turn(turn)             { }
function mod_post_turn(turn)            { }
```

Everything else is on the `gearbox` global:

```js
gearbox.log(gearbox.INFO, "hello");     // TRACE / INFO / WARN / ERROR
gearbox.env();                          // { isHeadless, screenW, platform, … }
gearbox.abort("unrecoverable");         // does not return
gearbox.fuelBudget();                   // number, or Infinity when unmetered

gearbox.turnNumber();
gearbox.countryCount();
gearbox.countryAt(i);                   // 0-based; null if out of range
gearbox.countryName(c);                 // string; two-call sizing done for you
gearbox.countryTreasury(c);             // number
gearbox.countryProvinceCount(c);
gearbox.provincePopulation(p);
gearbox.provinceOwner(p);               // null if none

gearbox.panelRegister(title, minW, minH);
gearbox.drawText(panel, x, y, rgba, text);
gearbox.drawRect(panel, x, y, w, h, rgba);
gearbox.button(panel, x, y, w, h, label);   // returns a BOOLEAN, not 0/1

gearbox.assetSize(name);                // only with -DGBX_WITH_ASSETS=1
gearbox.assetRead(name);                // Uint8Array, or null if absent
```

`console.log`, `.info`, `.warn` and `.error` all route to the host log. There is
no stdout a player can see, and `quickjs-libc` is not compiled in, so without
this there would be no `console` at all.

Two deliberate departures from the raw C ABI, because matching it exactly would
make the JavaScript worse:

- **`button` returns a boolean**, so `if (gearbox.button(…))` reads correctly.
- **absent entities are `null`**, not the ABI's `0xFFFFFFFF`.

Indices stay **0-based**, unlike the Lua SDK — JS arrays are 0-based, so
matching the ABI is also the least surprising choice.

## TypeScript

`types/gearbox.d.ts` types the whole `gearbox` global, so a wrong argument order
is a compile error rather than a mod that draws nonsense in the game. That is
the entire reason to prefer TypeScript here; the runtime is the same QuickJS.

The example's `tsconfig.json` sets `noEmitOnError`, so a type error stops the
build rather than shipping.

**Do not add `import` or `export` to your entry file.** The host finds hooks as
globals, and tsc treats any file with a top-level import or export as a module,
which puts everything in module scope where the host cannot see it. The
`tsconfig.json` deliberately sets no `module` option for this reason. If you
want to split a mod across files, list them all in `files` and let tsc
concatenate them into the one output script.

## What is not in the sandbox

`quickjs-libc` is **not compiled in** — not merely unregistered. That is the
half of QuickJS providing `std.open`, `os.exec`, `os.read` and the rest, so no
filesystem or process call is linked into the module at all. There is no
`require`, no `import` at runtime, no timers, and no network.

Everything in the language itself works: classes, closures, generators,
`async`/`await`, destructuring, `Map`/`Set`, `JSON`, `RegExp`, `BigInt`,
`Proxy`, template literals, and the whole of `Math`.

The module imports four WASI functions — `fd_write`, `clock_time_get`,
`environ_sizes_get` and `environ_get`. They come from Emscripten's libc, they
are covered by the `WasiStub` capability, and the ones that touch a filesystem
are refused by the host rather than answered. That is why the manifests list
`WasiStub`.

## Capabilities are chosen at build time

Every wasm import must resolve when the module is instantiated, whether or not
the script ever calls it. So each capability group is a build flag, exactly as
in the Lua SDK:

| Flag | Default | Manifest capability |
|---|---|---|
| `GBX_WITH_UI` | on | `UI` |
| `GBX_WITH_GAMESTATE` | on | `GameState.Read` |
| `GBX_WITH_ASSETS` | **off** | `Assets` |

Turn a group off in `build_mod.sh` and drop the matching entry from `modules`.
After building, `tools/wasm_imports.py mod.wasm` prints what you actually
imported — keep the manifest in step with that.

## Adding your own mod

`gearbox_qjs.c` is the binding and `build_mod.sh` does the work:

```bash
sdk/js/build_mod.sh <mod-dir> <script.js> <name.odmod>
```

`<mod-dir>` needs a `MANIFEST.json`. Copy either example directory to start.
The script is re-embedded on every build, so it is never out of date with the
module.
