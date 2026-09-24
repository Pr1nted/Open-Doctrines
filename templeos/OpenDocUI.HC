/* Open Doctrines, drawn.
 *
 * The text client (OpenDoc.HC) proved the protocol: a turn comes in as a file,
 * an order line goes back. This is the same turn with the map under it -- 640
 * by 480, sixteen colours, a mouse, which is what this machine has.
 *
 * ── WHERE THE MAP COMES FROM ──
 *
 * Not from the .odmap. This OS has no PNG decoder and no interest in 460 KB of
 * province raster. tools/templeos_map.py resolves every pixel to its owner,
 * snaps that to one of the sixteen colours, inks the borders and run-length
 * encodes the result: 32 KB, decoded here with one store per pixel into an
 * off-screen DC, then blitted whole on every frame.
 *
 * Province centres ride along in the same file, so a click can be answered
 * without carrying a per-pixel index there is no room for.
 *
 * ── WHAT IT SHARES WITH THE TEXT CLIENT ──
 *
 * Everything above the drawing. OpenDoc.HC is included, not copied, so there
 * is one parser for the door's output and one place where the order line is
 * written -- and tests/templeos_client_test.sh guards that one.
 */
#include "OpenDoc"

#define ODU_MAXP   2048
#define ODU_FONTH  8
#define ODU_FONTW  8
#define ODU_PICKR  400      /* 20 pixels, squared: further than that is sea */

#define ODU_MAXC   256

CDC *odu_map = NULL;
I64  odu_mw = 0, odu_mh = 0, odu_np = 0, odu_nc = 0;
I64  odu_pid[ODU_MAXP], odu_px[ODU_MAXP], odu_py[ODU_MAXP], odu_cid[ODU_MAXP];
I64  odu_ccid[ODU_MAXC];        /* country ids, and their ISO codes */
U8   odu_ciso[ODU_MAXC][4];
I64  odu_me = -1;               /* the player's country id, joined by ISO */
I64  odu_mine[ODU_MAXP], odu_nmine = 0;   /* just this country's, for drawing */
I64  odu_sel = -1;          /* index into the centre arrays, not a province id */
I64  odu_mod = 0;           /* the module whose menu is on screen */
I64  odu_cur = 0;           /* highlighted item within it */
I64  odu_pick[4];           /* chosen items, one bit per item */
ODTurn odu_t;
U8   odu_msg[96];

/* Little-endian words, assembled a byte at a time.
 *
 * A cast through a U16* would be shorter and would also be a bet on this
 * compiler's alignment rules and on the host's byte order. The file is written
 * little-endian by a Python struct.pack on the other side; saying so explicitly
 * costs four lines and cannot be wrong. */
I64 ODU16(U8 *b) { return b[0] + (b[1] << 8); }
I64 ODU32(U8 *b) { return b[0] + (b[1] << 8) + (b[2] << 16) + (b[3] << 24); }

U0 ODULoadMap(U8 *path)
{
  U8 *b;
  I64 size = 0, i, p, total, n, c, off, y, x, rle_len;

  b = ODReadEither(path, &size);
  if (!b)
  {
    StrPrint(odu_msg, "cannot read %s", path);
    return;
  }
  if (b[0] != 'O' || b[1] != 'D' || b[2] != 'T' || b[3] != 'M')
  {
    StrCpy(odu_msg, "that is not a map file");
    Free(b);
    return;
  }
  odu_mw  = ODU16(b + 6);
  odu_mh  = ODU16(b + 8);
  odu_np  = ODU16(b + 10);
  odu_nc  = ODU16(b + 12);
  rle_len = ODU32(b + 14);
  if (odu_np > ODU_MAXP) odu_np = ODU_MAXP;
  if (odu_nc > ODU_MAXC) odu_nc = ODU_MAXC;

  /* One byte per pixel, straight into the bitmap. width_internal is the width
   * rounded up to a multiple of eight, which is what the rows are actually
   * spaced by -- using width here would shear the map on any odd size. */
  odu_map = DCNew(odu_mw, odu_mh);
  total = odu_mw * odu_mh;
  i = 18;
  p = 0;
  while (p < total && i + 1 < size)
  {
    n = b[i];
    c = b[i + 1];
    i += 2;
    while (n > 0 && p < total)
    {
      y = p / odu_mw;
      x = p - y * odu_mw;
      odu_map->body[y * odu_map->width_internal + x] = c;
      p++;
      n--;
    }
  }

  off = 18 + rle_len;
  for (p = 0; p < odu_np; p++)
  {
    odu_pid[p] = ODU32(b + off);
    odu_px[p]  = ODU16(b + off + 4);
    odu_py[p]  = ODU16(b + off + 6);
    odu_cid[p] = ODU16(b + off + 8);
    off += 10;
  }
  /* The ISO table, and the join that makes "yours" mean anything: the door
   * says SWE, the map counts in country ids, and neither knows the other. */
  for (p = 0; p < odu_nc; p++)
  {
    odu_ccid[p] = ODU16(b + off);
    odu_ciso[p][0] = b[off + 2];
    odu_ciso[p][1] = b[off + 3];
    odu_ciso[p][2] = b[off + 4];
    odu_ciso[p][3] = 0;
    off += 6;
  }
  odu_me = -1;
  for (p = 0; p < odu_nc; p++)
    if (!StrCmp(odu_ciso[p], odu_t.iso)) odu_me = odu_ccid[p];

  /* Own provinces collected once. Filtering 1,480 of them inside the draw
   * callback costs that scan sixty times a second to find the same twelve. */
  odu_nmine = 0;
  for (p = 0; p < odu_np; p++)
    if (odu_cid[p] == odu_me)
    {
      odu_mine[odu_nmine] = p;
      odu_nmine++;
    }

  StrPrint(odu_msg, "%d provinces, you are %s", odu_np, odu_t.iso);
  Free(b);
}

