# OpenDoctrines

A grand strategy game about running a country: its industry, its armies, its
research, its politics and its neighbours. Six historical scenarios on a
1641-province world map, a map editor for building your own, multiplayer that
needs no port forwarding, and a mod SDK for thirteen languages.

Alpha. See [Status](#status) for what that word is doing here, and for which
platforms have actually been sat down in front of.

![The world map](docs/img/world-map.png)

## What a game looks like

Every turn is recorded, so a finished game can be replayed as a timelapse and
exported as a GIF from inside the game. This is the 1939 scenario with every
country played by the AI:

![Political timelapse](docs/img/timelapse-political.gif)

The same history renders as population and as troop concentration:
[population](docs/img/timelapse-population.gif) ·
[troops](docs/img/timelapse-troops.gif).

## The game

Pick a country and run it. Provinces have population, industry, fortification,
resources and an ethnic composition; countries have a treasury, a research
programme, a political compass, claims on their neighbours and opinions about
each other.

**Provinces and countries.** Industry levels and specialisations, forts,
garrisons, ports and navies. Eight map modes: population, industry, defence,
relations, army navigation, navy, resources, country names.

![Province panel](docs/img/province.png)

**Research.** A tech tree over fortification, industry, ports and more, funded
by a slider against the rest of your budget.

![Research](docs/img/research.png)

**Economy.** Gross income, net income and expenses, per country and globally, so
you can see who is actually winning.

![Economy](docs/img/economy.png)

**Doctrines.** Policies along a left/right and authoritarian/libertarian
compass, with implementation times, ethnic policy, and the unrest that follows a
bad one.

![Politics](docs/img/policies.png)

Rebellions spawn from unrest and hold real territory. Wars pull in guarantee
chains. Ceasefires are negotiated over provinces, money and dropped claims.

### Scenarios

| Scenario | Year | |
|---|------|---|
| Modern Day | 2000    | 185 countries. Population from World Bank totals, deposits from USGS surveys. |
| The Powder Keg | 1914 | Five empires and a continent of alliances waiting on one funeral. |
| The Last Spring | 1918 | Germany has won in the east and is losing in the west. |
| The Gathering Storm | 1939 | Europe on the morning the Wehrmacht crossed the Polish border. |
| Year Zero | 1945 | The war is over and nothing has been settled. |
| The Missile Crisis | 1962 | Two blocs, one ocean between them, and missiles in Cuba. |

### Map editor

![Map editor](docs/img/map-editor.png)

Draw land and sea, cut provinces, place countries, populations, resources and
claims, then export a `.odmap` anyone else can load. There is also a procedural
generator for when you want a world rather than a specific one.

## Multiplayer

![Multiplayer](docs/img/multiplayer.png)

Players dial **out** over `wss://`, so joining needs no router configuration and
works from a browser tab. Hosting is a listening socket on the host's machine,
and the game can open a [cloudflared](docs/multiplayer-hosting.md) tunnel to it
for you — so in practice there is still nothing to forward, but the host does
need to be a desktop build. **You cannot host from a browser.**

The host is authoritative. State only ever flows server to client, orders are
validated server-side and re-attributed to the authenticated player's country,
and a client never computes a turn. A player who disconnects keeps their seat
and their country: the turn resolves without them, and they get that same
country back when they return.

Detail in [docs/multiplayer.md](docs/multiplayer.md), hosting in
[docs/multiplayer-hosting.md](docs/multiplayer-hosting.md).

## Mods

![Mods](docs/img/mods.png)

Mods are WebAssembly, so a mod is the same file on every platform including the
browser, and it runs in a sandbox with a capability grant the player can see and
revoke. The Gearbox SDK has bindings for **C, C++, Rust, Zig, Go, Java, Kotlin,
JavaScript, TypeScript, AssemblyScript, Lua, Python and hand-written WAT** —
every one builds the same example, and the test suite checks that all of them
render byte-identical output.

```bash
tools/gearbox new my-mod        # scaffold (--lang picks the language)
tools/gearbox build my-mod      # compile, pack and verify -> my-mod.odmod
```

[Modding guide](docs/modding.md) · [SDK](docs/gearbox-sdk.md) ·
[ABI reference](docs/gearbox-abi.md) ·
[Troubleshooting](docs/gearbox-troubleshooting.md)

## Installing

### macOS — the first launch needs one extra step

The app is **not signed with an Apple Developer ID**, because the project does
not pay for one. macOS therefore refuses to open it the first time:

> **"OpenDoctrines" cannot be opened because the developer cannot be verified.**

That is Gatekeeper doing its job. It is telling you truthfully that Apple has
not vouched for this app — not that anything is wrong with it. You get to make
that call yourself, once:

**Right-click the app → Open → Open.**

Not double-click. Double-clicking gives you the refusal with no way past it;
right-click → Open gives you the same dialog with an **Open** button on it.
macOS remembers the decision, so every launch after the first is normal.

If macOS says the app is *"damaged and can't be opened"*, that is a different
message with a different cause: the download picked up a quarantine flag that
survived being unzipped. Clear it:

```bash
xattr -dr com.apple.quarantine /Applications/OpenDoctrines.app
```

**On macOS the game does not update itself**, for the same reason. A replaced
binary would not carry the approval you just granted, so the update would break
the install; the update button opens the releases page and you install the new
copy the way you installed this one.

Requires **macOS 11 (Big Sur) or later**, Apple Silicon or Intel.

### Linux

Requires **glibc 2.35 or newer** — Ubuntu 22.04, Debian 12, Fedora 36 and
anything more recent. The published binary is built on Ubuntu 22.04, and a glibc
binary does not run on an older glibc than it was built against, so Ubuntu 20.04,
Debian 11 and RHEL 9 need a build from source rather than the download. Building
from source works fine on all of them.

You also need the runtime libraries raylib links against — X11, ALSA and GL.
Every mainstream desktop install already has them; a minimal or headless install
may not.

Only **x86_64** is published. Arm Linux builds from source; GitHub hosts no Arm
Linux runner, so there is no artifact for it.

### Windows

Requires **Windows 10 or later**, 64-bit. The build is unsigned, so SmartScreen
will show *"Windows protected your PC"* on first run — **More info → Run
anyway**.

## Building

Needs CMake 3.20+ and a C++20 compiler. Everything else is fetched or vendored.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Linux, install the X11/ALSA development packages raylib needs first:

```bash
sudo apt-get install -y libasound2-dev libx11-dev libxrandr-dev libxi-dev libgl1-mesa-dev libglu1-mesa-dev libxcursor-dev libxinerama-dev libwayland-dev libxkbcommon-dev
```

Two features are on by default and can be turned off if their dependencies are a
problem: `-DOD_ENABLE_NET=OFF` (multiplayer, needs mbedTLS) and
`-DOD_ENABLE_MODS=OFF` (the mod runtime). The game builds and plays without
either — the menus say the build cannot do it, rather than the build failing.

### Running

```bash
./build/OpenDoctrines                       # the .app bundle on macOS
./build/OpenDoctrines path/to/save.odsv     # straight into a save
```

Non-interactive modes:

```bash
OpenDoctrines --simulate data/STDmaps/1939.odmap 40 "My World"
OpenDoctrines --export-timelapse save.odsv out.gif 960x480 political
OpenDoctrines --screenshots docs/img save.odsv
OpenDoctrines --train-ai
```

`--simulate` plays a scenario with every country AI-driven and leaves a save
with its full turn history — which is where the timelapses above come from. It
is also the smallest end-to-end check that a build actually works: it loads a
map, resolves turns and writes an archive with nobody at the keyboard.

## Tests

```bash
tests/run_all.sh build
```

Mod archive, runtime, manager and ABI conformance; every SDK's example rebuilt
from source and compared byte for byte; the network protocol against hostile
input; crypto against RFC vectors; a real host and four real players over
loopback; the GIF encoder decoded back with Pillow; and drift checks that fail
if generated bindings, third-party notices or flag licences fall out of date.

Multiplayer specifically:

```bash
tools/playtest.sh            # four windows, four different players
tools/playtest.sh --verify   # the same rules checked with nobody at the keyboard
```

See [docs/multiplayer-testing.md](docs/multiplayer-testing.md).

Every image in this README is regenerated by `tools/screenshots.sh`, against a
throwaway install and a simulated world, so they can be retaken after any change
instead of going quietly out of date.

## Status

Alpha, and the honest version of that word:

- **Platforms.** macOS (arm64) is the one the test suite has been run on end to
  end. Windows, Linux and web are built by CI and are targeted, not yet
  qualified — the four-platform matrix in `.github/workflows/release-game.yml`
  compiles them, and "it compiled" is not "somebody played it". Qualifying them
  means `tests/run_all.sh` and `tools/playtest.sh --verify` passing on each.
- Multiplayer works: hosting, joining, seats, turns, disconnects and reconnects
  are built and tested. There is **no dedicated server** yet — hosting means a
  running copy of the game, and closing it ends the session.
- There is **no tutorial**. Province actions live behind the view tabs on the
  bottom bar, and the game does not currently tell you that.
- Long-form (play-by-paste) turns are designed and not built.

Known gaps are tracked in the issue tracker, kept as a record of what is
actually true rather than what would be nice.

## Licence

[OpenDoctrines Non-Commercial License](LICENSE). Free to play, modify and share
non-commercially; the mods, maps, saves and videos you make are yours.
Third-party data and libraries are credited in [NOTICE.md](NOTICE.md), generated
from `tools/provenance.json` and checked in CI so attribution cannot silently
drift.

[Contributing](CONTRIBUTING.md) · [Code of conduct](CODE_OF_CONDUCT.md)
