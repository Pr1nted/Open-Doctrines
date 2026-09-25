/* Open Doctrines on TempleOS. All of it, on the machine.
 *
 * The world is baked into world.odw; the rules are in Rules.HC; the screen is
 * a 1024x768 true-colour framebuffer this program takes from the card. No
 * host, no bridge, no file waiting to be fetched -- once this is running the
 * only thing outside is the disk it was read from.
 *
 *     #include "ODGame"
 *     ODStart;          // the menu
 *     ODGame("SWE");    // or straight into a seat
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
#define G_NVIEW  8
#define G_NTAB   8

/* ── EVERY ACTION THIS BUILD OFFERS ──
 *
 * The desktop game names 39 in Game_AITrain.cpp; 28 of them are here and the
 * other 11 are recorded in tools/templeos_sync.py with the reason they cannot
 * be. One assignment per line, which is the shape the sync tool reads and the
 * only shape HolyC takes for an array of pointers. */
#define G_NACT  39
U8 *g_act_ids[G_NACT];
U8 *g_act_keys[G_NACT];

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
/* ── SENDING A FLEET SOMEWHERE ──
 *
 * Entered with m at a harbour you have ships in: the sea is flooded out to
 * the fleet's range, every harbour of yours it reaches becomes a candidate,
 * and h/l walks them. The distance is real -- cells of open water crossed --
 * so a policy that changes naval speed changes where your ships can be next
 * turn, which is the only way that lever could mean anything. */
/* ── ONE PICKER, FOUR ORDERS ──
 *
 * Sailing, reinforcing, landing and engaging all ask the same question --
 * which of these places -- so they share the machinery and differ only in
 * what fills the list and what happens on ENTER. */
#define G_PICK_NONE   0
#define G_PICK_SAIL   1
#define G_PICK_REINF  2
#define G_PICK_LAND   3
#define G_PICK_ENGAGE 4
#define G_PICK_ARTY   5

