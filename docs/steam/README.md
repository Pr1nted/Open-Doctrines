# The Steam page

```
README.md          this
banner-steam-*.png the capsules, from tools/banner.py --out-dir docs/steam
```

The counterpart to `docs/itch/README.md`. Read that one too — the screenshots,
the honest "what is not finished" copy and the reasoning behind them are shared,
and only the mechanics differ.

## Read this first: Steam is not itch, in three ways that cost time

**It costs $100 per app.** Recoupable against $1,000 of revenue, which a free
game will never reach, so treat it as spent. Valve also requires bank details
and tax forms (a W-8BEN if you are outside the US) before the app can be
released **even though the game is free**. There is no way to skip that step and
no donate button on Steam to justify it — funding stays on GitHub Sponsors and
itch.

**There is a 30-day wall.** Steam will not let an app release until 30 days
after the app fee clears, and the store page has to be public for at least two
weeks before the release date. So the *next* version cannot be the Steam launch.
What it can be is the first build sitting on a private Steam branch while the
page is in review — which is worth doing, because the first upload is the first
real proof the depots are configured correctly.

**Valve reviews things.** The store page and the first build both, a few
business days each. Neither is a formality and both can come back with notes.

Add it up: from paying the fee to a public page is about six weeks of calendar,
most of it waiting. Nothing in this repository shortens it.

---

## 1. Become a Steamworks partner

Your ordinary Steam account is the right starting point — partner signup runs on
top of it rather than beside it.

1. <https://partner.steamgames.com/> → **Sign up**, using the Steam account you
   already have.
2. Digital distribution agreement, identity verification, then the tax and bank
   sections. All of it is yours to fill in; none of it can be automated and none
   of it should be shared.
3. Pay the $100 app fee. An **appid** appears once it clears.

Write the appid down. Everything below refers to it.

---

## 2. Create the app and its depots

**Steamworks → your app → Edit Steamworks Settings.**

| Depot | Name | OS | Suggested id |
|---|---|---|---|
| Windows | OpenDoctrines Windows | Windows | `appid + 1` |
| Linux | OpenDoctrines Linux | Linux | `appid + 2` |

`appid + 1` is the depot Steamworks creates for a new app on its own; the second
you add. If yours came out in another order, set `STEAM_DEPOT_WINDOWS` and
`STEAM_DEPOT_LINUX` (below) rather than renumbering anything — a depot id that
is not yours belongs to somebody else's app, and the push fails outright.

### Launch options

**Installation → General Installation → Launch Options.** Without these Steam
installs the game and has no idea how to start it.

| Executable | Arguments | OS |
|---|---|---|
| `OpenDoctrines.exe` | *(none)* | Windows |
| `OpenDoctrines` | *(none)* | Linux |

The depot root is the install directory, so those are the paths exactly as
`package.py` lays them out — no subfolder.

### No macOS, no browser build, no Android

Say this out loud on the store page rather than letting a Mac owner find out
after buying in:

- **macOS** — Steam requires macOS builds to be signed *and* notarized, and this
  project ships neither. It also wants one universal binary where we build arm64
  and x64 separately, so the two would need `lipo`-ing together first. Both are
  solvable; neither is solved. Mac players keep the zips and the `.dmg` from
  GitHub and itch.io.
- **The browser build** has no Steam equivalent at all. It stays on itch, which
  is the only place anyone can try the game without downloading it.
- **The APK** likewise.

---

## 3. The CI credential

itch needs one API key. Steam needs a **sealed login session**, which is fussier
and expires on Valve's schedule rather than yours.

Make a **separate builder account** — not the partner account you log into the
website with. Give it access to this app only, in **Users & Permissions**. A CI
credential that can publish one app is a much smaller problem than one that can
publish everything and change the bank details.

Then, once, on your own machine:

```bash
steamcmd +login od-builder +quit
```

Type the password and the Steam Guard code. On success steamcmd writes a
`config.vdf` holding a refresh token:

| Platform | Path |
|---|---|
| macOS | `~/Library/Application Support/Steam/config/config.vdf` |
| Linux | `~/Steam/config/config.vdf` |

Base64 it and store that as the repository secret:

```bash
base64 -i ~/Library/Application\ Support/Steam/config/config.vdf | gh secret set STEAM_CONFIG_VDF
```

**It will expire.** Not on a schedule you control, and the symptom is a release
whose very last step fails after everything else has published. That is exactly
what `.github/workflows/check-steam.yml` is for — run it before you tag, the way
`check-itch.yml` exists for the same reason. If it reports a Steam Guard prompt,
redo the two commands above.

### Variables and secrets

| Name | Kind | Example | Absent means |
|---|---|---|---|
| `STEAM_APPID` | variable | `3401230` | Steam step skips with a warning |
| `STEAM_USERNAME` | variable | `od-builder` | Steam step skips with a warning |
| `STEAM_CONFIG_VDF` | **secret** | base64 blob | Steam step skips with a warning |
| `STEAM_DEPOT_WINDOWS` | variable | `3401231` | defaults to appid + 1 |
| `STEAM_DEPOT_LINUX` | variable | `3401232` | defaults to appid + 2 |
| `STEAM_BRANCH` | variable | *(empty)* | uploads without promoting |

