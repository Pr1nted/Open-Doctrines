/* Open Doctrines' rules, running on TempleOS.
 *
 * The engine on the other side of the bridge is C++ with OpenGL under it and
 * cannot be brought here. Its RULES can, and this is them: income, recruitment,
 * assault and an opponent, resolved on the machine itself. Once world.odw is
 * loaded nothing else is consulted -- no host, no file, no turn waiting to be
 * fetched.
 *
 * ── WHAT IS FAITHFUL ──
 *
 * Combat is the real model, with the real constants (src/Game.h):
 *
 *   width   = max(20000, area x 25), narrowed 12% per fort level, floored at
 *             a quarter -- a fort narrows a front, it does not seal a province
 *   engaged = min(men, width)   ON BOTH SIDES
 *   defence = engaged x (1 + fort x 10%)
 *
 * The width rule is the one that makes this game itself rather than a
 * numbers-beat-numbers game: above the frontage, extra men do not fight. Ten
 * million attackers and one million attackers hit a narrow province with the
 * same force, and the difference only shows in who can afford the losses.
 * Area is counted on the ORIGINAL raster so these constants mean here exactly
 * what they mean there.
 *
 * ── WHAT IS SIMPLIFIED, AND SAID SO ──
 *
 * No force composition -- every man is line infantry, which is what the real
 * engine's headcount path raises too. No supply, no research, no diplomacy:
 * an AI attacks a neighbour it heavily outnumbers rather than declaring war
 * first. No navies. These are absences, not approximations; nothing here
 * pretends to a number it has not got.
 */
#include "World"
#include "Data"

#define R_WIDTH_MIN      20000
#define R_WIDTH_PER_AREA    25
#define R_FORT_NARROW_PCT   12      /* per fort level */
#define R_FORT_DEF_PCT      10      /* per fort level */
#define R_RECRUIT_CAP_PCT   20      /* of a province's people, per turn */
#define R_COST_PER_10K       1      /* dollars; treasuries are in tenths */
#define R_GROWTH_PER_MILLE   5      /* population, per turn */
#define R_AI_ODDS            2      /* attack at this multiple of the defence */

/* ── WHO IS AT WAR WITH WHOM ──
 *
 * One enemy per country, by slot, or -1 for at peace. A full relations matrix
 * is 180x180 and this game has no diplomacy to fill it with; one enemy at a
 * time gives the thing every strategy game needs, which is that most borders
 * are quiet most of the time. Without it every country attacks every neighbour
 * it outnumbers on every turn, and the map dissolves in six turns -- which is
 * exactly what the first version of these rules did.
 */
I64 r_enemy[OD_MAXC];               /* country id, or -1 */
I64 r_wartime[OD_MAXC];             /* turns this war has run */

#define R_WAR_MAX_TURNS     12      /* then exhaustion, and peace */
#define R_WAR_ODDS         600      /* 1 in this many per country per turn */

/* ── WHAT A POLICY ACTUALLY DOES ──
 *
 * Every lever is stored in TENTHS of its own unit, so a percentage lever is
 * applied as (1000 + v) / 1000 and a growth lever is already per-mille. The
 * eight are the ones these rules can act on; tools/templeos_data.py records
 * why the other twelve cannot, and fails the build if a new one appears.
 */
#define RL_POPGROWTH  0
#define RL_PASSIVE    1
#define RL_RESOURCE   2
#define RL_ATK        3
#define RL_DEF        4
#define RL_CONSCRIPT  5
#define RL_CONSCOST   6
#define RL_MAINT      7
#define RL_NAVYATK    8
#define RL_NAVYDEF    9
#define RL_NAVYCOST  10
#define RL_NAVYSPEED 11

/* ── KEEPING AN ARMY COSTS SOMETHING ──
 *
 * It did not, until the policy levers arrived and one of them modified a
 * maintenance cost that was not being charged. The desktop game bills fuel
 * and upkeep every turn (Game_Economy.cpp); these rules had men who ate
 * nothing, so an army once raised was free for ever and there was no reason
 * not to raise the maximum every turn. One tenth of a dollar per 20,000 men.
 */
