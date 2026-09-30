# art

The editable sources for things the game ships as flattened images. Kept here
so that "make the icon say Beta" is an edit rather than a redraw.

Nothing in this directory is shipped. `tools/package.py` builds a release from
`data/` against an allowlist, so a top-level directory is outside the package by
construction rather than by being remembered — which is the point of putting
them here instead of under `data/`.

`.kra` files are declared binary in `.gitattributes`. They are zip archives of
PNG and XML, and git's heuristics will otherwise decide some of them are text,
try a line-by-line merge, and hand Krita a file it cannot open.

## The icon

| file | what it is |
|---|---|
| `icon-alpha.kra` | the icon as it ships today, labelled **Alpha** in red |
| `icon-beta.kra` | the same artwork labelled **Beta** in green, for the beta release |

Both are 1024×1024. The mark is the pixel **O.D.** over a political world map
drawn from the game's own `land_sea.png` and `political.png` layers, with the
stage label in the bottom-right corner.

### Exporting without opening Krita

A `.kra` already contains a full-resolution flatten, so the 1024×1024 PNG the
icon pipeline wants can be taken straight out of the archive:

```bash
unzip -p art/icon-beta.kra mergedimage.png > data/Icon/icon.png
```

`preview.png` is in there too and is NOT the one to use: it is a 256×256
thumbnail, and `tools/generate_icons.py` asserts on a 1024×1024 source, so
taking the wrong one fails loudly rather than shipping a blurry icon.

## Turning Alpha into Beta at release

`data/Icon/icon-beta.png` is already extracted and committed, so the release
does not depend on anyone having Krita. The flip is deliberately NOT done in
advance: a build from `main` before the beta release should still call itself
Alpha, because it is one.

1. `cp data/Icon/icon-beta.png data/Icon/icon.png`
2. `python3 tools/generate_icons.py` — rebuilds `icon.icns` (macOS) and
   `icon.ico` (Windows) from it
3. `python3 tools/generate_web_favicon.py` — rebuilds `packaging/web/favicon.png`,
   which is also the site favicon
4. README: the two places that say the project is in Alpha — the line under the
   title and the first line of **Status**
5. `docs/igdb.md` — the store listing's Status field
6. Delete `data/Icon/icon-beta.png`, or it becomes the stale one nobody notices

Step 6 matters more than it looks. Two icons in a directory, one of them
correct, is the arrangement that ships the wrong one eventually.

## The mark

`logo/od-mark.svg` is the alternate logo: the **O.D.** lockup on its own, in one
colour.

It exists because the app icon cannot be monochrome. The icon IS colour — its
political world map is the artwork, and a black-and-white version of it is a
grey smear. So anywhere the game needs one flat colour (a print, a sponsor
strip, a dark README header, an embroidered patch, a terminal splash) this is
the mark to use instead of a desaturated icon.

- `fill="currentColor"`, so black on light and white on dark are the *same
  file*. There is no second variant to keep in step.
- The letterforms are the icon's own, so the two read as one identity.
- Legible down to about 32px. Below that use the app icon, which is designed
  for it.

### Where the letterforms come from

They are outlines of **Press Start 2P** (Cody "CodeMan38" Boisclair, SIL Open
Font License 1.1), the face the icon is drawn in, converted to SVG paths with
fontTools.

Recorded here rather than in `tools/provenance.json` because that file is for
third-party components the game SHIPS, and this is neither: the font is not in
the repository and not in any release. What ships is artwork whose shapes were
drawn with it, which the OFL does not reach — its terms govern redistributing
the font, and the OFL FAQ is explicit that documents and artwork made with a
font are unaffected.

Converting to paths is also what makes the logo portable: a `<text>` element in
Press Start 2P renders as Press Start 2P only where that font is installed, and
as something else everywhere else. The Krita sources still contain live text
layers, which is why they need the font to edit and this file does not to view.

To regenerate after editing the lockup:

```bash
python3 tools/make_logo_svg.py     # outlines "O.D." from the installed font
```
