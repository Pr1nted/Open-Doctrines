/* Open Doctrines on TempleOS. All of it, on the machine.
 *
 * The world is baked into world.odw; the rules are in Rules.HC; the screen is
 * a 1024x768 true-colour framebuffer this program takes from the card. No
 * host, no bridge, no file waiting to be fetched -- once this is running the
 * only thing outside is the disk it was read from.
 *
 *     #include "ODGame"
 *     ODGame("SWE");
 *
 * The file is called ODGame.HC and not OpenDocGame.HC because a name longer
 * than eight characters needs a long-filename entry on FAT, and an #include
 * of one here does nothing at all: no error, no output, no compile. Short
 * names, on this machine, are not a style preference.
 */
#include "Paint"
#include "Rules"

#define G_MAPY    128           /* the map is 2:1; the screen is 4:3 */
#define G_BARY    720

/* The real game's palette, read off it: a near-black ground rather than blue,
 * panels that are dark glass over the map, and one accent each for money,
 * gain and warning. Country colours come from the world file and are the same
 * hexes the desktop game uses. */
#define G_BG_R      8
#define G_BG_G     10
#define G_BG_B     16

I64 g_pr[OD_MAXP], g_pg[OD_MAXP], g_pb[OD_MAXP];    /* province fill  */
I64 g_br[OD_MAXP], g_bg[OD_MAXP], g_bb[OD_MAXP];    /* province edge  */
I64 g_sel = -1, g_mine[OD_MAXP], g_nmine = 0, g_minei = -1;
U8  g_status[96];

/* Colour every province from whoever owns it now. Recomputed whenever a
 * province changes hands, which is the only time it can change. */
U0 GShade()
{
  I64 i, s, r, g, b;
  for (i = 0; i < w_np; i++)
  {
    s = WSlot(w_own[i]);
    if (s < 0)
    {
      r = 70; g = 70; b = 76;          /* owned by nobody this file knows */
    }
    else
    {
      r = w_cr[s]; g = w_cg[s]; b = w_cb[s];
    }
    /* Darkened towards the real game's look: the hexes in the map file are
     * the desktop game's own, but it renders them over a black ground with
     * the land pulled down, and at full strength they read as boiled sweets
     * rather than as a political map. */
    g_pr[i] = r * 78 / 100;
    g_pg[i] = g * 78 / 100;
    g_pb[i] = b * 78 / 100;
    /* The border is the country's own colour darkened, not a black line. On
     * a dark map a black border eats the small provinces entirely. */
    g_br[i] = r * 38 / 100;
    g_bg[i] = g * 38 / 100;
    g_bb[i] = b * 38 / 100;
  }
}

U0 GDrawMap()
{
  I64 x, y, k, ix, o, r, g, b;

  PFill(0, 0, P_W, P_H, G_BG_R, G_BG_G, G_BG_B);

  for (y = 0; y < w_h; y++)
    for (x = 0; x < w_w; x++)
    {
      k = y * w_w + x;
      ix = w_pmap[k];
      if (ix != OD_SEA)          /* no `continue` in HolyC; the body is the test */
      {
        r = g_pr[ix]; g = g_pg[ix]; b = g_pb[ix];
        /* A border is where the province index changes; drawn on one side of
         * each edge so the line stays a pixel wide. */
        if ((x + 1 < w_w && w_pmap[k + 1] != ix) ||
            (y + 1 < w_h && w_pmap[k + w_w] != ix))
        {
          r = g_br[ix]; g = g_bg[ix]; b = g_bb[ix];
        }
        if (ix == g_sel)
        {
          r = 255; g = 255; b = 255;
        }
        o = ((y + G_MAPY) * P_W + x) * P_BPP;
        p_fb[o] = b;
        p_fb[o + 1] = g;
        p_fb[o + 2] = r;
      }
    }
}

