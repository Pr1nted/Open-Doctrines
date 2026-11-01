# LSDoctrines

A band of colour rolls diagonally across the map, continuously, with a panel
to make it calmer, stronger, or stop.

**What it actually looks like.** `Render` draws a tint as a DISC AT THE
PROVINCE CENTRE -- `DrawCircleV(p, max(3, 7 * zoom))` in
`Game::drawModRenderLayer` -- not as a filled province. The capability cannot
touch the draw loop, deliberately: "a mod that tints wrongly makes the map
ugly and gets switched off, while a mod that owns rendering can make the
screen it would be switched off from unreadable." So this is a field of
coloured dots, one per province, shifting hue. It is not the landmass itself
changing colour, and no mod can make it be.

```
./build.sh            # produces lsdoctrines.odmod
```

Then drop `lsdoctrines.odmod` in your mods folder and enable it.

## What it demonstrates

A mod that draws on **the map** rather than in a panel. `hello-panel` shows the
panel path; this shows `gearbox:render`, which is a different thing: tints
belong to the mod that drew them, survive between frames, and are dropped when
the mod unloads.

Four capabilities, which is all it needs: `Core`, `UI`, `Map`, `Render`. It
reads no game state at all — it never asks who owns a province, only how many
there are.

## Two things worth copying

**The panel is the clock.** `mod_draw_panel` is the only per-frame export in
the ABI; `mod_pre_turn` and `mod_post_turn` fire once a turn, which would give
one colour change per turn and no animation. So an animated map effect has to
own a panel to get a frame hook. Making that panel the off switch and the
intensity control is the honest version of the arrangement — a mod that
repaints the whole map should be visibly present and easy to stop.

**Colour comes from position, not from the loop counter.** The first version
keyed hue on the province index, and province order has nothing to do with
geography: neighbours got unrelated hues and the map read as confetti.
`province_center_x/y` and `map width/height` turn it into a function of
position, so what crosses the map is a band. Worth knowing before reaching
for the obvious `for i` version.

**An alpha of zero removes a tint.** One call sets and clears, so the "off"
state is the same loop with `intensity 0` rather than a second code path, and
a per-frame repaint cannot grow the tint list forever. The cap is 4096 tints
per mod; a larger map stops at the cap and says so in the log at load, rather
than painting a partial rainbow that reads as a broken mod.

## Freestanding

No libc, no allocator, no float — `-nostdlib` means no libm, so a `sinf()` in
the colour wheel would not link. The wheel is integer arithmetic over six
256-step ramps, which also makes it byte-identical on every platform. That
matters more than it looks: a mod that drew different colours on two machines
in a multiplayer game would be a bug nobody could reproduce.

## Gearbox version

The manifest declares `"gearbox": "1.3"`, which is when `Render` arrived.
Declaring `1.0` like `hello-panel` would compile and pack perfectly happily and
then fail at instantiation on a 1.0–1.2 host, with an unresolved import — loud,
but later than it needed to be.
