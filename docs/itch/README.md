# The itch.io page

Three files, and the order matters — the CSS assumes the theme colours are
already set, and the description assumes the images are already uploaded.

```
description.html   paste into the page description (HTML mode)
theme.css          paste into Edit theme > Custom CSS
README.md          this
```

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

## 2. Set the theme colours

**Edit theme**, and set these *before* pasting the CSS. The custom CSS restates
them, but itch uses the colour fields in places the CSS does not reach — and if
a future itch redesign breaks a selector, these keep the page dark and readable
instead of dumping it back to white.

| Field | Value |
|---|---|
| Background | `#050813` |
| Text | `#c8ccd8` |
| Link | `#ffd700` |
| Button background | `#ffd700` |
| Button text | `#050813` |

These are the game's own colours, sampled from `docs/img/main-menu.png`: the
menu background and the title. Gold rather than green because gold is the
**shipped default** — the accent is configurable in Settings, so screenshots
from a customised build will not match.

---

## 3. Paste the CSS

**Edit theme → Custom CSS**, paste all of `theme.css`, save, and look at the
page.

If something looks unstyled, an itch class name has changed — they are not a
public API. The CSS is written so that costs you a detail rather than the page:
step 2's colours still apply. Fix it by finding the element in your browser's
inspector and adding its current class alongside the old one.

---

## 4. Upload the screenshots

The description references nine images by placeholder. Upload each from the
description editor's **image button** so itch hosts it and writes the URL for
you — that is easier and safer than editing the `src` by hand.

In the order they appear:

| Placeholder | File |
|---|---|
| `UPLOAD:world-map.png` | `docs/img/world-map.png` |
| `UPLOAD:timelapse-political.gif` | `docs/img/timelapse-political.gif` |
| `UPLOAD:province.png` | `docs/img/province.png` |
| `UPLOAD:research.png` | `docs/img/research.png` |
| `UPLOAD:economy.png` | `docs/img/economy.png` |
| `UPLOAD:map-editor.png` | `docs/img/map-editor.png` |
| `UPLOAD:mods.png` | `docs/img/mods.png` |

The GIF is 1.2 MB and animates on the page, which is the single most useful
thing on it — a still screenshot of a strategy game tells you very little.

**Cover image** (the thumbnail in listings, 630×500): itch crops to that ratio,
so crop `docs/img/world-map.png` yourself rather than letting it choose. Put
Europe and the Atlantic in frame; the automatic crop takes the middle, which is
ocean.

---

## 5. Paste the description

In the description editor, click the **`</>`** (edit HTML) button and paste all
of `description.html`. Without HTML mode the tags arrive as literal text.

Then replace each `UPLOAD:` image with a real upload, per step 4.

---

## 6. Uploads

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

## 7. Before you publish

- Read the "What is not finished" section once more and make sure it is still
  true. It is the part of the page that earns trust, and a stale one costs more
  than it saved.
- Check the page on a phone. The CSS has a `max-width: 700px` block, but the
  screenshots are wide and worth looking at.
- Leave it **Restricted** and open the link on another machine first. Published
  is public immediately and there is no draft state to fall back to.