#define R_UPKEEP_PER_MAN  20000

/* ── THE FLEET ──
 *
 * A hull is worth what its class is worth, scaled by how much of it is left.
 * Ships act from the harbour they sit in: they bombard what is next to it and
 * they stiffen its defence. There is no sea to move across in these rules,
 * which is why navySpeedPct is recorded as having nothing to act on rather
 * than quietly multiplying something unrelated.
 */
#define R_SHIP_UPKEEP      2       /* tenths of a dollar, per hull per turn */
#define R_SHIP_BUILD      200      /* tenths, before the harbour and levers */
I64 r_shipstr[7];                  /* by class; filled by RInit */
I64 r_shipcap[7];                  /* men a hull can carry */
I64 r_shipcost[7];                 /* tenths, before harbour and levers */
I64 r_fund = 0;                    /* percent of income diverted to research */
I64 r_shipspeed[7];                /* cells of sea, per turn */

/* ── HOW FAR A FLEET CAN SAIL ──
 *
 * Breadth-first across navigable cells, marking every harbour reached within
 * range. The grid is 2,048 cells, so this is small enough to run whenever
 * somebody asks where a fleet could go -- which is the only time it runs.
 */
#define R_SEA_MAX   4096
I64 r_seadist[R_SEA_MAX];          /* cells from the origin, -1 for unreached */
I64 r_seaq[R_SEA_MAX];

I64 r_ord_recruit[OD_MAXP];         /* men to raise here this turn */
I64 r_ord_attack[OD_MAXP];          /* province index to assault, or -1 */
I64 r_ord_bombard[OD_MAXP];         /* province index to shell from here, or -1 */
I64 r_last_report = 0;              /* battles resolved in the last turn */
U8  r_log[8][80];                   /* what happened, for the panel */
I64 r_nlog = 0;

U0 RLog(U8 *fmt, ...)
{
  U8 *s;
  I64 i;
  s = StrPrintJoin(NULL, fmt, argc, argv);
  if (r_nlog < 8)
  {
    /* Copied by hand and bounded: there is no StrNCpy here, and a StrCpy into
     * a fixed buffer is how a log line becomes a memory corruption. */
    for (i = 0; i < 79 && s[i]; i++)
      r_log[r_nlog][i] = s[i];
    r_log[r_nlog][i] = 0;
    r_nlog++;
  }
  Free(s);
}

/* The player's active policies, summed. Only the player has policies in this
 * build, so an AI country's levers are all zero and every formula below
 * reduces to what it was. */
I64 RLever(I64 cid, I64 which)
{
  I64 i, n = 0;
  if (cid != w_me) return 0;
  for (i = 0; i < d_npol; i++)
    if (d_active[i]) n += d_plev[i][which];
  return n;
}

/* v scaled by a percentage lever given in tenths of a percent. */
I64 RScale(I64 v, I64 tenths)
{
  I64 r = v * (1000 + tenths) / 1000;
  if (r < 0) r = 0;
  return r;
}

U0 RClearOrders()
{
  I64 i;
  for (i = 0; i < w_np; i++)
  {
    r_ord_recruit[i] = 0;
    r_ord_attack[i] = -1;
    r_ord_bombard[i] = -1;
  }
}

/* What a country's ships in one province are worth, together. */
I64 RFleetAt(I64 ix, I64 cid)
{
  I64 i, n = 0;
  for (i = 0; i < w_ns; i++)
    if (w_sprov[i] == ix && w_scid[i] == cid && w_shp[i] > 0)
      n += r_shipstr[w_skind[i]] * w_shp[i] / 100;
  return n;
}

