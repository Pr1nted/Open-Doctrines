Import your own interface font
==============================

Drop a .ttf or .otf file in this folder, then open
  Settings -> Display -> Font
and cycle to its name. "Pixel" is the original bitmap look, "Readable" is the
built-in vector font, and anything you add here appears after those.

For safety, an imported font is checked before it is used. It is refused if it:
  - is larger than 4 MB (a UI font is far smaller),
  - is not a single TrueType/OpenType font (no font collections),
  - has a table that points outside the file (the usual exploit),
  - carries a hinting program (fpgm/prep) -- so imported fonts cannot ship code,
  - has no character map or no glyph outlines.

A font that is rejected is simply not listed; the reason is written to the log.
