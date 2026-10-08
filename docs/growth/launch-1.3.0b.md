# Launching 1.3.0b — paste-ready

Everything here is written to be pasted somewhere. Nothing in this file is
posted by any workflow: the announcement board needs the developer badge, and
itch's description, devlog and upload order are dashboard-only (`docs/itch/README.md`).

Images are served from the repository through jsdelivr, the same way
`docs/itch/description.html` serves its screenshots, so nothing has to be
re-uploaded:

    https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/<file>

**The trailer exists** — 90 seconds, 1920x1080, H.264 + AAC, and it follows
`docs/trailer/plan.md`: the world, a country picked, the neuron joke, the title
card. Four cuts, v1 to v4; **v4** is the one.

**It is in `/private/tmp`, which macOS clears on reboot, and it is nowhere
else.** Move it before posting anything:

    mkdir -p ~/OpenDoctrines-media
    cp /private/tmp/od-trailer-v*.mp4 ~/OpenDoctrines-media/

It is 67 MB, which is over itch's 50 MB devlog image limit and fine for YouTube.
Upload it to YouTube and embed the link in the devlog; itch renders a YouTube
embed inline.

---

## 1. In-game announcement board

**Where:** Unifico → **Admin → News**, or the game's admin announce screen.
Both need the developer badge; the service checks it on every request.

Keep it short — this is read in a main menu, not on a page.

**Title**

    Open Doctrines is in beta

**Body**

    1.3.0b is out. Two hundred and eighty changes since the last release, and
    the headline is the letter: alpha meant the shape was not settled, beta
    means it is. Saves made from now on are expected to keep working.

    New: monuments that charge rent and go with the land, a world raw market
    so a metal-poor country can still build machinery, sector taxes that phase
    in, a navy with its own view and hulls, and a war you can read before you
    commit to it.

    It runs on FreeBSD, OpenBSD, Linux on four architectures, Windows on three,
    Android, the web — and TempleOS.

    There is a launcher now, Unifico. It is optional, and the plain download
    has not moved.

---

## 2. Discord

**Where:** `#announcements`. One message, under a screenful.

> ## Open Doctrines 1.3.0b — the first beta
>
> Alpha meant the shape wasn't settled. **Beta means it is** — saves made from
> here are expected to keep working. 280 changes since 1.2.2a.
>
> **What changed about playing it**
> • **Monuments** — eleven of them, each doing something different. They charge
> rent for as long as they stand and they go *with the land*, so taking a
> province takes the monument and the bill. That's what stops the answer being
> "build more".
> • **An economy with scarcity** — a world raw market means a metal-poor country
> can buy metal and build machinery instead of making food for ever. Sector
> taxes let you tax or subsidise a specialisation, phasing in two points a turn.
> • **A navy** — its own view, its own hulls, and a sea to cross.
> • **A war you can read** — what the battle would be before you commit, what it
> was after, how wide the front is, and what a troop type actually fights like.
>
> **Where it runs**
> FreeBSD · OpenBSD · Linux on arm64, armv7, riscv64 and x86 · Windows on x64,
> ARM64 and Win32 · Android · the web · and TempleOS, which draws the map, plays
> a real turn and talks to a network.
>
> **A launcher, if you want one**
> Unifico installs and updates the game, keeps versions side by side, signs you
> in once, and carries your worlds, mods, servers and achievements. Optional —
> the plain download hasn't moved.
>
> Notes: <https://github.com/Pr1nted/Open-Doctrines/releases/tag/v1.3.0b>

*Attach `docs/img/monuments-globe.png` — it is the one new thing that reads at
a glance in a Discord embed.*

---

## 3. itch.io devlog

**Where:** itch dashboard → **Devlog → New post**. Title below, body in HTML or
Markdown — itch accepts both.

**Title:** `Open Doctrines is in beta — 1.3.0b`

---

Open Doctrines has been in alpha its whole life. It isn't any more.

Not because a feature list got long enough. Because the things that make a
strategy game worth replaying are finally all present at once, and the gaps are
written down instead of found by whoever is playing.

**[Trailer — 90 seconds](PASTE THE YOUTUBE LINK HERE)**

*(Paste the YouTube URL on a line of its own and itch turns it into a player.
If it is not up yet, the timelapse below carries the post on its own.)*