U0 GPanel()
{
  I64 s, prov, men, inc, y, ix;

  s = WSlot(w_me);
  RCountryStats(w_me, &prov, &men, &inc);

  /* ── THE LEFT BLOCK ──
   * Dark glass over the map, the way the real one sits over the Atlantic. */
  PGlass(0, 0, 330, 300, 6, 8, 12, 205);

  PText(14, 12, "PLAYING AS", 130, 135, 145);
  PPrint(14, 26, 240, 240, 245, 2, "%s", w_cname[s]);
  /* ── THE CURRENCY SIGN IS 0x24, WRITTEN AS A NUMBER ──
   *
   * The character itself opens a DolDoc command wherever it appears, and
   * "wherever" includes places that are not code. In a string it is a parse
   * error you can see. In a character literal the compiler stops reading the
   * file at that point, prints nothing, and reports success -- every function
   * after it silently does not exist, which is how this display came to be
   * missing from a build that looked like it had worked. In a COMMENT it does
   * the same thing, which is why this paragraph spells it out instead of
   * showing it. */
  PPrint(14, 48, 120, 215, 130, 1, "%c%d.%d", 0x24, w_tre[s] / 10,
         w_tre[s] % 10);

  PPrint(14, 74, 235, 235, 240, 1, "%d provinces", prov);
  PText(14, 92, "Country Statistics", 235, 235, 240);
  PFill(14, 103, 180, 1, 70, 74, 84);

  y = 112;
  PPrint(14, y, 230, 200, 90, 1, "Income: %d.%d/turn", inc / 10, inc % 10);
  y += 14;
  PPrint(14, y, 200, 205, 215, 1, "Standing army: %d", men);
  y += 14;
  PPrint(14, y, 200, 205, 215, 1, "Turn %d", w_turn);
  y += 20;

  /* ── THE SELECTED PROVINCE ── */
  if (g_sel >= 0)
  {
    ix = g_sel;
    PPrint(14, y, 235, 235, 240, 1, "%s", w_name[ix]);
    y += 14;
    PPrint(14, y, 150, 160, 175, 1, "%s", WCName(w_own[ix]));
    y += 14;
    PPrint(14, y, 200, 205, 215, 1, "Population: %d", w_pop[ix]);
    y += 12;
    PPrint(14, y, 200, 205, 215, 1, "Garrison:   %d", w_army[ix]);
    y += 12;
    PPrint(14, y, 230, 200, 90, 1, "Income:     %d.%d", w_inc[ix] / 10,
           w_inc[ix] % 10);
    y += 12;
    PPrint(14, y, 200, 205, 215, 1, "Fortification: %d", w_fort[ix]);
    y += 12;
    PPrint(14, y, 150, 160, 175, 1, "Frontage:   %d", RWidth(ix));
    y += 12;
    if (r_ord_recruit[ix] > 0)
    {
      PPrint(14, y, 120, 215, 130, 1, "raising %d men", r_ord_recruit[ix]);
      y += 12;
    }
    if (r_ord_attack[ix] >= 0)
    {
      PPrint(14, y, 235, 120, 110, 1, "assaulting %s",
             w_name[r_ord_attack[ix]]);
      y += 12;
    }
  }
}

U0 GBar()
{
  I64 i;
  U8 *tabs[6];

  PGlass(0, G_BARY, P_W, P_H - G_BARY, 34, 36, 40, 225);
  PFill(0, G_BARY, P_W, 1, 70, 74, 84);

  /* The green button, bottom left, which is where the real one is. */
  PFill(16, G_BARY + 12, 170, 26, 38, 104, 58);
  PRect(16, G_BARY + 12, 170, 26, 70, 150, 90);
  PText(44, G_BARY + 21, "Process Turn", 240, 245, 240);

  tabs[0] = "h/l province";
  tabs[1] = "r raise";
  tabs[2] = "a assault";
  tabs[3] = "ENTER turn";
  tabs[4] = "ESC quit";
  tabs[5] = NULL;
  for (i = 0; i < 5; i++)
    PText(230 + i * 130, G_BARY + 21, tabs[i], 185, 190, 200);

  PPrint(P_W - 150, G_BARY + 21, 200, 205, 215, 1, "Turn %d", w_turn);

  if (*g_status)
    PText(16, G_BARY - 18, g_status, 230, 200, 90);
}

U0 GDraw()
{
  GDrawMap;
  GPanel;
  GBar;
}

