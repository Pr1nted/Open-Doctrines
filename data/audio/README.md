# data/audio

Drop files in, and the game plays them. There is no index to rebuild and no
code to change — the game reads these folders at startup.

Nothing here is required. With the folders empty the game runs exactly as it
did before it had sound.

```
sfx/     one file per event, named after the event
music/   the compositions, plus a .json per track saying what it is for
midi/    editable sources — never loaded by the game
```

## Sound effects — `sfx/`

One file per sound, named after the event. `.wav` is the right choice; `.ogg`,
`.mp3` and `.qoa` also work.

| File | Plays when |
| --- | --- |
| `click.*` | A menu item is activated, and while a volume slider is dragged |
| `hover.*` | The main-menu selection moves to a new row |
| `notify.*` | A notification appears |
| `popup.*` | A popup opens |

Names the game never asks for are simply never played, so extra files here are
harmless. To add a new one, drop the file in and call
`Audio::get().playSfx("yourname")` from the event.

Keep them short and quiet — under a second, and mixed well below the music.

## Music — `music/`

Scanned **recursively**, so subfolders are yours to organise with; they carry no
meaning. What a track is for lives in its sidecar instead.

### The sidecar

Next to `Parade Uniform.ogg` sits `Parade Uniform.json` — same name, `.json`
extension:

```json
{
  "title": "Parade Uniform",
  "author": "Pr1nted",
  "contexts": ["menu", "game", "editor"],
  "mood": { "tension": 0.45, "energy": 0.5, "valence": -0.3 },
  "weight": 1.0
}
```

A track with no sidecar still plays: it gets a neutral mood, the filename as its
title, and permission to play anywhere. The sidecar is how you take control, not
a requirement.

**`contexts`** is the hard filter — where a track is *allowed*. Omit it or leave
it empty and the track plays anywhere. The three the game reports are:

| Context | Where |
| --- | --- |
| `menu` | Menus, browsers, loading — everywhere that is not below |
| `game` | A game in progress |
| `editor` | The map editor |

**`mood`** is the soft one — it decides *which* of the allowed tracks comes next.

| Axis | Range | Meaning |
| --- | --- | --- |
| `tension` | 0 → 1 | Nothing at stake → under real threat |
| `energy` | 0 → 1 | Still → driving |
| `valence` | −1 → +1 | Bleak → neutral → triumphant |

**`weight`** (default 1.0) breaks ties in a track's favour without letting it
beat a genuinely closer match. Above 1 it comes up more often.

**`"valenceMode": "magnitude"`** (optional) scores `valence` by magnitude rather
than by sign — the track's value is matched against `|valence|`. That is how one
piece can cover a war being *won* and a war being *lost catastrophically*, which
are opposite valences and which no single signed point can express. It asks for
"decisively one way or the other" and stays out of the undecided middle.
`March to the Death` is the only track that uses it.

### What ships

| Track | tension | energy | valence | Wins |
| --- | --- | --- | --- | --- |
| March to the Death | 0.85 | 0.75 | 0.85 **mag** | A decisive war, won or lost |
| Bombing Voronezh | 0.65 | 0.85 | −0.10 | War, alongside the theme |
| Instability | 0.60 | 0.30 | −0.40 | Unrest building at home |
| Anthem of Survivors | 0.50 | 0.60 | 0.00 | The general theme (weight 1.2) |
| Rebel | 0.50 | 0.80 | +0.40 | A rebellion, or a far smaller enemy |
| Global Politics | 0.30 | 0.50 | +0.20 | A country entangled in treaties |
| Parade Uniform | 0.45 | 0.50 | −0.30 | Main menu; leads in a peer war |
| Navy and Arms | 0.45 | 0.65 | +0.15 | Arming up in peacetime |
| Cutting Costs | 0.30 | 0.25 | −0.55 | The treasury running dry |
| Technology! | 0.25 | 0.70 | +0.45 | A research push; a growing empire |
| Reprise | 0.20 | 0.42 | +0.35 | Rebuilding after a war |
| Creator | 0.10 | 0.25 | +0.15 | Map editor, peacetime |
| Looking Around | 0.05 | 0.15 | 0.00 | Trades off with Creator when idling |
| Great Diplomacy | 0.20 | 0.30 | −0.95 | An empire that has come apart |

The library is deliberately spread across the corners rather than clustered.
`Instability` is the only tense piece that is *quiet*, which is why dread never
sounds like alarm; `Rebel` is the only loud one that is *happy*, which is why a
one-sided war never sounds like a desperate one; `Technology!` is the only
energetic piece at rest, so "busy and doing well" is never mistaken for combat.
Each new piece is easiest to place by asking which corner is still empty.

