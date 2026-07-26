# Hello Panel — the reference example (C)

**Status: built and loaded successfully with clang 21 (wasm32), verified against
the host with `odmod-check`.**

The canonical Gearbox mod. Every other language's example does the same thing,
so this is the one to read first and the one to diff against when a port
misbehaves.

```bash
./build.sh          # produces hello-panel.odmod
```

Builds to **1.1 KB** of wasm.

## What it does

Registers a panel showing the turn number, how many countries exist, and one
country's name, province count and treasury, with a button to step through them.
It reads the world and draws. It changes nothing.

It uses three capabilities — `Core`, `UI`, `GameState.Read` — and demonstrates
the four things every mod has to get right:

1. **Check `gearbox_env` first.** If `is_headless`, there is no renderer;
   registering a panel is pointless and your UI code should not run. Self-play
   training does thousands of turns in exactly that state.
2. **Survive a revoked capability.** `panel_register` returning 0 is not an
   error — the user is allowed to say no in **Advanced**. The mod logs a warning
   and carries on rather than trapping.
3. **Two-call sizing.** `gearbox_country_name` returns the full length and fills
   at most what you gave it. A return greater than your buffer means truncation.
4. **Format your own integers.** There is no libc, so there is no `snprintf`.
   `u64_to_str` is nine lines and every mod needs it.

## Files

| | |
|---|---|
| `mod.c` | The whole mod. ~130 lines, no dependencies beyond `gearbox.h`. |
| `MANIFEST.json` | Declares id, capabilities and limits. |
| `build.sh` | Finds a wasm-capable clang, compiles, packs with `tools/pack_odmod.sh`. |

## Try changing something

Good first edits, in rising order of difficulty:

- Change the panel title or colours (`0xRRGGBBAA`).
- Add a second button that steps backwards.
- Sort countries by treasury — you will need to hold an array, and you will
  discover the fuel budget when you sort the whole world every frame.
- Add `"GameProcess"` to `modules` and a `mod_post_turn` hook that logs the
  turn. Note it will only be called once you reload the mod.