/* Fill r_seadist from a starting cell, stopping at `range`. */
U0 RSeaFlood(I64 from, I64 range)
{
  I64 head = 0, tail = 0, c, cx_, cy_, nx, ny, k, i, cells;

  cells = w_gw * w_gh;
  if (cells > R_SEA_MAX) cells = R_SEA_MAX;
  for (i = 0; i < cells; i++) r_seadist[i] = -1;
  if (from < 0 || from >= cells || !w_nav[from]) return;

  r_seadist[from] = 0;
  r_seaq[tail] = from;
  tail++;
  while (head < tail)
  {
    c = r_seaq[head];
    head++;
    if (r_seadist[c] < range)
    {
      cy_ = c / w_gw;
      cx_ = c - cy_ * w_gw;
      for (i = 0; i < 4; i++)
      {
        nx = cx_;
        ny = cy_;
        if (i == 0) nx++;
        if (i == 1) nx--;
        if (i == 2) ny++;
        if (i == 3) ny--;
        /* The map wraps east to west: a fleet leaving the Pacific on one
         * edge arrives on the other, which is the same ocean. */
        if (nx < 0) nx = w_gw - 1;
        if (nx >= w_gw) nx = 0;
        if (ny >= 0 && ny < w_gh)
        {
          k = ny * w_gw + nx;
          if (w_nav[k] && r_seadist[k] < 0 && tail < R_SEA_MAX)
          {
            r_seadist[k] = r_seadist[c] + 1;
            r_seaq[tail] = k;
            tail++;
          }
        }
      }
    }
  }
}

/* The fleet's range out of a harbour: the slowest hull in it sets the pace. */
I64 RFleetRange(I64 ix, I64 cid)
{
  I64 i, r = -1;
  for (i = 0; i < w_ns; i++)
    if (w_sprov[i] == ix && w_scid[i] == cid && w_shp[i] > 0)
      if (r < 0 || r_shipspeed[w_skind[i]] < r) r = r_shipspeed[w_skind[i]];
  if (r < 0) return 0;
  return RScale(r, RLever(cid, RL_NAVYSPEED));
}

/* Can this fleet reach that harbour? Call RSeaFlood first. */
Bool RSeaReaches(I64 to)
{
  if (w_pcell[to] < 0) return FALSE;
  return r_seadist[w_pcell[to]] >= 0;
}

/* ── WHAT A FLEET IS CARRYING ──
 *
 * Troops ride in the hulls, not in the province: a landing is the cargo
 * going ashore, and a fleet sunk at sea takes what it was carrying with it.
 */
I64 RCargoAt(I64 ix, I64 cid)
{
  I64 i, n = 0;
  for (i = 0; i < w_ns; i++)
    if (w_sprov[i] == ix && w_scid[i] == cid && w_shp[i] > 0)
      n += w_scargo[i];
  return n;
}

I64 RCapacityAt(I64 ix, I64 cid)
{
  I64 i, n = 0;
  for (i = 0; i < w_ns; i++)
    if (w_sprov[i] == ix && w_scid[i] == cid && w_shp[i] > 0)
      n += r_shipcap[w_skind[i]] - w_scargo[i];
  return n;
}

/* Load men from the province the fleet is sitting in. */
I64 REmbark(I64 ix, I64 cid, I64 men)
{
  I64 i, room, took = 0;
  for (i = 0; i < w_ns && men > 0; i++)
    if (w_sprov[i] == ix && w_scid[i] == cid && w_shp[i] > 0)
    {
      room = r_shipcap[w_skind[i]] - w_scargo[i];
      if (room > men) room = men;
      w_scargo[i] += room;
      men -= room;
      took += room;
    }
  w_army[ix] -= took;
  if (w_army[ix] < 0) w_army[ix] = 0;
  return took;
}

/* Put everything ashore, and say how many went. */
I64 RDisembark(I64 ix, I64 cid)
{
  I64 i, n = 0;
  for (i = 0; i < w_ns; i++)
    if (w_sprov[i] == ix && w_scid[i] == cid && w_shp[i] > 0)
    {
      n += w_scargo[i];
      w_scargo[i] = 0;
    }
  return n;
}

/* ── FLEET AGAINST FLEET ──
 *
 * Strength against strength, with the loser's hulls damaged in proportion.
 * No frontage here: the sea has no flanks in these rules, so a bigger fleet
 * simply wins and pays for it.
 */
