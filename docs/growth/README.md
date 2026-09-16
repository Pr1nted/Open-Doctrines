# Where the players go, and where they stop

**Status: a reading of the numbers, and a plan. The code items marked
SHIPPED are in the tree; everything else is not.** Written 2026-09-16 from the
itch.io creator dashboard, the GA4 property, the Discord server and the
CurseForge page, all read on 15–16 September 2026.

---

## 1. The month, as a funnel

30 days to 15 September 2026. Impressions and click-through are itch's 7-day
figures; everything else is 30-day.

| Stage | Count | Survival |
|---|---:|---|
| Impressions on itch | 7,935 | 8.87% click through |
| Page views | 7,188 | **55.1% press play** |
| Browser plays | 3,964 | 15.9% take the download |
| Downloads | 629 | Windows 276 · Android 239 · Linux 43 · macOS 50 |
| Collections | 31 | 0.78% of plays |
| Discord members | 15 | cumulative since July, not this month |
| Comments | 14 | 0.35% of plays |
| **Ratings** | **3** | **0.076% — one per 1,321 players** |

And on the website (GA4, 28 days): 358 users, all new, 34 s average engaged
time, and **0% week-one retention in every cohort**.

**The shape:** the top of the funnel works and is growing. Everything below
"played it" is at or under 1%. Four thousand plays a month leave behind three
ratings, and next month starts where this one did.

## 2. Why three ratings is the expensive number

45% of all traffic to the page is itch's **own tag pages** — `tag-hoi4` alone
sent 824 visits this month, grand-strategy another 454, alternate-history 167,
4X 58. That is the channel carrying the game, and itch ranks those pages partly
on rating signal.

With three ratings there is nothing to rank on. The page is riding tag
*recency* from the 1.2.1a release, and recency decays. The channel delivering
almost half the audience has nothing holding it up.

Three ratings is not three people who disliked it. It is three people who were
**asked**: the in-game prompt's fallback fired at 45 minutes of cumulative
play, and six of every seven people who play this game play it in a browser
tab, where nobody reaches 45 minutes. For most of the audience it never fired.

## 3. Where the audience comes from

Referrers to the itch page, 30 days, by domain.

| Source | Visits | Note |
|---|---:|---|
| itch.io tag and browse pages | 3,267 | `hoi4` 824 · grand-strategy 454 · alternate-history 167 · 4X 58 |
| Google | 441 | the only channel that compounds on its own |
| Bing | 91 | |
| ChatGPT | 89 | plus 27 GA sessions attributed to "AI Assistant" |
| GitHub | 23 | |
| Reddit | 17 | posting has not worked so far |
| DuckDuckGo | 12 | |

**239 Android downloads**, second only to Windows, sideloaded from an APK, with
no Google Play listing and no F-Droid entry. That is the largest unserved
demand in the dataset. See [google-play.md](google-play.md).

## 4. The plan

Ordered, and the order is the argument: acquisition multiplies the conversion
rate. At 0.08% it multiplies nothing.

### Move 1 — ask the people who are already here

- **SHIPPED** The browser build's rating fallback cut from 45 minutes to 15 —
  a reachable session rather than a desktop number. `RATING_FALLBACK_WEB_MINUTES`
  in `src/Game_Feedback.cpp`.
- Reply to all 14 comments. Commenters are the people most likely to rate.
- A devlog with every release; itch puts devlogs in followers' feeds and on
  browse pages. 14 followers is small, but it is the only owned audience there
  is. Template in [store-copy.md](store-copy.md).

### Move 2 — stop losing them silently

The browser build keeps everything in IndexedDB, which fails invisibly in a
private tab, in a browser blocking site data, on an unanswered quota prompt,
and — the common one — in a third-party iframe whose storage the browser
partitions or refuses. **itch.io serves this game in exactly such an iframe.**
The game plays perfectly, reports every save as written, and loses the lot when
the tab closes. That is the most plausible cause of 0% retention.

- **SHIPPED** `odPersistWorking()` in `src/util/WebPersist.h`, and a warning
  that fires only for the players it is true for, offering a backup file and
  the download rather than an OK button. `src/Game_Feedback.cpp`.
- **SHIPPED** One line in the console at startup, either way: `[persist]
  storage is working` or `[persist] THIS BROWSER IS NOT KEEPING YOUR GAME`.
  Checking a browser is now opening the console and reading one line.
- **TO MEASURE** Open the itch embed in Safari, Firefox and Chrome and read
  that line. Safari's tracking prevention blocks third-party storage by
  default, so it is the browser most likely to be losing games — and every iOS
  browser is Safari. This decides whether move 2 is a footnote or the story.

### Move 3 — a reason to come back tomorrow

- **SHIPPED** The looking-for-a-game board, in the game and mirrored to
  Discord, with reporting and the four channel guidelines enforced. See
  [../looking-for-a-game.md](../looking-for-a-game.md).
- **SHIPPED** Three asks at the three moments of want: the multiplayer hub, the
  host's own empty lobby, and once on the way out of a solo game.
