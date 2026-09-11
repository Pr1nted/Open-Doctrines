# IGDB

Checked 2026-09-11: searching IGDB for "OpenDoctrines" returns **0 results**.

## Why this one before the others

IGDB is the database Twitch builds its game directory from. A game with no IGDB
entry has no Twitch category, which means a streamer who wants to play this one
has nothing to set their stream to. They end up under a generic category where
nobody browsing finds them, and no OpenDoctrines directory page ever exists for
viewers to accumulate on.

So this is not a listing that competes with writing to streamers. It is the
thing that has to be true before writing to them is worth doing at all.

It also feeds a long tail of trackers, launchers and "what should I play" apps
that read IGDB rather than maintaining their own catalogue.

Submissions are open to any logged-in account, at <https://www.igdb.com/games/new>.
Entries are reviewed by moderators, so expect a wait and expect edits.

## The form

IGDB uses controlled vocabularies for most of this: pick from the dropdown, do
not type a synonym. The values below are the exact ones IGDB uses -- they were
read off an existing entry rather than guessed.

| Field | Value |
|---|---|
| **Name** | `OpenDoctrines` |
| **Release date** | `2026-08-02` — the first public tag, v1.0.3a, and the day itch traffic starts |
| **Status** | `Alpha` (or `Early Access`; use whichever the version string says at the time) |
| **Developer** | `Pr1nted` |
| **Publisher** | `Pr1nted` |
| **Platforms** | `PC (Microsoft Windows)`, `Mac`, `Linux`, `Android`, `Web browser` |
| **Genres** | `Strategy`, `Turn-based strategy (TBS)`, `Simulator`, `Indie` |
| **Themes** | `Historical`, `Warfare`, `4X (explore, expand, exploit, and exterminate)`, `Sandbox` |
| **Game Modes** | `Single player`, `Multiplayer` |
| **Player Perspectives** | `Bird view / Isometric` |
| **Game Engine** | `raylib` |

### Summary

> OpenDoctrines is a free grand strategy game about running a country: its
> industry, its armies, its research, its politics and its neighbours. Six
> historical scenarios play out across a 1,641-province world map, from 1914 to
> the present day, and a map editor covers the ones that do not ship.
>
> Provinces carry population, industry, fortification, resources and an ethnic
> composition; countries carry a treasury, a research programme, a political
> compass and opinions about each other. Industry is bounded by the ground it
> stands on, so building an industrial power means holding the places worth
> industrialising rather than buying the same factory everywhere.
>
> Unusually for the genre it runs in a browser tab with no download and no
> account, as well as on Windows, macOS, Linux and Android. Multiplayer joins by
> invite code and needs no port forwarding. Mods are compiled to WebAssembly
> from thirteen languages and run sandboxed. The interface is translated into
> 21 languages. It is free, its source is public, and there is nothing to buy
> inside it.

### Keywords

`grand strategy`, `alternate history`, `world war ii - ww2`, `cold war`,
`nation building`, `politics`, `economy`, `management`, `moddable`,
`browser game`, `wargame`, `turn based`, `map editor`, `open world`

### Websites

| Type | URL |
|---|---|
| Official | `https://opendoctrines.pages.dev` |
| itch.io | `https://pr1nted.itch.io/open-doctrines` |
| GitHub | `https://github.com/Pr1nted/Open-Doctrines` |
| Discord | `https://discord.gg/wqS65jzVv5` |

## Assets to upload

| Slot | File | Notes |
|---|---|---|
| Cover | `docs/steam/banner-steam-library.png` | 600x900. IGDB covers are portrait, roughly 3:4; this is the only portrait asset that is not a tiny capsule. Rebuild with `python3 tools/banner.py --size steam-library --region atlas --out-dir docs/steam`. |
| Screenshots | `docs/img/world-map.png`, `province.png`, `economy.png`, `research.png`, `policies.png`, `comms.png` | 1600x900, already the press kit's selection. |
| Video | — | None yet. The timelapse GIFs are not video and IGDB wants a YouTube link, so leave it empty rather than fake it. |

Do not upload `docs/img/map-editor.png`: it is the editor's menu on a black
background and shows nothing. Same reason the press kit leaves it out.

## Say what it is, not what it is not

The licence is source-available, not OSI open source, and IGDB has no field for
either. Do not put "open source" in the summary or the keywords to fill the
gap -- it is the one claim about this project that is contested, and a database
entry is exactly where a wrong one gets copied from.

## After it is live

Two things become possible that are not possible now:

1. A Twitch category exists, so a stream can be tagged with the game and turns
   up for anybody browsing it. Check with
   `https://www.twitch.tv/directory/category/opendoctrines`.
2. The entry propagates to the trackers and launchers that read IGDB, with no
   further submissions.

Neither needs maintaining. Re-check the entry after a version bump only if the
platform list or the status changes.