Bool REngage(I64 from, I64 to, I64 cid)
{
  I64 i, mine, theirs, defcid, dmg;

  defcid = w_own[to];
  mine = RScale(RFleetAt(from, cid), RLever(cid, RL_NAVYATK));
  theirs = RScale(RFleetAt(to, defcid), RLever(defcid, RL_NAVYDEF));
  if (mine <= 0 || theirs <= 0) return FALSE;

  if (mine > theirs)
  {
    dmg = theirs * 100 / mine;          /* what it cost to win, as a share */
    for (i = 0; i < w_ns; i++)
    {
      if (w_sprov[i] == to && w_scid[i] == defcid) w_shp[i] = 0;
      else if (w_sprov[i] == from && w_scid[i] == cid)
      {
        w_shp[i] -= dmg;
        if (w_shp[i] < 0) w_shp[i] = 0;
      }
    }
    return TRUE;
  }

  dmg = mine * 100 / theirs;
  for (i = 0; i < w_ns; i++)
  {
    if (w_sprov[i] == from && w_scid[i] == cid) w_shp[i] = 0;
    else if (w_sprov[i] == to && w_scid[i] == defcid)
    {
      w_shp[i] -= dmg;
      if (w_shp[i] < 0) w_shp[i] = 0;
    }
  }
  return FALSE;
}

/* ── DIPLOMACY ──
 *
 * One enemy at a time, and pacts that are simply recorded: these rules have
 * no negotiation, so a pact is a statement about who will not be attacked
 * rather than an agreement somebody could refuse. */
U0 RDeclareWar(I64 a, I64 b)
{
  I64 sa = WSlot(a);
  if (sa < 0) return;
  r_enemy[sa] = b;
  r_wartime[sa] = 0;
}

U0 RCeasefire(I64 a)
{
  I64 sa = WSlot(a), sb;
  if (sa < 0) return;
  sb = WSlot(r_enemy[sa]);
  r_enemy[sa] = -1;
  r_wartime[sa] = 0;
  if (sb >= 0 && r_enemy[sb] == a)
  {
    r_enemy[sb] = -1;
    r_wartime[sb] = 0;
  }
}

/* Pacts, by kind: 1 alliance, 2 non-aggression, 3 guarantee, 4 trade. */
#define R_MAXPACT 64
I64 r_pact_a[R_MAXPACT], r_pact_b[R_MAXPACT], r_pact_k[R_MAXPACT];
I64 r_npact = 0;

I64 RPactWith(I64 a, I64 b)
{
  I64 i;
  for (i = 0; i < r_npact; i++)
    if ((r_pact_a[i] == a && r_pact_b[i] == b) ||
        (r_pact_a[i] == b && r_pact_b[i] == a))
      return r_pact_k[i];
  return 0;
}

Bool RPact(I64 a, I64 b, I64 kind)
{
  I64 i;
  for (i = 0; i < r_npact; i++)
    if ((r_pact_a[i] == a && r_pact_b[i] == b) ||
        (r_pact_a[i] == b && r_pact_b[i] == a))
    {
      r_pact_k[i] = kind;
      return TRUE;
    }
  if (r_npact >= R_MAXPACT) return FALSE;
  r_pact_a[r_npact] = a;
  r_pact_b[r_npact] = b;
  r_pact_k[r_npact] = kind;
  r_npact++;
  return TRUE;
}

U0 RMoveFleet(I64 from, I64 to, I64 cid)
{
  I64 i;
  for (i = 0; i < w_ns; i++)
    if (w_sprov[i] == from && w_scid[i] == cid && w_shp[i] > 0)
      w_sprov[i] = to;
}

I64 RFleetOf(I64 cid)
{
  I64 i, n = 0;
  for (i = 0; i < w_ns; i++)
    if (w_scid[i] == cid && w_shp[i] > 0) n++;
  return n;
}

