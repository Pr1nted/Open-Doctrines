# Changelog

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

