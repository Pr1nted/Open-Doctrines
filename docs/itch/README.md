# The itch.io page

```
description.html   paste into the page description (HTML mode)
theme.css          reference only -- see the note below
README.md          this
```

## Read this first: itch has no custom CSS field

The **Edit theme** panel gives you colour pickers, two font dropdowns, a
screenshot layout dropdown and three image uploads. That is the whole surface.
There is no box to paste a stylesheet into, so `theme.css` cannot be applied as
written.

It is kept because it is the design in a form you can read — every value in the
table below comes from it — and because if you ever move the page somewhere that
does take CSS, it is ready.

What this actually means for the page: **the description carries it.** Structure,
images and honest copy are what you control, and they matter more than styling
would have.

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

**Genre:** Strategy. **Tags:** `grand-strategy`, `strategy`, `map`, `moddable`,
`multiplayer`, `singleplayer`, `open-source`, `alpha`, `history`, `simulation`.

Tags are how anybody finds this. `grand-strategy` and `moddable` are the two
that describe it most specifically; do not spend all ten on generic words.

---

## 2. Edit theme — every field, and what to put in it

**Manage → Edit theme.** Set all of these; the ones behind *More options…* are
the ones that make it stop looking like a default itch page.

| Section | Field | Value | Why |
|---|---|---|---|
| Color | **BG** | `#050813` | The game's menu background, sampled from `docs/img/main-menu.png`. |
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

### The three image slots

| Slot | What it is | Suggestion |
|---|---|---|
| **Banner** | Wide image above the page | Skip it, or crop the top third of `docs/img/world-map.png`. A banner competes with the first screenshot directly below it. |
| **Background** | Behind the content column | Upload **`page-background.png`**, in this folder. Ready to use. |
| **Embed BG** | Behind the web build's frame | `#050813` flat, only if you upload the web build. |

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

### 3b. Inline images in the description

Separate uploads — the gallery and the description do not share files.

For each `UPLOAD:` placeholder still in the pasted description:

1. Click it once to select the broken image, and delete it.
2. With the cursor in that spot, click the **image button** in the editor
   toolbar.
3. Choose the matching file. itch uploads it and writes the `src` itself.

| Placeholder | File |
|---|---|
| `UPLOAD:world-map.png` | `docs/img/world-map.png` |
| `UPLOAD:timelapse-political.gif` | `docs/img/timelapse-political.gif` |
| `UPLOAD:province.png` | `docs/img/province.png` |
| `UPLOAD:research.png` | `docs/img/research.png` |
| `UPLOAD:economy.png` | `docs/img/economy.png` |
| `UPLOAD:map-editor.png` | `docs/img/map-editor.png` |
| `UPLOAD:mods.png` | `docs/img/mods.png` |

**You do not have to do all seven.** With the gallery already carrying them, the
two worth keeping inline are **the timelapse GIF** and **`mods.png`** — the GIF
because a moving map says more than any sentence on the page, and the mod menu
because it is the one screenshot whose subject the surrounding text cannot
convey. Delete the rest of the placeholders; the gallery covers them, and a
description that alternates image-paragraph-image-paragraph is harder to read
than one that does not.

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
- `build-web/data/audio/music/` **must** be inside the zip. Music is streamed
  rather than preloaded, and a zip without it gives a silent game.

The web build also cannot host or join multiplayer, which the description
already says.

---

## 6. Before you publish

- Read the "What is not finished" section once more and make sure it is still
  true. It is the part of the page that earns trust, and a stale one costs more
  than it saved.
- Check the page on a phone. itch is responsive on its own, but the
  screenshots are wide and worth looking at on a small screen.
- Leave it **Restricted** and open the link on another machine first. Published
  is public immediately and there is no draft state to fall back to.