U0 RInit()
{
  I64 i;

  r_shipstr[0] = 1;    /* boat       */
  r_shipstr[1] = 4;    /* destroyer  */
  r_shipstr[2] = 6;    /* cruiser    */
  r_shipstr[3] = 5;    /* submarine  */
  r_shipstr[4] = 10;   /* carrier    */
  r_shipstr[5] = 8;    /* battleship */
  r_shipstr[6] = 3;    /* frigate    */

  /* Smaller hulls move further; a carrier group is not a torpedo boat. */
  r_shipspeed[0] = 8;  /* boat       */
  r_shipspeed[1] = 7;  /* destroyer  */
  r_shipspeed[2] = 6;  /* cruiser    */
  r_shipspeed[3] = 7;  /* submarine  */
  r_shipspeed[4] = 5;  /* carrier    */
  r_shipspeed[5] = 5;  /* battleship */
  r_shipspeed[6] = 7;  /* frigate    */

  /* What a hull carries, and what it costs. A carrier is a floating airfield
   * rather than a troopship, but it is the biggest thing here and moving an
   * army needs the biggest thing here. */
  r_shipcap[0] = 5000;    r_shipcost[0] = 60;
  r_shipcap[1] = 12000;   r_shipcost[1] = 200;
  r_shipcap[2] = 20000;   r_shipcost[2] = 320;
  r_shipcap[3] = 2000;    r_shipcost[3] = 240;
  r_shipcap[4] = 50000;   r_shipcost[4] = 600;
  r_shipcap[5] = 30000;   r_shipcost[5] = 480;
  r_shipcap[6] = 8000;    r_shipcost[6] = 140;
  for (i = 0; i < OD_MAXC; i++)
  {
    r_enemy[i] = -1;
    r_wartime[i] = 0;
  }
  r_npact = 0;
  r_fund = 0;
  RClearOrders;
  w_turn = 0;
}

Bool RAtWar(I64 a, I64 b)
{
  I64 sa = WSlot(a), sb = WSlot(b);
  if (sa < 0 || sb < 0) return FALSE;
  /* A pact is worth something: an alliance or a non-aggression pact keeps a
   * border quiet, which is the only thing these rules can offer for signing
   * one. Declaring war regardless is allowed, and tears it up. */
  if (RPactWith(a, b) == 1 || RPactWith(a, b) == 2) return FALSE;
  return r_enemy[sa] == b || r_enemy[sb] == a;
}

/* Total men a country has, for deciding whether a war is worth starting. */
I64 RStrength(I64 cid)
{
  I64 i, n = 0;
  for (i = 0; i < w_np; i++)
    if (w_own[i] == cid) n += w_army[i];
  return n;
}

/* ── THE FRONTAGE ──
 *
 * How many men can be on the line at once. Everything else about a battle is
 * arithmetic; this is the rule with an opinion in it. */
I64 RWidth(I64 ix)
{
  I64 w, narrow;
  w = w_area[ix] * R_WIDTH_PER_AREA;
  if (w < R_WIDTH_MIN) w = R_WIDTH_MIN;
  if (w_fort[ix] > 0)
  {
    narrow = 100 - w_fort[ix] * R_FORT_NARROW_PCT;
    if (narrow < 25) narrow = 25;      /* a fort narrows, it does not seal */
    w = w * narrow / 100;
  }
  if (w < 1) w = 1;
  return w;
}

/* One assault. Returns TRUE if the province changed hands.
 *
 * Losses are the force each side actually overcame, so a defence that holds
 * costs the attacker the whole engaged assault and the defender only what the
 * fort did not absorb. */
Bool RAssault(I64 from, I64 to, I64 men)
{
  I64 wid, eatk, edef, def, fortmul, lossA, lossD;

  if (from < 0 || to < 0 || men <= 0) return FALSE;

  wid = RWidth(to);
  eatk = men;
  if (eatk > wid) eatk = wid;
  edef = w_army[to];
  if (edef > wid) edef = wid;

  fortmul = 100 + w_fort[to] * R_FORT_DEF_PCT;
  def = edef * fortmul / 100;

  /* Policy, applied to the men on the line rather than to the headcount: a
   * doctrine makes a front hit harder, it does not conjure more soldiers past
   * the frontage. */
  eatk = RScale(eatk, RLever(w_own[from], RL_ATK));
  def  = RScale(def,  RLever(w_own[to],   RL_DEF));

  /* A fleet in the harbour fights for it. Worth men rather than a multiplier,
   * so a squadron helps a small garrison and barely shows beside a large one
   * -- which is what a handful of hulls should do to a land battle. */
  def += RScale(RFleetAt(to, w_own[to]) * 2000, RLever(w_own[to], RL_NAVYDEF));

  if (eatk > def)
  {
    /* The field is carried: the assault costs what it overcame, and whatever
     * stood there is gone. */
    lossA = def;
    w_army[from] -= men;
    if (w_army[from] < 0) w_army[from] = 0;
    w_army[to] = men - lossA;
    if (w_army[to] < 0) w_army[to] = 0;
    w_own[to] = w_own[from];
    return TRUE;
  }

  lossA = eatk;
  lossD = eatk * 100 / fortmul;
  w_army[from] -= lossA;
  if (w_army[from] < 0) w_army[from] = 0;
  w_army[to] -= lossD;
  if (w_army[to] < 0) w_army[to] = 0;
  return FALSE;
}

