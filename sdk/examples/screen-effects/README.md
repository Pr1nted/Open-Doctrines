# Screen Effects

What the `Shaders` capability is, and — more usefully — what it refuses to do.

```
./build.sh                      # produces screen-effects.odmod
cp screen-effects.odmod ../../../data/mods/
```

Then enable it in the Mod Menu. Mods load from there and nowhere else.

## What it does

| target | effect |
|---|---|
| `MAP` | hue rolling continuously, saturation pushed up |
| `PANEL` | a vignette and film grain, on this mod's panel alone |

The panel is also the clock: `mod_draw_panel` is the only per-frame export the
ABI has, so a mod that animates anything needs one.

## What it cannot do, which is the point

The **try to strobe** button does exactly what a mod would do to flash the
screen — full grain on one frame, none on the next, for as long as it is on.

Watch the map. It fades, slowly, and settles somewhere in the middle. The host
moves the rendered value toward the asked-for one at a bounded rate, so a
request that reverses every frame never arrives anywhere. A full-screen value
alternating at the frame rate is the thing that triggers photosensitive
seizures, and the host makes it impossible rather than asking mods not to do
it.

The panel prints what it asked for beside what it is holding, so the gap is
visible rather than something you take on trust.

## Three things that will catch you out

**A target has one owner.** If another mod already holds `MAP`, `set_effect`
returns false and the panel says `refused 1`. It is not a silent fight over the
same pixels. The same false also means "there is no renderer here" — a server,
or `odmod-check` — and `supported()` does not separate the two, because it
answers what the build can do rather than whether anything is being drawn.

**A player can switch screen effects off** (Settings → Display → Mod Screen
Effects). Your calls still succeed and nothing in the ABI reports it — a mod
that could read the setting could route round it. If the panel says the map is
being asked for a strong effect and the map looks normal, that is probably what
happened, and it is the correct amount of explanation for a mod to be able to
give.

**You never get the frame.** There is no framebuffer, no shader and no draw
callback here, and there is no target for the interface. The Mod Menu is drawn
after every target and clears every effect while it is open, so whatever a mod
does, the way to switch it off stays legible.
