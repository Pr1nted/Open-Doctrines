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
#include "Data"
#include "Sync.HH"

#define G_MAPY    128           /* the map is 2:1; the screen is 4:3 */
#define G_BARY    720

/* The real game's palette, read off it: a near-black ground rather than blue,
 * panels that are dark glass over the map, and one accent each for money,
 * gain and warning. Country colours come from the world file and are the same
 * hexes the desktop game uses. */
#define G_BG_R      8
#define G_BG_G     10
#define G_BG_B     16

/* ── THE MAP VIEWS ──
 *
 * The desktop game owns this list; tools/templeos_sync.py reads it out of
 * Game_Render.cpp and fails the build if this table has fallen behind. The
 * ids, not the labels: the words are translated, the slot is not.
 */
#define G_NVIEW  7
#define G_NTAB   8

U8 *g_view_ids[G_NVIEW];
U8 *g_view_tabs[G_NTAB];
U8 *g_side[4];

/* ── g_view IS THE C++ INDEX, NOT AN INDEX INTO THE LIST ABOVE ──
 *
 * The SY_VIEW_* constants come from the desktop game's own ordering, which
 * has eight slots. Numbering the implemented views 0..6 separately and then
 * comparing them against those constants is off by one from Navy onwards:
 * pressing Country Names drew Resources, because this build's seventh view
 * and the C++'s sixth share a number and nothing else. One numbering, and it
 * belongs to the C++.
 */
I64 g_view_live[G_NTAB];          /* which of the eight this build can draw */
I64 g_view = -1;                  /* a SY_VIEW_* value, or -1 for political */

/* Declared before GInitTables fills it, because HolyC compiles top to bottom
 * and a global used above its declaration is simply an undefined identifier. */

/* Filled one line at a time rather than with a brace initialiser: HolyC takes
 * those for scalars and not for arrays of pointers ("Missing ';'"). The shape
 * is also what tools/templeos_sync.py reads, so this list and the C++ one
 * cannot drift apart without the build saying so. */
U0 GInitTables()
{
  g_view_ids[0] = "view.population";
  g_view_ids[1] = "view.industry";
  g_view_ids[2] = "view.defence";
  g_view_ids[3] = "view.relations";
  g_view_ids[4] = "view.army";
  g_view_ids[5] = "view.resources";
  g_view_ids[6] = "view.names";

  g_view_tabs[0] = "Population";
  g_view_tabs[1] = "Industry";
  g_view_tabs[2] = "Defence";
  g_view_tabs[3] = "Relations";
  g_view_tabs[4] = "Army Navigation";
  g_view_tabs[5] = "Navy";
  g_view_tabs[6] = "Resources";
  g_view_tabs[7] = "Country Names";

  g_view_live[SY_VIEW_POPULATION] = 1;
  g_view_live[SY_VIEW_INDUSTRY]   = 1;
  g_view_live[SY_VIEW_DEFENCE]    = 1;
  g_view_live[SY_VIEW_RELATIONS]  = 1;
  g_view_live[SY_VIEW_ARMY]       = 1;
  g_view_live[SY_VIEW_NAVY]       = 0;   /* no fleets in these rules yet */
  g_view_live[SY_VIEW_RESOURCES]  = 1;
  g_view_live[SY_VIEW_NAMES]      = 1;

  g_side[0] = "Politics";
  g_side[1] = "Economy";
  g_side[2] = "Claims";
  g_side[3] = "Research";
}


I64 g_pr[OD_MAXP], g_pg[OD_MAXP], g_pb[OD_MAXP];    /* province fill  */
I64 g_br[OD_MAXP], g_bg[OD_MAXP], g_bb[OD_MAXP];    /* province edge  */
I64 g_sel = -1, g_mine[OD_MAXP], g_nmine = 0, g_minei = -1;
U8  g_status[96];

/* A cold-to-hot ramp for the numeric views: dark blue through green to
 * amber. Anything above `hot` saturates rather than wrapping round, because a
 * scale that wraps makes the biggest province look like the smallest. */
