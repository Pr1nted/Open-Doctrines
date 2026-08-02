# Changelog

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

