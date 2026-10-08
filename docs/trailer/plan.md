# The trailer

Eighty seconds, for the Steam page, the itch page and YouTube. One joke,
told straight.

## The joke

It opens as a prestige grand strategy trailer — gold on black, portentous,
sweeping claims over a lamplit map — and then every claim is undercut by what
the game actually does. The humour is not jokes written on top of the footage.
It is the footage, captioned honestly. The game is genuinely large and
genuinely unfinished and its AI is genuinely not very good, and saying so in
the voice a publisher uses for "a world of endless possibility" is the whole
bit.

It has to be TRUE to be funny. The moment a caption oversells, the trailer
becomes the thing it is making fun of, and the ending — which is sincere —
stops working.

Three things only this game can end on, and they are saved for last: it is
played by a fruit fly, it runs on TempleOS, and it costs nothing.

## The cut

Times are where a beat LANDS. The authoritative version is the SHOTS table in
`tools/trailer_build.py`; this is the intent it was written from. 40 shots,
90 seconds, mean shot length 2.25 s.

| Time | On screen | Caption |
|---|---|---|
| 0:00 | Black, then the Modern Day map fades up under a slow push | — |
| 0:04 | Push to central Asia | A GRAND STRATEGY GAME |
| 0:08 | The world, then one province's panel | 1,641 provinces |
| 0:11 | **The globe, in space. Then the globe at night.** | — |
| 0:15 | The research tree, two angles | 86 technologies |
| 0:18 | Doctrines, then districts, then monuments | 59 doctrines |
| 0:22 | Armies, orders, the troop map | Your orders are carried out |
| 0:27 | **A rebellion spawns. Two closes, no caption.** | — |
| 0:30 | Hold. Music stops dead. Pull back to the whole world. | By someone |
| 0:33 | Economy, then goods, then the factories | Industry you cannot afford |
| 0:38 | A portrait, then a country at war with its own ally | Allies who will not come |
| 0:41 | Europe on turn three, then the Declare War button | A war you started on turn three |
| 0:44 | **The advisor, answering** | Advice, confidently given |
| 0:48 | South America gone, then the far east | A country that no longer exists |
| 0:51 | The Atlantic full of ships, then the whole world, sped up | It is played by |
| 0:55 | The tutorial, talking to you | you |
| 0:58 | The multiplayer lobby | your friends |
| 1:00 | **The fly at its desk, brain lit beside it** | and a fruit fly |
| 1:05 | Hold on the brain. Let it sit. | 138,639 neurons. It is not good at it. |
| 1:10 | A third angle on the fly, silent | — |
| 1:14 | Four marks, one at a time, each word lighting as it lands | Windows. macOS. Linux. A browser tab. |
| 1:19 | **The TempleOS build, palettised, in the map** | And an operating system one man wrote for God. |
| 1:23 | Gold wordmark on the sunken ground | Free. All of it. |
| 1:26 | | opendoctrines.pages.dev |

The two held beats — 0:30 and 1:05 — are the trailer. Do not cut inside them.

## Why 40 shots and not 21

The first assembly had 21, and eight were the identical world view at the
identical scale. Thirty seconds of one picture with things moving in it, which
a viewer reads as one long shot. Three rules fixed it:

**No two adjacent map shots share a scale.** `geo(lon, lat, span)` names a crop
in degrees — the map is equirectangular at 2:1, so a degree is the same number
of pixels both ways. `EUROPE`, `SASIA`, `SAMER`, `ATLANT`, `FAREAST` are
115-165 degrees wide against the world's 360, and `WORLD` is now spent only on
the establisher, the full stop, and the release.

**It is not all map.** The globe, the tech tree, districts, monuments, goods,
the sector taxes, the diplomatic actions, the advisor answering a question, the
tutorial and the multiplayer lobby are all in it now. None of them is captioned
as a feature: each sits under a caption that was already there, which is how
the trailer shows what the game is without becoming the feature list this
document forbids. Goods lands under "Industry you cannot afford", the Declare
War button under "A war you started on turn three", the tutorial under "you".

**One caption, two angles.** Every deadpan beat is two short shots rather than
one long one. Mean shot length went 4.0 s to 2.25 s without moving a music cue,
because the section boundaries the score depends on did not move.

