# A Shaders capability for a future Gearbox

**Status:** Tier A is BUILT and shipped in Gearbox 1.4 as the `Shaders`
capability. Tier B (mod-authored WGSL) is replaced by
[gearbox-effect-expressions.md](gearbox-effect-expressions.md) and remains
unbuilt.

What landed, against what this document proposed:

| proposed | built |
|---|---|
| `set_effect(target, effect, permille)`, `clear(target)` | both, plus `supported(target, effect)` so a mod can degrade on an older host |
| targets MAP, MAP_WORLD, SCREEN, PANEL | MAP, PANEL and SCREEN. MAP_WORLD is not built — see below |
| effects hue rotate, saturation, wobble, vignette, grain | hue rotate, saturation, vignette, grain, and HUE_CYCLE, which was not proposed. Wobble distorts geometry rather than colour, so it is not one uniform on the same pass |
| clamp every parameter host-side | `odeffect::Layer::setEffect`, and a mod cannot tell it was applied |
| clamp the flash rate, and the per-frame luminance delta | ONE mechanism does both: the rendered value slews toward the requested one at 1.5 of range per second. `tests/mod_effect_test.cpp` drives it the way a hostile mod would and measures 0 half-cycles per second against WCAG's 6 |
| an accessibility setting a mod cannot read | Settings → Display → Mod Screen Effects. Nothing in the ABI reports it |
| the Mod Menu clears effects before opening | every frame it is open, not on entry: a mod draws every frame and could re-assert |
| headless is a no-op | by construction — the server's `LoadShaderFromMemory` stub returns id 0 and the pass never binds |
| screenshots and OJH captures run with effects off | both, and neither asks the mod |
| one pass, host-written | one fragment shader bound around draws that were going to happen anyway: no extra geometry, no render target, no resolve |

**HUE_CYCLE was not in this proposal and should have been.** `mod_draw_panel`
is the only per-frame export the ABI has, so every proposal here quietly
assumed a mod would drive its own animation from a panel it registered as a
clock — which is what LSDoctrines did, and why it had a window on screen that
existed only to tick. Worse, it put the animation RATE in the mod's hands, and
the rate is the parameter this document spends two sections worrying about. So
one effect takes a SPEED instead of a value, the host integrates the phase at
up to half a turn per second, and a mod that wants movement does not need a
frame hook at all. The capability is smaller and the dangerous number is the
host's.

**SCREEN is built, and the way round the seam problem was to stop needing a
seam.** This document assumed the world layer had to be captured BELOW the
interface, and `Game::drawInner()` has no such boundary — it interleaves world
drawing with panels and click handlers across two thousand lines. But the
request that prompted it was for the interface to move as well, and capturing
the WHOLE frame needs no boundary at all: `beginFrameCapture()` right after
`BeginDrawing`, `endFrameCapture()` at the top of `endFrame()`, and everything
`endFrame` draws afterwards — toasts, the feedback form, the pad cursor — is
the always-on-top layer that stays clean. The Mod Menu suppresses effects
outright while it is open, so the off switch is never in the pass.

It is off unless asked: with no mod holding SCREEN the capture returns before
it touches anything, and the frame goes to the back buffer as it always did.
That is the only basis on which redirecting the whole frame is reasonable.

**MAP_WORLD is still not built** and is deliberately NOT declared: a target
that exists and does nothing is worse than one honestly absent.

**WOBBLE was not in this proposal under that name** (it was listed as an
effect and then dropped as "geometry, not colour"). It turns out to be neither
— it is a displacement of the texture coordinate inside the same pass, so it
costs nothing extra and can only move what the pass was already drawing.

What it CANNOT do is move hit boxes. Input resolves against the layout, not
the picture, so a button drawn fifteen pixels from itself still answers where
it really is. That is why the amplitude is capped per target and capped hardest
on SCREEN: enough to read as movement, not enough to make a control miss. A
mod cannot reach the cap.