Four are restricted to `game` — `March to the Death`, `Cutting Costs`,
`Rebel` and `Great Diplomacy`. Everything else plays anywhere. Nothing keeps the calm pieces out of a
war except their mood being wrong for one, which is the system doing its job and
is why the `contexts` list is rarely needed; the three exceptions earn it,
because no mood value keeps a death march off a title screen and a bankruptcy or
counter-insurgency theme means nothing outside a running game.

What that produces, walking six consecutive picks per situation:

| Situation | Rotation |
| --- | --- |
| Main menu | Parade Uniform ⇄ Anthem of Survivors |
| Map editor | Creator ⇄ Reprise |
| Peace, idle | Creator ⇄ Looking Around |
| Peace, tangled in treaties | **Global Politics** ⇄ Reprise |
| Peace, a couple of pacts | Creator ⇄ Global Politics |
| The empire has come apart | **Great Diplomacy**, held |
| Peace, rebuilding after a war | **Reprise** ⇄ Technology! |
| Peace, arming heavily | **Navy and Arms** ⇄ Anthem of Survivors |
| Peace, unrest rising | **Instability** ⇄ Parade Uniform |
| Peace, researching hard | Technology! ⇄ Reprise |
| Peace, treasury running out | Cutting Costs, held |
| War against a peer | Parade Uniform ⇄ Anthem of Survivors |
| War against rebels, holding | **Rebel** ⇄ Navy and Arms |
| War against rebels, losing ground | March to the Death — a breakaway winning is not a small war |
| War against a far smaller state | **Rebel** ⇄ Technology! |
| A war being won or lost decisively | March to the Death |
| War *and* broke | March to the Death — a war outranks a budget |

Pairs alternate when they sit close enough for the repeat penalty to decide
between them, and hold when one is a clearly better fit. Tuning a new piece is
mostly a matter of deciding which of the existing ones it should trade off with.

### How a track gets chosen

The game reports where it is on those same three axes, several times a second:

- **Menus** are poised rather than neutral — ceremonial, about to begin. Truly
  neutral would sit closest to whichever track is calmest, and the theme would
  never play on the title screen.
- **The map editor** is calm and mildly positive: building, nothing at stake.
- **In game**, `tension` jumps the moment the player is at war at all and climbs
  with each additional war; `energy` follows it; `valence` tracks whether the
  player's province count has grown or shrunk since they picked the country up.
- **An active research project** raises `energy` and `valence`, scaled by how
  much of the budget is committed to it. It is the one thing a player can be
  busy with that the map does not show, so nothing else would pick it up.
- **Who you are fighting** matters as much as whether you are. A rebellion
  (rebel countries have their own CID range) or an enemy holding a fraction of
  your provinces lowers `tension` and raises `energy` and `valence`: still loud,
  no longer frightening. That confidence is then scaled back by how the map is
  actually going — being the bigger country is not the same as winning, and an
  empire coming apart is usually at war with something small.
- **Standing treaties** — alliances, pacts and guarantees, counted only while at
  peace — raise all three axes mildly. The first two are subtracted before
  scaling: a pact with each neighbour is ordinary, and counting from zero would
  make every peaceful country a political one.
- **Peace shortly after a war** raises `energy` and `valence`, fading over about
  forty turns. Peace looks identical either side of a war in a snapshot, so the
  last war is remembered rather than inferred.
- **Army and navy upkeep as a share of income, while at peace** raises `tension`
  and `energy` — a country paying for forces it is not using is preparing to.
- **Rebellion pressure across your own provinces** raises `tension` while
  *lowering* `energy`. It is the only signal that moves before anything visible
  happens, and dread is quiet where alarm is loud.
- **Money trouble** lowers `valence` and `energy` and raises `tension` slightly.
  It is measured as *runway* — how many turns the reserve survives the current
  deficit — not as a treasury figure, because a balance means nothing without
  the burn rate behind it: a small country with a small deficit is solvent, a
  large one haemorrhaging is not, and the raw number cannot tell them apart.

The nearest allowed track wins, weighted so tension counts most — the wrong
energy is a slightly odd choice, but peace music over a collapsing front is just
wrong.

**Mood never interrupts.** A war breaking out mid-phrase does not yank the track
away; it changes what is chosen when the current one ends. Only moving into a
context the playing track is not allowed in crossfades immediately, because that
really is a different place. Crossfades are 1.5 seconds.

The track that just played carries a small penalty on the next pick, so a piece
stops looping forever once something else fits nearly as well. It is deliberately
small — a clearly better match still repeats, which is what keeps the theme on
the menu instead of trading off with the calm one every few minutes. Two tracks
sitting close together in mood space will alternate; two far apart will not.

With one track installed all of this collapses to "play the one track", which is
the correct answer to that question.

### Formats, and why the choice matters

`.ogg`, `.mp3`, `.qoa`, `.wav` and the `.xm` / `.mod` tracker formats all work.
`.flac` does **not** — raylib is built here without it, and a `.flac` dropped in
will simply fail to load.