/* ── THE OPPONENT ──
 *
 * Deliberately legible rather than clever: raise men where there are people
 * and money, and attack a neighbour you heavily outnumber. It plays a
 * recognisable game of expansion and it can be read in one sitting, which for
 * a rule set nobody can attach a debugger to is worth more than strength. */
U0 RAiTurn(I64 cid)
{
  I64 i, k, best, bestpop, tgt, slot, cost, men, nb, mine, theirs, cand;

  slot = WSlot(cid);
  if (slot < 0) return;

  /* ── WAR AND PEACE ──
   *
   * A country at peace occasionally picks a neighbour it outnumbers and
   * declares. A war that has run long enough ends, whatever its state: without
   * that, the first war a country starts is the last thing that ever happens
   * on that border. */
  if (r_enemy[slot] >= 0)
  {
    r_wartime[slot]++;
    if (r_wartime[slot] > R_WAR_MAX_TURNS)
    {
      r_enemy[slot] = -1;
      r_wartime[slot] = 0;
    }
  }
  else if (!(RandU32 % R_WAR_ODDS))
  {
    mine = RStrength(cid);
    cand = -1;
    for (i = 0; i < w_np && cand < 0; i++)
      if (w_own[i] == cid)
        for (k = 0; k < w_adjn[i]; k++)
        {
          nb = WNeighbour(i, k);
          if (nb >= 0 && w_own[nb] != cid)
          {
            theirs = RStrength(w_own[nb]);
            if (mine > theirs * 3 / 2)
            {
              cand = w_own[nb];
              k = w_adjn[i];
            }
          }
        }
    if (cand >= 0)
    {
      r_enemy[slot] = cand;
      r_wartime[slot] = 0;
    }
  }

  /* Raise men in the most populous province that can pay for them. */
  best = -1;
  bestpop = 0;
  for (i = 0; i < w_np; i++)
    if (w_own[i] == cid && w_pop[i] > bestpop)
    {
      best = i;
      bestpop = w_pop[i];
    }
  if (best >= 0)
  {
    men = w_pop[best] * R_RECRUIT_CAP_PCT / 100;
    if (men > 200000) men = 200000;
    cost = men / 1000;                 /* tenths of a dollar */
    if (cost > 0 && w_tre[slot] >= cost)
    {
      w_tre[slot] -= cost;
      w_army[best] += men;
    }
  }

  /* Attack a neighbour that is outnumbered enough to be worth it. */
  for (i = 0; i < w_np; i++)
    if (w_own[i] == cid && w_army[i] > 0)
      for (k = 0; k < w_adjn[i]; k++)
      {
        nb = WNeighbour(i, k);
        if (nb >= 0 && w_own[nb] != cid && RAtWar(cid, w_own[nb]))
        {
          tgt = w_army[nb] * R_AI_ODDS;
          if (tgt < 1) tgt = 1;
          if (w_army[i] > tgt && w_army[i] > 10000)
          {
            if (RAssault(i, nb, w_army[i]))
              r_last_report++;
            k = w_adjn[i];             /* one assault per province per turn */
          }
        }
      }
}

/* ── A TURN ──
 *
 * Income, then the player's orders, then everybody else, then the people. The
 * order matters: orders are paid for out of income that has already landed, and
 * an assault resolves before the province it targets can grow.
 */
