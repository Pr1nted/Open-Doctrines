# Gearbox SDK Guide — v1.0

**Status: implemented.** The ABI in [`sdk/gearbox.h`](../sdk/gearbox.h) is the
contract; this document explains it.

If you read one section, read [Rules that will bite you](#rules-that-will-bite-you).

**Check your mod before you ship it:**

```bash
odmod-check mymod.odmod
```

It reads the archive under the same limits the game uses, validates the
manifest, instantiates your module and calls `mod_load` — every rejection the
game would produce, on your terminal. `--revoke UI` rehearses a user turning a
capability off in **Advanced**, which you must survive.

A complete working mod lives in
[`sdk/examples/hello-panel`](../sdk/examples/hello-panel); `./build.sh` there
produces a loadable `.odmod`.

**The rest of the documentation:**

| | |
|---|---|
| [gearbox-abi.md](gearbox-abi.md) | Every import and export, with exact wire signatures. Generated from `sdk/abi.json`. |
| [gearbox-languages.md](gearbox-languages.md) | Which languages work today, and why Ruby and C# were dropped. |
| [gearbox-troubleshooting.md](gearbox-troubleshooting.md) | Every message the loader can produce, what causes it, how to fix it. |
| [modding.md](modding.md) | Container format, archive limits, security model, host internals. |
| [sdk/README.md](../sdk/README.md) | SDK layout and tools. |

---

## 1. What a mod is

A `.odmod` file — a ZIP containing a WebAssembly module, a manifest, and
optional assets. The host instantiates your module in a sandbox with **no
ambient authority**: no filesystem, no network, no processes, no clock you can
use to identify a machine. You get exactly the capabilities you declared and the
user granted, as WASM imports. Anything else does not exist in your instance.

This is the opposite of a Lua modding API, where everything is reachable by
default and the host spends its life taking things away. Here, nothing is
reachable and the manifest is you asking.

```
my-mod.odmod
├── MANIFEST.json     required, first entry in the archive
├── mod.wasm          required
├── thumbnail.png     optional, <= 512x512
├── data/             optional, read-only via the Assets capability
└── signature.bin     optional
```

## 2. Hello, panel

Complete C mod. Nothing else needed.

```c
#include "gearbox.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_panel g_panel;
static uint32_t      g_clicks;

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    gearbox_env_t env;
    env.size = sizeof env;
    gearbox_env(&env);

    if (env.is_headless) {          /* training run: no renderer, do nothing */
        gearbox_log(GEARBOX_LOG_INFO, S("headless, UI disabled"));
        return 0;
    }
    g_panel = gearbox_panel_register(S("Click Counter"), 200, 100);
    return 0;                       /* non-zero refuses the load */
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) { g_clicks = 0; }

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    (void)w; (void)h;
    if (gearbox_button(panel, 10, 40, 120, 24, S("Click me"))) g_clicks++;

    /* There is no libc, so there is no snprintf. Formatting an integer is the
     * first thing every mod needs and the first thing every modder is
     * surprised by. It is four lines: */
    char buf[32];
    uint32_t n = 0, v = g_clicks;
    char tmp[16];
    uint32_t k = 0;
    buf[n++] = 'c'; buf[n++] = ':'; buf[n++] = ' ';
    if (v == 0) tmp[k++] = '0';
    while (v > 0) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k > 0) buf[n++] = tmp[--k];

    gearbox_draw_text(panel, 10, 10, 0xFFFFFFFFu, buf, n);
}
```

Build:

```bash
clang --target=wasm32 -nostdlib -O2 -I sdk \
      -Wl,--no-entry -Wl,--allow-undefined \
      -o mod.wasm mod.c
```

Apple's bundled clang cannot target wasm32; the one inside Emscripten can, as
can any upstream LLVM. Then package it — `MANIFEST.json` **must** be the first
entry, which is what `tools/pack_odmod.sh` handles:

```bash
tools/pack_odmod.sh my-mod-dir my-mod.odmod
```

Manifest:

```json
{
  "schema": 1,
  "id": "com.example.click-counter",
  "name": "Click Counter",
  "version": "1.0.0",
  "description": "Counts clicks.",
  "authors": ["you"],
  "gearbox": "1.0",
  "modules": ["Core", "UI"],
  "limits": { "memoryPages": 64, "fuelPerTurn": 100000 }
}
```

Zip it, rename to `.odmod`, load it from the mod menu.

The `GEARBOX_EXPORT(...)` attributes on the definitions above are optional in
C: `gearbox.h` already declares each export carrying the attribute, and clang
propagates it from the declaration. They are written out anyway because being
able to see what a module exports without cross-referencing a header is worth
three lines — and because in a language without that propagation, or if you do
not include the header, they are the only thing making the export exist.

Either way, confirm before shipping — a missing export shows up as
`mod does not export mod_load` at load time, which is a poor way to find out:

```bash
python3 tools/wasm_imports.py mod.wasm
```

## 3. Capabilities

Declare the least you need. Users see this list before granting, and can revoke
individual modules in **Advanced**. A revoked module means those imports are
absent — so **a mod must tolerate being granted less than it asked for**.

| Module | You get | Trust cost | Status |
|---|---|---|---|
| `Core` | Logging, env, abort, fuel budget | None. Always granted. | **works** |
| `GameState.Read` | Turn, countries, names, treasuries, provinces | Low | **works** |
| `UI` | Register panels, draw in them | Low | **works** |
| `GameProcess` | Turn hooks (`mod_pre_turn`, `mod_post_turn`) | Medium — useless alone; pair with GameState | **works** |
| `Assets` | Read your own `data/` | Low | **works** |
| `GameState.Write` | Mutate the world. Implies Read. | High | **works** |
| `Neural` | **Observe** AI features and rewards | High. Observe-only: no import here can write to the model. | **works** |
| `Map` | Province geometry, adjacency | Low | **works** |
| `Diplomacy` | Read relations, propose war | High | **works** |
| `Storage` | Persistent KV, namespaced to your id | Low | **works** |

Every module in this table is implemented. If a future one is not, requesting
it means your mod is **refused at load**: the import does not exist, so it
cannot link. That is deliberate — running you without a capability you believe
you hold would be worse.

`Neural` is **observe-only** and will stay that way. It has no import that can
write to the model, the optimiser state or the reward history. An override
would be a separate capability rather than a widening of this one, so that
granting "read the AI" can never silently become "retrain it".

Asking for `GameState.Write` when you only read is not neutral — it costs you
installs, and users are right to be suspicious.

## 4. Rules that will bite you

**Handles are per-hook.** `gearbox_country` / `gearbox_province` are opaque
handles valid only for the duration of the call that gave them to you. Caching
one across turns gives you a stale or reassigned entity. Store the country's
name or your own key instead.

**Strings are (ptr, len), never null-terminated.** They are borrowed. The host
does not keep your pointer after the call returns, and you must not keep the
host's.

**Two-call sizing.** Anything returning variable-length data (e.g.
`gearbox_country_name`) returns the *full* length and writes at most `cap`.
Call with `cap = 0` to size, allocate, call again. A returned length greater
than your `cap` means truncation, not error.

**You will be granted less than you asked for.** Check `gearbox_env` and degrade
gracefully. A mod that traps because `UI` was revoked is a bug in the mod.

**`is_headless` is not hypothetical.** Self-play training runs thousands of
turns with no renderer. Every `UI` import no-ops there, but your *logic* still
runs — guard it or you will waste fuel drawing nothing.

**Fuel is finite.** You get `limits.fuelPerTurn` instructions per hook call.
Exceed it and the interpreter terminates you mid-call and the mod is disabled.

**`mod_load` is the exception**, and you should not have to think about it. It
draws on a separate, much larger allowance (`limits.loadFuel`, 500,000,000 by
default) because it runs once, when the user enables the mod, and for an
interpreter SDK it is dominated by starting the interpreter — Ruby needs roughly
87 million instructions and CPython 130 million before they reach a line of your
script. Charging that to `fuelPerTurn` would force every Lua, Java or Ruby mod
to declare a per-turn budget far larger than any turn actually needs, which
would be the opposite of a tight sandbox.

You only need to set `loadFuel` if you want *less* than the host allows. The
per-turn budget is unaffected, and that is the one that stops a mod hanging the
game turn after turn.

`gearbox_fuel_budget()` returns that limit, **not** a live countdown — it does
not decrease as you run. The engine enforces the budget but exposes no readable
counter, and a fake one would be worse than none. So size your work up front
against the budget and count your own iterations; on a large map a loop over
provinces is thousands of them.

**No autorun.** Mods load only from the mod menu — never from a save, a map, a
CLI flag, or another mod. Your `mod_load` runs every session the user enables
you, and nothing of yours exists before it.

**Your state does not survive a reload, and there is no hook to save it.** This
is deliberate. A mod added or enabled while a game is running is *not* started —
it sits in `PendingReload`, highlighted in the mod menu, until the user reloads.
Reload then discards every instance and calls `mod_load` fresh. So:

- Never assume `mod_load` is the first time you have ever run.
- Never assume you were running last turn.
- Put anything that must persist in `Storage`, keyed by your mod id.

Reload happens only between turns, so you will never be torn down partway
through `mod_pre_turn` or `mod_post_turn`.

**On web, `Storage` does not survive a refresh.** Deliberate. Check
`env.is_web` and treat storage as session-scoped there.

**On web, your fuel budget is not enforced.** The browser has no instruction
limit to impose, so on `env.is_web` nothing stops a runaway loop except the tab
freezing. On desktop the interpreter terminates you at the limit. Do not rely on
being stopped: bound your own loops.

**AI Learning and mods never coexist.** If the user has the experimental
`AI Learning` option on, enabling a mod prompts them to turn it off first, and
the option cannot be enabled while any mod is active. You will never observe a
turn where both are true.

## 5. Language support

Short version: **twelve languages are verified** — C, C++, Rust, Zig, TinyGo,
AssemblyScript, raw WAT, Lua, JavaScript, TypeScript, Java and Kotlin. Each is
compiled, packed, loaded, and rendered against a synthetic world here, and each
must draw byte-identical output to all the others.

The interpreted ones (Lua, JS/TS) work through the `WasiStub` capability, which
is what resolves the old problem that every off-the-shelf interpreter-in-wasm
build targets WASI while this host deliberately does not link it. Swift's
toolchain is being brought up; C# and Ruby were dropped.

The full table, the reasoning, and the three routes out of the WASI problem are
in [gearbox-languages.md](gearbox-languages.md).

Whichever language you use, run this before you ship:

```bash
python3 tools/wasm_imports.py mymod.odmod
```

A stray import is the failure mode in every language, and it is the one thing
you cannot discover by reading your own source.

## 6. Manifest reference

See [modding.md](modding.md) for the full schema, archive limits, and the
signature/trust model. Two things modders get wrong:

- **`id` is immutable, lowercase, and is the pinning key.** `[a-z0-9._-]`, must
  contain a dot. Uppercase is refused outright — on a case-insensitive
  filesystem `com.you.Mod` and `com.you.mod` would be one storage namespace
  shared by two identities. Changing your id makes a new mod, orphaning your
  users' settings and their pinned trust in your key.
- **`MANIFEST.json` must be the archive's first entry.** Most zip tools preserve
  the order you add files in; add the manifest first. If you see
  `ManifestNotFirst`, that is why.
- **A signature proves the file was not altered after you signed it. It does
  not prove who you are**, and it does not make your mod safe. Capability grants
  are the security boundary; the signature only protects the update channel.

## 7. Reporting ABI problems

The ABI is versioned `MAJOR.MINOR`. Same major is compatible. If the host
provides a lower minor than you target, missing imports trap on first call —
so check `gearbox_env().gearbox_minor` if you use anything recent.

If something in this document disagrees with `sdk/gearbox.h`, the header wins
and the document is a bug.

## 8. What the host guarantees you

Pinned by tests, not just asserted here:

- An ungranted import refuses the load and the diagnostic names the module.
- `limits.memoryPages` binds exactly: you can grow to it and no further.
- A hook that exceeds `limits.fuelPerTurn` is terminated (desktop only).
- An out-of-bounds pointer handed to a host function is refused, not read.
- A panel handle you do not own is ignored.
- A mod enabled while a game is running is not started until you reload.
