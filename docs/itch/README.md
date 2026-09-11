# The itch.io page

```
description.html   paste into the page description (HTML mode)
theme.css          the page CSS -- inert until itch enables the box, see below
README.md          this
```

## Read this first: the CSS box is on — asked 2026-08-04, granted 2026-08-14

The **Edit theme** panel gives you colour pickers, two font dropdowns, a
screenshot layout dropdown and three image uploads. On a normal account that is
the whole surface — which is exactly why an earlier version of this file said
itch had no custom CSS at all. It does, and it is enabled per ACCOUNT, by hand,
on request:

> get in touch with us and ask for custom CSS to be enabled for your account
> — itch.io/docs/creators/design

There is no self-serve toggle. The request took ten days with no acknowledgement
along the way, which is normal for a manual queue and indistinguishable from a
contact form that silently failed — so if you ask for anything else there, do
not read silence as refusal.

`theme.css` is ready to paste into the box at the bottom of the **Edit theme**
sidebar. Two things to do in the SAME sitting, not after:

- change **BG** from `#050813` to `#080E20` — see *§2a*, and the note at the top
  of `theme.css`. The stylesheet paints the sea, and BG is what shows where the
  stylesheet cannot reach; leave it and the join is visible.
- check the page at phone width. itch's own docs ask for this after heavy
  customisation. `theme.css` already switches its effects off under 700px and
  honours `prefers-reduced-motion`, so this is a confirmation, not a fix.

It is ONE paste. `page-blasts.css` is the generated shell-position fragment and
is already merged into `theme.css`; it lives here only so `tools/banner.py
--page-bg` has somewhere to write.

**butler cannot do any of this.** It pushes build files to a channel and nothing
else — not the theme, not the CSS, not the description, cover or screenshots.
Those are dashboard-only, and there is no API for them. See §5 for what butler
IS for.

Everything the page shows is in `docs/img/`, produced by `tools/screenshots.sh`.
When the game's UI changes, re-run that and re-upload; the page and the game do
not otherwise stay in step.

---

## 1. Create the project

**Dashboard → Create new project.**

| Field | Value |
|---|---|
| Title | OpenDoctrines |
| Short description | A grand strategy game about running a country: industry, armies, research, politics and neighbours. |
| Classification | Games |
| Kind of project | Downloadable — add HTML later if you upload the web build |
| Release status | **In development** |
| Pricing | No payments, or Donate — the licence is non-commercial |

**Genre:** Strategy.

## Links, which the website was missing from entirely

The info panel's **Links** field held only GitHub and Discord, and the
description body linked the same two and nothing else. So the website, the
press kit and the teachers' note were unreachable from the page with 6,316
views and 3,496 browser plays — the one surface where a journalist, a streamer
or a teacher actually lands.

Set the Links field to:

| Label | URL |
|---|---|
| Website | `https://opendoctrines.pages.dev` |
| Press kit | `https://opendoctrines.pages.dev/press` |
| GitHub | `https://github.com/Pr1nted/Open-Doctrines` |
| Discord | `https://discord.gg/wqS65jzVv5` |

The press kit earns its own row rather than being left one click deep. Somebody
deciding whether to cover this is the visitor worth the least friction, and the
licence answer they need — that monetised video is explicitly allowed — is on
that page and nowhere else they would think to look.

`description.html` now links all three in *Where everybody is* as well, which
matters more: the info panel is a sidebar people skim past, the description is
the thing they read.

**Tags actually on the live page** (checked 2026-09-11, nine of the ten allowed):
`2d`, `4x`, `alternate-history`, `grand-strategy`, `historical`, `hoi4`,
`indie`, `multiplayer`, `turn-based-strategy`.

**Intended set** — `2d` and `indie` out, `eu4` and `victoria-3` in, leaving one
slot free. See *The change being tested* below for why, and for how to tell
whether it worked. This is set by hand in the itch dashboard; nothing in this
repository can apply it.

This list used to read `grand-strategy, strategy, map, moddable, multiplayer,
singleplayer, open-source, alpha, history, simulation`, which was a plan and
never what was set. Note in particular that there is no `open-source` tag on
itch and there never was, so there is nothing there to keep or to correct.

## What the tags are actually worth

Tags are not one channel among several here, they are THE channel. Of roughly
3,050 visits itch sent the page in the 30 days to 2026-09-11, about 1,410 came
from tag browse pages, against 363 from Google and 87 from ChatGPT.

