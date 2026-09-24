/* Drawing Open Doctrines on a screen TempleOS does not believe it has.
 *
 * Gfx.HC puts the card into a 1024x768 linear framebuffer at 32 bits per
 * pixel. From that moment the OS's own graphics stack is wrong about the
 * screen -- it is still writing planar VGA -- so everything here writes to the
 * aperture directly and owns the display until the mode is put back.
 *
 * That is the price of not looking like 1985, and it buys the actual palette:
 * the real game's country colours, a near-black ground, borders that are a
 * darker shade of the country rather than a black line, and panels that are
 * translucent over the map instead of a bar beside it.
 */
#include "Gfx"
#include "World"   /* for WReadEither: the shouted-filename dance */

#define P_W    1024
#define P_H     768
#define P_BPP     4

U8 *p_fb = NULL;

/* ── STAYING OUT OF THE OLD VGA WINDOW ──
 *
 * TempleOS's window manager redraws its own planar screen thirty times a
 * second through the legacy aperture at 0xA0000, and that aperture is the
 * FIRST 64 KB of video memory -- which in a 1024-wide 32-bit mode is the top
 * sixteen rows of this display. It arrives as a band of hatched garbage along
 * the top of the picture, written by an OS that has not been told the screen
 * changed underneath it.
 *
 * It cannot be switched off for long: the refresh is gated by a semaphore the
 * window manager clears every pass, so suppressing it means fighting for it
 * every frame. Moving out of the way is better. The card is told the visible
 * page starts P_SKIP rows into memory, and everything this file draws is
 * offset to match, so the corruption lands on rows nobody is shown. */
#define P_SKIP   32          /* rows of video memory left to the old aperture */

Bool PaintBegin()
{
  U8 *base;
  base = GfxFrameBuffer;
  if (!base || !GfxAvailable) return FALSE;
  DispiMode(P_W, P_H, 32);
  DispiWrite(6, P_W);              /* virtual width  */
  DispiWrite(7, P_H + P_SKIP);     /* virtual height, with room for the skip */
  DispiWrite(8, 0);                /* x offset */
  DispiWrite(9, P_SKIP);           /* y offset: the visible page starts here */
  p_fb = base + P_SKIP * P_W * P_BPP;
  return TRUE;
}

U0 PaintEnd()
{
  GfxRestore;
  p_fb = NULL;
}

/* BGRX, which is what this card wants and what "32-bit colour" means here:
 * the X byte is not alpha, nothing composites it, it is padding. */
U0 Px(I64 x, I64 y, I64 r, I64 g, I64 b)
{
  I64 o;
  if (x < 0 || y < 0 || x >= P_W || y >= P_H) return;
  o = (y * P_W + x) * P_BPP;
  p_fb[o]     = b;
  p_fb[o + 1] = g;
  p_fb[o + 2] = r;
}

/* Blend over what is already there. The panels in the real game are dark
 * glass over the map, not opaque boxes, and the map showing faintly through
 * them is most of why it reads as one picture rather than a UI beside an
 * image. */
U0 PxBlend(I64 x, I64 y, I64 r, I64 g, I64 b, I64 a)
{
  I64 o, ia;
  if (x < 0 || y < 0 || x >= P_W || y >= P_H) return;
  o = (y * P_W + x) * P_BPP;
  ia = 255 - a;
  p_fb[o]     = (p_fb[o] * ia + b * a) / 255;
  p_fb[o + 1] = (p_fb[o + 1] * ia + g * a) / 255;
  p_fb[o + 2] = (p_fb[o + 2] * ia + r * a) / 255;
}

U0 PFill(I64 x, I64 y, I64 w, I64 h, I64 r, I64 g, I64 b)
{
  I64 i, j;
  for (j = y; j < y + h; j++)
    for (i = x; i < x + w; i++)
      Px(i, j, r, g, b);
}

U0 PGlass(I64 x, I64 y, I64 w, I64 h, I64 r, I64 g, I64 b, I64 a)
{
  I64 i, j;
  for (j = y; j < y + h; j++)
    for (i = x; i < x + w; i++)
      PxBlend(i, j, r, g, b, a);
}

U0 PRect(I64 x, I64 y, I64 w, I64 h, I64 r, I64 g, I64 b)
{
  I64 i;
  for (i = x; i < x + w; i++)
  {
    Px(i, y, r, g, b);
    Px(i, y + h - 1, r, g, b);
  }
  for (i = y; i < y + h; i++)
  {
    Px(x, i, r, g, b);
    Px(x + w - 1, i, r, g, b);
  }
}

/* ── THE GAME'S OWN FONT ──
 *
 * Open Doctrines draws its interface with raylib's built-in bitmap font, so
 * matching it is not a matter of choosing something similar: tools/
 * templeos_font.py bakes the SAME 224 glyphs out of raylib's rtext.c, and the
 * text here is pixel-for-pixel what the desktop game draws.
 *
 * It is one bit per pixel, which also settles the obvious temptation: the
 * desktop game's text is not antialiased either, and a smooth atlas baked
 * from a TTF would look less like the game rather than more.
 *
 * Without the file everything still works in the OS's own 8x8 font -- worse
 * looking, never broken.
 */