- **BLOCKED ON SETUP** The Discord application, and locking
  `#looking-for-a-game` to `/lfg`. The board is inert until this is done.
- **SEED IT** Host one long-form game a week yourself, at a fixed time, and
  post it. An empty board teaches people to stop opening it, and four thousand
  plays a month is far more traffic than one game needs.

### Move 4 — a second channel, in demand order

1. **Google Play.** 239 Android downloads a month with no listing.
   [google-play.md](google-play.md) has the blockers; two are real.
2. **Lean the itch page into `hoi4`.** 824 visits a month arrive from that tag
   expecting a Hearts-of-Iron-like. Copy in [store-copy.md](store-copy.md).
3. **Web-game portals.** 55% of viewers press play; the web build is the
   product. Newgrounds and CrazyGames cost one submission each.
4. **LLM referral.** 116 sessions a month from ChatGPT and assistants,
   unoptimised. `packaging/web/site/llms.txt` now carries the benchmark figures
   and the multiplayer board, which are the two things that answer "why this
   one".
5. **GEGI is working.** 83 downloads in its first 13 hours on CurseForge, with
   Open Doctrines as its first recommendation. Best acquisition-per-effort in
   the portfolio.
6. **Open Fly &mdash; CORRECTED 2026-09-17.** The original call here was "not a
   growth channel, stop spending effort on it", from 43 itch views, 8 browser
   plays and 9 visits from Hacker News. That was wrong, or at least far too
   early: a post to r/StrategyGames on 16 September did **32,000 views, 47
   upvotes at an 84% ratio and 24 comments**, and went first in the subreddit.

   What the numbers say on a second look:

   - **Hacker News was the wrong room, not the wrong idea.** 9 visits there
     against 32,000 impressions on a games subreddit.
   - **The title did the work**, not the page: *"I wired a simulated fruit-fly
     brain (138,639 neurons) into my grand strategy game. Here it is running a
     country."* One concrete claim, one number, no adjectives.
   - **24 comments against 47 upvotes** is a very high ratio &mdash; roughly five
     times typical. Most people scrolled past; the ones who stopped asked real
     questions. That is a small, technical, high-intent audience, which is the
     kind that installs things.
   - **0.15% upvote rate on views is low.** The title travelled much further
     than the pitch closed. Reach is not the constraint; what happens after the
     click is.

   **Still unmeasured, and it is the whole question:** whether any of those
   32,000 became a play or a download. Reddit sent 17 visits to the Open
   Doctrines itch page in the whole of the previous 30 days. Check Open Fly's
   itch analytics and the OD referrer table for 16&ndash;17 September before
   drawing any conclusion from this.

### Move 4a — make the wait shorter, since 3,964 people a month do it

Not in the original plan, because it did not show up until the package was
opened. The browser preload is downloaded **in full before the menu draws**,
and 5.6 MB of it was the sixty-five translation files — 46% of the package, of
which a player reads exactly one. Another 12 KB was two `.DS_Store` files.

- **SHIPPED** `setLanguage()` fetches its one language through `odEnsureAsset()`,
  the same way the scenarios, the music, the model and the full font already
  do. English costs nothing either way: the English text *is* the lookup key, so
  the game never opens `en.json` at all.
- **SHIPPED** The web staging drops dot-files, as the Android staging already
  did. What ships is decided by `OD_SHIPPED_DATA`, not by which folders were
  opened in Finder.
- **Measured:** `OpenDoctrines.data` **13.33 MB → 7.68 MB**, a 42% cut. With the
  wasm, first load goes from about 24.1 MB to 18.5 MB raw, and 8.8 MB to 6.9 MB
  gzipped. The menu still draws and every language still serves at the URL
  `odEnsureAsset()` asks for.

The remaining 4.2 MB of the preload is `data/flags` — 496 SVGs, of which five
are 1.45 MB between them. Optimising the worst offenders is pure data work with
no code change, and is the obvious next cut.

### Move 5 — measure, so the next decision is not a guess

GA4 had **no key events configured at all**: 784 page views, 46 scrolls, 35
clicks, and no way to tell a visitor who bounced from one who found the
download.

- **SHIPPED** `packaging/web/site/analytics.js` now sends five named events
  after consent and only after consent: `play_opened`, `download_started`,
  `itch_opened`, `discord_opened`, `source_opened`. A name and nothing else.
- **TO DO** Mark them as key events in the GA4 UI, then answer the question the
  property currently cannot: of 358 website visitors, how many reached a
  download at all?

## 5. What to expect

Moves 1–3 add no visitors. They change what a visit leaves behind, and that is
the compounding part, because ratings feed the tag pages already delivering 45%
of the traffic. Move 4 is worth doing after that loop closes; a new channel on
top of a 0.08% conversion rate buys a bigger number in the same shape.

The honest read is good news badly spent. A game that 55% of viewers choose to
play has a real product behind it. None of this is a marketing problem — it is
four thousand people who were never asked for anything.