Size is worth caring about, because the web build preloads all of `data/` before
the game starts (see `--preload-file` in CMakeLists.txt). Every megabyte here is
a megabyte every browser player downloads before the main menu appears.

The fourteen pieces ship as **Ogg Vorbis, q3 (~112 kbps VBR)** — 23 MB, down from
40 MB as 192 kbps MP3. Vorbis rather than a lower-bitrate MP3 because it is
markedly better at that rate, and 192 kbps CBR is a bitrate for music somebody
sits and listens to, not for music playing under a map at half volume through a
reverb. Encoded with the reference encoder:

```
ffmpeg -i in.mp3 -vn -c:a pcm_s16le tmp.wav && oggenc -q 3 -o out.ogg tmp.wav
```

Two caveats worth knowing before re-doing this. The renders are already lossy, so
every re-encode compounds artifacts — go back to `midi/` rather than to an
existing `.ogg` if the music is ever reworked. And **do not leave both an `.mp3`
and an `.ogg` with the same stem in here**: the loader takes every matching
extension, so the piece would be indexed twice and could follow itself.

## MIDI sources — `midi/`

Editable sources, kept beside the renders so a remix does not start from a
lossy file.
**The game never reads this folder** — there is no synthesiser in it, and a
`.mid` is not a format raylib can play. It is here for people, not the engine.

| File | What it is |
| --- | --- |
| `Parade Uniform.mid` | Source for the main theme |
| `Creator.mid` | Source for Creator |
| `Looking Around.mid` | Source for Looking Around |
| `Bombing Voronezh.mid` | Source for Bombing Voronezh |
| `Technology!.mid` | Source for Technology! |
| `March to the Death.mid` | Source for March to the Death |
| `Cutting Costs.mid` | Source for Cutting Costs |
| `Instability.mid` | Source for Instability |
| `Great Diplomacy.mid` | Source for Great Diplomacy |
| `Global Politics.mid` | Source for Global Politics |
| `Anthem of Survivors.mid` | Source for Anthem of Survivors |
| `Navy and Arms.mid` | Source for Navy and Arms |
| `Rebel.mid` | Source for Rebel |
| `Reprise.mid` | Source for Reprise |
| `OpenDoctrines_Theme_SEED.mid` | **Reference.** Not a track — the bare motif |

The seed is melody, chords and bass and nothing else. Every shipped piece is
built on it — the same phrase carries the march in `Parade Uniform`, the
nylon-guitar setting in `Creator`, the idle in `Looking Around`, the sax line
over the drum kit in `Bombing Voronezh` and the synth lead in `Technology!`.
Start a new piece from the seed and it will belong to the same score without
sounding like any of them.

## Settings › Audio

Five settings, stored in `data/config.json`.

`masterVolume`, `musicVolume`, `sfxVolume` — what reaches the device is
`master × category`, so pulling master to zero silences everything without
disturbing the two settings under it.

`nowPlayingToast` — names each new track in the bottom-left corner for a few
seconds, over whatever screen is up. It is the only way to tell what the mood
picker chose, so it defaults to on. It stays quiet when a track repeats, which
with a small library is most of the time.

`mapAtmosphere` — makes the map sound like a different place from the menus, by
**how far out the camera is** rather than by which screen you are on:

| View | Sounds like |
| --- | --- |
| Whole world on screen | −7.5 dB, a small room (0.35 s tail) and a 270 ms echo |
| Zoomed part-way in | Proportionally between the two |
| Zoomed right in | Indistinguishable from the menus |

Three effects, one number. The reverb is a room; the echo is distance — five
audible repeats at full zoom-out, none at all zoomed in.

The curve is logarithmic, because zoom is multiplicative, and it is measured
against the map's own fit-to-screen level rather than a fixed number — what
counts as "zoomed out" depends on the map and the window. It reaches dry about
eight times closer than fully out. Both the dim and the wet level slide over
~0.8 s, so no camera move is ever heard as a step.

The dim, the reverb and the echo are deliberately one setting driven by one
number. All three run *before* the volume is applied and add on top of the dry
signal — together reaching 1.58x worst case — and the dim is the only thing
keeping that inside the headroom. Because all of them scale with the same
intensity, the worst-case peak is `(1 + 0.582i)(1 - 0.58i)`, which tops out at
exactly 1.0 when i = 0 and falls to 0.66 when fully zoomed out. Deepening a wet
level or weakening the dim without re-deriving that will clip on zoom-out; the
figures are in `src/Audio.cpp` next to the constants.

## Fades

Every track fades in over 1.5 s, including the first one after startup. The
successor is chosen a crossfade *before* the current track ends, so the two
genuinely overlap instead of one stopping and the next starting into silence.

## Licensing

Anything here that you did not write yourself needs an entry in
`tools/provenance.json` — `NOTICE.md` and the third-party section of
`data/credits.txt` are generated from it, and `tests/run_all.sh` fails if they
have drifted. Add the entry first; the paperwork follows.
