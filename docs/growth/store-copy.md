# Copy to paste

Ready to use, and written from the numbers in [README.md](README.md): most of
the audience arrives from itch's `hoi4` tag, most of them play in a browser
rather than downloading, and almost none of them are asked for anything.

Every factual claim below is checkable. The speed figures are from
[Objective Judge Horizon](https://github.com/Pr1nted/objective-judge-horizon)'s
matched-map round of 15 September 2026; the scale figures are the game's own.

**Do not call the licence "open source".** It is the OpenDoctrines
Non-Commercial License v1.1 — source-available, and `llms.txt` is already
careful about this. "Source is public" and "free" are both true and neither
oversells.

---

## itch.io — short description

The one line that appears under the title on every tag and browse page. This is
what the 824 people a month arriving from `tag-hoi4` read before they click.

> Grand strategy on 1,641 provinces — industry, armies, research and politics.
> Free, in a browser tab.

*(100 characters. itch cuts the short description at 120, and truncates it
further in some browse layouts, so the first half has to stand alone — which is
why "grand strategy on 1,641 provinces" comes before anything else.)*

## itch.io — page body

The embed sits above this, so the first line does not need to sell the click,
it needs to answer "what am I looking at" for somebody who has already pressed
play.

---

**You are running a country.** Its factories, its armies, its research, its
parliament and its minorities, turn by turn, across 1,641 provinces and six
scenarios from 1914 to the present day.

It is free. There is nothing to buy, no advertising, and single-player needs no
account. The source is public.

### It runs where the big ones do not

That is the whole reason this exists in a browser tab. There is no installer,
no launcher and no sign-in, so it starts in about ten seconds on a laptop, a
school Chromebook or a phone — and it is quick where it matters:

| On the same 1939 world, 35–37 AI players | Turns per minute |
|---|---:|
| **Open Doctrines** | **2,373** |
| Unciv 4.22 | 300 |
| Greater Diplomacy 5 | 74 |
| Freeciv 3.2.6 | 52 |

Measured by [an independent benchmark](https://github.com/Pr1nted/objective-judge-horizon)
that puts every game on the same map with the same number of players, and that
measures the other games through hooks contributed to *those* projects rather
than by guessing from outside. Its method, its caveats and its raw results are
public.

### What is in it

- **Six scenarios** — 1914, 1918, 1939, 1945, 1962 and a present-day map — plus
  a map editor for your own.
- **An economy** that is about industry, goods and living standards rather than
  a single money number.
- **Politics** — parties, elections, unrest, and ethnic minorities that partition
  a province rather than colouring it in.
- **Multiplayer** that joins by invite code with no port forwarding, and a
  looking-for-a-game board in the menu so you do not need to already know
  somebody.
- **Mods** compiled to WebAssembly from Go, Rust, Zig or C, sandboxed, and the
  same file on every platform including the browser.
- **44 languages.**
- Windows, macOS, Linux, Android and a Discord Activity, as well as the browser.

### It is an alpha

Playable start to finish, with a tutorial, and still changing between versions.
Saying so is fairer than not.

### Two things that help more than you would think

**Rate it.** This page lives or dies by itch's own browse pages, and those rank
on ratings. It takes ten seconds and it is the single most useful thing anybody
who enjoyed this can do.

**Come and play a human.** The Discord has a `#looking-for-a-game` channel that
is the same board you see in the game's multiplayer menu — post there or in the
game and it appears in both. The AI does not negotiate.

---

## Google Play — short description (80 characters max)

> Grand strategy on 1,641 provinces. Free, no account, runs on almost anything.

*(77 characters.)*

## Google Play — full description

> **You are running a country** — its industry, its armies, its research and its
> parliament — turn by turn, across 1,641 provinces and six scenarios from 1914
> to the present day.
>
> Free. Nothing to buy, no advertising, no account needed to play.
>
> **BUILT TO RUN ON WHAT YOU HAVE**
> An independent benchmark put it on the same 1939 world as four comparable
> games with the same number of AI players. Open Doctrines resolved 2,373 turns
> a minute; the next fastest managed 300. That is why it plays on a phone at
> all.
>
> **WHAT IS IN IT**
> • Six scenarios — 1914, 1918, 1939, 1945, 1962 and a present-day map
> • A map editor for your own worlds
> • An economy about industry, goods and living standards, not one money number
> • Politics: parties, elections, unrest, and ethnic minorities that divide a
>   province rather than colour it in
> • Multiplayer by invite code, with no port forwarding, and a board in the menu
>   for finding a game when you do not already know somebody
> • Mods compiled to WebAssembly and run sandboxed
> • 44 languages
>
> **ALSO ON** Windows, macOS, Linux, in a browser with no download, and inside a
> Discord voice channel.
>
> **THIS IS AN ALPHA.** It is playable start to finish and has a tutorial, and
> it is still changing between versions. A controller works; touch works;
> a bigger screen helps.
>
> The source is public. The licence is the OpenDoctrines Non-Commercial License
> v1.1 — you may read, change and share it; commercial use is reserved.

## Newgrounds / CrazyGames — blurb

> A free grand strategy game about running a country: its factories, armies,
> research and parliament, across 1,641 provinces and six eras from 1914 to
> today. No download, no account — it starts in about ten seconds. Six
> scenarios, a map editor, multiplayer by invite code, 44 languages, and it is
> an alpha that is still growing.

---

## Devlog template

itch puts devlogs in followers' feeds and on browse pages, which is why one per
release is worth the twenty minutes. The shape that works:

> ## <version> — <the one thing that changed, in plain words>
>
> **<One sentence a player would care about.>** Not "refactored the turn
> pipeline" — "turns resolve about twice as fast on big maps".
>
> ### What is new
> Three to six bullets. Each one says what a *player* can now do or see.
>
> ### What was broken and is not any more
> The bugs somebody actually reported. Name the symptom they saw, not the
> cause. People who reported something read this looking for their own line.
>
> ### Still not right
> One or two honest items. This is the part that earns the next devlog's
> readers.
>
> ---
> Playable in a browser above, or downloadable for Windows, macOS, Linux and
> Android. If you have five minutes, a rating on this page is the thing that
> helps most — it is how anybody else finds it.

**Every devlog ends with the rating ask.** Not a banner, not a popup: one line
at the bottom, where somebody who read to the end will see it.