## The plates

`tools/screenshots.sh` photographs 123 screens of a SIMULATED world in a
scratch data directory, so re-running it gives the same kind of picture rather
than whatever was on this machine. The ones the trailer uses are copied into
`docs/trailer/assets/plates/` so a build does not depend on `/tmp`.

Three traps in that set, all of which shipped into a cut before being caught:

- **Names do not mean what they look like.** The tour's `world-map.png` is the
  PAUSE MENU. `docs/img/world-map.png` is the world map. They are different
  pictures with the same filename in different directories.
- **Some shots carry a touch keyboard.** `monuments-map` and `monuments-globe`
  are the on-screen-keyboard variants, and half the frame is keys.
- **`claims-uk` is in Ukrainian**, because it is the localisation shot. There
  is no English claims screen in the tour.

And one structural fact that shapes every crop: the tour photographs ONE world
from ONE camera, so every in-game plate shares the same West African coastline.
A gentle crop leaves a dozen shots that are the same picture with different
furniture on it. The fix is `zoom` — a real punch-in onto the panel, so the
shot is the goods table or the Declare War button rather than the map behind
it. The plates are 1600x900 and that is hardcoded in the tour
(`Game_Screenshots.cpp`), so zoom stays at or under 2.4.

## The look

## The look

| | |
|---|---|
| Frame | 2.39:1, letterboxed into a 1920x1080 file |
| Ground | `#090B11` — the site's `--sunken`, not black |
| Text | `#E8E4DA` — warm, "this is a paper-and-maps game" |
| Accent | `#C9A227` — the gold, used sparingly and never on two things at once |

Captions are the game's own font, left-aligned, held long enough to read twice,
and they FADE rather than appear — the plan says the type does not move, which
a fade is not. No motion graphics, no whoosh, no lens flare. The map moves; the
type does not.

**Transitions are chosen, not defaulted.** 39 straight cuts in a row read as a
slideshow. `into` on each shot picks one of four, and the final assembly is a
single `xfade` chain rather than a concat:

| | |
|---|---|
| `cut` | a true cut (two frames of overlap, invisible) — 14 of them, all in the deadpan stretch, because deadpan is cuts |
| `mix` | a dissolve, for two shots of the same subject or the same place |
| `dip` | through black, for a section break, where a dissolve would lie about the two sides being continuous |
| `smear` | a dissolve through motion, between two unrelated places |

Each transition is CENTRED on the incoming shot's slot start, so the outgoing
shot gives up half of it and the incoming shot the other half. That is what an
editor means by a dissolve ON the beat, and it is why adding transitions did
not move a single music cue. Shots are rendered longer than their slot by the
widest transition so a dissolve has material to cross over instead of freezing.

Five things make it read as shot rather than assembled, and they are all in
`trailer_build.py`:

- **Scope.** The picture is 2.39:1 and the file is 16:9. Cropping 156 rows off
  a world map sounds like damage and is the opposite: the bottom eighth of
  every map layer is the Antarctic ice shelf drawn as flat grey, and it was the
  brightest thing on screen in half the shots.
- **A grade.** Raw game output is pure black behind saturated province colour,
  which is not "a lamplit map table". The blacks are lifted INTO the sunken
  blue, the highlights pulled toward the gold, saturation down a tenth, with a
  vignette and fine grain over the print.
- **Per-shot exposure.** One look, but not one exposure: `research.png` and
  `policies.png` average 12.7 and 15.3 against the world map's 42.5, and a
  curve that lifts a lit map's blacks pushes those two further down. They carry
  a gamma lift. Gamma ONLY — a flat brightness offset lifts the toe as well and
  the result is milky, which trades away the deep black the grade exists for.
- **Eased moves.** Raised-cosine, so a push accelerates away from rest and
  settles into it. A linear ramp starts and stops dead and reads as a crop
  animating. The two held beats pull OUT rather than in, so not everything
  drifts the same way.
- **Punch-ins, not crops.** `zoom` scales a plate past the frame and takes a
  window around the focus point, which is what turns a screenshot of the whole
  interface into a shot of one panel. Aiming it is per-plate work and five of
  them landed on empty sea or an unlit panel the first time; the fastest way to
  aim is to render the plate with a 0.1 grid over it and read the coordinates
  off.