| Tag | Visits in 30 days |
|---|---|
| `hoi4` | **761** |
| `grand-strategy` | 333 |
| `alternate-history` (with `historical`) | 228 |
| `4x` | 47 |
| `multiplayer` | 45 |
| `2d`, `indie`, `turn-based-strategy` | below the top-20 cutoff (42) |

`hoi4` alone is 54% of it. That is the finding worth acting on, and it is not
about grand strategy: `hoi4` is a COMPETITOR'S NAME. People browse itch for the
game they already play, and this appears next to it. The tags that describe the
genre honestly earn a fraction of what one borrowed title does.

So the free slot, and any slot freed by dropping `2d` or `indie` — both
enormous, generic, and describing nothing a person would search for on purpose
— is worth spending on another title people look for: `eu4`, `victoria-3`,
`crusader-kings`, `paradox`, `civilization`, `wargame`, `cold-war`.

## Shelf size is the mechanism, and it runs backwards

Checked on itch, 2026-09-11:

| Tag | Games carrying it | Visits it sent in 30 days |
|---|---|---|
| `hoi4` | **13** | **761** |
| `grand-strategy` | 58 | 333 |
| `2d`, `indie` | tens of thousands | under 42 |

A tag is a shelf. What a slot is worth is the demand for that shelf divided by
how many games are standing on it, and `hoi4` wins on BOTH terms: a lot of
people want Hearts of Iron and there are thirteen games there, so this one is
seen. `grand-strategy` has four times the shelf and sends under half. `2d` and
`indie` are shelves so crowded that being on them is the same as not being.

So "is this a big tag" is the wrong question and picking the biggest available
is the wrong move. Small and wanted beats large and generic, every time.

None of these are curated: `hoi4` and `grand-strategy` both still show itch's
"Suggest description for this tag" link, exactly as `eu4` and `victoria-3` do.
An uncurated community tag with thirteen games is what 54% of the traffic
arrives through.

## The change being tested

Dropping `2d` and `indie` for `eu4` and `victoria-3`.

The honest state of it: `eu4` has 0 games and `victoria-3` has 1, so those
shelves are empty. That is either the best possible position -- sole occupant
of a shelf people want, which is what `hoi4` nearly is -- or it is an empty
shelf because nobody browses it. Nothing visible from outside itch separates
those two, and demand is not a number itch publishes.

What makes it worth doing anyway is the price. `2d` and `indie` each send fewer
than 42 visits a month and the cost is capped there; `hoi4` shows the upside is
761. The Analytics referrer report names the tag each visit came through, so
the answer arrives in days and the change is undone with a click.

Read it in Analytics -> incoming visits -> By URL, looking for
`itch.io/games/tag-eu4` and `tag-victoria-3`. If neither appears within a
fortnight, the shelves are empty because nobody wants them: put the slots on
`wargame` (112 games) or `paradox` (19), both of which at least exist.

---

## 2. Edit theme — every field, and what to put in it

**Manage → Edit theme.** Set all of these; the ones behind *More options…* are
the ones that make it stop looking like a default itch page.

| Section | Field | Value | Why |
|---|---|---|---|
| Color | **BG** | `#050813` (`#080E20` once CSS is on — §2a) | The game's menu background, sampled from `docs/img/main-menu.png`. |
| Color | **BG 2** | `#0b1122` | Must NOT equal BG. This is the content column; identical values make the page one flat black rectangle with the text floating on it. |
| Color | **Text** | `#c8ccd8` | |
| Color | **Link** | `#ffd700` | |
| Color → More options | **Headers** | `#ffd700` | Left blank, headings render in the default colour and the page loses its structure. |
| Color → More options | **Buttons** | `#ffd700` | The download button. This is the one thing on the page you want someone to press. |
| Color → More options | **BG2 Alpha** | full (slider hard right) | Anything less lets the background image bleed through the text column. |
| Text | **Font** | Lato | |
| Text | **Size** | Large | The description is long; large is easier on it. |
| Text → More options | **Header font** | Default | |
| Layout | **Screenshots** | Auto | |

Gold rather than green because gold is the **shipped default** — the accent is
configurable in Settings, so screenshots from a customised build will not match
what a new player sees.

### 2a. BG has two correct values, and which one depends on the CSS

