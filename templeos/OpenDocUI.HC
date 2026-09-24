/* Open Doctrines, drawn and played.
 *
 * ── WHY THIS READS THE WORLD AND NOT A PICTURE ──
 *
 * The first version of this drew a raster of colours. It looked like a map and
 * could not be played on: a province that changes hands cannot be repainted
 * when the only thing on file is the colour it used to be, and a click could
 * only be answered by guessing the nearest province centre.
 *
 * So the file is a world, not a view of it. Every pixel carries a province
 * INDEX; colour is decided here, from who owns that province right now. That
 * one change buys three things at once:
 *
 *   * province borders, because a border is where the index changes,
 *   * an exact answer to a click, by reading the index under the pointer,
 *   * a map that can be repainted when a province is taken.
 *
 * tools/templeos_world.py bakes it: 1,615 provinces with adjacency,
 * population, armies and income, 97 KB.
 */
#include "OpenDoc"

#define ODW_MAXP    4096
#define ODW_MAXC     256
#define ODW_SEA   0xFFFF        /* the index meaning "no province" */
#define ODW_FONTH     8
#define ODW_HDR      22         /* header bytes before the runs */
#define ODW_PREC     24         /* bytes per province record */
#define ODW_CREC     10         /* bytes per country record */
#define ODW_COL_SEA   1         /* BLUE  */
#define ODW_COL_LINE  0         /* BLACK */

CDC *odw_dc = NULL;
U16 *odw_pmap = NULL;           /* province index per pixel */
U16 *odw_adj = NULL;            /* flattened adjacency lists */
I64  odw_w = 0, odw_h = 0, odw_np = 0, odw_nc = 0, odw_nadj = 0;

I64  odw_pid[ODW_MAXP], odw_cx[ODW_MAXP], odw_cy[ODW_MAXP];
I64  odw_owner[ODW_MAXP], odw_pop[ODW_MAXP], odw_army[ODW_MAXP];
I64  odw_inc[ODW_MAXP], odw_fort[ODW_MAXP];
I64  odw_adjn[ODW_MAXP], odw_adjo[ODW_MAXP];

I64  odw_ccid[ODW_MAXC], odw_ccol[ODW_MAXC], odw_ctre[ODW_MAXC];
U8   odw_ciso[ODW_MAXC][4];

I64  odw_sel = -1;              /* province index, not a province id */
I64  odw_mine[ODW_MAXP], odw_nmine = 0, odw_minei = -1;
I64  odw_me = -1;               /* the player's country id */
I64  odw_mod = 0, odw_cur = 0, odw_pick[4];
ODTurn odw_t;
U8   odw_msg[96];
F64  odw_sx, odw_sy;      /* the mouse scale we borrowed */

I64 ODWU16(U8 *b) { return b[0] + (b[1] << 8); }
I64 ODWU32(U8 *b) { return b[0] + (b[1] << 8) + (b[2] << 16) + (b[3] << 24); }

I64 ODWColourOf(I64 cid)
{
  I64 i;
  for (i = 0; i < odw_nc; i++)
    if (odw_ccid[i] == cid) return odw_ccol[i];
  return 8;                     /* DKGRAY: owned by nobody this file knows */
}

U8 *ODWIsoOf(I64 cid)
{
  I64 i;
  for (i = 0; i < odw_nc; i++)
    if (odw_ccid[i] == cid) return odw_ciso[i];
  return "--";
}

