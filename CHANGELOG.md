# Changelog

## Licence

Nothing to install: this changed in the repository, between releases, and
applies to anyone opening a pull request.

**The licence is version 1.1, and contributions are assigned rather than
licensed.** Clause 4 used to say a contributor keeps the copyright in their
patch; it now points at [CLA.md](CLA.md), which assigns it to the project —
with a licence back, so a contributor keeps the right to use their own work
anywhere else, commercially included. The reason is that clause 5 lets the
licence change later, and that power reaches only rights the project holds.

Playing is unaffected, and so are mods, maps, saves and videos: clause 3 still
says those are yours, and the CLA says it again. Every commit in the repository
predates this, which is the only moment such a thing can arrive without asking
somebody to re-sign for work they already gave.

## game 1.1.0a

- **Twenty-one languages** — interface, country names and dialogue. Arabic,
  Hindi and Urdu are properly shaped, not drawn letter by letter.
- **Advisors and officers speak to you**, drawn in your country's own colours.
- **A tutorial**, with a map built for it.
- **The AI holds its ground when attacked.** Recruits come out of a province's
  population, its action cap is gone (requests are limited instead),
  coalitions no longer form on the hardest difficulty, and it fortifies a
  threatened border. Where the last build was reduced to almost nothing in a
  world that attacks without pause, this one keeps its starting share — rules
  and trained model both changed, so that is the two together.
- **A score you can measure yourself against.** `tools/od_bench.py` plays six
  fixed seats; 100 means a seat kept its ground. The AI scores 107: strong in
  a peaceful world (Sweden, France 200), weak in a hostile one (China 8).
- **Map scripting, version 2.** Conditions are real expressions now —
  arithmetic, `and`/`or`, parentheses, `min`/`max`/`clamp` — where before a
  condition was one comparison and nothing else. New: `elseif`, `unless`,
  `for i = 1 to N`, `repeat N`, `break`, `continue`, `print`, and
  `set x = ...` with `+=`. Version 1 scripts run unchanged. Fixed: an `if`
  with an `else` ran both halves.
- **Scripts can be edited as blocks.** A Text/Blocks toggle in the map
  editor's script IDE: drag statements around, add them from a palette, and
  switch back. The two views are the same file — a script that goes to blocks
  and back is byte for byte what it was.
- **Greater Diplomacy 5 translation.** Exporting a map in the browser no
  longer loses large worlds: the download link was being released in the same
  tick it was clicked, before the browser had finished reading it.
- Stability and assorted bug fixes.

## game 1.0.8a

- **Trade deals.** "Propose Trade" sits in the peacetime diplomacy list:
  provinces, money and claims moving both ways by agreement. A ceasefire
  without the war. The AI proposes them too, and prices land at what it earns
  rather than by counting provinces.
- **Flags are recoloured, not replaced.** A country whose politics move keeps
  its own flag, shifted toward the palette its politics imply, so reverting is
  exact. Seven symbols that shipped with nothing able to name them — anchor,
  torch, rose, fasces, cross pattée, four-pointed star, star of David — can be
  used now, and one device means a government where several mean a union.
  Previously every ideology reached for the same star.
- **Rebellion names.** 18.5% of generated breakaway names were malformed —
  "Mestizo Mexicia", "Han Chin". They are named after places now.