**The globe is not covered either.** The sphere samples a cached composite
inside its own shader, so there is nowhere to bind the pass; doing it needs
either a rebuild of that composite every frame or a second target whose
orientation wants checking against a real globe. A mod cannot ask which view is
up, so for now MAP means the flat map.

What follows is the original proposal, unedited.

Written after LSDoctrines
(`sdk/examples/lsdoctrines`) ran into the ceiling: a mod can tint provinces and
move the theme accent, and that is the whole of what reaches the screen.

## What is being asked for

A mod that changes how the whole screen looks — the acid-trip case, but the
same machinery would serve a sepia 1914 filter, a night mode, a damaged-radio
effect during a war, or colour-blind simulation for testing.

## Why it is currently refused

`Game::drawModRenderLayer` says it plainly:

> a mod that tints wrongly makes the map ugly and gets switched off, while a
> mod that owns rendering can make the screen it would be switched off from
> unreadable.

That is the whole problem in one sentence. A mod that can paint the frame can
paint over the Mod Menu, which is the only way to disable it.

## The rule that makes it safe

**A mod never owns the frame. It names a target and either picks a host
effect or supplies a shader for that target — and the host composites, always
with the interface on top and a final safety pass the mod cannot reach.**

This is the load-bearing decision, and it is structural rather than policed. A
mod says "hue rotate 0.4, wobble 0.2"; it does not receive a framebuffer, a
shader, or a draw callback. There is no sequence of calls that produces an
arbitrary image, so "can a mod hide the off switch?" stops being a question
anyone has to keep answering as the API grows.

It also keeps the expensive part — one shader pass the host wrote and measured
— out of reach of a mod that would write a slower one.

```
gearbox:shaders set_effect(target, effect, permille)   a host effect, clamped
                clear(target)
```

Per-mille integers, because the ABI has no floats in its import signatures and
a clamped integer cannot arrive as NaN. Every value is clamped **host-side**
on the way in; a mod cannot learn the clamp and cannot detect that it was
applied, so it cannot search for the edge.

Effects: hue rotate, saturation, wobble, vignette, grain. The list is the
host's and grows by release, which is the point — each one is a thing somebody
wrote a shader for once, with a cost somebody measured once.

## Targets: what a shader is attached to

Not one mode. A `target` names a layer, and the useful ones are different
jobs:

| target | covers | why you would |
|---|---|---|
| `MAP` | the province raster alone | a sepia 1914, a night map |
| `MAP_WORLD` | map plus world-space marks — army counters, labels, mod tints | the whole board as one picture |
| `SCREEN` | everything below the interface | the acid trip |
| `PANEL` | one of the mod's own panels | a CRT panel, a damaged readout |

**There is no target for the interface layer.** That is property 1 below, and
it is what the whole design rests on: the Mod Menu is drawn after every
target, so whatever a shader does, the way to switch it off survives it.

`PANEL` is the safest of the four and worth having for its own sake — a panel
is already scissor-clipped to its rectangle, so a shader bound to one cannot
reach a pixel outside it whatever it computes.

## Mod-authored shaders, which are a different trust level

> **Superseded.** This section and the four that follow it (validation,
> hot-loading, passive safety) were written for mod-authored WGSL. That is
> replaced by [gearbox-effect-expressions.md](gearbox-effect-expressions.md),
> in which a mod supplies an expression the host compiles rather than a
> shader the host validates. Nearly everything below exists to contain
> arbitrary GPU code, and goes away with it. Kept because the reasoning is
> what led to the replacement, and because it is the argument to re-read if
> anyone proposes arbitrary shaders again.


"Apply shaders" can mean two things, and they are not variations of one
feature:

- **Tier A — the host's effects, parameterised.** Everything above. A mod
  picks from a list and passes a clamped number.
- **Tier B — the mod ships its own GLSL.** Strictly more expressive, and it
  invalidates the argument Tier A rests on.