Bool ODWLoad(U8 *path)
{
  U8 *b;
  I64 size = 0, i, p, total, n, ix, off, runs, cn;

  b = ODReadEither(path, &size);
  if (!b)
  {
    StrPrint(odw_msg, "cannot read %s", path);
    return FALSE;
  }
  if (b[0] != 'O' || b[1] != 'D' || b[2] != 'T' || b[3] != 'W')
  {
    StrCpy(odw_msg, "that is not a world file");
    Free(b);
    return FALSE;
  }
  odw_w    = ODWU16(b + 6);
  odw_h    = ODWU16(b + 8);
  odw_np   = ODWU16(b + 10);
  cn       = ODWU16(b + 12);
  runs     = ODWU32(b + 14);
  odw_nadj = ODWU32(b + 18);
  odw_nc   = cn;
  if (odw_np > ODW_MAXP) odw_np = ODW_MAXP;
  if (odw_nc > ODW_MAXC) odw_nc = ODW_MAXC;

  /* The index raster. Three bytes a run: a count and a 16-bit index. */
  total = odw_w * odw_h;
  odw_pmap = MAlloc(total * 2);
  i = ODW_HDR;
  p = 0;
  while (p < total && i + 2 < size)
  {
    n  = b[i];
    ix = ODWU16(b + i + 1);
    i += 3;
    while (n > 0 && p < total)
    {
      odw_pmap[p] = ix;
      p++;
      n--;
    }
  }

  off = ODW_HDR + runs;
  for (p = 0; p < odw_np; p++)
  {
    odw_pid[p]   = ODWU32(b + off);
    odw_cx[p]    = ODWU16(b + off + 4);
    odw_cy[p]    = ODWU16(b + off + 6);
    odw_owner[p] = ODWU16(b + off + 8);
    odw_pop[p]   = ODWU32(b + off + 10);
    odw_army[p]  = ODWU16(b + off + 14);
    odw_inc[p]   = ODWU16(b + off + 16);
    odw_adjn[p]  = b[off + 18];
    odw_fort[p]  = b[off + 19];
    odw_adjo[p]  = ODWU32(b + off + 20);
    off += ODW_PREC;
  }

  odw_adj = MAlloc(odw_nadj * 2 + 2);
  for (p = 0; p < odw_nadj; p++)
  {
    odw_adj[p] = ODWU16(b + off);
    off += 2;
  }

  for (p = 0; p < odw_nc; p++)
  {
    odw_ccid[p] = ODWU16(b + off);
    odw_ccol[p] = b[off + 2];
    odw_ciso[p][0] = b[off + 4];
    odw_ciso[p][1] = b[off + 5];
    odw_ciso[p][2] = b[off + 6];
    odw_ciso[p][3] = 0;
    odw_ctre[p] = ODWU16(b + off + 8);
    off += ODW_CREC;
  }

  odw_me = -1;
  for (p = 0; p < odw_nc; p++)
    if (!StrCmp(odw_ciso[p], odw_t.iso)) odw_me = odw_ccid[p];

  /* ── THE PROVINCES YOU CAN ACT ON ──
   *
   * Collected once so they can be stepped through from the keyboard. A map
   * you can only address with a pointer is a map you cannot play on a machine
   * whose pointer is not behaving, and stepping your own twelve provinces is
   * what a player wants nine times in ten anyway. */
  odw_nmine = 0;
  for (p = 0; p < odw_np; p++)
    if (odw_owner[p] == odw_me)
    {
      odw_mine[odw_nmine] = p;
      odw_nmine++;
    }

  Free(b);
  StrPrint(odw_msg, "%d provinces  you are %s", odw_np, odw_t.iso);
  return TRUE;
}

/* Paint the whole map from current ownership.
 *
 * Every pixel, every time -- about a quarter of a million of them, which is
 * nothing once per selection or once per turn and would be ruinous per frame.
 * That is exactly why it happens here and not in DrawIt.
 */
U0 ODWPaint()
{
  I64 x, y, k, ix, c, w, wi;
  U8 *body;

  if (!odw_dc || !odw_pmap) return;
  w = odw_w;
  wi = odw_dc->width_internal;
  body = odw_dc->body;

  for (y = 0; y < odw_h; y++)
    for (x = 0; x < w; x++)
    {
      k = y * w + x;
      ix = odw_pmap[k];
      if (ix == ODW_SEA)
        c = ODW_COL_SEA;
      else if (ix == odw_sel)
        c = WHITE;              /* the selection, unmistakably */
      else
        c = ODWColourOf(odw_owner[ix]);
      body[y * wi + x] = c;
    }

  /* ── THE LINES ──
   *
   * A border is simply where the province index changes. Drawn after the fill
   * so it cannot be painted over, and only on one side of each edge so a line
   * stays one pixel wide instead of two.
   */
  for (y = 0; y < odw_h; y++)
    for (x = 0; x < w; x++)
    {
      k = y * w + x;
      ix = odw_pmap[k];
      if (ix != ODW_SEA)
      {
        if (x + 1 < w && odw_pmap[k + 1] != ix)
          body[y * wi + x] = ODW_COL_LINE;
        else if (y + 1 < odw_h && odw_pmap[k + w] != ix)
          body[y * wi + x] = ODW_COL_LINE;
      }
    }
}