U0 GHeat(I64 v, I64 hot, I64 *r, I64 *g, I64 *b)
{
  I64 t;
  if (hot < 1) hot = 1;
  t = v * 255 / hot;
  if (t > 255) t = 255;
  if (t < 0) t = 0;
  if (t < 128)
  {
    *r = 20 + t / 3;
    *g = 40 + t;
    *b = 90 + t / 2;
  }
  else
  {
    *r = 60 + (t - 128) * 3 / 2;
    *g = 168 - (t - 128) / 3;
    *b = 154 - (t - 128);
  }
}

/* Colour every province for the current view. Recomputed when a province
 * changes hands or the view changes -- never per frame, because it walks
 * every province and the screen is only redrawn on input anyway. */
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

    if (g_view == SY_VIEW_POPULATION)
      GHeat(w_pop[i] / 1000, 3000, &r, &g, &b);
    else if (g_view == SY_VIEW_INDUSTRY)
      GHeat(w_ind[i], 10, &r, &g, &b);
    else if (g_view == SY_VIEW_DEFENCE)
      GHeat(w_fort[i], 5, &r, &g, &b);
    else if (g_view == SY_VIEW_ARMY)
      GHeat(w_army[i] / 1000, 300, &r, &g, &b);
    else if (g_view == SY_VIEW_RESOURCES)
    {
      /* One colour per deposit, in the order the desktop game lists them;
       * a province with several shows the first, which is what a single
       * flat layer can honestly say. */
      if (w_res[i] & 1)       { r = 40;  g = 40;  b = 40;  }   /* oil       */
      else if (w_res[i] & 2)  { r = 200; g = 170; b = 60;  }   /* gold      */
      else if (w_res[i] & 4)  { r = 130; g = 140; b = 150; }   /* metal     */
      else if (w_res[i] & 8)  { r = 90;  g = 150; b = 90;  }   /* rubber    */
      else if (w_res[i] & 16) { r = 170; g = 110; b = 200; }   /* gemstones */
      else                    { r = 30;  g = 34;  b = 40;  }
    }
    else if (g_view == SY_VIEW_RELATIONS)
    {
      if (w_own[i] == w_me)            { r = 230; g = 230; b = 240; }
      else if (RAtWar(w_me, w_own[i])) { r = 170; g = 50;  b = 50;  }
      else                             { r = 55;  g = 60;  b = 70;  }
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

/* ── COUNTRY NAMES ──
 *
 * One label per country, at the centre of its largest province, drawn only in
 * the view that asks for it. Largest rather than the average of all of them,
 * because the average of an empire with islands lands in the sea. */
U0 GNames()
{
  I64 i, s, best[OD_MAXC], bestarea[OD_MAXC], tw, x, y;

  if (g_view != SY_VIEW_NAMES) return;

  for (i = 0; i < w_nc; i++)
  {
    best[i] = -1;
    bestarea[i] = 0;
  }
  for (i = 0; i < w_np; i++)
  {
    s = WSlot(w_own[i]);
    if (s >= 0 && w_area[i] > bestarea[s])
    {
      bestarea[s] = w_area[i];
      best[s] = i;
    }
  }
  for (i = 0; i < w_nc; i++)
    if (best[i] >= 0)
    {
      tw = PTextW(w_ciso[i]);
      x = w_cx[best[i]] - tw / 2;
      y = w_cy[best[i]] + G_MAPY - 5;
      /* The code, not the name: at this scale a full country name covers its
       * neighbours, and the desktop game's own label layer thins out too. */
      PText(x, y, w_ciso[i], 235, 235, 245);
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

/* ── THE SIDE BUTTONS ──
 *
 * Politics, Economy, Claims and Research, stacked down the right, where the
 * desktop game puts them. Two of them open something; the other two are drawn
 * dimmed and say so when pressed, because a button that looks live and does
 * nothing is worse than one that admits it. */
I64 g_side_live[4] = {1, 1, 1, 1};   /* all four carry real tables now */
I64 g_overlay = -1;              /* which side panel is open, or -1 */
I64 g_ocur[4];                   /* the highlighted row in each panel */
I64 g_otop[4];                   /* the first row shown, for scrolling */
#define G_ROWS  26               /* rows a panel shows at once */

#define G_SIDEX   (P_W - 104)
#define G_SIDEY   200
#define G_SIDEW   88
#define G_SIDEH   64

U0 GSide()
{
  I64 i, y, tw;
  for (i = 0; i < 4; i++)
  {
    y = G_SIDEY + i * (G_SIDEH + 10);
    PGlass(G_SIDEX, y, G_SIDEW, G_SIDEH, 18, 20, 26, 215);
    if (i == g_overlay)
      PRect(G_SIDEX, y, G_SIDEW, G_SIDEH, 200, 170, 80);
    else
      PRect(G_SIDEX, y, G_SIDEW, G_SIDEH, 60, 64, 74);
    tw = PTextW(g_side[i]);
    if (g_side_live[i])
      PText(G_SIDEX + (G_SIDEW - tw) / 2, y + G_SIDEH - 18, g_side[i],
            215, 220, 230);
    else
      PText(G_SIDEX + (G_SIDEW - tw) / 2, y + G_SIDEH - 18, g_side[i],
            95, 100, 110);
  }
}

U0 GBar()
{
  I64 i, x, w, tw, lit;

  PGlass(0, G_BARY, P_W, P_H - G_BARY, 34, 36, 40, 225);
  PFill(0, G_BARY, P_W, 1, 70, 74, 84);

  /* The green button, bottom left, where the real one is. */
  PFill(16, G_BARY + 14, 150, 24, 38, 104, 58);
  PRect(16, G_BARY + 14, 150, 24, 70, 150, 90);
  PText(36, G_BARY + 21, "Process Turn", 240, 245, 240);

  /* ── THE GAME'S OWN EIGHT TABS ──
   *
   * All eight are drawn, in the desktop game's order, whether or not this
   * build can render them: a bar that silently omits Navy would be a quiet
   * claim that the game has seven views. The one with nothing behind it is
   * dimmed. */
  w = (P_W - 200 - 120) / G_NTAB;
  for (i = 0; i < G_NTAB; i++)
  {
    x = 200 + i * w;
    lit = g_view_live[i];
    tw = PTextW(g_view_tabs[i]);
    if (g_view == i)
    {
      PFill(x, G_BARY + 4, w - 4, 2, 235, 200, 90);
      PText(x + (w - tw) / 2, G_BARY + 22, g_view_tabs[i], 235, 200, 90);
    }
    else if (lit)
      PText(x + (w - tw) / 2, G_BARY + 22, g_view_tabs[i], 190, 196, 206);
    else
      PText(x + (w - tw) / 2, G_BARY + 22, g_view_tabs[i], 92, 96, 104);
  }

  PPrint(P_W - 96, G_BARY + 22, 200, 205, 215, 1, "Turn %d", w_turn);

  if (*g_status)
    PText(16, G_BARY - 18, g_status, 230, 200, 90);
}

/* ── THE OVERLAYS ──
 *
 * Economy and Claims, from the numbers actually in the world file. Politics
 * and Research need policy and technology tables this build does not carry,
 * and rather than draw a convincing empty frame they say so. */
/* One row of a panel, with the highlight the cursor sits on. */
U0 GRow(I64 y, Bool cur, U8 *text, I64 r, I64 g, I64 b)
{
  if (cur)
  {
    PFill(166, y - 2, P_W - 432, 13, 30, 38, 52);
    PText(170, y, ">", 235, 200, 90);
  }
  PText(182, y, text, r, g, b);
}

/* ── THE PANELS ──
 *
 * Politics, Economy, Claims and Research, from the tables the build actually
 * carries: 59 policies and 86 research nodes baked out of the desktop game's
 * own definitions, and the real claims table. Nothing here is invented, and
 * where a number is not available it is left out rather than guessed.
 */
U0 GOverlay()
{
  I64 i, s, prov, men, inc, y, n, upkeep, k, nb, cur, top, shown;
  U8 line[128], *word, *branch;

  if (g_overlay < 0) return;
  PGlass(150, 90, P_W - 400, 560, 8, 10, 14, 238);
  PRect(150, 90, P_W - 400, 560, 70, 74, 84);
  PText(170, 108, g_side[g_overlay], 240, 240, 245, 2);
  PFill(170, 134, P_W - 440, 1, 70, 74, 84);

  s = WSlot(w_me);
  RCountryStats(w_me, &prov, &men, &inc);
  y = 152;
  cur = g_ocur[g_overlay];
  top = g_otop[g_overlay];

  if (g_overlay == 0)            /* ── POLITICS ── */
  {
    PPrint(170, y, 150, 160, 175, 1,
           "%d policies.  ENTER enacts or repeals.  Upkeep comes out of income "
           "every turn.", d_npol);
    y += 20;
    PText(170, y, "Policy                    Category    Cost/turn  Turns",
          150, 160, 175);
    y += 14;
    shown = 0;
    for (i = top; i < d_npol && shown < G_ROWS; i++)
    {
      StrPrint(line, "%-24s  %-10s  %6d.%d  %5d",
               d_pname[i], d_pcat[i], d_pcost[i] / 10, d_pcost[i] % 10,
               d_pturns[i]);
      if (d_active[i])
        GRow(y, i == cur, line, 120, 215, 130);
      else
        GRow(y, i == cur, line, 205, 210, 220);
      y += 13;
      shown++;
    }
  }
  else if (g_overlay == 1)       /* ── ECONOMY ── */
  {
    PPrint(170, y, 235, 235, 240, 1, "Treasury        %c%d.%d",
           0x24, w_tre[s] / 10, w_tre[s] % 10);
    y += 16;
    PPrint(170, y, 230, 200, 90, 1, "Income          %d.%d per turn",
           inc / 10, inc % 10);
    y += 16;
    upkeep = 0;
    for (i = 0; i < d_npol; i++)
      if (d_active[i]) upkeep += d_pcost[i];
    PPrint(170, y, 205, 130, 120, 1, "Policy upkeep   %d.%d per turn",
           upkeep / 10, upkeep % 10);
    y += 16;
    PPrint(170, y, 200, 205, 215, 1, "Standing army   %d men", men);
    y += 16;
    PPrint(170, y, 200, 205, 215, 1, "Research        %d points, %d per turn",
           d_rp, DResearchRate(inc));
    y += 24;
    PText(170, y, "Province            People      Income   Ind  Fort",
          150, 160, 175);
    y += 14;
    n = 0;
    for (i = 0; i < w_np && n < 20; i++)
      if (w_own[i] == w_me)
      {
        PPrint(182, y, 205, 210, 220, 1, "%-18s %9d %8d.%d %4d %5d",
               w_name[i], w_pop[i], w_inc[i] / 10, w_inc[i] % 10,
               w_ind[i], w_fort[i]);
        y += 13;
        n++;
      }
  }
  else if (g_overlay == 2)       /* ── CLAIMS ── */
  {
    k = DClaimsOf(w_me);
    if (k < 0)
      PText(170, y, "This country claims nothing.", 150, 160, 175);
    else
    {
      PPrint(170, y, 150, 160, 175, 1,
             "%d provinces claimed.  A claim is land the desktop game's claims "
             "table says is yours.", d_cln[k]);
      y += 20;
      PText(170, y, "Province                Held by      Garrison   Adjacent",
            150, 160, 175);
      y += 14;
      for (i = 0; i < d_cln[k] && i < G_ROWS; i++)
      {
        nb = d_clprov[d_clfirst[k] + i];
        n = 0;
        for (s = 0; s < w_adjn[nb]; s++)
          if (WNeighbour(nb, s) >= 0 && w_own[WNeighbour(nb, s)] == w_me) n = 1;
        word = "no";
        if (n) word = "yes";
        StrPrint(line, "%-22s  %-11s  %8d   %s",
                 w_name[nb], WCName(w_own[nb]), w_army[nb], word);
        if (w_own[nb] == w_me)
          GRow(y, FALSE, line, 120, 215, 130);
        else
          GRow(y, FALSE, line, 225, 130, 120);
        y += 13;
      }
    }
  }
  else                           /* ── RESEARCH ── */
  {
    PPrint(170, y, 150, 160, 175, 1,
           "%d technologies.  %d points, %d per turn.  ENTER starts one.",
           d_nres, d_rp, DResearchRate(inc));
    y += 20;
    PText(170, y, "Technology                Branch        Cost   State",
          150, 160, 175);
    y += 14;
    shown = 0;
    for (i = top; i < d_nres && shown < G_ROWS; i++)
    {
      /* Spelled out rather than nested in a conditional expression, which
       * HolyC does not have at all. */
      branch = "-";
      if (d_rcat[i] < 6) branch = d_rcatname[d_rcat[i]];
      word = "locked";
      if (DResAvailable(i)) word = "available";
      if (i == d_researching) word = "researching";
      if (d_done[i]) word = "done";
      StrPrint(line, "%-24s  %-12s  %5d   %s",
               d_rname[i], branch, d_rcost[i], word);
      if (d_done[i])
        GRow(y, i == cur, line, 120, 215, 130);
      else if (DResAvailable(i))
        GRow(y, i == cur, line, 205, 210, 220);
      else
        GRow(y, i == cur, line, 110, 116, 128);
      y += 13;
      shown++;
    }
  }

  PText(170, 620, "j/k move   ENTER act   ESC close",
        130, 138, 150);
}

U0 GDraw()
{
  GDrawMap;
  GNames;
  GPanel;
  GSide;
  GBar;
  GOverlay;
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

/* ── THE MAIN MENU ──
 *
 * The desktop game's, as closely as this screen allows: the world in
 * silhouette behind, the title in yellow over it, and the items centred with
 * the selected one underlined. Four of the seven do nothing here and are
 * dimmed, for the same reason the Navy tab is -- a menu that hides what it
 * cannot do is lying about the game.
 */
U8 *g_menu[7];
I64 g_menu_live[7];

U0 GMenuInit()
{
  g_menu[0] = "Play Singleplayer";   g_menu_live[0] = 1;
  g_menu[1] = "Play Multiplayer";    g_menu_live[1] = 0;
  g_menu[2] = "Map Editor";          g_menu_live[2] = 0;
  g_menu[3] = "Mod Menu";            g_menu_live[3] = 0;
  g_menu[4] = "Community";           g_menu_live[4] = 0;
  g_menu[5] = "Credits";             g_menu_live[5] = 1;
  g_menu[6] = "Quit";                g_menu_live[6] = 1;
}

/* The world as land and sea only, which is what the menu behind the title is
 * in the real game. Drawn from the same raster the map uses. */
U0 GSilhouette(I64 top)
{
  I64 x, y, k, o;
  for (y = 0; y < w_h; y++)
    for (x = 0; x < w_w; x++)
    {
      k = y * w_w + x;
      o = ((y + top) * P_W + x) * P_BPP;
      if (w_pmap[k] == OD_SEA)
      {
        p_fb[o] = 40; p_fb[o + 1] = 26; p_fb[o + 2] = 16;
      }
      else
      {
        p_fb[o] = 78; p_fb[o + 1] = 74; p_fb[o + 2] = 70;
      }
    }
}

U0 GMenuDraw(I64 cur)
{
  I64 i, y, tw, r, g, b;

  PFill(0, 0, P_W, P_H, 6, 7, 11);
  GSilhouette(128);
  /* The band the items sit on, so the text is legible over Africa. */
  PGlass(0, 280, P_W, 300, 0, 0, 0, 205);

  tw = PTextW("OpenDoctrines", 4);
  PText((P_W - tw) / 2, 90, "OpenDoctrines", 235, 205, 60, 4);
  tw = PTextW("A Grand Strategy Game", 1);
  PText((P_W - tw) / 2, 140, "A Grand Strategy Game", 175, 180, 190);
  PFill((P_W - 300) / 2, 154, 300, 1, 150, 130, 60);

  y = 310;
  for (i = 0; i < 7; i++)
  {
    tw = PTextW(g_menu[i], 2);
    r = 205; g = 210; b = 220;
    if (!g_menu_live[i]) { r = 95; g = 100; b = 110; }
    if (i == cur)        { r = 235; g = 205; b = 60; }
    PText((P_W - tw) / 2, y, g_menu[i], r, g, b, 2);
    if (i == cur)
      PFill((P_W - tw) / 2, y + 21, tw, 1, 235, 205, 60);
    y += 36;
  }

  PText(14, P_H - 18, "j/k move   ENTER choose   ESC quit", 120, 128, 140);
  PText(P_W - 150, P_H - 18, "TempleOS build", 120, 128, 140);
}

/* ── CHOOSING A COUNTRY ──
 *
 * Every country with land, by province count, because that is the only
 * measure of a seat this build can show before the game starts. */
I64 GSelect()
{
  I64 ch, i, s, cur = 0, top = 0, y, n, rows = 24;
  I64 cnt[OD_MAXC];
  U8 line[96];
  Bool dirty = TRUE;

  for (i = 0; i < w_nc; i++) cnt[i] = 0;
  for (i = 0; i < w_np; i++)
  {
    s = WSlot(w_own[i]);
    if (s >= 0) cnt[s]++;
  }

  /* ── DRAWN ON INPUT, NOT ON A LOOP ──
   *
   * A full repaint is half a million pixels and the silhouette is most of
   * them. Redrawing every pass through the loop does not merely waste the
   * machine: the screen is visibly half-painted most of the time, because
   * nothing here double-buffers. Nothing on this screen moves by itself, so
   * it is drawn when something changes and left alone otherwise. */
  while (TRUE)
  {
    if (dirty)
    {
    dirty = FALSE;
    PFill(0, 0, P_W, P_H, 6, 7, 11);
    GSilhouette(128);
    PGlass(0, 0, P_W, P_H, 0, 0, 0, 215);
    PText(60, 40, "Choose a country", 235, 235, 245, 2);
    PText(60, 74, "j/k move   ENTER play   ESC back", 140, 148, 160);

    y = 110;
    n = 0;
    for (i = top; i < w_nc && n < rows; i++)
    {
      if (cnt[i] > 0)
      {
        StrPrint(line, "%-22s  %-4s  %4d provinces", w_cname[i], w_ciso[i],
                 cnt[i]);
        if (i == cur)
        {
          PFill(56, y - 2, 420, 13, 30, 38, 52);
          PText(60, y, ">", 235, 200, 90);
          PText(74, y, line, 235, 205, 60);
        }
        else
          PText(74, y, line, 205, 210, 220);
        y += 13;
        n++;
      }
    }
    }

    ch = ScanChar;
    if (ch == CH_ESC || ch == CH_SHIFT_ESC) return -1;
    if (ch == '\n' && cnt[cur] > 0) return w_ccid[cur];
    if (ch == 'j' || ch == 'k')
    {
      dirty = TRUE;
      if (ch == 'j') cur++;
      else           cur--;
      if (cur >= w_nc) cur = 0;
      if (cur < 0) cur = w_nc - 1;
      while (cur < w_nc && cnt[cur] == 0) cur++;
      if (cur >= w_nc) cur = 0;
      if (cur < top) top = cur;
      if (cur >= top + rows) top = cur - rows + 1;
    }
    Sleep(10);
  }
}

/* ── ONE GAME, ON A SCREEN SOMEBODY ELSE OPENED ──
 *
 * Takes a country and plays, assuming the world is loaded and the framebuffer
 * is already ours. Split out so the menu can start a game and get control
 * back when it ends, without either of them owning the video mode. */
U0 ODPlay(I64 cid)
{
  I64 ch, i, s, men, cost, tgt, n, prov, inc;

  w_me = cid;
  RInit;
  GOwnList;
  GShade;
  g_sel = -1;
  g_minei = -1;
  g_overlay = -1;
  g_view = -1;
  for (i = 0; i < 4; i++)
  {
    g_ocur[i] = 0;
    g_otop[i] = 0;
  }
  for (i = 0; i < d_nres; i++) d_done[i] = 0;
  for (i = 0; i < d_npol; i++) d_active[i] = 0;
  d_rp = 0;
  StrCpy(g_status, "h/l province  r raise  a assault  1-8 views  P E C R panels");

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
        else if (ch >= '1' && ch <= '8')
        {
          /* The tab keys follow the desktop game's bar, so "3" is Defence
           * there and Defence here. A tab with nothing behind it says so
           * rather than silently doing nothing. */
          i = ch - '1';
          if (g_view_live[i])
          {
            g_view = i;
            StrPrint(g_status, "%s", g_view_tabs[i]);
          }
          else
            StrPrint(g_status, "%s is not in the TempleOS build",
                     g_view_tabs[i]);
          GShade;
        }
        else if (ch == '0')
        {
          g_view = -1;
          StrCpy(g_status, "political map");
          GShade;
        }
        else if (g_overlay >= 0 && (ch == 'j' || ch == 'k'))
        {
          /* While a panel is open the cursor keys belong to it, not to the
           * map underneath. */
          n = 0;
          if (g_overlay == 0) n = d_npol;
          if (g_overlay == 3) n = d_nres;
          if (n)
          {
            if (ch == 'j') g_ocur[g_overlay]++;
            else           g_ocur[g_overlay]--;
            if (g_ocur[g_overlay] >= n) g_ocur[g_overlay] = n - 1;
            if (g_ocur[g_overlay] < 0) g_ocur[g_overlay] = 0;
            if (g_ocur[g_overlay] < g_otop[g_overlay])
              g_otop[g_overlay] = g_ocur[g_overlay];
            if (g_ocur[g_overlay] >= g_otop[g_overlay] + G_ROWS)
              g_otop[g_overlay] = g_ocur[g_overlay] - G_ROWS + 1;
          }
        }
        else if (g_overlay == 0 && ch == '\n')
        {
          /* Enact or repeal. The cost is per turn and comes out of income, so
           * nothing is charged here -- the turn charges it. */
          i = g_ocur[0];
          if (i >= 0 && i < d_npol)
          {
            if (d_active[i])
            {
              d_active[i] = 0;
              StrPrint(g_status, "repealed %s", d_pname[i]);
            }
            else
            {
              d_active[i] = 1;
              StrPrint(g_status, "enacted %s (%d.%d per turn)",
                       d_pname[i], d_pcost[i] / 10, d_pcost[i] % 10);
            }
          }
        }
        else if (g_overlay == 3 && ch == '\n')
        {
          i = g_ocur[3];
          if (i >= 0 && i < d_nres)
          {
            if (d_done[i])
              StrPrint(g_status, "%s is already researched", d_rname[i]);
            else if (!DResAvailable(i))
              StrPrint(g_status, "%s needs its prerequisites first",
                       d_rname[i]);
            else if (d_rp < d_rcost[i])
              StrPrint(g_status, "%s costs %d points and you have %d",
                       d_rname[i], d_rcost[i], d_rp);
            else
            {
              d_rp -= d_rcost[i];
              d_done[i] = 1;
              StrPrint(g_status, "researched %s", d_rname[i]);
            }
          }
        }
        else if (ch == 'P' || ch == 'E' || ch == 'C' || ch == 'R')
        {
          i = 0;
          if (ch == 'E') i = 1;
          if (ch == 'C') i = 2;
          if (ch == 'R') i = 3;
          if (g_overlay == i) g_overlay = -1;
          else                g_overlay = i;
        }
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
          /* Policy upkeep and research income ride with the turn, so a policy
           * enacted here is paid for from the same purse everything else is. */
          s = WSlot(w_me);
          RCountryStats(w_me, &prov, &men, &inc);
          d_rp += DResearchRate(inc);
          n = 0;
          for (i = 0; i < d_npol; i++)
            if (d_active[i]) n += d_pcost[i];
          if (s >= 0) w_tre[s] -= n;
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

}

/* The direct entry: load a world, take the screen, play one seat, give it
 * back. Convenient when you already know which country you want. */
U0 ODGame(U8 *iso = "SWE", U8 *world = "world.odw")
{
  I64 i, cid = -1;

  if (!WLoad(world))
  {
    "$$FG,RED$$cannot load %s$$FG$$\n", world;
    return;
  }
  DLoad;
  GInitTables;
  for (i = 0; i < w_nc; i++)
    if (!StrCmp(w_ciso[i], iso)) cid = w_ccid[i];
  if (cid < 0)
  {
    "$$FG,RED$$no country %s in this world$$FG$$\n", iso;
    return;
  }
  PFontLoad;
  if (!PaintBegin)
  {
    "$$FG,RED$$no linear framebuffer here -- this needs the DISPI registers "
    "(QEMU, Bochs, VirtualBox)$$FG$$\n";
    return;
  }
  ODPlay(cid);
  PaintEnd;
}

/* Runs at include time: if this does not appear, the file was never
 * compiled, whatever the silence suggests. */

/* ── THE WHOLE THING ──
 *
 * Boot into the menu, pick a country, play, come back. This is the entry
 * point; ODGame() is still callable directly when you know the seat you want.
 *
 *     #include "ODGame"
 *     OD;
 */
U0 OD(U8 *world = "world.odw")
{
  I64 ch, cur = 0, cid;
  Bool dirty;

  if (!WLoad(world))
  {
    "$$FG,RED$$cannot load %s$$FG$$\n", world;
    return;
  }
  if (!DLoad)
    "$$FG,YELLOW$$no game.odd -- politics, research and claims will be "
    "empty$$FG$$\n";
  GInitTables;
  GMenuInit;
  PFontLoad;

  if (!PaintBegin)
  {
    "$$FG,RED$$no linear framebuffer here -- this needs the DISPI registers "
    "(QEMU, Bochs, VirtualBox)$$FG$$\n";
    return;
  }

  try
  {
    dirty = TRUE;
    while (TRUE)
    {
      if (dirty)
      {
        GMenuDraw(cur);
        dirty = FALSE;
      }
      ch = ScanChar;
      if (ch == CH_ESC || ch == CH_SHIFT_ESC)
        goto od_done;
      else if (ch == 'j' || ch == 'k')
      {
        if (ch == 'j') cur++;
        else           cur--;
        if (cur > 6) cur = 0;
        if (cur < 0) cur = 6;
        dirty = TRUE;
      }
      else if (ch == '\n')
      {
        if (cur == 6)
          goto od_done;
        else if (cur == 0)
        {
          cid = GSelect;
          if (cid >= 0) ODPlay(cid);
          dirty = TRUE;
        }
        else if (cur == 5)
        {
          PFill(0, 0, P_W, P_H, 6, 7, 11);
          PText(60, 80, "Open Doctrines", 235, 205, 60, 3);
          PText(60, 130, "TempleOS build.", 215, 220, 230);
          PText(60, 150, "The rules, the map and the interface run on this "
                         "machine.", 175, 182, 195);
          PText(60, 170, "The world is baked from the desktop game's own data "
                         "by tools/templeos_world.py.", 175, 182, 195);
          PText(60, 200, "Font: raylib's built-in face, the same glyphs the "
                         "desktop game draws.", 175, 182, 195);
          PText(60, 230, "TempleOS is public domain, by Terry A. Davis.",
                175, 182, 195);
          PText(60, 280, "any key", 140, 148, 160);
          while (!ScanChar) Sleep(20);
          dirty = TRUE;
        }
      }
      Sleep(20);
    }
od_done:
  }
  catch
    PutExcept;

  PaintEnd;
}
