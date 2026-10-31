# Accessibility

What Open Doctrines does for players who see, hear or handle a game
differently, and where to find it. Everything below is in **Settings**, and
every setting takes effect immediately, without a restart.

If something here doesn't work for you, or something you need is missing,
please open an issue. Name the setting, or the screen you couldn't use.

## Seeing

- **Colour-blind palettes**: *Settings → Display → Colourblind Colours*.
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
- **A readable font by default**: *Settings → Display → Font*.
  - The interface uses a clear vector face. The original pixel font is still
    available for anyone who prefers it.
  - You can also import a font of your own, such as a dyslexia-friendly face.
    It is checked before it is used, and takes effect as soon as it is
    selected.
- **Accent colour**: *Settings → Display → Accent Color*. Changes the colour
  used for highlights and selections.
- **Zoom**: *Settings → Display → Max Zoom* sets how far the map can zoom in.

## Hearing

- **Separate volumes** for the whole game, the music and the sound effects,
  under *Settings → Audio*.
- **Now Playing toast**: *Settings → Audio → Now Playing Toast* shows the name
  of each new music track on screen.

## Moving and handling

- **Turn-based, with no clock.** Nothing in a single-player game happens
  until you end your turn, and nothing asks for speed or precise timing.
- **Every key can be rebound**: *Settings → Keybinds*.
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

- **A high-contrast mode.** The interface's colours are written out where
  each screen draws, not taken from one theme. A real high-contrast mode
  means gathering them into one theme first, and that work is planned.
- **Screen-reader support.** The interface is drawn by the game itself, not
  by the operating system's widgets, so screen readers can't see it.
