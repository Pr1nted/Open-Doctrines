# Effect expressions: a mod ships an expression, not a program

**Status:** proposal. Nothing implemented. This **replaces Tier B** of
[gearbox-screen-effects.md](gearbox-screen-effects.md) — mod-authored GLSL or
WGSL — and most of the machinery that proposal needed exists only because of
Tier B.

## The move

A mod does not supply a shader. It supplies an **expression** over a few
named inputs, and the host compiles that into a fragment shader the host
wrote.

```
# a slow diagonal hue roll, which is what LSDoctrines wanted all along
hue = fract(x * 0.5 + y * 0.5 + t * 0.1)
sat = 1.0 + 0.3 * sin(t * 2.0)
```

The mod contributes no token to the shader source. The host parses the
expression into a tree, checks the tree, and emits GLSL from the tree — the
same relationship the WASM sandbox has with a mod's code, and the same one
dragoman has with a map file: do not trust the input, re-derive the output.

## Why this is better than validating a real shading language

The previous design put mod-authored WGSL through Naga, and then needed:
a Rust toolchain on nine platforms including OpenBSD and a HolyC build; a
signed revocation list; an expiry window so a stale build fails closed; and
GPU-process isolation to survive a driver hang. Every one of those exists to
contain **arbitrary GPU code**.

Remove arbitrary GPU code and they go with it:

| problem | with WGSL + Naga | with expressions |
|---|---|---|
| unbounded loop hangs the GPU | validator must prove termination | **no loop syntax exists** |
| out-of-bounds sampling | bounds-check pass | **no sampling the host did not provide** |
| unknown cost | instruction cap + runtime budget | **cost is the tree size, known at parse** |
| two GLSL dialects | Naga emits both | **the host emits both; it wrote them** |
| validator vulnerability | dependency bump, expiry, revocation | the compiler is ours and small |
| build on OpenBSD / TempleOS | no Naga prebuilt | **arithmetic; it is just C++** |

The security argument stops being "a maintained upstream catches hostile
input" and becomes "there is no construct in which to express the hostile
thing". That is a smaller claim and a much easier one to keep true.

## The language

Deliberately tiny. If something is not listed, it is not in the language.

```
program   := binding* assignment+
binding   := "let" ident "=" expr
assignment:= channel "=" expr
channel   := "r" | "g" | "b" | "hue" | "sat" | "val"
expr      := term (("+" | "-") term)*
term      := factor (("*" | "/") factor)*
factor    := number | ident | call | "(" expr ")" | "-" factor
call      := func "(" expr ("," expr)* ")"
```

**Inputs**, all read-only:

| | |
|---|---|
| `x`, `y` | position in the target, 0..1 |
| `t` | seconds since the effect started, wrapped at 3600 |
| `sr`, `sg`, `sb` | the source pixel's colour, 0..1 |
| `lum` | its luminance, for convenience |

**Functions**, the whole list: `sin cos abs min max clamp floor fract mix
smoothstep step sqrt pow length`.

**Not in the language**: loops, user functions, recursion, conditionals,
texture sampling, array indexing, pointers, assignment to an input, any
identifier not named above.

### The three things that still need care

Removing loops does not make arithmetic safe on its own.

- **Division.** `a / b` with `b` near zero gives inf, which propagates to a
  white or black screen. Emit a guarded divide — `a / (abs(b) < 1e-6 ? 1e-6 :
  b)` — rather than forbid division.
- **`pow`.** `pow(negative, fractional)` is undefined in GLSL and differs by
  driver. Emit `pow(max(a, 0.0), b)`.
- **NaN.** Any of the above, or a hostile constant, can reach the output. The
  emitted shader clamps every channel to 0..1 at the end, and a NaN compares
  false against both bounds, so clamp alone does not remove it. Emit an
  explicit `isnan` replace, or compute `min(max(v, 0.0), 1.0)` in an order
  that forces a defined result.

These are three lines in the emitter, written once, rather than three rules a
validator has to enforce against a language that allows them.

## Cost, bounded at parse

The tree's node count *is* the instruction count, near enough. Cap it — a few
hundred nodes — and the cost of an expression is known before it runs, which
is the thing a general validator cannot promise.

`sin` and `cos` are the expensive calls; cap their count separately. A mod
that wants twenty sines wants a host effect instead, and can have one.

## What it cannot do, and what covers that

No neighbour sampling, so no blur, bloom, or anything needing more than the
pixel under consideration. Those stay **Tier A host effects**, which is where
they belong: somebody writes the two-pass gaussian once, measures it, and
every mod gets the measured one.

If neighbour sampling is ever wanted, the bounded form is a `sample(dx, dy)`
with the offsets clamped to a small radius and the call count capped — still
provable, because the bound is in the grammar rather than in the analysis.

## What does not change

- **The output safety pass stays.** An expression can strobe perfectly
  legally — `val = step(0.5, fract(t * 30.0))` is a 30 Hz full-screen flash in
  one line. Photosensitivity has to be enforced on the composited frame, after
  the effect, exactly as before.
- **Targets stay** — `MAP`, `MAP_WORLD`, `SCREEN`, `PANEL`, and no target for
  the interface.
- **Headless is a no-op**, and nothing here touches the simulation.

## What this removes from the earlier proposal

Expiry, the signed revocation list, GPU-process isolation, the Naga
dependency, and the "how do players get a fresh validator" problem that
prompted them. A compiler bug here is an ordinary bug fixed in an ordinary
release, because the worst an expression can do is be ugly.

Tier A — host effects with clamped parameters — is unchanged and remains the
first thing to build.

## Open questions

- Is `hue`/`sat`/`val` the right output basis, or should it be `r`/`g`/`b`
  only with the HSV conversion left to the expression? HSV is friendlier and
  costs a conversion per pixel.
- Should an expression be able to read the player's accent, so an effect can
  harmonise rather than override? `theme_accent` already exists for panels.
- Where does the expression live — a string passed through the ABI, or a file
  in the mod package? A file is diffable and reviewable; a string can be
  built at runtime, which is both useful and harder to review.