U8 *pf_data = NULL;
I64 pf_count = 0, pf_height = 0, pf_first = 32, pf_rows = 0, pf_widths = 0;

Bool PFontLoad(U8 *path = "font.odf")
{
  U8 *b;
  I64 size = 0;

  b = WReadEither(path, &size);
  if (!b) return FALSE;
  if (b[0] != 'O' || b[1] != 'D' || b[2] != 'T' || b[3] != 'F')
  {
    Free(b);
    return FALSE;
  }
  pf_count  = b[6] + (b[7] << 8);
  pf_height = b[8] + (b[9] << 8);
  pf_first  = b[10] + (b[11] << 8);
  pf_widths = 12;
  pf_rows   = 12 + pf_count;
  pf_data   = b;                   /* kept for the life of the program */
  return TRUE;
}

I64 PFWidth(I64 ch)
{
  I64 gi = ch - pf_first;
  if (!pf_data || gi < 0 || gi >= pf_count) return 8;
  return pf_data[pf_widths + gi];
}

U0 PFChar(I64 x, I64 y, I64 ch, I64 r, I64 g, I64 b, I64 scale)
{
  I64 gi, row, col, bits, off, w, i, j;

  gi = ch - pf_first;
  if (gi < 0 || gi >= pf_count) return;
  w = pf_data[pf_widths + gi];
  off = pf_rows + gi * pf_height * 2;
  for (row = 0; row < pf_height; row++)
  {
    bits = pf_data[off + row * 2] + (pf_data[off + row * 2 + 1] << 8);
    for (col = 0; col < w; col++)
      if (bits & (1 << col))
      {
        if (scale == 1)
          Px(x + col, y + row, r, g, b);
        else
          for (j = 0; j < scale; j++)
            for (i = 0; i < scale; i++)
              Px(x + col * scale + i, y + row * scale + j, r, g, b);
      }
  }
}

/* ── TEXT ──
 *
 * text.font is 256 glyphs of eight bytes, one byte per row, bit 0 leftmost.
 * Drawn a pixel at a time because there is no blitter here and no need for
 * one: a panel of text is a few thousand pixels and the screen is only
 * redrawn when something changes.
 *
 * `scale` doubles or trebles the glyph. The font is 8x8 and the real game's
 * is a thin condensed face; this will not be mistaken for it, but at 1x it is
 * small enough to carry the same density of information, which matters more.
 */
U0 PChar(I64 x, I64 y, I64 ch, I64 r, I64 g, I64 b, I64 scale = 1)
{
  U8 *gl;
  I64 row, col, i, j;

  /* text.font is NOT a U8* and must be cast to be indexed as bytes -- the OS
   * does exactly this in GrBitMap.HC. Without the cast the pointer strides
   * eight times too far and every glyph is read out of the middle of another
   * one, which draws text-shaped noise. */
  gl = text.font(U8 *) + (ch & 0xFF) * 8;
  for (row = 0; row < 8; row++)
    for (col = 0; col < 8; col++)
      if (gl[row] & (1 << col))
      {
        if (scale == 1)
          Px(x + col, y + row, r, g, b);
        else
          for (j = 0; j < scale; j++)
            for (i = 0; i < scale; i++)
              Px(x + col * scale + i, y + row * scale + j, r, g, b);
      }
}

/* Proportional when the game's font is loaded, fixed-pitch when it is not.
 * The advance is width + 1, which is the divisor raylib lays the sheet out
 * with, so a line of text is the same length here as on the desktop. */
U0 PText(I64 x, I64 y, U8 *s, I64 r, I64 g, I64 b, I64 scale = 1)
{
  I64 i = 0, pen = x;
  while (s[i])
  {
    if (pf_data)
    {
      PFChar(pen, y, s[i], r, g, b, scale);
      pen += (PFWidth(s[i]) + 1) * scale;
    }
    else
    {
      PChar(pen, y, s[i], r, g, b, scale);
      pen += 8 * scale;
    }
    i++;
  }
}

/* How wide a string will be, for right-aligning and for centring in a tab. */
I64 PTextW(U8 *s, I64 scale = 1)
{
  I64 i = 0, n = 0;
  while (s[i])
  {
    if (pf_data) n += (PFWidth(s[i]) + 1) * scale;
    else         n += 8 * scale;
    i++;
  }
  return n;
}

/* Printf into a buffer, then draw it. Saves every caller writing the same
 * three lines, and keeps the format strings next to what they describe. */
U0 PPrint(I64 x, I64 y, I64 r, I64 g, I64 b, I64 scale, U8 *fmt, ...)
{
  U8 *s = StrPrintJoin(NULL, fmt, argc, argv);
  PText(x, y, s, r, g, b, scale);
  Free(s);
}