## Music

Original, written for this. ~88 BPM, D minor, trip-hop: a dusty broken beat,
sub bass, a hypnotic two-bar ostinato on something plucked and filtered, and a
wordless vocal pad used as texture rather than melody.

**On the reference.** The brief is Phantogram's *Black Out Days* — take its
MOOD: sparse, hypnotic, patient, a low vocal sitting under the beat rather
than over it, and a drop that removes almost everything. Take none of its
material: no melodic quotation, no interpolated hook, no lyric, nothing that
could be matched. A trailer on Steam and YouTube runs through content ID, and
a track that merely resembles a well-known song in genre is fine while one
that reuses its hook is a claim on the video. Write the ostinato from scratch
in a different interval pattern and the resemblance stays where it belongs.

Cue sheet, mapped to the cut above:

| Time | What the music does |
|---|---|
| 0:00 | Ostinato alone, far back, filtered almost shut |
| 0:08 | Filter opens. Sub bass enters on the first caption |
| 0:21 | Drums. The only place anything approaches triumphant |
| 0:28 | **Everything stops.** One reverb tail over the rebellion |
| 0:31 | Beat returns halved, flat, no bass — the honest stretch |
| 0:47 | Rebuild. Bass back |
| 0:57 | Strip to the ostinato again for the fly |
| 1:07 | Full, warm, resolved — this part is sincere |
| 1:19 | Tail out under the URL |

## What this needs that does not exist yet

1. ~~A 1920x960 timelapse.~~ **Done, and it was never missing.**
   `--export-timelapse` has been in `main.cpp` all along -- at the repo root,
   not under `src/`, which is why a search for callers of
   `exportTimelapseHeadless()` came up empty and this document originally
   called it a blocker. It is fully headless: no window, no display, no audio.

   ```
   build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines \
       --export-timelapse "data/saves/Bench modern CHN (5).odsv" \
       out.gif 1920x960 --no-watermark
   ```

   Takes about two minutes and writes 721 frames. `population` or `troops` as
   a further argument renders those map modes instead. Then, because Steam and
   YouTube want video rather than a 102 MB GIF:

   ```
   ffmpeg -i out.gif -vf "fps=30,scale=1920:960:flags=lanczos,format=yuv420p" \
          -c:v libx264 -preset slow -crf 18 -movflags +faststart out.mp4
   ```

   113 MB becomes 11 MB at 58 seconds.

   **The save is `Bench modern CHN (5)`, and it was chosen by measurement.**
   All 692 saves in `data/saves/` were screened on the last turn's
   `conqueredProvincePrevOwner` and rebellion counts; the three best were
   rendered in full and compared frame by frame against turn 0, after a
   downsample that washes out the unit markers so the number counts territory
   and not blinking icons:

   | save | share of the map that changes hands by turn 720 |
   |---|---|
   | `Bench 1914 FRA (100)` | 8.4% |
   | `Bench modern CHN (36)` | 12.6% |
   | **`Bench modern CHN (5)`** | **15.3%, peaking at 16.1% on turn 432** |

   Nearly double the 1914 game, and the arc builds rather than settling early
   — CHN (36) is already consolidated by its midpoint and then sits still,
   which is the one thing a timelapse cannot survive. What CHN (5) actually
   shows: Russia losing its entire far east, Brazil eaten, the Sahel going to
   one power, Greenland changing hands, and a few hundred ships crossing the
   Atlantic at once in the middle of it. Cutting at turn ~500 keeps the arc
   strictly rising if 58 seconds is more than the edit wants.

   Four clips exist, frame-aligned, so the edit can cut between map modes on
   the same turn: political, `population`, `troops` (all from
   `Bench 1914 FRA (100)`) and the political collapse above.
2. **Screen capture of real play** — panels, orders, a battle resolving. The
   timelapse cannot show the interface, and this is the part a player
   recognises. Has to be recorded by a person. The cut currently stands in the
   docs screenshots with a slow push on each, which carries the pacing but not
   the motion.