I64 g_pick = G_PICK_NONE;
Bool g_help = FALSE;
I64 g_shell = 0;                 /* which shell type is loaded */
I64 g_navfrom = -1;
I64 g_navlist[512], g_navn = 0, g_navi = 0;

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
  g_view_ids[6] = "view.navy";
  g_view_ids[7] = "view.names";

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
  g_view_live[SY_VIEW_NAVY]       = 1;
  g_view_live[SY_VIEW_RESOURCES]  = 1;
  g_view_live[SY_VIEW_NAMES]      = 1;

  g_act_ids[0]  = "war/hold";        g_act_keys[0]  = "(default)";
  g_act_ids[1]  = "war/recruit";     g_act_keys[1]  = "r";
  g_act_ids[2]  = "war/reinforce";   g_act_keys[2]  = "v";
  g_act_ids[3]  = "war/attack";      g_act_keys[3]  = "a";
  g_act_ids[4]  = "war/declare war"; g_act_keys[4]  = "w";
  g_act_ids[5]  = "war/ceasefire";   g_act_keys[5]  = "W";
  g_act_ids[6]  = "econ/save";       g_act_keys[6]  = "(default)";
  g_act_ids[7]  = "econ/industry";   g_act_keys[7]  = "i";
  g_act_ids[8]  = "econ/fort";       g_act_keys[8]  = "o";
  g_act_ids[9]  = "econ/port";       g_act_keys[9]  = "p";
  g_act_ids[10] = "econ/destroyer";  g_act_keys[10] = "B";
  g_act_ids[11] = "econ/carrier";    g_act_keys[11] = "N";
  g_act_ids[12] = "econ/fund up";    g_act_keys[12] = "u";
  g_act_ids[13] = "econ/fund down";  g_act_keys[13] = "d";
  g_act_ids[14] = "pol/hold";        g_act_keys[14] = "(default)";
  g_act_ids[15] = "pol/enact";       g_act_keys[15] = "P then ENTER";
  g_act_ids[16] = "pol/cancel";      g_act_keys[16] = "P then ENTER";
  g_act_ids[17] = "pol/alliance";    g_act_keys[17] = "A";
  g_act_ids[18] = "pol/nap";         g_act_keys[18] = "T";
  g_act_ids[19] = "pol/guarantee";   g_act_keys[19] = "G";
  g_act_ids[20] = "pol/trade";       g_act_keys[20] = "D";
  g_act_ids[21] = "navy/hold";       g_act_keys[21] = "(default)";
  g_act_ids[22] = "navy/move";       g_act_keys[22] = "m";
  g_act_ids[23] = "navy/bombard";    g_act_keys[23] = "b";
  g_act_ids[24] = "navy/embark";     g_act_keys[24] = "e";
  g_act_ids[25] = "navy/land";       g_act_keys[25] = "L";
  g_act_ids[26] = "navy/scrap";      g_act_keys[26] = "s";
  g_act_ids[27] = "navy/engage";     g_act_keys[27] = "g";

  g_act_ids[28] = "war/artillery";   g_act_keys[28] = "x  (X picks)";
  g_act_ids[29] = "war/stage";       g_act_keys[29] = "S";
  g_act_ids[30] = "econ/specialize"; g_act_keys[30] = "y";
  g_act_ids[31] = "econ/focus bldg"; g_act_keys[31] = "q";
  g_act_ids[32] = "econ/focus army"; g_act_keys[32] = "Q";
  g_act_ids[33] = "econ/focus navy"; g_act_keys[33] = "z";
  g_act_ids[34] = "pol/pacify up";   g_act_keys[34] = "+";
  g_act_ids[35] = "pol/pacify dn";   g_act_keys[35] = "-";
  g_act_ids[36] = "pol/calming";     g_act_keys[36] = "k";
  g_act_ids[37] = "pol/conciliate";  g_act_keys[37] = "K";
  g_act_ids[38] = "pol/repress";     g_act_keys[38] = "Z";

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
    else if (g_view == SY_VIEW_NAVY)
    {
      /* Harbours by size, and everything inland pushed right down: a naval
       * view whose land is as bright as its ports is a political map with
       * extra steps. */
      if (w_port[i] > 0)
        GHeat(w_port[i], 5, &r, &g, &b);
      else
      {
        r = 26; g = 29; b = 34;
      }
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
/* ── THE FLEETS, DRAWN ──
 *
 * One mark per hull at its harbour, stacked upwards so a big naval base reads
 * as a column rather than a single dot. Only in the navy view: everywhere
 * else they would be noise over the thing you asked to see. */
/* The five deposits, in the order their bits are packed. */
U8 *GResName(I64 bit)
{
  if (bit == 0) return "oil";
  if (bit == 1) return "gold";
  if (bit == 2) return "metal";
  if (bit == 3) return "rubber";
  if (bit == 4) return "gemstones";
  return "nothing";
}

U0 GShips()
{
  /* The array on its own line: a 256-entry buffer declared in the same
   * statement as six scalars is the sort of thing that works everywhere
   * except where it does not, and this is not a compiler to find out with. */
  I64 i, j, x, y, k, n;
  I64 seen[OD_MAXS];

  if (g_view != SY_VIEW_NAVY) return;

  for (i = 0; i < w_ns; i++) seen[i] = 0;
  for (i = 0; i < w_ns; i++)
    /* A hull whose harbour is out of range would index the centroid arrays
     * past their end, which on this machine is a fault and not a wrong
     * pixel. Cheap to refuse, and it costs nothing when the file is sound. */
    if (!seen[i] && w_shp[i] > 0 && w_sprov[i] >= 0 && w_sprov[i] < w_np)
    {
      n = 0;
      for (j = i; j < w_ns; j++)
        if (w_sprov[j] == w_sprov[i] && w_scid[j] == w_scid[i] && w_shp[j] > 0)
        {
          seen[j] = 1;
          n++;
        }
      x = w_cx[w_sprov[i]];
      y = w_cy[w_sprov[i]] + G_MAPY;
      for (k = 0; k < n && k < 12; k++)
      {
        if (w_scid[i] == w_me)
          PFill(x - 1, y - k * 3, 3, 2, 235, 235, 245);
        else
          PFill(x - 1, y - k * 3, 3, 2, 200, 120, 110);
      }
    }
}

/* The harbour a fleet is being sent to, ringed while you choose. */
U0 GNavTarget()
{
  I64 x, y, i;
  if (g_pick == G_PICK_NONE || !g_navn) return;
  x = w_cx[g_navlist[g_navi]];
  y = w_cy[g_navlist[g_navi]] + G_MAPY;
  for (i = 0; i < 7; i++)
  {
    PFill(x - 6 + i, y - 6, 1, 1, 235, 205, 60);
    PFill(x - 6 + i, y + 6, 1, 1, 235, 205, 60);
    PFill(x - 6, y - 6 + i, 1, 1, 235, 205, 60);
    PFill(x + 6, y - 6 + i, 1, 1, 235, 205, 60);
  }
  x = w_cx[g_navfrom];
  y = w_cy[g_navfrom] + G_MAPY;
  PFill(x - 2, y - 2, 5, 5, 120, 215, 130);
}

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
  I64 s, prov, men, inc, y, ix, n;

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
    if (r_unrest[ix])
    {
      PPrint(14, y, 225, 130, 120, 1, "Unrest:     %d", r_unrest[ix]);
      y += 12;
    }
    if (r_align[ix] != 50)
    {
      PPrint(14, y, 150, 190, 215, 1, "Alignment:  %d", r_align[ix]);
      y += 12;
    }
    if (r_spec[ix])
    {
      n = 0;
      for (s = 0; s < 5; s++)
        if (r_spec[ix] == (1 << s)) n = s;
      PPrint(14, y, 150, 190, 215, 1, "Works:      %s", GResName(n));
      y += 12;
    }
    n = DMinorityOf(ix);
    if (n >= 0 && d_mgrp[n][1] != 0xFF && d_mpct[n][1] > 0)
    {
      PPrint(14, y, 150, 160, 175, 1, "Largest minority: %s %d%%",
             d_gname[d_mgrp[n][1]], d_mpct[n][1]);
      y += 12;
    }
    if (w_port[ix] > 0)
    {
      PPrint(14, y, 150, 190, 215, 1, "Harbour:    level %d", w_port[ix]);
      y += 12;
      n = RFleetAt(ix, w_own[ix]);
      if (n)
      {
        PPrint(14, y, 150, 190, 215, 1, "Fleet:      %d strength", n);
        y += 12;
      }
    }
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

    /* ── WHAT THE HIGHLIGHTED POLICY ACTUALLY DOES ──
     *
     * Listed rather than summarised, because a policy that costs money every
     * turn and shows no effect is indistinguishable from one that is broken.
     * A policy with nothing here pulls only levers these rules cannot act on,
     * and says so. */
    i = cur;
    y = 560;
    PFill(170, y - 6, P_W - 440, 1, 70, 74, 84);
    if (i >= 0 && i < d_npol)
    {
      PPrint(170, y, 235, 235, 240, 1, "%s", d_pname[i]);
      y += 14;
      n = 0;
      for (k = 0; k < DT_NLEV; k++)
        if (d_plev[i][k])
        {
          word = "+";
          if (d_plev[i][k] < 0) word = "";
          PPrint(182 + n * 210, y, 200, 205, 215, 1, "%s  %s%d.%d",
                 d_levname[k], word, d_plev[i][k] / 10, AbsI64(d_plev[i][k]) % 10);
          n++;
          if (n == 3) { n = 0; y += 13; }
        }
      if (!n && y == 574)
        PText(182, y, "pulls no lever these rules act on", 150, 160, 175);
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
    n = RScale(men / R_UPKEEP_PER_MAN, RLever(w_me, RL_MAINT));
    PPrint(170, y, 205, 130, 120, 1, "Army upkeep     %d.%d per turn",
           n / 10, n % 10);
    y += 16;
    PPrint(170, y, 200, 205, 215, 1, "Standing army   %d men", men);
    y += 16;
    k = RLever(w_me, RL_PASSIVE);
    if (k)
    {
      PPrint(170, y, 120, 215, 130, 1, "Policy income   %d.%d per turn",
             k / 10, k % 10);
      y += 16;
    }
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

/* ── WHAT YOU CAN DO ──
 *
 * The whole action list, with the key for each. Worth a panel of its own
 * because there are twenty-eight of them and no menus to discover them in --
 * and because a list that matches the desktop game's own enumeration is the
 * honest way to say what this build does and does not have. */
U0 GHelp()
{
  I64 i, x, y;

  if (!g_help) return;
  PGlass(120, 70, P_W - 240, 620, 8, 10, 14, 242);
  PRect(120, 70, P_W - 240, 620, 70, 74, 84);
  PText(140, 88, "Actions", 240, 240, 245, 2);
  PPrint(140, 118, 150, 160, 175, 1,
         "All %d the desktop game offers.", G_NACT);

  for (i = 0; i < G_NACT; i++)
  {
    x = 140 + (i / 20) * 260;
    y = 146 + (i % 20) * 15;
    PPrint(x, y, 235, 200, 90, 1, "%-11s", g_act_keys[i]);
    PText(x + 92, y, g_act_ids[i], 205, 210, 220);
  }

  y = 146 + 20 * 15 + 16;
  PFill(140, y, P_W - 280, 1, 70, 74, 84);
  y += 12;
  PText(140, y, "All 39. The four models the last eleven needed -- shells, "
                "specialisation, unrest and", 150, 160, 175);
  y += 14;
  PText(140, y, "who lives where -- are simpler than the desktop game's, and "
                "they are real: they have", 150, 160, 175);
  y += 14;
  PText(140, y, "causes, they have consequences, and these actions move them.",
        150, 160, 175);
  y += 22;
  PText(140, y, "1-8 views   P E C R panels   ENTER end turn   ? close",
        130, 138, 150);
}

U0 GDraw()
{
  GDrawMap;
  GShips;
  GNavTarget;
  GNames;
  GPanel;
  GSide;
  GBar;
  GOverlay;
  GHelp;
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
  /* The version the rest of the build stamps: CMake and this both read the
   * VERSION file, so the menu cannot claim a release nobody cut. */
  PPrint(P_W - 170, P_H - 18, 120, 128, 140, 1, "v%s  TempleOS", SY_VERSION);
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
public U0 ODPlay(I64 cid)
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
  g_navfrom = -1;
  for (i = 0; i < 4; i++)
  {
    g_ocur[i] = 0;
    g_otop[i] = 0;
  }
  for (i = 0; i < d_nres; i++) d_done[i] = 0;
  for (i = 0; i < d_npol; i++) d_active[i] = 0;
  d_rp = 0;
  StrCpy(g_status, "h/l province  r raise  a assault  b shell  B build  "
                   "? for every action");

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
        {
          /* Escape backs out of whatever is open before it leaves the game:
           * quitting because somebody cancelled a fleet order would be a
           * memorable way to lose a campaign. */
          if (g_help) g_help = FALSE;
          else if (g_pick != G_PICK_NONE)
          {
            g_pick = G_PICK_NONE;
            g_navfrom = -1;
          }
          else if (g_overlay >= 0) g_overlay = -1;
          else goto g_done;
        }
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
        else if ((ch == 'l' || ch == 'h') && g_pick == G_PICK_NONE)
        {
          /* ── ORDER IN THIS CHAIN IS MEANING ──
           *
           * h and l walk your provinces, and they also choose a destination
           * once a fleet is being sent somewhere. Whichever branch comes
           * first in the chain wins, so the one that must NOT win says so in
           * its own condition rather than relying on where it happens to sit.
           * Without this guard the fleet dialogue was on screen, taking keys,
           * and silently unable to change its answer. */
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
        else if (ch == 'f')
        {
          /* Jump to the next harbour of yours with ships in it. Walking
           * forty provinces to find the three with a fleet is not a naval
           * interface. */
          n = -1;
          for (i = 1; i <= w_np && n < 0; i++)
          {
            s = g_sel + i;
            if (s >= w_np) s -= w_np;
            if (s >= 0 && w_own[s] == w_me && RFleetAt(s, w_me) > 0) n = s;
          }
          if (n < 0)
            StrCpy(g_status, "you have no ships");
          else
          {
            g_sel = n;
            StrPrint(g_status, "%s: fleet of %d strength, range %d",
                     w_name[n], RFleetAt(n, w_me), RFleetRange(n, w_me));
          }
        }
        else if (ch == 'm' && g_sel >= 0 && w_own[g_sel] == w_me &&
                 g_pick == G_PICK_NONE)
        {
          n = RFleetRange(g_sel, w_me);
          if (!n)
            StrPrint(g_status, "no ships at %s", w_name[g_sel]);
          else
          {
            RSeaFlood(w_pcell[g_sel], n);
            g_navn = 0;
            for (i = 0; i < w_np && g_navn < 512; i++)
              if (i != g_sel && w_own[i] == w_me && w_port[i] > 0 &&
                  RSeaReaches(i))
              {
                g_navlist[g_navn] = i;
                g_navn++;
              }
            if (!g_navn)
              StrPrint(g_status, "nowhere within %d cells of sea from %s", n,
                       w_name[g_sel]);
            else
            {
              g_pick = G_PICK_SAIL;
              g_navfrom = g_sel;
              g_navi = 0;
              StrCpy(g_status, "sail where?  h/l choose  ENTER go  ESC cancel");
            }
          }
        }
        else if (ch == 'v' && g_sel >= 0 && w_own[g_sel] == w_me &&
                 g_pick == G_PICK_NONE)
        {
          /* Reinforce: men walk to a province next door that is already
           * yours. Half the garrison, because a province stripped bare is
           * how a front collapses behind you. */
          g_navn = 0;
          for (i = 0; i < w_adjn[g_sel] && g_navn < 512; i++)
          {
            s = WNeighbour(g_sel, i);
            if (s >= 0 && w_own[s] == w_me)
            {
              g_navlist[g_navn] = s;
              g_navn++;
            }
          }
          if (!w_army[g_sel])
            StrPrint(g_status, "no men in %s", w_name[g_sel]);
          else if (!g_navn)
            StrPrint(g_status, "%s has no province of yours next door",
                     w_name[g_sel]);
          else
          {
            g_pick = G_PICK_REINF;
            g_navfrom = g_sel;
            g_navi = 0;
            StrCpy(g_status,
                   "reinforce where?  h/l choose  ENTER march  ESC cancel");
          }
        }
        else if (ch == 'L' && g_sel >= 0 && g_pick == G_PICK_NONE)
        {
          /* Put the cargo ashore. Onto your own ground it is a reinforcement;
           * onto anybody else's it is a landing, and a landing that fails
           * drowns -- there is no ground behind it to fall back to. */
          if (!RCargoAt(g_sel, w_me))
            StrPrint(g_status, "nothing embarked at %s", w_name[g_sel]);
          else
          {
            g_navn = 0;
            for (i = 0; i < w_adjn[g_sel] && g_navn < 512; i++)
            {
              s = WNeighbour(g_sel, i);
              if (s >= 0)
              {
                g_navlist[g_navn] = s;
                g_navn++;
              }
            }
            if (!g_navn)
              StrPrint(g_status, "nowhere to land from %s", w_name[g_sel]);
            else
            {
              g_pick = G_PICK_LAND;
              g_navfrom = g_sel;
              g_navi = 0;
              StrCpy(g_status,
                     "land where?  h/l choose  ENTER go ashore  ESC cancel");
            }
          }
        }
        else if (ch == 'g' && g_sel >= 0 && w_own[g_sel] == w_me &&
                 g_pick == G_PICK_NONE)
        {
          n = RFleetRange(g_sel, w_me);
          if (!n)
            StrPrint(g_status, "no ships at %s", w_name[g_sel]);
          else
          {
            RSeaFlood(w_pcell[g_sel], n);
            g_navn = 0;
            for (i = 0; i < w_np && g_navn < 512; i++)
              if (w_own[i] != w_me && w_port[i] > 0 && RSeaReaches(i) &&
                  RFleetAt(i, w_own[i]) > 0)
              {
                g_navlist[g_navn] = i;
                g_navn++;
              }
            if (!g_navn)
              StrPrint(g_status, "no enemy fleet within reach of %s",
                       w_name[g_sel]);
            else
            {
              g_pick = G_PICK_ENGAGE;
              g_navfrom = g_sel;
              g_navi = 0;
              StrCpy(g_status,
                     "engage whom?  h/l choose  ENTER attack  ESC cancel");
            }
          }
        }
        else if (g_pick != G_PICK_NONE && (ch == 'h' || ch == 'l'))
        {
          if (ch == 'l') g_navi++;
          else           g_navi--;
          if (g_navi >= g_navn) g_navi = 0;
          if (g_navi < 0) g_navi = g_navn - 1;
          i = g_navlist[g_navi];
          if (g_pick == G_PICK_SAIL || g_pick == G_PICK_ENGAGE)
            StrPrint(g_status, "%s  (%d cells)  h/l choose  ENTER go",
                     w_name[i], r_seadist[w_pcell[i]]);
          else
            StrPrint(g_status, "%s  (%s, %d men)  h/l choose  ENTER go",
                     w_name[i], WIso(w_own[i]), w_army[i]);
        }
        else if (g_pick != G_PICK_NONE && ch == '\n')
        {
          i = g_navlist[g_navi];
          if (g_pick == G_PICK_SAIL)
          {
            RMoveFleet(g_navfrom, i, w_me);
            StrPrint(g_status, "the fleet sailed from %s to %s",
                     w_name[g_navfrom], w_name[i]);
            g_sel = i;
          }
          else if (g_pick == G_PICK_REINF)
          {
            n = w_army[g_navfrom] / 2;
            w_army[g_navfrom] -= n;
            w_army[i] += n;
            StrPrint(g_status, "%d men marched from %s to %s", n,
                     w_name[g_navfrom], w_name[i]);
            g_sel = i;
          }
          else if (g_pick == G_PICK_LAND)
          {
            n = RDisembark(g_navfrom, w_me);
            if (w_own[i] == w_me)
            {
              w_army[i] += n;
              StrPrint(g_status, "%d men came ashore at %s", n, w_name[i]);
            }
            else if (RAssault(g_navfrom, i, n))
              StrPrint(g_status, "%d men stormed %s and took it", n,
                       w_name[i]);
            else
              StrPrint(g_status, "the landing at %s was thrown back",
                       w_name[i]);
            g_sel = i;
          }
          else if (g_pick == G_PICK_ARTY)
          {
            s = WSlot(w_me);
            cost = d_acost[g_shell] * 10;
            if (w_tre[s] < cost)
              StrPrint(g_status, "a %s shell costs %c%d", d_aname[g_shell],
                       0x24, d_acost[g_shell]);
            else
            {
              w_tre[s] -= cost;
              RArtillery(g_navfrom, i, g_shell);
              StrPrint(g_status, "%s fell on %s", d_aname[g_shell],
                       w_name[i]);
              GShade;
            }
          }
          else
          {
            if (REngage(g_navfrom, i, w_me))
              StrPrint(g_status, "the enemy fleet at %s was sunk", w_name[i]);
            else
              StrPrint(g_status, "our fleet was lost off %s", w_name[i]);
          }
          g_pick = G_PICK_NONE;
          g_navfrom = -1;
          GShade;
        }
        else if (ch == 'b' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          /* Shelling takes no ground, so the target is simply the strongest
           * enemy garrison next door -- the one worth thinning. */
          tgt = -1;
          n = -1;
          for (i = 0; i < w_adjn[g_sel]; i++)
          {
            s = WNeighbour(g_sel, i);
            if (s >= 0 && w_own[s] != w_me && w_army[s] > n)
            {
              n = w_army[s];
              tgt = s;
            }
          }
          if (!RFleetAt(g_sel, w_me))
            StrPrint(g_status, "no ships at %s", w_name[g_sel]);
          else if (tgt < 0)
            StrPrint(g_status, "nothing next to %s to shell", w_name[g_sel]);
          else
          {
            r_ord_bombard[g_sel] = tgt;
            StrPrint(g_status, "%s will shell %s", w_name[g_sel], w_name[tgt]);
          }
        }
        else if (ch == 'B' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          s = WSlot(w_me);
          cost = RScale(R_SHIP_BUILD - w_port[g_sel] * 20,
                        RLever(w_me, RL_NAVYCOST));
          if (!w_port[g_sel])
            StrPrint(g_status, "%s has no harbour", w_name[g_sel]);
          else if (w_ns >= OD_MAXS)
            StrPrint(g_status, "no room for another hull");
          else if (w_tre[s] < cost)
            StrPrint(g_status, "a hull at %s costs %c%d.%d", w_name[g_sel],
                     0x24, cost / 10, cost % 10);
          else
          {
            w_tre[s] -= cost;
            w_scid[w_ns] = w_me;
            w_skind[w_ns] = 1;              /* destroyer */
            w_shp[w_ns] = 100;
            w_sprov[w_ns] = g_sel;
            w_ns++;
            StrPrint(g_status, "laid down a destroyer at %s for %c%d.%d",
                     w_name[g_sel], 0x24, cost / 10, cost % 10);
          }
        }
        else if (ch == 'i' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          s = WSlot(w_me);
          cost = (w_ind[g_sel] + 1) * 100;
          if (w_ind[g_sel] >= 10)
            StrPrint(g_status, "%s is at the top industry level",
                     w_name[g_sel]);
          else if (w_tre[s] < cost)
            StrPrint(g_status, "industry at %s costs %c%d.%d", w_name[g_sel],
                     0x24, cost / 10, cost % 10);
          else
          {
            w_tre[s] -= cost;
            w_ind[g_sel]++;
            w_inc[g_sel] += 5;
            StrPrint(g_status, "%s industry is now level %d", w_name[g_sel],
                     w_ind[g_sel]);
            GShade;
          }
        }
        else if (ch == 'o' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          s = WSlot(w_me);
          cost = (w_fort[g_sel] + 1) * 150;
          if (w_fort[g_sel] >= 5)
            StrPrint(g_status, "%s is fortified as far as it goes",
                     w_name[g_sel]);
          else if (w_tre[s] < cost)
            StrPrint(g_status, "fortifying %s costs %c%d.%d", w_name[g_sel],
                     0x24, cost / 10, cost % 10);
          else
          {
            w_tre[s] -= cost;
            w_fort[g_sel]++;
            StrPrint(g_status, "%s is now fort level %d (frontage %d)",
                     w_name[g_sel], w_fort[g_sel], RWidth(g_sel));
            GShade;
          }
        }
        else if (ch == 'p' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          s = WSlot(w_me);
          cost = (w_port[g_sel] + 1) * 120;
          if (w_pcell[g_sel] < 0)
            StrPrint(g_status, "%s is not on the sea", w_name[g_sel]);
          else if (w_port[g_sel] >= 5)
            StrPrint(g_status, "%s has the largest harbour there is",
                     w_name[g_sel]);
          else if (w_tre[s] < cost)
            StrPrint(g_status, "a harbour at %s costs %c%d.%d", w_name[g_sel],
                     0x24, cost / 10, cost % 10);
          else
          {
            w_tre[s] -= cost;
            w_port[g_sel]++;
            StrPrint(g_status, "%s harbour is now level %d", w_name[g_sel],
                     w_port[g_sel]);
            GShade;
          }
        }
        else if (ch == 'e' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          n = RCapacityAt(g_sel, w_me);
          if (n > w_army[g_sel]) n = w_army[g_sel];
          if (!RFleetAt(g_sel, w_me))
            StrPrint(g_status, "no ships at %s", w_name[g_sel]);
          else if (n <= 0)
            StrPrint(g_status, "nothing to embark at %s", w_name[g_sel]);
          else
            StrPrint(g_status, "%d men embarked at %s",
                     REmbark(g_sel, w_me, n), w_name[g_sel]);
        }
        else if (ch == 's' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          n = -1;
          for (i = 0; i < w_ns; i++)
            if (w_sprov[i] == g_sel && w_scid[i] == w_me && w_shp[i] > 0)
              n = i;
          if (n < 0)
            StrPrint(g_status, "no ships at %s", w_name[g_sel]);
          else
          {
            s = WSlot(w_me);
            w_tre[s] += r_shipcost[w_skind[n]] / 4;   /* scrap value */
            w_shp[n] = 0;
            StrPrint(g_status, "scrapped a %s at %s", w_kindname[w_skind[n]],
                     w_name[g_sel]);
          }
        }
        else if (ch == 'N' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          s = WSlot(w_me);
          cost = RScale(r_shipcost[4], RLever(w_me, RL_NAVYCOST));
          if (!w_port[g_sel])
            StrPrint(g_status, "%s has no harbour", w_name[g_sel]);
          else if (w_ns >= OD_MAXS)
            StrPrint(g_status, "no room for another hull");
          else if (w_tre[s] < cost)
            StrPrint(g_status, "a carrier costs %c%d.%d", 0x24, cost / 10,
                     cost % 10);
          else
          {
            w_tre[s] -= cost;
            w_scid[w_ns] = w_me;
            w_skind[w_ns] = 4;
            w_shp[w_ns] = 100;
            w_sprov[w_ns] = g_sel;
            w_scargo[w_ns] = 0;
            w_ns++;
            StrPrint(g_status, "laid down a carrier at %s", w_name[g_sel]);
          }
        }
        else if (ch == 'X')
        {
          /* Cycle to the next shell this country has actually researched. */
          n = -1;
          for (i = 1; i <= d_narty && n < 0; i++)
          {
            s = g_shell + i;
            if (s >= d_narty) s -= d_narty;
            if (DArtyReady(s)) n = s;
          }
          if (n < 0)
            StrCpy(g_status, "no artillery researched yet");
          else
          {
            g_shell = n;
            StrPrint(g_status, "%s loaded: %d%% of troops, %d%% of people, "
                     "%c%d a shot", d_aname[n], d_atroop[n], d_apop[n],
                     0x24, d_acost[n]);
          }
        }
        else if (ch == 'x' && g_sel >= 0 && w_own[g_sel] == w_me &&
                 g_pick == G_PICK_NONE)
        {
          if (!DArtyReady(g_shell))
            StrCpy(g_status, "that shell is not researched -- X to pick another");
          else
          {
            g_navn = 0;
            for (i = 0; i < w_adjn[g_sel] && g_navn < 512; i++)
            {
              s = WNeighbour(g_sel, i);
              if (s >= 0 && w_own[s] != w_me)
              {
                g_navlist[g_navn] = s;
                g_navn++;
              }
            }
            if (!g_navn)
              StrPrint(g_status, "nothing next to %s to shell", w_name[g_sel]);
            else
            {
              g_pick = G_PICK_ARTY;
              g_navfrom = g_sel;
              g_navi = 0;
              StrPrint(g_status, "shell where with %s?  h/l choose  ENTER fire",
                       d_aname[g_shell]);
            }
          }
        }
        else if (ch == 'S' && g_sel >= 0 && w_own[g_sel] == w_me &&
                 g_pick == G_PICK_NONE)
        {
          /* Staging: men cross into an ALLY'S province to attack from it.
           * The desktop game's own phrasing is "stage troops into allied
           * prov X from Y", and an alliance is what makes it possible. */
          g_navn = 0;
          for (i = 0; i < w_adjn[g_sel] && g_navn < 512; i++)
          {
            s = WNeighbour(g_sel, i);
            if (s >= 0 && w_own[s] != w_me && RPactWith(w_me, w_own[s]) == 1)
            {
              g_navlist[g_navn] = s;
              g_navn++;
            }
          }
          if (!w_army[g_sel])
            StrPrint(g_status, "no men in %s", w_name[g_sel]);
          else if (!g_navn)
            StrPrint(g_status, "no allied ground next to %s", w_name[g_sel]);
          else
          {
            g_pick = G_PICK_REINF;       /* the march is the same march */
            g_navfrom = g_sel;
            g_navi = 0;
            StrCpy(g_status, "stage into whose ground?  h/l  ENTER march");
          }
        }
        else if (ch == 'y' && g_sel >= 0 && w_own[g_sel] == w_me)
        {
          /* Work the next deposit the province actually has, wrapping round
           * to none. Written as an index over five bits rather than shifting
           * a mask about, because the shifting version was wrong in a way
           * that read as correct. */
          cost = -1;
          for (i = 0; i < 5; i++)
            if (r_spec[g_sel] == (1 << i)) cost = i;
          n = -1;
          for (i = 1; i <= 5 && n < 0; i++)
          {
            s = cost + i;
            while (s >= 5) s -= 5;
            if (w_res[g_sel] & (1 << s)) n = s;
          }
          if (!w_res[g_sel])
            StrPrint(g_status, "%s has nothing to work", w_name[g_sel]);
          else
          {
            r_spec[g_sel] = 1 << n;
            StrPrint(g_status, "%s now works its %s  (+20%% income)",
                     w_name[g_sel], GResName(n));
            GShade;
          }
        }
        else if (ch == 'q' || ch == 'Q' || ch == 'z')
        {
          if (ch == 'q') i = RFocus("Buildings", FALSE);
          else if (ch == 'Q') i = RFocus("Army", FALSE);
          else i = RFocus("navy", TRUE);
          if (i < 0)
            StrCpy(g_status, "nothing available in that branch");
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
        else if (ch == '+' || ch == '-')
        {
          if (ch == '+') r_pacify += 5;
          else           r_pacify -= 5;
          if (r_pacify > 50) r_pacify = 50;
          if (r_pacify < 0) r_pacify = 0;
          StrPrint(g_status, "pacification budget %d%% of income", r_pacify);
        }
        else if (ch == 'k' || ch == 'K' || ch == 'Z')
        {
          s = WSlot(w_me);
          if (ch == 'k')
          {
            /* Calming: a national gesture, priced nationally. */
            cost = 0;
            for (i = 0; i < w_np; i++)
              if (w_own[i] == w_me) cost += 5;
            if (w_tre[s] < cost)
              StrPrint(g_status, "calming the country costs %c%d.%d", 0x24,
                       cost / 10, cost % 10);
            else
            {
              w_tre[s] -= cost;
              for (i = 0; i < w_np; i++)
                if (w_own[i] == w_me)
                {
                  r_align[i] += 5;
                  if (r_align[i] > 100) r_align[i] = 100;
                }
              StrCpy(g_status, "the country was calmed");
            }
          }
          else if (ch == 'K' && g_sel >= 0 && w_own[g_sel] == w_me)
          {
            if (w_tre[s] < 100)
              StrPrint(g_status, "conciliation costs %c10.0", 0x24);
            else
            {
              w_tre[s] -= 100;
              r_align[g_sel] += 15;
              if (r_align[g_sel] > 100) r_align[g_sel] = 100;
              StrPrint(g_status, "%s was conciliated (alignment %d)",
                       w_name[g_sel], r_align[g_sel]);
            }
          }
          else if (ch == 'Z' && g_sel >= 0 && w_own[g_sel] == w_me)
          {
            /* Repression buys quiet now and resentment later, which is the
             * whole of what it is for. */
            r_unrest[g_sel] -= 25;
            if (r_unrest[g_sel] < 0) r_unrest[g_sel] = 0;
            r_align[g_sel] -= 10;
            if (r_align[g_sel] < 0) r_align[g_sel] = 0;
            StrPrint(g_status, "%s was repressed: unrest %d, alignment %d",
                     w_name[g_sel], r_unrest[g_sel], r_align[g_sel]);
            GShade;
          }
        }
        else if (ch == '?')
          g_help = !g_help;
        else if (ch == 'u' || ch == 'd')
        {
          /* Research funding: income diverted before it reaches the treasury.
           * The desktop game's slider, in five-point steps. */
          if (ch == 'u') r_fund += 5;
          else           r_fund -= 5;
          if (r_fund > 50) r_fund = 50;
          if (r_fund < 0) r_fund = 0;
          StrPrint(g_status, "research funding %d%% of income", r_fund);
        }
        else if (g_sel >= 0 && w_own[g_sel] != w_me &&
                 (ch == 'w' || ch == 'W' || ch == 'A' || ch == 'T' ||
                  ch == 'G' || ch == 'D'))
        {
          tgt = w_own[g_sel];
          if (ch == 'w')
          {
            RPact(w_me, tgt, 0);
            RDeclareWar(w_me, tgt);
            StrPrint(g_status, "war declared on %s", WCName(tgt));
          }
          else if (ch == 'W')
          {
            RCeasefire(w_me);
            StrPrint(g_status, "a ceasefire with %s", WCName(tgt));
          }
          else if (ch == 'A')
          {
            RPact(w_me, tgt, 1);
            StrPrint(g_status, "an alliance with %s", WCName(tgt));
          }
          else if (ch == 'T')
          {
            RPact(w_me, tgt, 2);
            StrPrint(g_status, "a non-aggression pact with %s", WCName(tgt));
          }
          else if (ch == 'G')
          {
            RPact(w_me, tgt, 3);
            StrPrint(g_status, "%s is guaranteed", WCName(tgt));
          }
          else
          {
            RPact(w_me, tgt, 4);
            StrPrint(g_status, "a trade agreement with %s", WCName(tgt));
          }
          GShade;
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
          /* Funding is income diverted BEFORE it is spent on anything else,
           * which is what makes "fund up" a decision rather than a free
           * upgrade. At zero you still learn slowly: the base is one point. */
          cost = inc * r_fund / 100;
          if (s >= 0) w_tre[s] -= cost;
          d_rp += DResearchRate(cost);
          n = 0;
          for (i = 0; i < d_npol; i++)
            if (d_active[i]) n += d_pcost[i];
          if (s >= 0) w_tre[s] -= n;
          RTurn;
          GOwnList;
          GShade;
          /* The selection survives the turn. Clearing it meant every order
           * after the first had to re-find its province, and an interface
           * that forgets where you were looking is one you fight. It is only
           * dropped if the province stopped being yours. */
          if (g_sel >= 0 && w_own[g_sel] != w_me) g_sel = -1;
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
public U0 ODGame(U8 *iso = "SWE", U8 *world = "world.odw")
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
  WFree;
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
/* ── public, AND NOT CALLED "OD" ──
 *
 * `public` is what puts a symbol in the table Load() adds to the running
 * system; without it the binary loads and exports nothing anybody can call.
 * And the short name resolved to a class rather than a function when called
 * from the shell -- "Invalid class at )" -- so the entry point has a name
 * nothing else wants. */
public U0 ODStart(U8 *world = "world.odw")
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
          if (cid >= 0)
          {
            ODPlay(cid);
            /* ── A NEW GAME STARTS FROM THE WORLD, NOT FROM THE LAST ONE ──
             *
             * The world lives in globals that a game plays on directly, so
             * the second game inherits the first one's borders, armies and
             * dead empires unless it is read again. Freed first: otherwise
             * every game leaks the raster and the adjacency. */
            WFree;
            WLoad(world);
          }
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
  WFree;
}
