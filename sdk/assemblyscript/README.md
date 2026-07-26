# Gearbox SDK — AssemblyScript

**Status: built and loaded successfully with AssemblyScript 0.27 (asc), verified
against the host with `odmod-check`.**

AssemblyScript is TypeScript-shaped syntax compiled directly to WebAssembly. It
is the easiest non-C route into this ABI: no `no_std` ceremony, real strings,
and a 6 KB hello-world.

## Build

```bash
./build.sh          # installs AssemblyScript on first run, then builds and packs
```

Or by hand:

```bash
npm install
./node_modules/.bin/asc assembly/index.ts --config asconfig.json --target release
../../tools/pack_odmod.sh . hello-panel-as.odmod
```

The example builds to **6.2 KB** of wasm, **3.6 KB** packed.

## Layout

- `assembly/gearbox.ts` — the bindings. The `declare` block at the top is the
  ABI verbatim, one entry per import in [`sdk/abi.json`](../abi.json); the rest
  is convenience over it.
- `assembly/index.ts` — the example mod.
- `asconfig.json` — compiler settings. **The `use: ["abort="]` line matters**; see
  below.

## The one thing that will bite you: stray imports

AssemblyScript emits an `env.abort` import by default, and the host refuses any
mod importing something outside the Gearbox ABI. That is why `asconfig.json`
contains:

```json
"options": { "use": ["abort="] }
```

Without it the mod is rejected at load with:

```
mod.wasm imports "env"."abort", which this host does not provide.
```

`Math.random()` and `trace()` pull in `env.seed` and `env.trace` the same way.
Avoid both. Check before shipping:

```bash
python3 ../../tools/wasm_imports.py mod.wasm
```

Everything listed must start with `gearbox:`.

## Strings

AssemblyScript strings are UTF-16; the ABI takes UTF-8 `(ptr, len)`. Every
string crossing the boundary is encoded first, which the binding does for you:

```ts
export function log(level: i32, msg: string): void {
  const b = String.UTF8.encode(msg);
  _log(level, changetype<usize>(b), b.byteLength);
}
```

`changetype<usize>(buffer)` gives the data pointer for an `ArrayBuffer`. Coming
back the other way, `String.UTF8.decode(buffer)`.

This allocates on every call. In `mod_draw_panel`, which runs every frame, that
is real garbage — the default incremental runtime collects it, but if you are
tight on fuel, build your strings once in `mod_load` and cache them.

## Gotchas

- **`noAssert: true`** in `asconfig.json` removes bounds checks. It makes the
  module smaller and faster; it also means an out-of-range index is undefined
  behaviour rather than a clean trap. Drop it while developing.
- **A panic compiles to `unreachable`**, which the host reports as
  `Exception: unreachable` and disables the mod. There is no stack trace, so
  guard your indices rather than relying on the trap to tell you where.
- **`exportRuntime: false`** keeps `__new`/`__pin` out of the exports. You do
  not need them; the host never allocates in your memory.
- **`i64` returns** (`fuel_budget`, `province_population`) are `i64`/`u64` in
  AssemblyScript and work natively — no BigInt dance, since this is real wasm.
