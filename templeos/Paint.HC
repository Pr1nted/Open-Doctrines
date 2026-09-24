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

U0 PText(I64 x, I64 y, U8 *s, I64 r, I64 g, I64 b, I64 scale = 1)
{
  I64 i = 0;
  while (s[i])
  {
    PChar(x + i * 8 * scale, y, s[i], r, g, b, scale);
    i++;
  }
}

/* Printf into a buffer, then draw it. Saves every caller writing the same
 * three lines, and keeps the format strings next to what they describe. */
U0 PPrint(I64 x, I64 y, I64 r, I64 g, I64 b, I64 scale, U8 *fmt, ...)
{
  U8 *s = StrPrintJoin(NULL, fmt, argc, argv);
  PText(x, y, s, r, g, b, scale);
  Free(s);
}