- Countries rename and restyle themselves as their politics move.
- **Quick Start** on the main menu puts you straight into a turn.
- Ceasefire offers can be any amount you actually have, including nothing.
- **Experimental:** translate maps to and from
  [Greater Diplomacy 5](https://github.com/GitGetGot415/Greater-Diplomacy-5).
  Off by default; Settings → Experimental. Lossy, and it says what it lost.
- Fixed: a way out of the multiplayer screen that is not Escape, Research
  greyed for spectators, and loading a scenario no longer asks a phone for
  1.7 GB.
- **Smaller, byte for byte the same game.** The download is 58 MB where it was
  62, and an installed copy is 9.2 MB lighter. The trained AI model is nine
  megabytes of float weights, and a plain deflate pass barely dents them:
  consecutive weights share a sign and an exponent byte and share nothing at
  all in the low mantissa — separate the four byte positions into their own
  streams first and it is 4.0 MB. The worlds, the flags, the app icon and the
  screenshots were each letting whatever wrote them pick a PNG scanline filter
  by a rule meant for photographs; flat art wants no filter, and the province
  layer alone was paying 18% for the wrong guess. The soundtrack, two thirds
  of the download, gave up 4.3 MB to OptiVorbis once a two-line bug in
  stb_vorbis was fixed: it read a zero-size allocation as a failed one, and so
  refused every optimised file. The zip is now built with zopfli, an ordinary
  zip found by searching harder for it, and the linker drops the code nothing
  reaches. Every map, every flag, every screenshot and every sample is exactly
  what it was: the encoders decode what they are about to write and compare it
  before it replaces anything.
- Windows builds in about half the time.

Not yet: the dedicated server has no downloads, and long-form turns are built
but never played across a real campaign.

## game 1.0.7a

The browser build is a ninth of the size it was, the borders are surveyed
rather than traced, armies have a button, and there is a dedicated server.

## The browser build

It used to download 69 MB before the menu drew, behind a canvas that stayed
black for all of it. It is now 12 MB, and it tells you what it is doing while
it works.

The six scenarios and the trained AI model are no longer in that download. None
of them is read until a player has picked a world, so preloading all six meant
waiting for five worlds nobody asked for; they are fetched when something asks
for them instead. The font was 11 MB of Unifont to draw about six hundred
characters, and is now a 147 KB subset of exactly those. The menu no longer
opens a scenario archive to find its own background.

**Settings and saves survive the tab now.** The web build's data lived in the
page and nowhere else, so closing it, reloading it, or letting the browser
reclaim it took the config, every save, every custom map and every installed
mod with it -- and said nothing, because every write had succeeded. They are
kept in the browser's own storage and restored on the next visit.

## The map

The borders of eleven countries and regions are now cut from OpenHistoricalMap's
surveyed outlines instead of being traced by hand: the German-Polish frontier,
Austria-Hungary, Finland, Hungary, Turkey, Asia, South America, Bhutan, Ecuador
in 1939, the inner-German border and Luxembourg.

The archives are also a quarter of their old size -- 32.6 MB down to 7.6 MB --
by encoding the layers as indexed images rather than truecolour. They decode to
the same pixels; a land/sea layer answers one question per pixel and was being
stored as four bytes of it.

The flag artwork went from 12 MB to 5.7 MB the same way, and three flags that
were drawing wrong are fixed. Belize, Bhutan and the Kingdom of Serbia carried
their fills in a stylesheet the renderer does not apply, so Serbia drew as a
black field instead of a tricolour and Belize drew a black disc where its arms
should be.

## Playing

**Armies have a button.** Moving one meant holding the army-move key and
dragging, which is discoverable only by reading the keybinds, and players
reasonably concluded armies could not be moved at all. The province panel now
has a Move Army button; the label carries the keybind too, so the faster way is
learned from the slower one rather than instead of it.

**Disband All and Scrap All**, with the order counts and a way to cancel them.
Bulk upgrade and bulk specialise for provinces. Resource income is shown per
province, with the specialisation boost broken out.

**Population growth is a rule of its own.** It used to be a side effect of the
default deportation policy, which reached only provinces that had a minority
and scaled with how many -- so an ethnically homogeneous province never grew at
all, and a three-minority province grew three times as fast as its neighbour.
Every province now grows once a turn, at a rate research modifies rather than
provides.

Diplomacy refuses what it used to accept twice: an offer already awaiting an
answer, a second round of talks in one turn, a declaration already queued.

## The AI

It was being punished for making peace. A ceasefire that landed cost the war
module half a point on top of the reward for the peace itself, and a ceasefire
that was refused cost it half a point for nothing. It was also charged for its
own conquests, so keeping what it had taken read as a loss. Both are fixed, and
the model shipped here is the one worker from an overnight pool that beat its
own starting point.

## The dedicated server

A console server that needs no graphics card, no display and no X11 -- it
compiles the same simulation as the game against a raylib of its own, so the
two cannot disagree about the rules, and the binary links nothing that draws.

**It is released separately, on its own tag and its own schedule.** A VPS
operator should not download a few hundred megabytes of artwork to run
something that never draws a pixel.

## Fixed

**The Discord and GitHub buttons did nothing** on Windows, Linux and in a
browser. They ran a macOS command, so the only route from the game to its
community worked on one platform and failed silently on the three where nearly
every player is.

**The main menu overlapped itself** on any window shorter than about 790 pixels
-- 720p, a laptop with browser chrome, the store page's embed -- drawing the
first menu item straight through the subtitle. The header now lays out in the
room the buttons leave.

**The map list had never been read from the file that describes it.** It was
looked for one directory up, under field names the generator does not write, so
every launch on every platform fell through to opening all six archives to find
out what they were.

## sdk 1.1

Gearbox 1.1. Nine new capability modules and 94 new imports, taking the ABI to
22 modules and 147 functions. Nothing in 1.0 changed.

## What is new

1.0 was enough to write an overlay and nowhere near enough to write a total
conversion. There were no ships, no armies, no research, no politics, no
economy, no way to author a scenario, and a UI that could draw rectangles and
14pt text.

| Module | What it grants |
|---|---|
| `Military.Read` | Ships -- type, position, health, crew, range -- army stacks per province, fortification and port levels |
| `Military.Write` | Army moves and ship move / engage / bombard orders |
| `Research.Read` | The technology tree, per-country completion, funding |
| `Research.Write` | Set research funding |
| `Politics.Read` | Political compass, policies, province unrest, minorities |
| `Politics.Write` | Enact and cancel policies |
| `Economy.Read` | Gross and net income, army and navy upkeep, bankruptcy, industry level and specialisation, province resources |
| `Economy.Write` | Set province industry level |
| `MapEditor` | Read and write the open map editor project |

Expanded: **UI** gains lines, circles, sized text, `measure_text`, panel
geometry, **your own images**, and the accent colour. **Map** gains coastline,
sea routes and a land test. **Neural** gains the AI's decision space by name.
**Net** gains peer enumeration.

## The two rules

**Every read is bounds-checked** and returns a neutral value -- 0, or an empty
string -- for an id that does not exist, rather than trapping. A mod iterating a
count that changed under it cannot crash the game.

**Every write goes through the same resolver the player's own click goes
through.** Nothing reaches into a container directly. An army order is still
checked for adjacency, a ship order is still clamped to range and routed around
land, a policy is still paid for and still subject to its prerequisites and the
per-turn cap. Granting `Military.Write` lets a mod issue orders, not fabricate
outcomes.

## Reskinning

Three levers, cheapest first:

- `ui/set_theme_accent` restyles the whole interface in one call -- the accent
  is read at over a hundred sites. It is not persisted and is dropped as soon as
  no mod is running, so it cannot outlive uninstalling your mod.
- `ui/draw_image` draws artwork from your own `.odmod` and nothing else. With
  lines, circles and sized text you can build an interface that looks nothing
  like this one.
- `MapEditor` reaches the same per-province data the editor's own tools write,
  so a generator can author a scenario in a loop instead of four thousand brush
  clicks.

Still out of reach: replacing textures the game itself draws outside a mod
panel. There is no central texture registry to hook, and adding one is a
renderer change rather than an ABI change.

## Compatibility

**A mod built against 1.0 runs unchanged, and that is tested.**
`sdk/compat/abi-1.0.json` freezes the 1.0 surface and `tools/check_abi_compat.py`
fails the build if any symbol in it is removed, re-signed, or moved to a
different capability. Within a major version the ABI may only be added to.

The existing conformance test could not have caught that on its own: it checks
that `abi.json` describes the host, and both files move together, so deleting a
function from both passed. All 13 shipped example mods declare
`"gearbox": "1.0"` and still load.

## Fixed

**`gearbox_is_multiplayer` and `gearbox_is_server` always lied.** Both read a
field nothing ever assigned, so they answered "single player, and you are the
authority" in every session. If you wrote a mod that checked before mutating --
the correct thing to do -- you got the wrong answer every time. The same dead
field gated the manifest's `"side"`, so a `"side": "server"` mod was never
masked off on a client.

There is deliberately no `net/is_multiplayer` import: those two already answer
it, and a second way to ask one question is worse than none.

**A mod's accent colour could outlive it.** It was written into the saved
config, so it survived uninstalling the mod with no way back but the reset
button. It now goes to a field the config file does not store.

**The docs and the manifest warning were wrong** about what happens when a mod
imports something the host does not have. It does not "trap on first call" --
an unresolved import cannot be linked, so there is no instance and no first
call. The failure is at load and it names the symbol.

## game 1.0.6a

The game runs on Android, mods can reach most of the game, and a long list of
things that never worked now do.

## Android

Open Doctrines runs on a phone. It is a real port -- a native library packaged
as an APK, not the web build in a wrapper -- and the interface has been resized
for a screen held at arm's length. Touch drives the game, a long press gives the
orders that need a right click, and a settings button sits on the map because a
phone has no ESC key.

Experimental, and labelled that way. It has been verified on an emulator rather
than on a shelf of real devices.

## Things that never worked

**Founding a port, or building your first factory.** The upgrade was looked up
in a province's existing industry or port entry -- which is exactly the entry a
FIRST factory or a NEW port does not have yet. The money was charged, the turns
were waited out, and the build was discarded. Founding a port was a total no-op.
This is also why the AI never industrialised: building was a pure loss, so it
learned to decline it.

**Ports on 127 coastlines.** A province was judged land-locked from the first
patch of water the scan happened to reach. Touch a lagoon and the open sea along
your other edge counted for nothing. On the 1939 map that is 22 British
provinces, 21 American, 17 Soviet, 9 French, and on down -- every one a coast you
can see and the game refused a port on.

**Left-wing doctrines, if you were left wing.** The political compass loaded
with both axes inverted, so the game had you on the opposite side of the board
from where your country actually stood. The Soviet Union loaded as hard right
with none of the four left doctrines available; Germany loaded as hard left with
all of them.

**Doctrine drift surviving a save.** Moving your country's politics is the whole
point of enacting a doctrine, and none of that movement was written to the save.
Governments snapped back to their 1939 positions on load while keeping the
doctrines they had passed to get away from them -- so a player who had worked
their way left found the left doctrines locked again.

**Being told why a doctrine is unavailable.** Every greyed-out doctrine blamed
conflicts you had never enacted. The real reason -- your treasury, or your
compass -- was never shown. Blocked doctrines now say which it is.

**A third of the ships in the game dealt no damage.** Cruisers and battleships
had no entry in the combat damage table and no fallback. The same omission left
them with no sprite. Battleships work now; cruisers are retired, since nothing
could build either.

**Ships sailing through land.** Any crossing whose straight line clipped a
coastline beached the hull. Scenario files also ship about a third of their
boats already aground -- 104 of 340 across the maps -- and those are refloated on
load.

**Ship range.** It bound your mouse and nothing else. An AI boat covered twice
the distance yours could, and no AI hull was slowed by its own speed rating.

**Putting down a rebellion froze your diplomacy.** A revolt is stored as a war,
and both war limits counted it, so a country suppressing a single uprising could
not declare a war or answer a call to arms.

**The terms of use link.** It returned a 404 on every platform for a week. The
terms were written and deployed -- to a service last updated the day before they
existed.

**Signing in from the web, or from a fresh install.** The account service was
only filled in when a config file was read, and neither of those has one, so the
Account screen said no service was configured and told you to edit a file you do
not have.

**Quitting in a browser.** The X and Escape froze the canvas with no way back
but a reload. Neither is offered there now.

## Mods

The Gearbox SDK roughly doubled: mods can now read and command ships and
armies, read and write research, politics and economy, author map projects
directly, draw their own artwork and restyle the interface. Every write goes
through the same rules your own clicks do, so a mod can issue an order but not
invent an outcome.

Existing mods keep working. That is now tested rather than intended -- the 1.0
interface is frozen in the repository and the build fails if any part of it is
removed or changed.

Two permissions -- Audio and Net -- were grantable by a mod and invisible in the
permissions screen, so you could neither see nor revoke them. All of them are
listed now.

## Multiplayer

**Long-form games.** A mode for playing a campaign with nobody online at the
same time: each turn is published, players submit orders back whenever they next
open the game, and a session survives everyone being away for days. Built and
tested, but not yet played through a real multi-day campaign, so treat it as new.

## The AI

Substantially rebuilt -- one shared encoder instead of eight, a longer planning
horizon, a critic that has an opinion about the moves it did not make, and
opponents drawn from its own past selves. Several parts of the network turned
out never to have been training at all.

It also now uses its navy like a player: it routes around land instead of
sailing into it, and it can attack enemy ships, which it previously could not do
at all.

Honestly reported: the model shipping here beats the previous one head to head,
but much of that is the engine fixes above rather than the learning. There is
more to do.

## Elsewhere

Controller support reaches the map, and tells you what each button does. Touch
works in the browser as well as on Android. The update badge is drawn instead of
typed. Windows continuous integration stopped failing on every commit.

## game 1.0.5a

Mostly repairs, and several of them are things that never worked at all.

**If you are on 1.0.4a you must install this one by hand.** The in-game updater
in 1.0.4a asks GitHub a question that can never return an answer, so it will
never offer you this release. That is fixed here; from 1.0.5a onwards the game
can update itself again.

## Things that never worked

**Signing in.** Every shipped copy said "No account service is configured" and
told you to edit a file that is not in the download. It could not be followed by
anyone who did not build the game themselves. The account service is now part of
the build.

**Updating.** The updater asked GitHub for the "latest release", an endpoint that
skips pre-releases by design -- and every alpha is one. It answered "nothing
found" for the entire life of the game, so no copy has ever been offered an
update. It now asks a question that has an answer.

**Playing in the browser on an ordinary machine.** The web build demanded 2 GB of
memory before it drew anything, and a scenario needs a good deal more on top of
that. Machines that could not spare it got a working menu and scenarios that
would not load. It now starts at 512 MB and grows only as needed.

**Exporting a timelapse GIF on Windows**, and **opening a multiplayer tunnel on
Windows**. Both ran commands that only exist on macOS and Linux.

**Playing at all, if your Windows account name is not plain English.** Every file
the game opened went through a text encoding that cannot represent most names, so
a Cyrillic or Japanese account name meant every single file failed to open. The
game started into a world with no fonts, no maps and no scenarios, and said
nothing about why.

## Things that now explain themselves

Several failures used to end in silence, which is the worst way to meet one.

- A scenario that fails to load now says which file and what tends to cause it,
  instead of returning you to the menu with no message.
- An empty scenario list now says the data folder is missing, and names where it
  looked. The most common cause is running the game from inside the .zip; the
  download now carries a READ ME FIRST explaining it.
- A graphics driver too old for the game now says so in a dialog rather than the
  game appearing not to start.
- Music no longer loops a fragment while the browser asks whether you meant to
  leave the page.

## Under the hood

The Windows build is now actually run by the build server rather than only
compiled there, including a full packaged copy playing a real scenario, so the
class of fault above cannot reach a release unseen again.

## Before you install

Both the zip and the installer are **unsigned**. Windows SmartScreen and macOS
Gatekeeper will warn on first launch; the README explains how to get past each.

There is no Linux Arm build. GitHub hosts no Arm Linux runner, so that platform
builds from source.

## game 1.0.4a

The first public release of the OpenDoctrines alpha.

Six historical scenarios on a 1641-province world map, a map editor for building
your own, multiplayer that needs no port forwarding, and a mod SDK for thirteen
languages. Playable in the browser as well as on Windows, macOS and Linux.

Alpha means it is playable from end to end and is not finished. The README's
Status section says which platforms have actually been sat down in front of.

## The AI fights again

The learned AI had gone quiet -- declaring no wars at all, 0.00 per thousand
country-turns against 4.72 for a control that picks at random, and playing the
map as though the only safe move were no move. Four separate causes, each enough
on its own:

- Wars were charged twice, once as aggression and once as a phoney-war penalty,
  so a war that went well still cost more than never declaring one.
- The idle penalty was combined across every module, so the war module could sit
  out an entire game uncharged as long as the economy was busy.
- The action mask offered wars the executor then refused to declare, so the
  policy was rewarded for choosing something that never happened.
- Rebel countries were counted in the trained AI's own statistics, making its
  behaviour unreadable exactly when it needed reading.

It now takes 32.4% of the land it plays for, up from 13.4%, and declares 1.73
wars per thousand country-turns. It still loses games it should win. That is
what the alpha label is for, and the game says so in as many words.

## Rebellions settle down

A province that put down a revolt could revolt again the next turn, because the
roll had no memory of the one before it. Across a 250-turn game that compounded
into 945 revolts and 921 rebellion wars, with one province rising 25 times.
Provinces now hold a cooldown once a rebellion is resolved: about 450 revolts
over the same game, and no province rising more than four times.

## Windows starts

The Windows build did not run. Double-clicking it did nothing at all -- no
window, no error, no crash dialog -- on any machine without the Visual C++
redistributable installed, because the executable depended on DLLs that are not
part of Windows and the package shipped none. The C++ runtime is now linked into
the executable, so there is nothing to install first.

And when the graphics driver genuinely cannot run the game, it now says so in a
dialog naming the cause, instead of vanishing silently. OpenGL 3.3 is required;
a machine without it gets an explanation rather than nothing.

## Play it in the browser

The web build ships as a release of its own, so the game can be tried without
downloading anything.

## Smaller downloads

The installers carried the AI trainer's entire workspace -- per-worker
checkpoints, backups, superseded models -- because the shipped data was chosen a
directory at a time and that directory doubles as scratch space. Installed size
drops from about 215 MB to 102 MB, with no change to what the game reads. The
browser build had the same problem in a worse place, and is down from 118 MB to
75 MB.

## Before you install

Both the zip and the installer are **unsigned**. Windows SmartScreen and macOS
Gatekeeper will warn on first launch; the README explains how to get past each.
Code signing needs a certificate this project does not have yet.

There is no Linux Arm build. GitHub hosts no Arm Linux runner, so that platform
builds from source.

## game 1.0.3a

The first tagged release of the OpenDoctrines alpha.

Six historical scenarios on a 1641-province world map, a map editor, multiplayer
that needs no port forwarding, and a mod SDK for thirteen languages. Alpha means
the game is playable end to end and is not finished; the README's Status section
says which platforms have actually been sat down in front of.

## The AI fights again

The learned AI had gone quiet. It was declaring no wars at all -- 0.00 per
thousand country-turns, against 4.72 for a control that picks its decisions at
random -- and playing the map as if the only safe move were no move. Four
separate causes, each of which alone was enough:

- Wars were charged twice, once as aggression and once as a phoney-war penalty,
  so a war that went well still cost more than never declaring one.
- The idle penalty was ANDed across every module, so the war module could sit
  out a whole game without ever being charged for it, as long as the economy
  was busy.
- The mask offered wars the executor then refused to declare, so the policy was
  rewarded for choosing an action that never happened.
- Rebel countries were counted in the trained AI's own statistics, which made
  its behaviour unreadable at exactly the moment it needed reading.

Against the random control the AI now takes 32.4% of the land it plays for, up
from 13.4%, and declares 1.73 wars per thousand country-turns. It is still
learning and still loses games it should win. That is what the alpha label is
for, and the in-game text says so.

## Rebellions settle down

A province that put down a revolt could revolt again the next turn, because the
roll had no memory of the one before it. Over a 250-turn game this compounded
into 945 revolts and 921 rebellion wars, with a single province rising 25 times.
Provinces now hold a cooldown after a rebellion is resolved: roughly 450 revolts
across the same game, and no province rising more than four times.

## Packaging

The installers carried the AI trainer's entire workspace -- per-worker
checkpoints, backups and superseded models -- because data/ was allowlisted a
directory at a time and that one directory is also scratch space. Installed size
drops from about 215 MB to 102 MB, with no change to what the game reads.

## Before you install

Both the zip and the installer are **unsigned**. Windows SmartScreen and macOS
Gatekeeper will warn on first launch; the README explains how to get past each.
Code signing needs a certificate this project does not have yet.

There is no Linux Arm build. GitHub hosts no Arm Linux runner, so that platform
builds from source.