![A world, 721 turns](https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/timelapse-political.gif)

### What "beta" is actually claiming

Alpha meant the shape was not settled: systems could be pulled out, saves could
stop loading. **Beta means the shape is settled, and saves made from now on are
expected to keep working.**

It is not claiming to be finished, balanced, or feature-complete. The AI still
doesn't build monuments. There is a section at the bottom that says what else is
missing, same as there has always been.

### Scarcity arrived

For three releases the only thing anyone built was food. A country would starve,
make food, and nothing else, for ever.

The fix wasn't a number. It was a **world raw market**: a country with no metal
can now buy metal and build machinery, instead of being locked out of technology
it can see. On top of that, **sector taxes** — tax or subsidise a specialisation,
inside ceilings your doctrines set, with the rate moving two points a turn
toward what you asked for. The lag is the point. Taxing a sector makes its goods
scarcer, and that is an economy rather than a slider.

![The goods economy](https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/economy.png)

### Monuments, and the arithmetic that stops them being "build more"

Eleven monuments, eleven different effects. The interesting decision isn't which
to build — it's that they charge **rent for as long as they stand**, and they go
**with the land**. Take the province, take the monument, take the bill.

That one rule is what stops the optimal play being "build all of them".

![A monument on the globe](https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/monuments-globe.png)

![The monuments screen](https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/monuments-panel.png)

### A war you can read

The honest description of combat used to be "numbers happen". Now the game says
what the battle would be before you commit, what it was afterwards, how wide the
front is, and what a troop type actually fights like.

And the AI stopped re-ordering battles it had already ordered — which was **two
thirds** of its attack orders.

![A province, and what is happening to it](https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/province.png)

### Where it runs

FreeBSD. OpenBSD. Linux on arm64, armv7, riscv64 and x86 — AppImage, deb, rpm
and Flatpak. Windows on x64, ARM64 and Win32. Android on a real device. The web.
And TempleOS, which draws the map, plays a real turn, and talks to a network.

![Open Doctrines running on TempleOS](https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/templeos-game.png)

The line worth writing down: **the Windows installer had never been run by
anything except the machine that built it.** A real Windows machine found two
bugs in an afternoon. So there is a gate now that runs before anything is
tagged, on guests that are not the build machine — the whole suite, the
packages, real guests, two machines playing each other, saves, mods and Android.

### One bug worth the whole section

The timelapse export was "corrupt above 1920x960". It wasn't. There was no
resolution limit — the disk was full, and not one write in the GIF encoder was
checked, so it printed "Saved 721 frames" and exited 0 over a truncated file.

A GIF has no length field and no checksum. The only symptom of a lost tail is a
decoder dying partway, which reads exactly like an encoder bug at that size.

**A write error and a format limit produce the same error message.** Check the
file size against a known-good run first.

Every write is checked now, and the picture at the top of this post is a
3840-wide export that could not have been made before.

### A launcher, if you want one

**Unifico** installs Open Doctrines and keeps it up to date, keeps several
versions side by side so an old save always has the build that made it, signs
you in once, and keeps your worlds, mods, servers and achievements in one place.
It also carries the TempleOS edition behind a single Play button.

It is **optional**. The plain download is where it has always been, nothing is
held back from it, and the game neither knows nor cares whether a launcher
started it.

### What is not finished

Being honest about a beta is cheaper than disappointing people later. The AI
does not build monuments. Steam is blocked on account verification rather than
on the build. The tutorial is young.

---

## 4. The opening shot

`tools/` can re-render the timelapse the trailer plan opens on, from any
finished save:

```
build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines \
    --export-timelapse "data/saves/<a finished game>.odsv" \
    out.gif 2560x1280 --no-watermark

ffmpeg -i out.gif -vf "fps=30,scale=2560:1280:flags=lanczos,format=yuv420p" \
       -c:v libx264 -preset slow -crf 18 -movflags +faststart out.mp4
```

721 frames, about three minutes to render at 2560x1280. The GIF is large and
the H.264 is not — which is the form YouTube and a store page both want.

This is the footage the trailer opens on, re-renderable from any finished save
whenever the map art changes — which is the point of keeping the command here
rather than only the output.
