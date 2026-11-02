# Accessibility

What Open Doctrines does for players who see, hear or handle a game
differently, and where to find it. Everything below is in **Settings**, most
of it under **Settings → Accessibility**, and every setting takes effect
immediately, without a restart.

If something here doesn't work for you, or something you need is missing,
please open an issue. Name the setting, or the screen you couldn't use.

## Seeing

- **High contrast**: *Settings → Accessibility → High Contrast*.
  - Every panel becomes opaque black and every line of text white, which is
    21:1 contrast.
  - Coloured text, outlines and small markers are brightened until they read
    against black at 7:1 or better. Coloured buttons are darkened until their
    white labels read at 7:1 or better.
  - Colours keep their meaning: war stays red and alliances stay green, so
    high contrast works together with the colour-blind palettes.
  - The map, flags and portraits are left as they are.
  - `tests/theme_test.cpp` checks these numbers across the whole colour range
    every time the suite runs.
- **Colour-blind palettes**: *Settings → Accessibility → Colourblind Colours*
  (also under *Settings → Display*).
  - One palette each for deuteranopia, protanopia and tritanopia.
  - They recolour everything whose colour carries meaning: the relations map
    (war, alliance, guarantee, non-aggression), and every good/bad/warning
    number in the interface.
  - Each palette was chosen by measuring how far apart its colours stay under
    that deficiency, not by eye: about 58 points apart for deuteranopia and
    protanopia, and about 40 for tritanopia, where 20 is the floor for
    "obviously different". `tools/check_palette.py` re-checks the numbers.
- **The map never relies on colour alone.** The legend and the country
  panel spell out every relation in words.
- **Interface size**: *Settings → Display → UI Scale*.
  - Eight steps from 0.75× to 2×, applied to the whole interface at once.
  - On Android it is added on top of the platform's own minimum, so a phone
    that needs extra size still gets it.
- **Text size**: *Settings → Accessibility → Text Size*.
  - 100%, 110% or 120%. It makes the text bigger without growing the panels
    around it.
  - Above 120% some screens begin to crowd. For more than that, use UI Scale,
    which grows everything together.
- **A readable font by default**: *Settings → Display → Font*.
  - The interface uses a clear vector face. The original pixel font is still
    available for anyone who prefers it.
  - **Hyperlegible**: Atkinson Hyperlegible, the Braille Institute's face for
    readers with low vision. Letters that are easy to confuse, such as I, l
    and 1, or O and 0, are drawn so that each one looks different. It covers
    Latin scripts; other scripts fall back to the game's usual font.
  - You can also import a font of your own, such as a dyslexia-friendly face.
    It is checked before it is used, and takes effect as soon as it is
    selected.
- **Accent colour**: *Settings → Display → Accent Color*. Changes the colour
  used for highlights and selections.
- **Zoom**: *Settings → Display → Max Zoom* sets how far the map can zoom in.
- **Reduce motion**: *Settings → Accessibility → Reduce Motion* stops the
  interface's pulsing highlights, such as the unread-mail glow and the alert
  markers, and holds them steady.

## Hearing

- **Separate volumes** for the whole game, the music and the sound effects,
  under *Settings → Audio*.
- **Sound cues**: *Settings → Accessibility → Sound Cues*. When the game
  refuses an action, it shows a marker at the pointer as well as playing the
  refusal sound. This works even with sound off or no audio device.
- **Now Playing toast**: *Settings → Audio → Now Playing Toast* shows the name
  of each new music track on screen.

## Moving and handling

- **Turn-based, with no clock.** Nothing in a single-player game happens
  until you end your turn, and nothing asks for speed or precise timing.
- **Every key can be rebound**: *Settings → Keybinds*.
- **Hold to confirm**: *Settings → Accessibility → Hold to Confirm*.
  - Declaring war and deleting a world or a map need the button held for just
    under a second, with a bar filling as you hold.
  - A tremor or a slipped click can't trigger them. Letting go early cancels.
- **Longer messages**: *Settings → Accessibility → Longer Messages* keeps
  notifications on screen three times as long.
- **Controllers.** A gamepad drives the same cursor and buttons as the mouse,
  so every screen works with one, not only the screens written with
  controllers in mind.
- **Touch.** Touchscreens, including the Steam Deck and touch laptops, deliver
  real touches: tap to click, drag to move, two fingers to zoom. An on-screen
  keyboard is available where there is no physical one.
- **Map speed**: *Settings → Controls → Fly Speed* sets how fast the camera
  moves.

## Reading

- **Languages.** The interface is translated. Choose a language under
  *Settings → Language*.

## Not yet supported

Better to say so here than to let you find out in the game:

- **Screen-reader support.** The interface is drawn by the game itself, not
  by the operating system's widgets, so screen readers can't see it.
