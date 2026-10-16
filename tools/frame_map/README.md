# Frame Map: a video as a map

`make_frame_map.py` turns a video into a `.odmap`. Each square province is one
pixel. Two countries, BLK and WHT, own the black and white pixels, and every
processed turn shows the next frame. The animation is an ordinary map script
(`scripts/frames.txt`, OD/MapEngine/3), so the engine is not changed and no
mod is needed. The tool needs ffmpeg and the Python standard library, nothing
else.

```
python3 tools/frame_map/make_frame_map.py my_video.mp4 --size 64x48 --fps 10 --max-frames 600
```

This writes `data/custom_maps/Frame_Map.odmap`. Open it from **Play Singleplayer
→ New World**, where it is listed as "Frame Map". Choose **Spectator** and
press End Turn to step through the frames. If you pick BLK or WHT instead, an
orders-review phase comes up each turn before the map repaints.

Options: `--threshold 0-255` (luma below it is black, default 128),
`--invert`, `--start S` and `--duration S` (cut a clip), `--no-loop` (stop on
the last frame; the default loops), `--name`, `--black`, `--white` and
`--pad-color` (colours), `-o PATH`.

Testing without a video: `--demo N` builds N frames of a bouncing ball, and
`--demo-video out.mp4` writes that animation as a video file. Add `--probes 8`
to make the script print eight cell owners every frame and to write
`<map>.expected.json`. Then run:

```
caffeinate -u -d -i ./build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines \
    --ai-readonly --simulate path/to/map.odmap 20 FrameTest > run.log
python3 tools/frame_map/verify_frame_map.py run.log path/to/map.expected.json
```

`--simulate` leaves a save, `data/saves/FrameTest*.odsv`. Delete it when you
are done.

## What this ships, and what it does not

**The tool converts a video you already have. It carries no video, and the map
it writes is not committed** -- `data/custom_maps/` is in `.gitignore`. That
distinction is the whole of the licensing position: a converter is not the
thing it converts, in the same way an encoder is not a film.

A generated `.odmap` IS the source video, re-encoded as province ownership.
So if you build one from somebody else's work, treat it as you would a copy of
that work: show it, screenshot it, post a clip of it -- but do not redistribute
the map file, and credit whoever made the thing you fed it.

The worked example is Bad Apple!!, which is three separate people's work and is
usually miscredited as one:

- the **song** is ZUN's, the stage 3 theme of *Lotus Land Story* (1998);
- the **vocal arrangement** everyone knows is Alstroemeria Records (2007),
  arranged by Masayoshi Minoshima, lyrics by Haruka, sung by nomico;
- the **shadow-art video** was made over 2008-09 by a group led by Anira, and
  posted to Nico Nico Douga on 26 October 2009.

One practical note that is not a legal one: if you post a video of a frame map
WITH its audio, expect an automated content claim on the arrangement. A silent
clip or a still avoids it, and this README is not legal advice.

## Layout

- The world is 8192x4096 like every map. The grid is W·k × H·k pixels, where k
  is the largest whole cell size that fits (85 px at 64x48), and it is
  centred.
- The space left over is **land** owned by the game's own Blocked Territory
  sentinel (BLC, cid 65535), split into up to four strip provinces. Nobody can
  enter, colonise or conquer BLC land. The map has no sea. At 64x48 the strips
  are 1376 px on the left and right and 8 px at the top and bottom. A 2:1 grid
  such as 128x64 has no padding at all.
- Cell (x, y) is province `1 + y*W + x`. The padding strips come after those.
- Frame 0 is baked into the starting owners. BLK and WHT start allied, with no
  armies, zero treasury and 1000 people per province, and the script turns
  rebellions off.

## Script shape

For each frame f:

```
waitUntil map.turn >= var.base + f
set province.<id>.owner BLK|WHT      # only the cells that changed
print "[FRAMEMAP] frame f ..."
```

It ends with `set var.base = var.base + N` / `jump loop`.

The language has no substring or other string functions. A reference's
province id must be literal digits, and `{}` interpolation only works in
`print`, so run-length or base-64 data cannot be decoded inside the script.
One `set` line per changed cell is the most compact form the engine can run,
and it has no loops, so the iteration caps never apply.

The waits are absolute rather than `wait 1 turn` because a loaded save
restarts its entry scripts from the top. An absolute wait catches up in one
burst on the next turn.

## Cost (M-series Mac, `--simulate`)

| | s/turn |
|---|---|
| 64x48 static (no flips) | 0.04 |
| 64x48 ball, ~220 flips/frame | 0.28–0.32 |
| 32x24 ball, ~56 flips/frame | 0.18 |

Profile at 64x48:
- About 38% is the political-map repaint (`odBuildGradientField` over the full
  8192x4096 raster). It runs on any turn where an owner changed, whatever the
  grid size.
- About 31% is the owner transfers. Each one calls `reconcileDistricts` twice,
  which costs O(country size), so per-flip cost grows with the grid.

Script size is about 28 bytes per flipped cell. A 2000-frame 64x48 clip with
643k flips is an 18 MB script inside a 2 MB `.odmap`. It costs about 0.4 s
extra at load, and turn time does not depend on script length.

Limits: W·H + 4 must stay under 65535 (save ids are uint16). Going much beyond
about 160x120 makes each turn slower, because of the per-flip cost above.