/* The ISO code of a country id, for naming whoever owns what you clicked. */
U8 *ODUIso(I64 cid)
{
  I64 i;
  for (i = 0; i < odu_nc; i++)
    if (odu_ccid[i] == cid) return odu_ciso[i];
  return "--";
}

/* The province nearest a point, or -1 for "that was the sea".
 *
 * Nearest-centre rather than a lookup, because the per-pixel province index is
 * 600 KB and this is 15 KB. It disagrees with the raster only near a border
 * between two oddly-shaped provinces, which is a worse place to click anyway. */
I64 ODUPick(I64 mx, I64 my)
{
  I64 i, best = -1, bd = 0, d, dx, dy;
  for (i = 0; i < odu_np; i++)
  {
    dx = odu_px[i] - mx;
    dy = odu_py[i] - my;
    d = dx * dx + dy * dy;
    if (best < 0 || d < bd)
    {
      best = i;
      bd = d;
    }
  }
  if (best >= 0 && bd > ODU_PICKR) best = -1;
  return best;
}

/* The order line, built from what is ticked. Same grammar the door reads. */
U0 ODUOrderLine(U8 *out)
{
  I64 m, i;
  Bool first = TRUE;
  *out = 0;
  for (m = 0; m < 4; m++)
    for (i = 0; i < odu_t.mod[m].count; i++)
      if (odu_pick[m] & (1 << i))
      {
        if (!first) StrPrint(out + StrLen(out), ", ");
        StrPrint(out + StrLen(out), "%c:%d",
                 odu_t.mod[m].letter, odu_t.mod[m].act[i].num);
        first = FALSE;
      }
}

