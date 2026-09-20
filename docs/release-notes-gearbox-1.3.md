**Gearbox 1.3 adds 41 imports and five capability modules.** A mod can now put
content in the game's own catalogues, hold fields on a country, claim a map
script command, mark the map, and ask what the process is costing.

- **Content.** `content.add` puts a doctrine, research node, troop type,
  artillery type or district law into the catalogue the game itself loads —
  and the definition crosses as the SAME JSON `data/policies.json` uses, through
  the same parser. There is no separate mod format, and no field that works from
  the data file but not from a mod. Ids are global within a catalogue, because a
  country records what it holds by id; the first mod to claim one keeps it, and
  a collision is refused rather than silently overwritten. `PERSIST` writes the
  definition into the save, so a campaign that adopted your doctrine still knows
  what it was after your mod is uninstalled.

  `docs/gearbox-custom-policy.md` documents every field, and `PolicyRulesTest`
  reads that document's example and checks each claim against the `Policy` the
  game ends up with — so the guide cannot drift from the parser.

- **Country.** `country.field_add` declares a field on every country, keyed by
  (mod, name), so two mods may both add `morale` and neither can see the
  other's. Persisted fields survive uninstalling the mod that made them.

- **Scripts.** A mod claims a map-script keyword and handles it.

- **Render.** Tints and labels on the map. An alpha of zero REMOVES, so a mod
  fading something out cannot grow the draw list by one entry per frame.

- **Core.Protected.** What the process weighs, and which mods are loaded. The
  player is told a mod asked for it, a debug console shows every request, and a
  mod cannot tell when that console is open.

**All thirteen language SDKs reach all 216 imports.** Verified by building every
example and driving it through its real draw path, not by a text lint. Doing that
found three bindings that had never worked:

- **Rust and Zig could not use Assets or Content at all.** In both, the declared
  symbol IS the wasm import name, so every renamed binding asked for
  `"gearbox:assets"."asset_size"` — a name no host provides — and the module was
  refused at instantiation. Rust now emits `#[link_name]`; Zig declares a renamed
  import inside a namespace where it keeps its wire name.

- **`type` was emitted as a parameter name**, which is a keyword in Go, Rust, Zig
  and AssemblyScript. Parameter names now carry a per-language escape.

- **Every script mod silently lost GameState.Read.** The generated bindings gate
  each group on the ABI's capability name; the Lua, Python and JavaScript hosts
  defaulted a macro nothing reads. A rebuilt mod died on "attempt to call a nil
  value (field 'turnNumber')". `tools/check_script_bindings.sh` now refuses any
  capability macro a host names that no generated group reads.

**`odmod-check` keeps a catalogue**, so a content mod's adds succeed in the dry
run and it prints what actually landed — and `--expect-refusal` makes a revoked
capability's refusal the pass, which is what `sdk/rust/build.sh` had been
reporting as a failure since the day it was written.

**Two new example mods.** `sdk/lua/examples/first-doctrine` is one file and one
doctrine with the JSON inline and no generation step: the one to read first.
`sdk/examples/custom-doctrine` is the same thing in C, with its two doctrines in
a `.json` the Lua port reads rather than copying.
