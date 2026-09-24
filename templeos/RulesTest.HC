/* The rules, run without a screen.
 *
 * A turn engine that only exists inside a UI is a turn engine nobody can check.
 * This plays the world headless and prints what moved, so the numbers can be
 * argued with before anything is drawn.
 */
#include "Rules"

U0 RulesTest(I64 turns = 8, U8 *iso = "SWE")
{
  I64 i, prov, men, inc, s, before, changed, t, d;
  U8 sign;

  if (!WLoad)
  {
    "$$FG,RED$$cannot load world.odw$$FG$$\n";
    return;
  }
  "loaded %d provinces, %d countries, %dx%d\n", w_np, w_nc, w_w, w_h;

  w_me = -1;
  for (i = 0; i < w_nc; i++)
    if (!StrCmp(w_ciso[i], iso)) w_me = w_ccid[i];
  if (w_me < 0)
  {
    "$$FG,RED$$no country %s$$FG$$\n", iso;
    return;
  }
  RInit;

  s = WSlot(w_me);
  RCountryStats(w_me, &prov, &men, &inc);
  "$$FG,YELLOW$$%s (%s)$$FG$$ starts with %d provinces, %d men, "
  "%d.%d/turn, treasury %d.%d\n\n",
      w_cname[s], iso, prov, men, inc / 10, inc % 10,
      w_tre[s] / 10, w_tre[s] % 10;

  /* A standing order, so the player is not merely a spectator: raise men in
   * every province each turn, and let the opponent do what it does. */
  for (t = 0; t < turns; t++)
  {
    before = 0;
    for (i = 0; i < w_np; i++)
      if (w_own[i] == w_me) before++;

    for (i = 0; i < w_np; i++)
      if (w_own[i] == w_me)
        r_ord_recruit[i] = w_pop[i] / 100;

    RTurn;

    changed = 0;
    for (i = 0; i < w_np; i++)
      if (w_own[i] == w_me) changed++;
    RCountryStats(w_me, &prov, &men, &inc);
    /* The sign by hand: HolyC's printf has no "%+d", and an unsupported
     * conversion does not just print oddly -- it consumes no argument, so
     * every value after it lands in the wrong column. The first run of this
     * printed "(d)" and then a treasury where the men should have been. */
    sign = ' ';
    if (changed > before) sign = '+';
    if (changed < before) sign = '-';
    d = changed - before;
    if (d < 0) d = -d;
    "turn %2d: %3d provinces (%c%d)  %9d men  treasury %6d.%d  "
    "%d battles world-wide\n",
        w_turn, prov, sign, d, men,
        w_tre[s] / 10, w_tre[s] % 10, r_last_report;
  }
  "\n";
  for (i = 0; i < r_nlog; i++)
    "  %s\n", r_log[i];
}
