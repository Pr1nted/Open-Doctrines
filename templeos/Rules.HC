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

I64 r_ord_recruit[OD_MAXP];         /* men to raise here this turn */
I64 r_ord_attack[OD_MAXP];          /* province index to assault, or -1 */
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

U0 RClearOrders()
{
  I64 i;
  for (i = 0; i < w_np; i++)
  {
    r_ord_recruit[i] = 0;
    r_ord_attack[i] = -1;
  }
}

U0 RInit()
{
  I64 i;
  for (i = 0; i < OD_MAXC; i++)
  {
    r_enemy[i] = -1;
    r_wartime[i] = 0;
  }
  RClearOrders;
  w_turn = 0;
}

Bool RAtWar(I64 a, I64 b)
{
  I64 sa = WSlot(a), sb = WSlot(b);
  if (sa < 0 || sb < 0) return FALSE;
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
  I64 i, s, men, cost;

  r_nlog = 0;
  r_last_report = 0;

  /* income */
  for (i = 0; i < w_np; i++)
  {
    s = WSlot(w_own[i]);
    if (s >= 0) w_tre[s] += w_inc[i];
  }

  /* the player's recruitment */
  for (i = 0; i < w_np; i++)
    if (r_ord_recruit[i] > 0 && w_own[i] == w_me)
    {
      men = r_ord_recruit[i];
      if (men > w_pop[i] * R_RECRUIT_CAP_PCT / 100)
        men = w_pop[i] * R_RECRUIT_CAP_PCT / 100;
      cost = men / 1000;
      s = WSlot(w_me);
      if (s >= 0 && cost > 0 && w_tre[s] >= cost)
      {
        w_tre[s] -= cost;
        w_army[i] += men;
        RLog("raised %d men in %s", men, w_name[i]);
      }
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

  /* the people */
  for (i = 0; i < w_np; i++)
    w_pop[i] += w_pop[i] * R_GROWTH_PER_MILLE / 1000;

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
