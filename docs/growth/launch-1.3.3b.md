# Launching 1.3.3b — paste-ready

The reach release: the game now runs on more than thirty platforms, turns
process about three times faster, and a new Accessibility tab arrives. Nothing
here is posted by a workflow. The itch devlog is dashboard-only, and the
announcement board needs the developer badge or the admin secret (section 2).
Images are served from the repo through jsdelivr:

    https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/<file>

The itch devlog (section 1) is plain ASCII on purpose: no em dashes, no curly
quotes, no arrows.

---

## 1. itch.io devlog

**Where:** itch dashboard -> **Devlog -> New post**.

**Title:** 1.3.3b - thirty platforms, three times faster, and accessibility

**Body:**

Hello strategists!

This is the biggest release we have made by reach. Open Doctrines now runs on
more than thirty platforms, a turn takes about a third of the time it used to,
and there is a whole new Accessibility tab in Settings.

### New places to play

- **Steam Deck and handhelds.** The Linux and Windows builds now have real
  touch: tap to click, drag to move, two fingers to zoom. On the Deck the
  window fits the screen, Game Mode goes fullscreen, and the controller drives
  everything.
- **Raspberry Pi OS** (64-bit and 32-bit), drawn on the Pi's GPU, with .deb
  packages.
- **ChromeOS**, through its Linux container, plus an Android APK for every CPU
  a Chromebook has.
- **NetBSD**, and **Arm builds** of FreeBSD, OpenBSD and NetBSD.
- **OpenIndiana (illumos)**, **Haiku** and **SerenityOS**.
- **PlayStation Vita**, **Nintendo Wii**, **Nintendo 3DS**, **DOS** and
  **RISC OS** as homebrew. They load smaller worlds to fit in memory, and play
  offline for now.
- Two experiments for the curious: a **FreeRTOS** edition you play over a
  serial line on a microcontroller, and a **Plan 9** edition with a console and
  a clickable map. Both play the same game, turn for turn, as every other build.

### Accessibility

Settings -> Accessibility, all off until you turn them on:

- **High contrast**: black panels, white text, and every coloured line and
  button made readable, while war stays red and alliances stay green.
- **Text size** up to 120%.
- **Reduce motion**: the pulsing highlights hold still.
- **Hold to confirm** for declaring war and deleting worlds or maps.
- **Longer messages**, and **sound cues** you can see.
- **A Hyperlegible font**: Atkinson Hyperlegible, designed by the Braille
  Institute for low vision.

They are translated into every language the game speaks.

### Faster

A turn now processes 2.7 to 3.1 times faster, with exactly the same results.
Saves store each turn as a difference from the last, so they write faster and
take less space, and the map editor generates worlds about three times faster.

### Also in this release

- A clear vector font draws the interface by default. The pixel font is still
  there, and you can import your own font.
- Mods can now reshape the map while the game runs: morph provinces and edit
  terrain. Modders: this is Gearbox 1.5, out alongside, and map scripts get the
  same two commands as engine version 4.
- The economy breakdown shows monuments and sector flows.
- The web version scales properly on Retina screens and has an on-screen
  keyboard on touch devices.
- Multiplayer orders resolve the same way on every machine, and a save can no
  longer be left half-written by a crash.
- Signing in got friendlier: more time for the browser sign-in, and a way back
  when an account link goes wrong.

Pick the build for your system from the download list. If yours is one of the
new ones, tell us how it runs: most of these platforms are new to us, too.

See you on the map.

---

## 2. In-game announcement board

**From the game:** main menu -> Reports -> Announcements (developer badge).

**Or from a terminal** (the `ADMIN_SECRET` set with `wrangler secret put`):

```sh
curl -sS -X POST https://opendoctrines-net.opendoctrines.workers.dev/admin/announcement \
  -H "x-od-admin: $ADMIN_SECRET" -H 'content-type: application/json' \
  --data-binary @- <<'JSON'
{"op":"put","item":{
  "id":"v1-3-3b",
  "title":"1.3.3b: thirty platforms, three times faster",
  "body":"Open Doctrines now runs on Steam Deck, Raspberry Pi, ChromeOS, every BSD on x64 and Arm, illumos, Haiku, SerenityOS, and as homebrew on Vita, Wii, 3DS, DOS and RISC OS. Turns process about three times faster, saves are smaller, and Settings has a new Accessibility tab: high contrast, larger text, reduce motion, hold to confirm, longer messages, visible sound cues and a Hyperlegible font.",
  "buttonLabel":"Community",
  "buttonAction":"community"
}}
JSON
```

To take it down later: `{"op":"hide","id":"v1-3-3b"}`.

---

## 3. Discord

**#announcements:**

**1.3.3b is out — the "everywhere" one.** :rocket:

**30+ platforms.** Steam Deck (touch, controller, Game Mode fullscreen),
Raspberry Pi OS, ChromeOS, NetBSD and Arm builds of every BSD, OpenIndiana,
Haiku and SerenityOS. Homebrew for **PS Vita, Wii, 3DS, DOS and RISC OS**. And
two experiments: a **FreeRTOS** edition played over a serial line, and a
**Plan 9** edition with a clickable map — both play the exact same game,
turn for turn.

**Turns are ~3× faster**, with bit-identical results. Saves store each turn as a
diff, so they write faster and stay smaller. The map editor generates worlds
~3× faster too.

**New Accessibility tab:** high contrast, text up to 120%, reduce motion, hold
to confirm war and deletes, longer messages, visible sound cues, and the
**Atkinson Hyperlegible** font. Translated into every language.

**Also:** a readable vector font by default (or import your own), mods can
reshape the map at runtime (**Gearbox 1.5**, and `#OD/MapEngine/4` for map scripts), monuments and sector flows in the economy
breakdown, Retina-sharp web build with an on-screen keyboard.

Running it on something unusual? Post a screenshot in #showcase — half these
platforms are new to us too.