U0 DrawIt(CTask *task, CDC *dc)
{
  /* All of them here, and no `?:` below: HolyC has neither block scope nor a
   * ternary operator, and both absences are compile errors rather than
   * warnings. */
  I64 i, y, x, n, row, col;
  U8 line[160], mark, *mine;
  I64 mx, my;

  no_warn task;

  if (odu_map) GrBlot(dc, 0, 0, odu_map);

  /* Every province this country owns, dotted; the selected one ringed. Owned
   * provinces only -- 1,480 rings would be a grey haze, and the ones that
   * matter are the ones that can be ordered about. */
  dc->color = WHITE;
  for (i = 0; i < odu_nmine; i++)
    GrPlot(dc, odu_px[odu_mine[i]], odu_py[odu_mine[i]]);
  if (odu_sel >= 0)
  {
    dc->color = WHITE;
    GrCircle(dc, odu_px[odu_sel], odu_py[odu_sel], 5);
    GrCircle(dc, odu_px[odu_sel], odu_py[odu_sel], 6);
  }

  /* ── the panel ── */
  y = odu_mh;
  dc->color = BLACK;
  GrRect(dc, 0, y, odu_mw, 480 - y);
  dc->color = WHITE;

  row = y + 2;
  GrPrint(dc, 2, row, "%s (%s)   Turn %d/%d   Land %d/%d (%s%%)   Army %d",
          odu_t.country, odu_t.iso, odu_t.turn, odu_t.turns,
          odu_t.mine, odu_t.owned, odu_t.share, odu_t.army);
  row += ODU_FONTH;
  GrPrint(dc, 2, row, "Treasury %s   Net %s (gross %s)   At war with: %s",
          odu_t.treasury, odu_t.net, odu_t.gross, odu_t.war);
  row += ODU_FONTH;

  dc->color = LTCYAN;
  if (odu_sel >= 0)
  {
    mine = "";
    if (odu_cid[odu_sel] == odu_me) mine = "   (yours)";
    GrPrint(dc, 2, row, "province %d   %s%s", odu_pid[odu_sel],
            ODUIso(odu_cid[odu_sel]), mine);
  }
  else
  {
    mx = ms.pos.x - Fs->pix_left - Fs->scroll_x;
    my = ms.pos.y - Fs->pix_top - Fs->scroll_y;
    GrPrint(dc, 2, row, "%s          pointer %d,%d", odu_msg, mx, my);
  }
  row += ODU_FONTH + 2;

  /* The active module's menu, three across. Only one module is shown at a
   * time: four of them at once is the text client's job, and there is not the
   * room here to do it without making every line unreadable. */
  dc->color = YELLOW;
  GrPrint(dc, 2, row, "%s  budget %d     e/p/w/n module  j/k move  "
                      "SPACE pick  ENTER send  ESC quit",
          odu_t.mod[odu_mod].label, odu_t.mod[odu_mod].budget);
  row += ODU_FONTH;

  n = odu_t.mod[odu_mod].count;
  for (i = 0; i < n; i++)
  {
    x = 4 + (i / 4) * 200;
    y = row + (i % 4) * ODU_FONTH;
    if (i == odu_cur)
    {
      dc->color = BLUE;
      GrRect(dc, x - 2, y - 1, 196, ODU_FONTH + 1);
    }
    col = LTGRAY;
    mark = ' ';
    if (odu_pick[odu_mod] & (1 << i))
    {
      col = LTGREEN;
      mark = '*';
    }
    dc->color = col;
    GrPrint(dc, x, y, "%c %c:%d %s", mark,
            odu_t.mod[odu_mod].act[i].letter,
            odu_t.mod[odu_mod].act[i].num,
            odu_t.mod[odu_mod].act[i].name);
  }

  ODUOrderLine(line);
  dc->color = LTGREEN;
  GrPrint(dc, 2, 480 - ODU_FONTH - 2, "orders: %s", line);
}

U0 OpenDocUI(U8 *turn = "turn.txt", U8 *map = "world.odt",
             U8 *orders = "orders.txt")
{
  I64 ch, i, mx, my;
  U8 *buf;
  I64 size = 0;
  U8 line[160];

  MemSet(odu_pick, 0, sizeof(odu_pick));
  StrCpy(odu_msg, "click a province");

  buf = ODReadEither(turn, &size);
  if (!buf)
  {
    "$$FG,RED$$Cannot read %s$$FG$$\n", turn;
    return;
  }
  ODParse(&odu_t, buf);
  Free(buf);

  SettingsPush;
  AutoComplete;        /* off: its window sits over the right of the screen */
  WinBorder;           /* off: the border costs two columns and a row */
  WinMax;              /* the desktop gives a task half the screen otherwise */
  DocCursor;
  DocClear;

  ODULoadMap(map);
  Fs->draw_it = &DrawIt;

  try
  {
    while (TRUE)
    {
      /* The mouse is polled rather than waited on, because GetKey blocks and a
       * map you can only click between keystrokes is not a map you can click. */
      if (ms.lb)
      {
        mx = ms.pos.x - Fs->pix_left - Fs->scroll_x;
        my = ms.pos.y - Fs->pix_top - Fs->scroll_y;
        if (my < odu_mh)
        {
          i = ODUPick(mx, my);
          if (i >= 0) odu_sel = i;
        }
        while (ms.lb) Yield;          /* one selection per press */
      }

      ch = ScanChar;
      if (ch)
      {
        if (ch == CH_ESC || ch == CH_SHIFT_ESC)
          goto odu_done;
        else if (ch == 'e') { odu_mod = 0; odu_cur = 0; }
        else if (ch == 'p') { odu_mod = 1; odu_cur = 0; }
        else if (ch == 'w') { odu_mod = 2; odu_cur = 0; }
        else if (ch == 'n') { odu_mod = 3; odu_cur = 0; }
        else if (ch == 'j')
        {
          if (odu_cur + 1 < odu_t.mod[odu_mod].count) odu_cur++;
        }
        else if (ch == 'k')
        {
          if (odu_cur > 0) odu_cur--;
        }
        else if (ch == ' ')
          odu_pick[odu_mod] ^= 1 << odu_cur;
        else if (ch == '\n')
        {
          ODUOrderLine(line);
          if (odu_t.waiting && ODCheck(&odu_t, line))
          {
            ODWriteOrders(&odu_t, line, orders);
            goto odu_done;
          }
        }
      }
      Sleep(10);
    }
odu_done:
  }
  catch
    PutExcept;

  if (odu_map) DCDel(odu_map);
  odu_map = NULL;
  Fs->draw_it = NULL;
  SettingsPop;
}