I64 ODWAt(I64 x, I64 y)
{
  I64 ix;
  if (x < 0 || y < 0 || x >= odw_w || y >= odw_h) return -1;
  ix = odw_pmap[y * odw_w + x];
  if (ix == ODW_SEA) return -1;
  return ix;
}

U0 ODWOrderLine(U8 *out)
{
  I64 m, i;
  Bool first = TRUE;
  *out = 0;
  for (m = 0; m < 4; m++)
    for (i = 0; i < odw_t.mod[m].count; i++)
      if (odw_pick[m] & (1 << i))
      {
        if (!first) StrPrint(out + StrLen(out), ", ");
        StrPrint(out + StrLen(out), "%c:%d",
                 odw_t.mod[m].letter, odw_t.mod[m].act[i].num);
        first = FALSE;
      }
}

U0 DrawIt(CTask *task, CDC *dc)
{
  I64 i, y, x, n, row, col, ix, mx, my;
  U8 line[160], mark;

  no_warn task;

  if (odw_dc) GrBlot(dc, 0, 0, odw_dc);

  y = odw_h;
  dc->color = BLACK;
  GrRect(dc, 0, y, 640, 480 - y);

  row = y + 2;
  dc->color = WHITE;
  GrPrint(dc, 2, row, "%s (%s)   Turn %d/%d   Land %d/%d (%s%%)   Army %d",
          odw_t.country, odw_t.iso, odw_t.turn, odw_t.turns,
          odw_t.mine, odw_t.owned, odw_t.share, odw_t.army);
  row += ODW_FONTH;
  GrPrint(dc, 2, row, "Treasury %s   Net %s (gross %s)   At war with: %s",
          odw_t.treasury, odw_t.net, odw_t.gross, odw_t.war);
  row += ODW_FONTH;

  /* The selected province, in full: this is the line that makes the map
   * something you can reason about rather than look at. */
  dc->color = LTCYAN;
  if (odw_sel >= 0)
  {
    ix = odw_sel;
    GrPrint(dc, 2, row,
            "province %d  %s   pop %d   army %d   income %d.%d   fort %d   "
            "%d neighbours",
            odw_pid[ix], ODWIsoOf(odw_owner[ix]), odw_pop[ix], odw_army[ix],
            odw_inc[ix] / 10, odw_inc[ix] % 10, odw_fort[ix], odw_adjn[ix]);
  }
  else
  {
    /* The pointer, in the same coordinates the map is in. Shown because the
     * mouse arrives scaled -- a host that asks for a click at 345 gets one at
     * 172 -- and a number on screen settles that in one look. */
    mx = ms.pos.x - Fs->pix_left - Fs->scroll_x;
    my = ms.pos.y - Fs->pix_top - Fs->scroll_y;
    GrPrint(dc, 2, row, "%s   -- click a province   [pointer %d,%d]",
            odw_msg, mx, my);
  }
  row += ODW_FONTH + 2;

  dc->color = YELLOW;
  GrPrint(dc, 2, row, "%s  budget %d   e/p/w/n module  j/k item  SPACE pick"
                      "  h/l province  ENTER send  ESC quit",
          odw_t.mod[odw_mod].label, odw_t.mod[odw_mod].budget);
  row += ODW_FONTH;

  n = odw_t.mod[odw_mod].count;
  for (i = 0; i < n; i++)
  {
    x = 4 + (i / 4) * 200;
    y = row + (i % 4) * ODW_FONTH;
    if (i == odw_cur)
    {
      dc->color = BLUE;
      GrRect(dc, x - 2, y - 1, 196, ODW_FONTH + 1);
    }
    col = LTGRAY;
    mark = ' ';
    if (odw_pick[odw_mod] & (1 << i))
    {
      col = LTGREEN;
      mark = '*';
    }
    dc->color = col;
    GrPrint(dc, x, y, "%c %c:%d %s", mark,
            odw_t.mod[odw_mod].act[i].letter,
            odw_t.mod[odw_mod].act[i].num,
            odw_t.mod[odw_mod].act[i].name);
  }

  ODWOrderLine(line);
  dc->color = LTGREEN;
  GrPrint(dc, 2, 480 - ODW_FONTH - 2, "orders: %s", line);
}