```bash
gh variable set STEAM_APPID --body 3401230
gh variable set STEAM_USERNAME --body od-builder
```

Missing Steam configuration **warns and does not fail the release**, the same
bargain the itch push makes: a storefront that is not set up yet is not a reason
to withhold a GitHub release that has already been built, tested and approved.

---

## 4. Store art

Steam has no theme panel. There are no colours to pick, no fonts, no background
behind the text column — the capsules and the screenshots are the whole of the
page's appearance, which makes them matter more here than the equivalents do on
itch.

Every size below is **required and exact**. Steam rejects a capsule that is one
pixel out rather than scaling it.

| Capsule | Size | Where it appears |
|---|---|---|
| Header | 460×215 | search, library, wishlists — the one seen most |
| Small | 231×87 | search results |
| Main | 616×353 | front page, "more like this" |
| Vertical | 374×448 | front-page carousel |
| Library | 600×900 | the player's own library grid |
| Library hero | 3840×1240 | across the top of the library page |
| Library logo | 1280×720 | the wordmark, **transparent**, over the hero |
| Page background | 1438×810 | behind the store page |

`tools/banner.py` builds all but the logo:

```bash
python3 tools/banner.py --size steam-header   --region europe --out-dir docs/steam
python3 tools/banner.py --size steam-main     --region europe --out-dir docs/steam
python3 tools/banner.py --size steam-hero     --region atlantic --out-dir docs/steam
python3 tools/banner.py --size steam-vertical --region atlas --out-dir docs/steam
python3 tools/banner.py --size steam-library  --region atlas --out-dir docs/steam
```

`--region atlas` for the two portrait capsules — the North Cape down to the Gulf
of Guinea, cut to roughly the shape of the 374×448 capsule already. Ask for a
portrait capsule from `europe` instead and the aspect trim throws away the outer
columns, leaving a sliver of the Baltic; ask for one from a region far *taller*
than the capsule and it trims the top and bottom instead, which is how the first
attempt at this came out as a picture of the Sahara with Europe cut off.

Each command writes two files — `banner-<size>.png` with the wordmark and
`-plain.png` without. Use the titled one everywhere except the library hero,
which sits *behind* the separate logo and must not have a second wordmark on it.

For the **231×87 small capsule**, check it at actual size before uploading. A
world map at that scale is a blue-grey smudge; if the wordmark does not read,
crop tighter rather than shipping something illegible.

For the **library logo**, `tools/itch-cover.py` already lifts the wordmark
pixel-for-pixel out of the game's own menu and masks everything but the gold
strokes to transparent. That mask is the asset — place it on a 1280×720
transparent canvas.

### Screenshots

The same seven from `docs/img/` that `docs/itch/README.md` lists, in the same
order, `world-map.png` first. Steam wants at least five and prefers 1920×1080.
Regenerate with `tools/screenshots.sh` when the UI changes; the page and the
game do not otherwise stay in step.

### A trailer

Steam pages without one convert badly, and `docs/img/timelapse-political.gif` is
the most persuasive thing this project has. Steam takes video, not GIF.

---

## 5. The description

**Not `docs/itch/description.html`.** Steam's editor is **BBCode** — `[h2]`,
`[list]`, `[b]` — so pasted HTML arrives as literal tags. The copy is worth
keeping identical in substance; only the markup is rewritten.

Keep the "What is not finished" section. It is the part of the page that earns
trust, and it earns more of it on a storefront than on itch.

Add one line the itch page does not need: **macOS is not on Steam**, per section
2 above.

---

## 6. What CI does on release day

`.github/workflows/release-game.yml` gains a `steam` job after `publish`:

1. Extracts the Windows and Linux zips and drops a `MANAGED` marker in each —
   Steam owns the install and updates it, so the in-game updater must hold off.
   Same reasoning as the itch push. See `GameUpdates::managedInstall()`.
2. Substitutes the appid, depot ids and content paths into
   `packaging/steam/app_build.vdf`.
3. Runs `steamcmd +run_app_build`.

**It uploads; it does not promote.** The build lands in Steamworks and sits
there. You set it live on a branch by hand, which is one step less automatic
than the itch push on purpose: Valve reviews the first build before it can go
anywhere, and a build set live on `default` reaches every owner within minutes
with no undo except shipping again.

Once the app is out of review and you want promotion in CI, set `STEAM_BRANCH`.
Use `beta` first. `default` only when you mean it.

---

## 7. Before you release

- Run `check-steam.yml` — before tagging, not after.
- Push a build to a **beta branch** and install it through the Steam client on a
  machine that has never had the game. That is the only way to catch a depot
  whose root is one level off: the upload succeeds, and Steam installs a folder
  the launch option cannot start.
- Confirm the in-game update screen says updates are managed. If it offers to
  update itself, the `MANAGED` marker did not reach the depot.
- Check `data/audio/` is in the install. It is on the allowlist, but a silent
  game is the failure that does not announce itself.
- Read "What is not finished" once more and make sure it is still true.