Tier A is safe because the host bounds the *input*. With Tier B there is no
input to bound: a fragment shader is a program, and no clamp on a parameter
says anything about what it computes.

**So Tier B has to be made safe on the output instead.** The host runs a final
pass the mod cannot reach, which compares the composited frame with the last
one and, if luminance moved further than the cap, blends toward the previous
frame until it did not. That holds whatever produced the pixels — a parameter,
a mod's shader, or two mods' shaders stacked — and it is the only form of the
photosensitivity guarantee that survives arbitrary code. It costs one more
pass, which is the honest price of Tier B.

Four further things Tier B needs that Tier A does not:

- **Two backends, already.** `MapRenderer` compiles `kPoliticalFrag330` or
  `kPoliticalFragEs` depending on `kOverlayEs`. A mod's shader has to exist
  for both, or be written in a restricted subset the host translates. Asking
  mod authors for two dialects they cannot test is how you get mods that work
  for their author and nobody else.
- **No unbounded loops.** A fragment shader that spins hangs the GPU, and the
  operating system's response is to reset the driver — the game does not get
  to catch that. v1 should refuse loops outright rather than try to prove
  termination.
- **Graceful compile failure.** A shader that does not compile falls back to
  passthrough and says so in the Mod Menu. It must never be a black screen,
  which is indistinguishable from the game being broken.
- **Web is harsher.** Under emscripten a bad shader can lose the WebGL
  context, and that takes the whole game with it, not just the mod. Tier B
  may have to be desktop-only at first, the way the GD5 WASM proposal keeps
  its runtime off web until the ABI has settled.

A reasonable first release is Tier A everywhere and Tier B confined to
`PANEL` — a mod's own scissor-clipped rectangle, where a hang is still a hang
but an ugly result is contained and obviously the mod's doing.

## Validating a mod's shader: do not write the validator

The instinct to sandbox this hard is right, and so is the reason behind it —
security that depends on one person noticing and patching quickly is not
security. But the shape needs correcting in one place.

**V8 cannot help here.** It runs JavaScript on the CPU; a shader has to become
instructions on the GPU, and nothing in a JS engine stands between those and
the driver. There is no "run the shader in V8" arrangement.

What *does* deliver the property wanted is the same trick, aimed at the right
target: **never hand a mod's text to the driver.** Parse it, validate it, and
**re-emit host-generated source** from the parsed form. The mod's string never
reaches the GPU; only the validator's output does. Same model as the WASM
sandbox — do not trust the input, re-derive it.

And the validator should not be ours:

| | |
|---|---|
| **Naga** (wgpu/Firefox) | validates and translates WGSL, GLSL, SPIR-V; bounds-checks by construction |
| **ANGLE's translator** (Chrome/WebGL) | what every browser already puts between a web page and your driver |
| **Tint** (Dawn) | WGSL for WebGPU |
| **glslang + SPIRV-Tools** (Khronos) | the reference pair |

These are maintained by people whose job is that a hostile shader cannot hurt
you, and they ship fixes on their own schedule. Depending on one means a
vulnerability found in shader validation is patched by a dependency bump, not
by this project rediscovering it.

**WGSL via Naga is worth a hard look**, because it answers three problems at
once: it is bounds-checked by design, Naga emits both GLSL 330 and GLSL ES so
the two-dialect problem above disappears, and the validation is upstream. The
cost is that mod authors write WGSL rather than the GLSL they may know.

### Hot-loading the validator: update the ruling, not the ruler

The goal — an old build stops being vulnerable without its owner installing
anything — is the right goal. The proposed mechanism inverts the threat model,
though, and the platform list makes it impractical besides.

**Downloading executable sandbox code is a larger hole than the one it
closes.** A game that fetches a validator at runtime and runs it has made its
update channel into remote code execution on every machine. Compromise that
channel once and you own every old client — including the ones that were never
going to enable Tier B. The vulnerability being patched requires a player to
install a hostile mod *and* grant it GPU code; the patch mechanism requires
nothing at all. Signing and pinning reduce this but do not remove it: the
signing key becomes the thing worth stealing.

