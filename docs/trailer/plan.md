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

## Look

The site's stylesheet calls it "a lamplit map table", and the trailer is that.
Nothing is invented for it.

| | |
|---|---|
| Ground | `#090B11` — the site's `--sunken`, not black |
| Text | `#E8E4DA` — warm, "this is a paper-and-maps game" |
| Accent | `#C9A227` — the gold, used sparingly and never on two things at once |
| Sea | `#16202E` |

Captions are the game's own font, left-aligned, held long enough to read twice.
No motion graphics, no whoosh, no lens flare. The map moves; the type does not.

## The cut

Times are where a beat LANDS, not where a clip starts.

| Time | On screen | Caption | Source |
|---|---|---|---|
| 0:00 | Black, then the 1914 map fades up under a slow push | — | timelapse, frame 1 |
| 0:04 | | A GRAND STRATEGY GAME | — |
| 0:08 | Map zooms from the world to one province's panel | 1,641 provinces | `world-map.png` → `province.png` live |
| 0:13 | The research tree, panning | 86 technologies | `research.png` live |
| 0:17 | Politics compass, a doctrine enacted | 59 doctrines | `policies.png` live |
| 0:21 | Armies march, a border moves | Your orders are carried out | army view |
| 0:25 | **A rebellion spawns. Half the country turns red.** | — | timelapse, a real collapse |
| 0:28 | Hold. Music stops dead. | By someone | — |
| 0:31 | Quick cuts, one per caption, deadpan | Industry you cannot afford | economy view |
| 0:35 | | Allies who will not come | diplomacy panel |
| 0:39 | | A war you started on turn three | war declaration |
| 0:43 | | A country that no longer exists | timelapse, a nation eaten |
| 0:47 | Wide: the whole world convulsing, sped up | It is played by | timelapse at speed |
| 0:51 | A person at a keyboard — ordinary, unremarkable | you | capture |
| 0:54 | The multiplayer lobby, seats filling | your friends | `multiplayer.png` live |
| 0:57 | **The fly at its desk, brain lit beside it** | and a fruit fly | Open Fly |
| 1:02 | Hold on the fly. Let it sit. | 138,639 neurons. It is not good at it. | Open Fly |
| 1:07 | Platform marks, one at a time | Windows. macOS. Linux. A browser tab. | icons |
| 1:12 | **The TempleOS build, 16-colour, booting into the map** | And an operating system one man wrote for God. | `templeos-game.png` live |
| 1:16 | Gold wordmark on the sunken ground | Free. All of it. | logo |
| 1:19 | | opendoctrines.pages.dev | — |

The two held beats — 0:28 and 1:02 — are the trailer. Everything else is
pacing between them. Resist shortening either.

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
       --export-timelapse "data/saves/<a 120-turn bench save>.odsv" \
       out.gif 1920x960 --no-watermark
   ```

   Takes about two minutes and writes 721 frames. `population` or `troops` as
   a further argument renders those map modes instead. Then, because Steam and
   YouTube want video rather than a 102 MB GIF:

   ```
   ffmpeg -i out.gif -vf "fps=30,scale=1920:960:flags=lanczos,format=yuv420p" \
          -c:v libx264 -preset slow -crf 18 -movflags +faststart out.mp4
   ```

   102 MB becomes 6.6 MB at 58 seconds. `data/saves/` holds hundreds of
   finished bench games to choose a collapse from.
2. **Screen capture of real play** — panels, orders, a battle resolving. The
   timelapse cannot show the interface, and this is the part a player
   recognises. Has to be recorded by a person.
3. **The music.** Spec above.
4. **A voice, or none.** Written for captions, deliberately: no narration to
   record, no translation to commission, and it works muted, which is how it
   will mostly be seen on a storefront.

Assembly is ffmpeg once the pieces exist; nothing here needs an editor.

## What it must not become

A feature list. The temptation is to add the map editor, the mod SDK, the
scenarios, multiplayer hosting, the benchmark. All of that is on the store
page. The trailer has one job: make somebody curious enough to click a free
game, and the fly does more of that than any feature.
