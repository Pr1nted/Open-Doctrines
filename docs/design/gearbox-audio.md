# Extending the Audio capability

**Status:** proposal. Nothing here is implemented.

## What exists already

`gearbox:audio` is four calls, and they are the right four to have started
with:

```
play(path, len, volume) -> handle    from the mod's own assets; outside is refused
stop(handle)                         only your own handles
set_volume(handle, volume)           0..1
is_playing(handle) -> bool
```

Two good decisions are already baked in and should not be lost: a path
outside the mod root is **refused rather than resolved**, and volume is
**multiplied by the player's own effects setting**, so "a mod cannot be louder
than they allowed".

So mod-supplied sounds already work. What is missing is everything that makes
it a *music player* rather than a sound effect.

## What is missing

### A music bus, separate from effects

Today everything a mod plays is scaled by the **effects** slider. A three
minute track is not an effect, and a player who turns effects down to hear the
music should not thereby silence a mod's music. Music needs its own bus,
scaled by the music slider, and `play` needs to say which it is.

### Looping, seeking, and the things a player does

`play_music(path, volume, loop)`, `pause`, `resume`, `seek`, `position`,
`duration`, and a `fade(handle, to, ms)` so a track can arrive and leave
without a click. Crossfade falls out of two fades.

### Control over the game's own soundtrack

The interesting case — a mod that scores the game — needs the game's music out
of the way. Three verbs, in increasing order of how much they should worry
somebody granting them:

- `duck(permille, ms)` — pull the game's music down while something plays over
  it. Reversible, bounded, and the obvious one for an event sting.
- `suspend()` / `resume_game_music()` — stop the soundtrack while the mod's
  own plays, restore it on unload.
- `replace` — there is no need for a verb. A mod that suspends the soundtrack
  and plays its own has replaced it, and the player can see both grants.

### Effects on playback — the piece worth doing first

`set_filter(handle, kind, permille)` — low-pass, high-pass, reverb, a
distortion. With filters and the bus work below, the capability is strong
enough to leave alone for a while; the rest of this section is nice to have. The engine already links SoLoud, which has these; this is exposing
what is there rather than writing a DSP.

Parameterised, host-implemented, clamped — the same rule as the shader
proposal's Tier A, and for the same reason: a mod picks from a list and passes
a number it cannot push past the clamp.

## Safety

### Loudness is the audio half of photosensitivity — agreed, and required

The visual proposal caps luminance change because a flashing screen can hurt
somebody. Sound has the same shape of hazard and it is easier to cause: a
sudden full-scale sample in headphones is painful and can damage hearing.

The player's volume setting already caps the ceiling. What it does not cap is
the **step** — a mod can go from silence to the maximum it is allowed between
one buffer and the next.

So: a host-side limiter the mod cannot reach, bounding peak and the rate of
change, applied to the mod bus after every mod has had its say. Same structure
as the output pass in the shader proposal, and for the same reason — it has to
hold regardless of what produced the samples.

### Licensing: the modder's, not ours — with one exception

An earlier draft of this document wanted per-asset licence declarations in the
manifest and `odmod-check` failing a mod with undeclared audio, by analogy
with the flag audit. That was the wrong analogy and it is withdrawn.

GREG and ACOR were removed because **Open Doctrines itself** was redistributing
them: the data was downloaded, processed, and baked into `map.odmap`, which
this project ships. That is first-party redistribution and the obligation was
ours.

A mod directory is a different posture. The modder chooses their licence, makes
the representation, and carries the responsibility — as they already do for
every other asset a mod can ship. A manifest field would not change who is
liable; it would add a gate for honest authors and a checkbox for dishonest
ones, and `odmod-check` cannot tell a true declaration from a false one.

What a host actually needs is not a declaration format:

- an **attestation at upload** — the author confirms they have the right to
  distribute what they are uploading;
- a **notice-and-takedown route** and someone who reads it;
- a **repeat-infringer policy**.

That is the machinery intermediary protection is built on, and none of it
lives in the ABI or in `odmod-check`.

**The exception, which is worth writing down before it comes up.** The moment
this project *bundles* a mod — ships it in the installer, features it as
official, includes it in a release — it stops being an intermediary and
becomes a redistributor again, and the GREG/ACOR rule applies in full. The
test is not "is it a mod", it is "are we the ones handing it to the player".

### The rest

- **Headless is silent.** A training or benchmark run has no audio device, and
  a mod that blocks waiting for one would hang the run.
- **Size.** Audio is the largest thing a mod will ship. Per-mod asset caps
  exist; they want a number chosen with music in mind rather than icons.
- **Multiplayer.** Sound is local. Nothing here touches the simulation or the
  decision hash.
