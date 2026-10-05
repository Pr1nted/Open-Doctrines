The first beta. Two hundred and eighty commits since 1.2.2a, and the reason
for the letter change is not a feature list: it is that the things a strategy
game is judged on — an economy with scarcity in it, a war you can read, a map
you can share, a build that installs on the machine somebody actually owns —
are all present now, and the parts that are missing are known and written down
rather than discovered by the person playing.

Alpha meant "the shape is not settled". Beta means it is. Saves made now are
expected to keep working.

## What a turn is about now

- **Monuments.** Eleven of them, each doing a different thing, built on a
  province and held with the land rather than by the country that raised them.
  They cost rent for as long as they stand, which is what stops the answer
  being "build more": a monument you cannot pay for is a monument that drags.
  They appear on both maps — an icon on the flat one, a figure standing on the
  globe — and the AI does not build them, which is stated on the screen rather
  than left for you to infer.

- **A goods economy with scarcity in it.** There is a world raw market now, so
  a country with no metal can still build machinery by buying it. Surplus goods
  flow to friendly neighbours behind their own gate, and a trade deal can move
  them across a border. For three releases running, the only thing anybody made
  was food; that is fixed, and the goods column no longer reads 0% for goods
  that were working.

- **Sector taxes.** Tax or subsidise each specialisation, inside ceilings your
  doctrines set. The rate in force moves two points a turn towards the one you
  set, so it is a decision with a lag rather than a slider. Taxing a sector
  makes its goods scarcer, which is the half of it a player asked for.

- **A navy that exists.** Its own view, its own hulls, its own levers, and a
  sea for the fleets to cross. Coastal checks, harbours and ship placement now
  read each province's own pixels instead of a guess.

- **A war you can read.** The game says what a battle would be before it is
  fought and what it was afterwards, shows how wide the front is, marks
  contested ground on the map and reads it over the whole fight, and says what
  a troop type fights like rather than only what it costs. A next-turn
  re-attack is refused when nothing about the fight has changed, and the AI
  stopped re-ordering battles it had already ordered — which was two thirds of
  its attack orders.

- **Achievements.** A catalogue, a screen, toasts, and the statistics behind
  them. Only grants signed by the account service count.

- **Regional laws, parties and comms**, with both shipped-data lists checked
  against each other so they cannot drift apart.

## Where it runs

- **FreeBSD and OpenBSD**, green end to end, built and tested in real BSDs.
- **Linux on arm64, armv7, riscv64 and x86**, as an AppImage, a .deb, an .rpm
  and a Flatpak, all of which install, run and uninstall on real Linux.
- **Windows on x64, ARM64 and Win32**, with the installer tested on a real
  Windows machine — which found two bugs in an afternoon that CI could not see,
  because CI runs on the machine that built the thing.
- **Android**, on a real device in CI, no longer upscaling itself.
- **TempleOS.** Not a joke build: it draws the map, plays a real turn, takes
  the engine's answer, and talks to a network. Nineteen commits of it.
- **The web build**, with the site publishing itself from release CI and
  stating the version it ships.

## Multiplayer

Network protocol 2, and refusals that say why instead of decoding as Unknown.
Cross-play between two machines needs no account at all. A world too large to
send is refused with a reason rather than emitted; a large message goes in
fragments rather than one frame a tunnel has to buffer; a peer closing its
socket no longer kills the process it was talking to. Thirty commits here, most
of them things that only appear when the two ends are on different computers.

## Under it

- The menu is down to 190 MB, the map no longer re-uploads itself every turn,
  the layers are painted on the GPU from small tables instead of full-map
  images, and saves are read from disk instead of whole into memory.
- A new game reused the last game's seed, so the AI replayed itself. Fixed.
- Renaming a world deleted it. A world name is a filename, and Windows refuses
  five characters we allowed. Both fixed, and a save that loses count of its
  own turns can be recovered rather than coming back as a new world.
- The timelapse export reported success over a truncated GIF. Every write is
  checked now: a failed export exits non-zero, says why, and removes the
  partial file. 3840x1920 exports work and are verified end to end.
- **A gate in front of the tag** that runs here rather than on the machine that
  built the thing: the whole suite, the packages, real guests, two machines
  playing each other, saves, mods and Android. Everything in this release went
  through it.

## A launcher, if you want one

Unifico 0.1.0 ships alongside this release, from its own repository
(Pr1nted/Unifico). It installs the game and keeps it up to date, keeps several
versions side by side, signs you in once, and keeps your worlds, mods, servers
and achievements in one place. It also carries the TempleOS edition behind a
single Play button.

It is optional and the game does not know or care whether it was started from
there. The plain download has not moved, and on itch both are on the same page.

## Still not done

- The AI does not build monuments.
- Steam is blocked on account verification, not on the build.
- The fitted-seed AI work is measurement, not a shipped improvement; the
  benchmark tab shows the numbers rather than claiming them.