U0 GOwnList()
{
  I64 i;
  g_nmine = 0;
  for (i = 0; i < w_np; i++)
    if (w_own[i] == w_me)
    {
      g_mine[g_nmine] = i;
      g_nmine++;
    }
}

/* The weakest enemy province next door, which is what an assault from here
 * would sensibly be aimed at. */
I64 GWeakestNeighbour(I64 ix)
{
  I64 k, nb, best = -1, bestmen = 0;
  for (k = 0; k < w_adjn[ix]; k++)
  {
    nb = WNeighbour(ix, k);
    if (nb >= 0 && w_own[nb] != w_me)
      if (best < 0 || w_army[nb] < bestmen)
      {
        best = nb;
        bestmen = w_army[nb];
      }
  }
  return best;
}

/* Named ODGame, not OpenDoc: a file called OpenDoc.HC sits in the same
 * directory defining a function of that name, and the two resolving against
 * each other produced a call that did nothing at all -- no output, no error,
 * no elapsed time. */
U0 ODGame(U8 *iso = "SWE", U8 *world = "world.odw")
{
  I64 ch, i, s, men, cost, tgt;

  if (!WLoad(world))
  {
    "$$FG,RED$$cannot load %s$$FG$$\n", world;
    return;
  }
  w_me = -1;
  for (i = 0; i < w_nc; i++)
    if (!StrCmp(w_ciso[i], iso)) w_me = w_ccid[i];
  if (w_me < 0)
  {
    "$$FG,RED$$no country %s in this world$$FG$$\n", iso;
    return;
  }
  RInit;
  GOwnList;
  GShade;
  StrCpy(g_status, "h/l to walk your provinces, r to raise men, ENTER to end the turn");

  if (!PaintBegin)
  {
    "$$FG,RED$$no linear framebuffer here -- this needs the DISPI registers"
    " (QEMU, Bochs, VirtualBox)$$FG$$\n";
    return;
  }

  try
  {
    GDraw;
    /* Drawn once, then only on input: a quarter of a million pixels a frame
     * under emulation is not a frame rate, and nothing here animates. */
    while (TRUE)
    {
      ch = ScanChar;
      if (ch)
      {
        if (ch == CH_ESC || ch == CH_SHIFT_ESC)
          goto g_done;
        else if (ch == 'l' || ch == 'h')
        {
          if (g_nmine)
          {
            if (ch == 'l') g_minei++;
            else           g_minei--;
            if (g_minei >= g_nmine) g_minei = 0;
            if (g_minei < 0) g_minei = g_nmine - 1;
            g_sel = g_mine[g_minei];
          }
        }
        else if (ch == 'r' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          men = w_pop[g_sel] / 10;
          cost = men / 1000;
          s = WSlot(w_me);
          if (s >= 0 && w_tre[s] >= cost && cost > 0)
          {
            r_ord_recruit[g_sel] = men;
            StrPrint(g_status, "%d men ordered in %s for %c%d.%d",
                     men, w_name[g_sel], 0x24, cost / 10, cost % 10);
          }
          else
            StrPrint(g_status, "not enough money to raise men in %s",
                     w_name[g_sel]);
        }
        else if (ch == 'a' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          tgt = GWeakestNeighbour(g_sel);
          if (tgt >= 0)
          {
            r_ord_attack[g_sel] = tgt;
            StrPrint(g_status, "%s will assault %s (%d defenders)",
                     w_name[g_sel], w_name[tgt], w_army[tgt]);
          }
          else
            StrPrint(g_status, "%s has no foreign neighbour", w_name[g_sel]);
        }
        else if (ch == '\n')
        {
          RTurn;
          GOwnList;
          GShade;
          g_sel = -1;
          g_minei = -1;
          if (r_nlog)
            StrCpy(g_status, r_log[0]);
          else
            StrPrint(g_status, "turn %d", w_turn);
        }
        GDraw;
      }
      Sleep(10);
    }
g_done:
  }
  catch
    PutExcept;

  PaintEnd;
  if (w_pmap) Free(w_pmap);
  if (w_adj) Free(w_adj);
  w_pmap = NULL;
  w_adj = NULL;
}

/* Runs at include time: if this does not appear, the file was never
 * compiled, whatever the silence suggests. */
