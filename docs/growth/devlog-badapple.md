# Devlog: Bad Apple on the world map — paste-ready

**Where:** itch dashboard → **Devlog → New post**. Body in HTML or Markdown.

**Banner:** `docs/itch/` style, built with `tools/banner.py` over a frame map
whose starting owners are one Bad Apple frame. Upload it in the devlog editor's
image button — no push needed. Frame 1910 is the one that survives a wide crop.

**Title:** Bad Apple runs on the Open Doctrines world map

---

Hello strategists!

Bad Apple plays on the map. All 2,191 frames of it, 3 minutes 39 seconds, one
frame per turn.

### No engine change, no mod

That is the part I am happiest about. It is a map script — the same scripting
any scenario uses, `set province.ID.owner`, `waitUntil`, `jump`. The game does
not know it is playing a video. If you have a copy of Open Doctrines you
already have everything needed to run it.

### Every province is a pixel

The grid is 64×48, so 3,072 provinces, each a square. There are two countries,
**BLK** and **WHT**, and they own the black and the white pixels. A frame is
nothing more than a redraw of who owns what, and the political map does the
rest.

They start allied, with no armies, no money and rebellions switched off,
because a war would be visible.

The letterboxing on either side is the game's own **Blocked Territory** — the
sentinel that means "nobody may enter this". The borders of the video are
literally unconquerable land.

### The script is 491,780 lines, and it has to be

The scripting language has no loops over data and no string functions. A
province id in a reference must be literal digits, and `{}` interpolation only
works inside `print`. So there is no way to decode run-length or base-64 data
from inside a script — the most compact thing the engine can actually execute
is one line per changed pixel.

Which gives: **491,780 `set province.N.owner` lines, 13.9 MB of text.** It
compresses to 1.38 MB, which is 88% of the finished 1.58 MB map. The map is
mostly script.

It costs about 0.4 s extra at load and nothing per turn — turn time depends on
how many pixels change, not on how long the script is.

### What it taught me about my own game

Profiling a turn: about **38% of it is repainting the political map** over the
full 8,192×4,096 world raster. That cost is identical whether the grid is 64×48
or 16×12, because it redraws the *world*, not the grid. Another 31% is the
ownership transfers themselves, where each one reconciles districts twice.

Abusing a feature is a good way to find out what it actually costs.

### Do it with your own video

```
python3 tools/frame_map/make_frame_map.py my_video.mp4 --size 64x48 --fps 10
```

ffmpeg and the Python standard library, nothing else. It writes a `.odmap` you
open from New World. Pick Spectator and hold End Turn.

The ceiling is the save format: province ids are 16-bit, so width × height has
to stay under 65,535 — around 160×120 before each turn gets slow.

### Credit where it is due

Bad Apple!! is three people's work, and is usually miscredited as one:

- the **song** is ZUN's, the stage 3 theme of *Lotus Land Story* (1998);
- the **vocal arrangement** everyone knows is Alstroemeria Records (2007),
  arranged by Masayoshi Minoshima, lyrics by Haruka, sung by nomico;
- the **shadow-art video** was made over 2008–09 by a group led by Anira, and
  posted to Nico Nico Douga on 26 October 2009.

None of it ships with the game. The tool converts a video you already have, and
the map it builds stays on your machine — what is in the repository is the
converter, not the film.

### Closing remarks

*(Pr1nted — yours.)*