`#050813` is right for the page as it stands. **Change it to `#080E20` in the
same sitting that `theme.css` goes in**, and not before.

`theme.css` paints the sea itself, from `initMenuBackground()`'s rgb(8,14,32),
because `page-bg-menu.png` carries land on a transparent sea — whatever colour
sits behind it *is* the sea. BG is then what shows in the places the stylesheet
cannot reach: the itch header strip, and the itch app. At `#050813` those sit a
shade off the sea the page is painting and the join is visible; at `#080E20`
they agree. With no CSS there is no painted sea to match, and `#050813` is the
menu background as sampled.

The same conditional is documented at the top of `theme.css`. If the two ever
disagree again, that file is the one that knows why.

### The three image slots

| Slot | What it is | Suggestion |
|---|---|---|
| **Banner** | Wide image above the page | Upload **`banner-itch-wide.png`** (1600x500, 460 KB). Required — see below. |
| **Background** | Behind the content column | Upload **`page-background.png`**, in this folder. Ready to use. |
| **Embed BG** | Behind the web build's frame | `#050813` flat, only if you upload the web build. |

### The banner slot is not optional once the CSS is on

An earlier version of this file said to skip the banner. With `theme.css`
applied that is wrong, and it is wrong in a way that looks like the stylesheet
is broken rather than like a missing upload.

The animated banner is painted by CSS, on `#header.has_image`. **itch only adds
`has_image` to that element when a banner has been uploaded** — with an empty
slot there is no class, no height, and no banner, however correct the CSS is.

So upload `banner-itch-wide.png`. What you upload is not what visitors see: the
rule carries `!important` and replaces it with the 42-frame
`banner-itch-wide-eras.webp` over jsDelivr, because itch's uploader takes only
PNG, GIF and JPG, and its GIFs have rendered as stills since October 2025. The
upload's only jobs are to switch the class on and to be the still that shows
where CSS cannot reach — the itch app, and any browser that fails to fetch the
WebP. That is why the small single-frame PNG is the right file and the 4.7 MB
animated one is not: ten times the weight for a frame nobody sees on the web
page.

`page-background.png` is 1920x960 and already prepared. It is built from the
LAST FRAME of the political timelapse rather than from a screenshot, because a
screenshot carries the game's UI — the Process Turn button, the sidebar, the
"PLAYING AS" caption — and a page background with buttons drawn on it reads as
broken. A timelapse frame is pure map.

It is at 30% brightness with the colour muted to 75%, so it sits behind the
content column without competing with the screenshots in it. To regenerate, or
to pick a different moment in the war:

```bash
python3 -c "
from PIL import Image, ImageEnhance
gif = Image.open('docs/img/timelapse-political.gif')
gif.seek(gif.n_frames - 1)          # last frame; try n_frames//2 for mid-war
im = gif.convert('RGB').resize((1920, 960), Image.LANCZOS)
im = ImageEnhance.Brightness(im).enhance(0.30)
im = ImageEnhance.Color(im).enhance(0.75)
im.save('docs/itch/page-background.png')
"
```

---

## 3. Images

itch must host every image. A `src` pointing at a local file, or at a raw
GitHub URL, will not work — GitHub sends headers that stop other sites
embedding its files, so it fails silently and looks exactly like a typo.

There are **two separate places** images live, and they are not the same thing.

### 3a. The Screenshots gallery — do this one first

**Edit game → Screenshots.** Upload all seven from `docs/img/`:

```
world-map.png            the hero: put it first, it is the thumbnail
timelapse-political.gif  animated, and the most persuasive thing you have
province.png
research.png
economy.png
map-editor.png
mods.png
```

This gallery is where players look. itch gives it a viewer and a lightbox for
free, the order is drag-to-sort, and it needs no HTML at all. **Order matters:**
the first one is what appears in listings and on your profile.

### 3b. Inline images in the description — there are none, on purpose

`description.html` contains **no `<img>` tags**. Uploading to the Screenshots
gallery does not make inline images work: they are separate uploads and do not
share files, so an `<img>` in the description needs its own upload through the
editor's image button, one at a time, every time the description is re-pasted.

That is a lot of clicking to duplicate images the gallery is already showing
directly above. So the description is text, the gallery is pictures, and pasting
the description is a single action that always looks right.