**And the binaries would not exist.** This game ships for Windows (x64,
ARM64, Win32), macOS, Linux across several architectures, FreeBSD, OpenBSD,
Android and the web — and there is a TempleOS edition written in HolyC.
Naga is Rust; there is no OpenBSD riscv64 prebuilt of it, nobody is going to
make one, and a HolyC build inside QEMU has no business fetching anything.
An offline player, which is most of them, gets nothing either.

**What can be hot and safe is data.** Ship the validator compiled in, so it
works offline and on every target. Then make the *decision* updatable:

- a signed **revocation list** — "disable Tier B", "refuse shaders matching
  this construct", "block this mod id" — checked at load, cached, and
  defaulting to the last known good when offline;
- data cannot execute, so a compromised channel can at worst switch the
  feature off, which is also what a real vulnerability would want.

That gives the property asked for — an old build that stops being exploitable
without a new build — while making the failure mode "the feature turns off"
rather than "the attacker runs code".

The rest is defence in depth, so that a validator bypass is not instantly
fatal: Tier B confined to `PANEL`, the output safety pass it cannot reach, the
runtime budget, and a grant the player had to give. A bypass should cost an
ugly panel, not a machine.

### Passive safety: make staleness fail closed

The requirement behind the hot-load idea is the right one and is worth stating
on its own: **safety must not depend on anybody remembering to do something.**
A number somebody has to bump is a number somebody forgets, and the forgetting
is silent.

That requirement does not actually ask for a fresh validator. It asks that an
old build not be dangerous. Those come apart, and the second is achievable
where the first mostly is not.

**Where a fresh validator does arrive for free: the web.** A browser's shader
validator is ANGLE, and the browser updates it. A web build that hands WGSL to
WebGPU, or GLSL ES to WebGL, is validated by a component Google and Mozilla
patch on their own schedule and the player receives without asking. Nothing to
do, and nothing we could do better.

**Where it does not: everywhere else.** A desktop build validates with what it
shipped with. Vulkan has independently-updated validation layers, but raylib
puts this game on OpenGL, and there is no system-provided GLSL validator to
defer to. An OpenBSD or TempleOS build is going to use the validator it was
compiled with, and no arrangement changes that.

So make the stale case safe instead:

- **Tier B carries an expiry.** The capability is enabled only while the build
  can see a revocation list newer than some window — say ninety days. No
  list, or a stale one, and Tier B switches itself off. The player sees "this
  build is too old to run mod shaders safely; update to re-enable", and the
  mod falls back to Tier A effects.
- **Forgetting now produces safety.** If nobody bumps anything, the feature
  turns off by itself. That is the inversion the requirement is really after:
  the failure mode of neglect is a disabled feature, not a live hole.
- **The list is data, cached, and defaults to last known good offline** — an
  offline player is not punished mid-session, but an offline build does not
  keep Tier B for ever either.

And underneath that, containment that does not age: the output safety pass,
`PANEL`-only, the runtime budget, the interface layer never a target, and a
grant the player had to give. None of those depend on the validator being
current. A validator bypass on a stale build should cost an ugly panel.

That is the honest shape of it. A fresh validator everywhere is not on offer;
a build that refuses to run untrusted GPU code once it can no longer vouch for
itself is, and it needs nobody to remember anything.

### What an ahead-of-time check can and cannot promise

It **can** reject: syntax errors, undefined behaviour, unbounded loops, array
indices it cannot prove in range, and anything past an instruction-count cap.

It **cannot** bound real cost. Texture fetch patterns, branch divergence and
overdraw are runtime facts. So static validation pairs with a runtime budget —
measure the pass, and if a shader exceeds its frame budget repeatedly, drop it
to passthrough and say so in the Mod Menu. Refusing later is still refusing.

