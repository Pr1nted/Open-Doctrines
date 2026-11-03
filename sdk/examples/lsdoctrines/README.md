# LSDoctrines

The map's own colour rolls continuously round the wheel. No panel, no dots, no
per-frame code — the whole mod is two calls in `mod_load`.

```
./build.sh            # produces lsdoctrines.odmod
```

Then drop `lsdoctrines.odmod` in your mods folder and enable it.

## What it demonstrates

`Shaders`, which recolours the map ITSELF — and `HUE_CYCLE`, where the host
turns the wheel so the mod does not have to be running every frame to animate
anything.

Two capabilities: `Core` and `Shaders`. It reads no game state at all. It does
not even ask how many provinces there are, because it is not drawing on them.

## What it used to be, and why that is the interesting part

Until Gearbox 1.4 this mod did the same job completely differently, and the
difference is worth understanding before you write your own:

| then | now |
|---|---|
| `Render`: a coloured **disc at each province centre** | the map texture's own colour |
| a **panel**, registered purely to get a per-frame hook | nothing; `HUE_CYCLE` rolls on its own |
| `set_theme_accent` cycled every frame for the menus | not done — see below |
| `Core`, `UI`, `Map`, `Render` | `Core`, `Shaders` |
| ~280 lines, including an integer colour wheel | ~40 |

The dots were never the effect anyone wanted. The map is a texture the
renderer owns, and until there was a way to ask the host to recolour it, a mod
could only stipple over the top. `Render` is still right for what it is for —
marking particular provinces — but "recolour the map" was never that.

The panel is the same story. `mod_draw_panel` is the only per-frame export in
the ABI, so before `HUE_CYCLE` an animated effect had to put a window on
screen and use it as a clock. That also meant the animation RATE was the
mod's, and the rate is the parameter that matters to anyone photosensitive.
Now it is the host's: `HUE_CYCLE`'s 0..1000 maps to at most half a turn per
second, the host integrates the phase, and there is no way to ask for faster.

## What it no longer controls, on purpose

**There is no off switch in the mod.** There is no panel to put one on, and
three better ones exist: disable the mod, open the Mod Menu (which clears
every effect for as long as it is open), or Settings → Display → Mod Screen
Effects, which turns all of it off without telling the mod.

**The menus are not tinted.** The old version moved the theme accent, which
reached every heading and button. The target that would actually cover the
interface is `SCREEN`, and it is not built — see
`docs/design/gearbox-screen-effects.md`, where the reason is draw order rather
than safety.

**The globe shows the map untinted.** `MAP` means the flat map for now.

## Gearbox version

The manifest declares `"gearbox": "1.4"`, which is when `Shaders` arrived.
If you are targeting an older host, the 1.3 version of this mod — `Render`
plus a panel — is in the git history and still works.

Note that updating a mod's capabilities does **not** widen its grants: the
manifest now asks for `Shaders` where it used to ask for `UI`, `Map` and
`Render`, and the host deliberately grants only what the mod asks for AND had
before. So an existing install needs `Shaders` ticked in the Mod Menu before
anything happens. That is the rule working, not a bug.
