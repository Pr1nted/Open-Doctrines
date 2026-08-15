# Translating worlds to Greater Diplomacy 5

**Experimental.** Off in every build, and off again inside the builds that have
it, until a player turns it on.

Open Doctrines can write a world out as a map for
[Greater Diplomacy 5](https://github.com/DasVerhaltnis/Greater-Diplomacy-5), and
read one of its maps back in. The conversion is done by
[open-dragoman](https://github.com/Pr1nted/dragoman), a separate MIT library
with no code from either game in it.

Neither game's maps are redistributable through the other, and nothing here
copies one. It converts what a player already has.

## Turning it on

Two switches, deliberately:

1. **Build with it.** `cmake -B build -DOD_ENABLE_GDTL=ON`. Off by default —
   the option fetches open-dragoman at a pinned tag, and a feature behind two
   switches is not worth adding to every build.
2. **Switch it on in the game.** Settings → Experimental → **GDTL: On/Off**.
   In a build without the library the option says so and refuses to turn on.

## Sending a world to Greater Diplomacy 5

New World → the **i** on any map → **Translate**.

The first thing that appears is a warning, and it cannot be skipped. It says
what is about to happen, because translation is lossy in ways that depend on the
map: fortification and port technology have no counterpart in the other game,
ocean provinces are invented because that game cannot draw or sail across water
that is not a province, and several research nodes do not correspond. Whatever
could not cross cleanly is listed afterwards, in the result dialog.

Then, where it should go:

| Where you are | What you get |
| --- | --- |
| Web | The map is built in the browser's own filesystem, zipped, and downloaded. Unzip it into that game's `base_maps` folder. |
| Desktop | **Save to a folder…** — anywhere you like, through the system's file chooser. |
| Desktop | **Install into that copy** — straight into `base_maps` of a Greater Diplomacy 5 installation, ready to appear in its map list. |

Nothing already on disk is overwritten. If something exists at the destination
the conversion stops and says so.

### Finding the other game

**The game does not look at your disk for other software unless you press the
button that says it will.**

Press **Search for the game** and it checks a fixed list of the places that game
is normally installed — the standard program directories, Steam's `common`
folder, and `Documents` / `Downloads` / `Games` under your home directory — one
level deep, and only opening subfolders whose *name* already mentions Greater
Diplomacy. It is not a recursive walk of the disk, it asks for no elevated
permission, and it reads nothing outside those folders. The dialog says how many
places it will look before you press it.

A folder counts as an installation only if it has both `base_maps/` and
`data/json/research_template.json` — that second file is what open-dragoman
reads technology ceilings from, so a folder without it could not be translated
into properly anyway.

**Point at it myself…** skips the search entirely. Whatever is found or chosen
is shown, and only then saved to `gd5Path` in `config.json`. **Forget that path**
clears it.

## Bringing a map back

New World → Custom → **Import Greater Diplomacy 5 map**, which appears next to
the ordinary **Import .odmap** card while GDTL is on.

That game keeps a map as a *folder*, so this asks for a directory rather than a
file. It is converted to an `.odmap` and handed to the importer you already
know — same name prompt, same validation, same thumbnail.

## What survives

Provinces, borders, owners, nations and their colours and flags, alliances and
wars, the date, and scripted events as far as the two scripting systems overlap.

Anything the destination game has no field for is written into a **sidecar** file
beside the map. Both games read a fixed list of filenames and ignore everything
else, so a Greater Diplomacy 5 map carrying Open Doctrines' ethnic minorities
loads there exactly as it would without them — and translating back returns
them. That is what makes a round trip lossless rather than merely close.

Research is the awkward one. Open Doctrines stores none in a map — its tree is
compiled into the game and seeded from a hardcoded list of ISO codes — so a map
crossing *into* Open Doctrines carries `research.json`, which **this game does
not read yet**. It is carried, not applied. open-dragoman's `docs/research.md`
sets out the change that would make it take effect.

For the full field-by-field account, see
[What Crosses](https://github.com/Pr1nted/dragoman/wiki/What-Crosses).

## Where things go

| | |
| --- | --- |
| `gdtl` | `data/config.json` — the Experimental toggle |
| `gd5Path` | `data/config.json` — the installation you pointed at, empty until you do |
| `data/gdtl_work/` | scratch space for a conversion; rebuilt each time |