U0 OpenDocUI(U8 *turn = "turn.txt", U8 *world = "world.odw",
             U8 *orders = "orders.txt")
{
  I64 ch, i, mx, my;
  U8 *buf;
  I64 size = 0;
  U8 line[160];

  MemSet(odw_pick, 0, sizeof(odw_pick));
  StrCpy(odw_msg, "no world loaded");

  buf = ODReadEither(turn, &size);
  if (!buf)
  {
    "$$FG,RED$$Cannot read %s$$FG$$\n", turn;
    return;
  }
  ODParse(&odw_t, buf);
  Free(buf);

  SettingsPush;
  /* ── THE POINTER, 1:1 ──
   *
   * ms.scale multiplies every mouse delta before it becomes a position, and
   * the default is not one: a click aimed at 400,200 arrived at 196,96, which
   * on a world map is a different continent. A game wants the pointer where
   * the pointer is, so this borrows the scale and gives it back on the way out. */
  odw_sx = ms.scale.x;
  odw_sy = ms.scale.y;
  ms.scale.x = 1.0;
  ms.scale.y = 1.0;

  AutoComplete;        /* off: its window sits over the right of the screen */
  WinBorder;           /* off: the border costs two columns and a row */
  WinMax;              /* the desktop gives a task half the screen otherwise */
  DocCursor;
  DocClear;

  if (ODWLoad(world))
  {
    odw_dc = DCNew(odw_w, odw_h);
    ODWPaint;
  }
  Fs->draw_it = &DrawIt;

  try
  {
    while (TRUE)
    {
      if (ms.lb)
      {
        mx = ms.pos.x - Fs->pix_left - Fs->scroll_x;
        my = ms.pos.y - Fs->pix_top - Fs->scroll_y;
        if (my < odw_h)
        {
          i = ODWAt(mx, my);
          if (i >= 0 && i != odw_sel)
          {
            odw_sel = i;
            ODWPaint;            /* the selection is painted into the map */
          }
        }
        while (ms.lb) Yield;
      }

      ch = ScanChar;
      if (ch)
      {
        if (ch == CH_ESC || ch == CH_SHIFT_ESC)
          goto odw_done;
        else if (ch == 'e') { odw_mod = 0; odw_cur = 0; }
        else if (ch == 'p') { odw_mod = 1; odw_cur = 0; }
        else if (ch == 'w') { odw_mod = 2; odw_cur = 0; }
        else if (ch == 'n') { odw_mod = 3; odw_cur = 0; }
        else if (ch == 'j')
        {
          if (odw_cur + 1 < odw_t.mod[odw_mod].count) odw_cur++;
        }
        else if (ch == 'k')
        {
          if (odw_cur > 0) odw_cur--;
        }
        else if (ch == ' ')
          odw_pick[odw_mod] ^= 1 << odw_cur;
        else if (ch == 'l' || ch == 'h')
        {
          if (odw_nmine)
          {
            if (ch == 'l') odw_minei++;
            else           odw_minei--;
            if (odw_minei >= odw_nmine) odw_minei = 0;
            if (odw_minei < 0) odw_minei = odw_nmine - 1;
            odw_sel = odw_mine[odw_minei];
            ODWPaint;
          }
        }
        else if (ch == '\n')
        {
          ODWOrderLine(line);
          if (odw_t.waiting && ODCheck(&odw_t, line))
          {
            ODWriteOrders(&odw_t, line, orders);
            goto odw_done;
          }
        }
      }
      Sleep(10);
    }
odw_done:
  }
  catch
    PutExcept;

  if (odw_dc) DCDel(odw_dc);
  odw_dc = NULL;
  if (odw_pmap) Free(odw_pmap);
  if (odw_adj) Free(odw_adj);
  odw_pmap = NULL;
  odw_adj = NULL;
  Fs->draw_it = NULL;
  ms.scale.x = odw_sx;
  ms.scale.y = odw_sy;
  SettingsPop;
}