3. ~~**The music.**~~ **Written and rendered**: `tools/trailer_music.py`,
   `docs/trailer/assets/score.wav`. Synthesised from nothing -- no sample, no
   loop pack, no MIDI import -- so the content-ID problem the spec worries about
   cannot arise: there is no borrowed material to match. Karplus-Strong for the
   plucked ostinato, detuned saws through a low-pass for the wordless pad, a
   Schroeder reverb for the one tail that has to carry the stop.

   **The grid is offset on purpose.** The cut's hinge is the full stop at 0:28,
   and a bar edge has to land on it exactly or the joke falls on an off-beat.
   At 88 BPM a bar is 2.7272 s, which does not divide 28.0, so the whole track
   starts 0.727 s late and bar 10 lands on 28.000. The two cues after it that
   matter -- the return at 0:31 and the rebuild at 0:47 -- land within a tenth
   of a second.

   Measured against the cue sheet, per band, in dB:

   | section | 30-120 Hz | 300-2.5k | 4-12k |
   |---|---|---|---|
   | 0:03 filtered almost shut | 7 | 10 | -22 |
   | 0:12 open, sub in | 53 | 20 | -1 |
   | 0:23 drums | 53 | 18 | 9 |
   | **0:28 the stop** | **28** | **-5** | **-11** |
   | 0:33 halved, no bass | 45 | 17 | 4 |
   | 0:59 ostinato alone | 29 | 13 | -15 |
   | 1:10 full | 56 | 18 | 11 |

   That is the arrangement doing what the sheet says. It is NOT a judgement of
   whether it sounds good, which nobody has checked, because it was written by
   something that cannot hear it. Listen before shipping.
4. **A voice, or none.** Written for captions, deliberately: no narration to
   record, no translation to commission, and it works muted, which is how it
   will mostly be seen on a storefront.
5. **One shot that cannot be made here: 0:51, "you".** A person at a keyboard.
   It renders as a slate that says PLACEHOLDER in gold, on purpose — a missing
   shot that reads as finished is how a trailer ships with a hole in it.

## Building it

`tools/trailer_build.py` holds the cut above as data and renders it. The table
in the script and the table in this document are the same table; change one and
change the other.

```
python3 tools/trailer_assets.py            # wordmark + platform marks, once
python3 tools/trailer_build.py             # all 21 shots, then concat
python3 tools/trailer_build.py 9 12        # rebuild two shots, reuse the rest
```

Every shot renders to its own 1920x1080 h264 segment with identical encoder
parameters, so the concatenation is a stream copy — no re-encode, no generation
loss, and one shot can be redone without touching the other twenty. 83 seconds,
about 20 MB, under a minute to build.

Decisions the script makes that are not obvious from the cut:

- **The marks are not drawn.** The platform icons are lifted from
  `packaging/web/site/download.html`, which already carries them under their
  own licences, and the browser mark is Lucide's `app-window`, vendored in
  `docs/trailer/vendor/`. `trailer_assets.py` asserts the page still has seven
  icons, so a new platform on the download page stops the build rather than
  silently relabelling every mark.
- **The end card is the name, not the monogram.** `art/logo/od-mark.svg` is
  `O.D.`, which is a favicon. The card at 1:16 exists so somebody can find the
  game again, and a monogram cannot do that, so the full wordmark is generated
  from `make_logo_svg.py` with the same font. The monogram returns at 1:19,
  under the URL, where the name has already been read.
- **Captions carry their own scrim.** They land over a lit map, and the economy
  screenshot has pale text exactly where they sit. The gradient is baked into
  the caption PNG, so no shot needs a second filter pass.
- **Stills are fitted, not filled.** The TempleOS build is 4:3 and was losing
  its menu bar and both poles to a crop before the fit was made explicit.
- **Captions are drawn with PIL, not `drawtext`.** They carry commas,
  apostrophes and newlines, and every one of those is a quoting bug waiting in
  a filtergraph string.

Assembly needs no editor. Adding the music is one more ffmpeg call.

## What it must not become

A feature list. The temptation is to add the map editor, the mod SDK, the
scenarios, multiplayer hosting, the benchmark. All of that is on the store
page. The trailer has one job: make somebody curious enough to click a free
game, and the fly does more of that than any feature.