U0 RTurn()
{
  I64 i, s, men, cost, cap;

  r_nlog = 0;
  r_last_report = 0;

  /* income, scaled by whatever the owner's policies do to it */
  for (i = 0; i < w_np; i++)
  {
    s = WSlot(w_own[i]);
    if (s >= 0)
      w_tre[s] += RScale(w_inc[i], RLever(w_own[i], RL_RESOURCE));
  }

  /* passive income, and what the standing army costs to keep */
  for (i = 0; i < w_nc; i++)
  {
    men = 0;
    for (s = 0; s < w_np; s++)
      if (w_own[s] == w_ccid[i]) men += w_army[s];
    cost = RScale(men / R_UPKEEP_PER_MAN + RFleetOf(w_ccid[i]) * R_SHIP_UPKEEP,
                  RLever(w_ccid[i], RL_MAINT));
    w_tre[i] -= cost;
    w_tre[i] += RLever(w_ccid[i], RL_PASSIVE);
    if (w_tre[i] < 0) w_tre[i] = 0;
  }

  /* the player's recruitment */
  for (i = 0; i < w_np; i++)
    if (r_ord_recruit[i] > 0 && w_own[i] == w_me)
    {
      men = r_ord_recruit[i];
      cap = RScale(w_pop[i] * R_RECRUIT_CAP_PCT / 100,
                   RLever(w_me, RL_CONSCRIPT));
      if (men > cap) men = cap;
      cost = RScale(men / 1000, RLever(w_me, RL_CONSCOST));
      s = WSlot(w_me);
      if (s >= 0 && cost > 0 && w_tre[s] >= cost)
      {
        w_tre[s] -= cost;
        w_army[i] += men;
        RLog("raised %d men in %s", men, w_name[i]);
      }
    }

  /* ── BOMBARDMENT ──
   *
   * Ships shell what is next to their harbour. It kills men and takes no
   * ground: a fleet cannot hold a province, and pretending otherwise would
   * make navies win land wars by themselves. */
  for (i = 0; i < w_np; i++)
    if (r_ord_bombard[i] >= 0 && w_own[i] == w_me)
    {
      s = r_ord_bombard[i];
      men = RScale(RFleetAt(i, w_me) * 1500, RLever(w_me, RL_NAVYATK));
      if (men > w_army[s]) men = w_army[s];
      w_army[s] -= men;
      RLog("shelled %s, %d dead", w_name[s], men);
    }

  /* the player's assaults */
  for (i = 0; i < w_np; i++)
    if (r_ord_attack[i] >= 0 && w_own[i] == w_me && w_army[i] > 0)
    {
      if (RAssault(i, r_ord_attack[i], w_army[i]))
        RLog("took %s from %s", w_name[r_ord_attack[i]],
             WIso(w_own[i]));
      else
        RLog("assault on %s failed, %d defenders left",
             w_name[r_ord_attack[i]], w_army[r_ord_attack[i]]);
    }

  /* everybody else */
  for (i = 0; i < w_nc; i++)
    if (w_ccid[i] != w_me)
      RAiTurn(w_ccid[i]);

  /* the people. popGrowthPct is a percentage, stored in tenths, which is the
   * same thing as per mille -- so it adds straight onto the base rate. */
  for (i = 0; i < w_np; i++)
  {
    cost = R_GROWTH_PER_MILLE + RLever(w_own[i], RL_POPGROWTH);
    if (cost < 0) cost = 0;
    w_pop[i] += w_pop[i] * cost / 1000;
  }

  RClearOrders;
  w_turn++;
}

/* What a country is worth now: provinces, men, and income per turn. */
U0 RCountryStats(I64 cid, I64 *prov, I64 *men, I64 *inc)
{
  I64 i;
  *prov = 0;
  *men = 0;
  *inc = 0;
  for (i = 0; i < w_np; i++)
    if (w_own[i] == cid)
    {
      *prov = *prov + 1;
      *men = *men + w_army[i];
      *inc = *inc + w_inc[i];
    }
}