Nor does validation prevent a **driver reset**. Browsers answer that with a
separate GPU process so a hang kills the process and not the tab. That is a
large architectural change here and is the honest reason to keep Tier B on
`PANEL` and on desktop until it has been lived with.

## The four properties that have to hold

### 1. The interface composites on top, after the effect

The effect applies to the world layer. Panels, dialogs, the turn bar and the
Mod Menu draw afterwards, untouched. Whatever a mod does, the thing you use to
switch it off stays legible.

The ask was that menus change too, and they can — but through a separate and
much tighter lever. `set_theme_accent` already exists and is the precedent: it
moves a colour the host then uses, rather than letting a mod paint UI pixels.
If menus need more than an accent, the right shape is a bounded UI tint with a
host-enforced **contrast floor** — the host computes the resulting text/
background contrast and refuses anything below it. Not a policy; a clamp.

### 2. Photosensitivity, which is the half that protects the player

This matters more than everything above it and is easy to miss, because the
other three are about protecting the program.

A full-screen effect that cycles colour and brightness is precisely the class
of thing that triggers photosensitive seizures. A mod author writing a
psychedelic effect is not thinking about that, and should not have to: the
host must make it impossible rather than ask nicely.

- **Clamp the flash rate.** WCAG 2.3.1's general threshold is three flashes
  per second; the host should rate-limit full-screen luminance change below
  that, whatever parameters it is given.
- **Clamp luminance delta per frame**, so a mod cannot strobe by alternating
  two "legal" values every frame — the obvious way to defeat a rate limit
  expressed only in Hz.
- **A global accessibility setting that disables screen effects**, which a mod
  **cannot read**. If it could, a mod could detect the setting and route round
  it, and the setting would be advice instead of a guarantee.

Under Tier B these cannot be clamps on parameters, because there are no
parameters. They become the output pass described above: the cap is enforced
on the composited frame, after every shader has had its say.

LSDoctrines as written today would need all three: it cycles hue every frame
with no rate limit at all, and nothing stops it.

### 3. A kill switch the effect cannot reach

The Mod Menu key clears all screen effects before opening, and the menu draws
outside the affected layer by (1). The existing rule stays: the override is
dropped the moment no mod is running, so it cannot outlive uninstalling the
mod.

### 4. A measured budget

Full-screen passes are not free here, and we have a number: a per-pixel
overlay shader doing four id lookups and eight table lookups cut fps from
~1000 to ~350 (2026-09-21). A single hue/saturation pass is far cheaper than
that, but the lesson is that it must be *one* pass, host-written, and measured
by OJH before and after rather than assumed cheap.

Headless is a no-op — a training or benchmark run has no renderer, and an
effect that cost frame time there would corrupt the measurement it sits in.

## What it must not touch

- **The simulation.** Client-side only, never serialised, never part of the
  decision hash. Two players with different screen mods must produce identical
  turns.
- **Screenshots used as evidence.** The tour and any OJH capture must run with
  effects off, or a mod could silently alter what a bug report shows.

## Open questions

- ~~Map only, or map plus world-space overlays?~~ Both, plus screen and panel:
  they are four targets, not one mode. See the table above.
- Does a contrast floor on a UI tint actually hold for every theme, or does it
  need per-element checking? Worth prototyping before promising it.
- ~~Is this its own capability or part of `Render`?~~ Own: `Shaders`. A mod
  that tints provinces is asking for far less than one that restyles the
  frame, and the Mod Menu has to be able to say which is which.
- Should Tier A and Tier B be separate grants? Probably — "may recolour the
  screen" and "may run its own GPU code on your machine" are different
  sentences to show somebody, and only the second needs the warning.
- What does the output safety pass cost? It is a full-screen read of the
  previous frame plus a blend. Cheaper than the 12-lookup overlay pass that
  cost 650 fps, but it is on every frame a Tier B shader is live and has to
  be measured, not assumed.