If you do want one inline later — the timelapse GIF is the only one that earns
it, because a moving map says more than any sentence on the page — put the
cursor where you want it, click the **image button** in the editor toolbar, and
choose the file. itch uploads it and writes the `src` itself. Do not paste a
path or a GitHub URL; neither works.

### 3c. Cover image

**Edit game → Cover image**, 630×500. Upload **`cover-titled.png`** from this
folder.

Two versions are here. `cover.png` is the map alone; `cover-titled.png` has the
game's wordmark across it. Use the titled one: at the ~315px itch actually
renders in a browse grid, the plain map is attractive but could be any map game,
while the titled one reads as a specific product. The blocky title survives the
downscale where a thin typeface would turn to mush.

The wordmark is lifted pixel-for-pixel out of the game's own menu rather than
set in a lookalike font, so it matches what a player sees on launch. Everything
but the gold strokes is masked to transparent before compositing — pasting the
crop as a rectangle stamps a dark box across North Africa.

The cost of the titled version is mild duplication: browse grids print the title
beside the thumbnail anyway. That is worth paying for the places the image
travels alone — social embeds, the itch app, "more like this" strips.

Both are cropped by hand rather than by itch, because itch's automatic crop
takes the middle of the world map, which is ocean. To rebuild the plain one:

```bash
python3 -c "
from PIL import Image
im = Image.open('docs/img/world-map.png')
# Europe/Atlantic, at the 630x500 aspect ratio
im.crop((520, 60, 1150, 560)).resize((630, 500), Image.LANCZOS) \
  .save('docs/itch/cover.png')
print('wrote docs/itch/cover.png')
"
```

And to rebuild the titled one after a UI change, `tools/itch-cover.py`:

```bash
python3 tools/itch-cover.py
```

---

## 4. Paste the description

In the description editor, click the **`</>`** (edit HTML) button and paste all
of `description.html`. Without HTML mode the tags arrive as literal text.

Then replace each `UPLOAD:` image with a real upload, per step 3.

---

## 5. Uploads

Attach the builds from the GitHub release and set the platform checkboxes — itch
will not show a download to a Windows visitor unless the file is marked Windows.

| File | Platform | Notes |
|---|---|---|
| `OpenDoctrines-windows-x64.zip` | Windows | |
| `OpenDoctrines-macos-arm64.zip` | macOS | Apple Silicon |
| `OpenDoctrines-macos-x64.zip` | macOS | Intel |
| `OpenDoctrines-linux-x64.zip` | Linux | glibc 2.35+ |

Installers (`.exe`, `.dmg`) can go up too. Mark them **"This file will be
downloaded on the platform"** so the itch app prefers the zip — the app manages
its own installs, and an installer inside a managed install is confusing.

### If you upload the web build

Zip the **contents** of `build-web/` (not the folder), with
`OpenDoctrines.html` at the root, tick *"This file will be played in the
browser"*, and set the viewport to **1280×800** with "Fullscreen button"
enabled.

Two things to know before you do:

- the package is ~97 MB, so first load is slow on a poor connection; and
- everything under `build-web/data/` **must** be inside the zip, not just the
  four files at the root. Four things live there because they are fetched at
  the moment they are needed rather than preloaded, and a zip without them
  fails silently and differently each time:

  | | what a zip without it looks like |
  |---|---|
  | `data/audio/music/` | a silent game |
  | `data/STDmaps/` | "This world could not be downloaded" on every scenario |
  | `data/ai/model.bin` | no trained opponent |
  | `data/fonts/unifont-full.ttf` | Japanese, Chinese, Korean and Arabic draw as blanks |

  The 11 MB font is the odd one: it is NOT part of the 97 MB anybody
  downloads. Only a player who picks a language the 147 KB subset cannot draw
  ever asks for it. See `Game::reloadFonts()`.

The web build cannot HOST multiplayer -- a browser tab cannot listen for
players -- but it can JOIN one, which the description says. Do not shorten that
to "no multiplayer on web" when writing the page: joining from a browser is a
feature, and this page told players it did not exist for a fortnight after it
started working.

---

## 6. Before you publish

- Read the "What is not finished" section once more and make sure it is still
  true. It is the part of the page that earns trust, and a stale one costs more
  than it saved.
- Check the page on a phone. itch is responsive on its own, but the
  screenshots are wide and worth looking at on a small screen.
- Leave it **Restricted** and open the link on another machine first. Published
  is public immediately and there is no draft state to fall back to.
