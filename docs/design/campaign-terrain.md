# A map that changes under you: terrain morph as a campaign mechanic

Noted 2026-11-06, for 1.4.0b. Not scheduled, not started.

## The capability nothing uses yet

1.3.3b shipped `morph` and `terrain` to map scripts (OD/MapEngine/4) and to
mods through `Map.Write`:

```
terrain land svg "PATH"    # raise sea to land, while the game is running
terrain sea  svg "PATH"    # sink land to sea
morph PROVINCE svg "PATH"  # hand those pixels to a province
```

Borders, neighbours, the raster and the labels all follow on their own. As of
this note **no shipped content uses any of it** — it is the newest and most
visually dramatic thing in the engine and it has zero scenarios behind it.

## Why it belongs in campaigns rather than in a one-off scenario

The first instinct was a standalone Atlantropa map as marketing: dam Gibraltar,
drain the Mediterranean over forty turns, post the timelapse. That is a good
*advert* and a thin *game* — the map changes whatever the player does, so it is
a thing you watch rather than a thing you play.

The same capability inside a campaign is the opposite. A map that changes
**because of what the player did**, or that changes and forces a response, is a
mechanic no comparable game has:

- a campaign objective whose completion raises the dam, and the sea retreats
  over the turns that follow, opening land the player then has to hold;
- a slow drowning the player is racing — coastline provinces lost on a
  schedule, so the economy has to move inland before it is underwater;
- a canal, a strait closed or opened, a river diverted, as the reward for a
  campaign rather than as scenery.

Campaigns are where this earns its keep, because a campaign already supplies
the reason, the pacing and the lore that a bare morph lacks.

## What is cheap and what is not

**The paths are mechanical, not artistic.** `land_sea.png` already knows
exactly which pixels are the Mediterranean, the North Sea or a coastal plain,
so a basin outline can be traced out of the raster and emitted as an SVG path
by a tool. The part that looks like a week of drawing is a scripting problem.

**The marketing artefact is free.** The GIF timelapse export already exists, so
a twenty-second loop of a sea retreating or a coast going under falls out of
the feature rather than being made separately. It needs no caption and carries
itself, which is more than a changelog does.

Worth knowing before building: `terrain sea` clears the province on those
pixels, `morph` skips ocean, and raising land needs `terrain land` first and
then a `morph` to give it an owner — there is never ownerless land. See
docs/scripting.md, "Reshaping the map".

## Status

Idea only. Recorded because the capability is new, unused, and easy to forget
by the time campaigns are actually being written.
