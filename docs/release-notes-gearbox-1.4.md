Sixty-five imports and three capabilities, all additive. A mod built against 1.3
runs unchanged: `tools/check_abi_compat.py` passes 874 checks against every
shipped ABI from 1.0 onward, and nothing was renamed, removed or re-signed.

**Fog of war — `Fog` and `Fog.Write`.** Thirteen reads and three writes. The
reads answer what a country can SEE and what it REMEMBERS, which under fog are
different questions from what is true: `fog_province_visible`,
`fog_province_seen_turn`, `fog_known_owner`, `fog_point_visible`,
`fog_ship_visible` and five `fog_intel_*` fields for the garrison, population,
industry, fort and port a country last saw. In a world without fog every one
of them answers as though everything were visible, so a mod written for fog
needs no branch for its absence.

The writes grant sight: `fog_add_sight` for an area, `fog_reveal_province` for
one province, `fog_clear_grants` to drop them. A grant is a watchpost, not a
reveal — the fog is rebuilt from scratch every turn, so grants are held and
folded in on every recompute and saved with the world. They join the country's
own sight and so reach an ally one hop, exactly as a coast watch does.

`Fog.Write` is a separate capability from `Fog` on purpose: it changes what
another player can see, which makes it the one to look hardest at after
`Core.Protected`.

**Economy — 29 imports.** Monuments (`can_build_monument`, slot costs and
upkeep, `build_monument`), the goods economy (`goods_economy_on`, stocks,
demand, shortfall, living standards, directed output), sector taxes and
subsidies, auto-sell and nationalisation.

**Diplomacy — 11.** Greater-diplomacy acts and their state.

**Elsewhere — 6.** Province monuments through `gamestate.read`, and three
military reads and writes.

**Screen effects — `Shaders`.** Three imports. A mod names a TARGET and picks
one of the host's effects with a 0..1000 parameter: `shaders_set_effect`,
`shaders_clear`, `shaders_supported`. The targets are `MAP` (the province
raster), `PANEL` (the mod's own panels, already clipped to their rectangle) and
`SCREEN` (the whole game frame — board, counters, panels, sidebar, dialogs).
The effects are hue rotate, saturation, vignette, grain, `HUE_CYCLE` and
`WOBBLE`, which pushes the image around rather than recolouring it — a
displacement of the texture coordinate inside the same single pass.

`WOBBLE` moves pixels and not hit boxes, because input resolves against the
layout rather than the picture. So its amplitude is capped per target and
capped hardest on `SCREEN`: a control stays where a click will find it. The cap
is the host's and a mod cannot reach it.

`HUE_CYCLE` is the one to reach for if you want movement: its parameter is a
SPEED, up to half a turn of the wheel per second, and the host integrates the
phase. That matters more than it sounds. `mod_draw_panel` is the only
per-frame export in the ABI, so without it a mod that wants a rolling colour
has to register a panel purely to use as a clock — and the animation rate ends
up being the mod's rather than the host's. `sdk/examples/lsdoctrines` was
exactly that mod and is now two calls in `mod_load` with no UI at all.

What a mod does NOT get is a framebuffer, a shader or a draw callback, so there
is no sequence of calls that produces an arbitrary image — and there is no
target for the interface. Panels, dialogs and the Mod Menu that is the only way
to switch a mod off are all drawn after every target, and the Mod Menu clears
every effect on every frame it is open. A mod that can paint the frame can
paint over its own off switch; a mod that can say "hue rotate 0.4" cannot.

Two things will surprise an author if nobody says them first.

The value a mod asks for is not what is drawn that frame. The host moves the
rendered value toward it at a bounded rate — 1.5 of full range per second — so a
parameter that reverses every frame comes out as a slow fade rather than a
strobe. A full-screen value alternating at the frame rate is what triggers
photosensitive seizures, and this is a clamp rather than a rule an author has to
know: `tests/mod_effect_test.cpp` drives the layer the way a hostile mod would
and measures 0 half-cycles per second against the 6 that WCAG 2.3.1 allows.

And a player can switch screen effects off (Settings → Display → Mod Screen
Effects). The calls still succeed and nothing in the ABI reports it, because a
mod that could read the setting could route round it. Effects are also off
during the screenshot tour and an OJH capture, so a mod cannot alter what a bug
report shows or what a benchmark measures.

`sdk/examples/screen-effects` is the worked example, including a button that
tries to strobe and cannot.

`OD_MOD_FX_DEBUG=1` prints the layer's state once a second — which mod owns
each target, the hue and saturation actually being rendered, and whether the
pass is running. When a screen effect does nothing the question is always
WHICH link is broken, and "no mod ever asked" and "the pass never bound" look
identical from the outside.

The MAP target applies on the flat map. The globe samples a cached composite
inside its own shader, so there is nowhere to bind the pass there yet; a mod
cannot ask which view is up, so for now MAP means the flat map.

A note on the capability mask: it was widened from 32 bits to 64 this release,
which was due — `Shaders` takes bit 31, the last one that was left. Nothing in a
save changes: grants are stored as a JSON number, which is exact well past
2^53.
