/* The tables the panels are made of: research, policies, claims.
 *
 * Baked by tools/templeos_data.py out of three different places -- the
 * research tree is C++ SOURCE (buildResearchNodes is eighty-six add() calls,
 * not a data file), policies and claims are JSON inside the .odmap. None of
 * that matters here; by the time it reaches this machine it is one file.
 *
 * tools/templeos_sync.py fails the build if the tree grows a node and this
 * table is not rebaked, which is the only way a missing technology would ever
 * be noticed.
 */
#ifndef OD_DATA_HC
#define OD_DATA_HC

#include "World"

#define DT_MAXR     128        /* research nodes */
#define DT_MAXPOL   128        /* policies       */
#define DT_MAXCL     64        /* countries with claims */
#define DT_MAXCLP   128        /* claimed provinces, total */
#define DT_DEPS       4
#define DT_RREC      48
#define DT_PREC      78

U8   d_rid[DT_MAXR][12], d_rname[DT_MAXR][24];
I64  d_rcost[DT_MAXR], d_rcat[DT_MAXR], d_rndep[DT_MAXR];
I64  d_rdep[DT_MAXR][DT_DEPS];
I64  d_nres = 0;

#define DT_NLEV      12        /* levers this build acts on */

U8   d_pid[DT_MAXPOL][16], d_pname[DT_MAXPOL][24], d_pcat[DT_MAXPOL][10];
I64  d_pcost[DT_MAXPOL], d_pturns[DT_MAXPOL];
I64  d_plev[DT_MAXPOL][DT_NLEV];   /* in TENTHS of the policy's own unit */
U8  *d_levname[DT_NLEV];
I64  d_npol = 0;

/* Claims arrive as province IDS because the world file's indices depend on the
 * resolution it was baked at, and two files that must be baked together to
 * agree is a coupling that breaks quietly later. Mapped to indices here. */
I64  d_clcid[DT_MAXCL], d_clfirst[DT_MAXCL], d_cln[DT_MAXCL];
I64  d_clprov[DT_MAXCLP];
I64  d_ncl = 0, d_nclp = 0;

U8  *d_rcatname[6];

/* ── PLAYER STATE ── */
I64  d_done[DT_MAXR];          /* researched */
I64  d_active[DT_MAXPOL];      /* enacted */
I64  d_rp = 0;                 /* research points */
I64  d_researching = -1;

I64 DU16(U8 *b) { return b[0] + (b[1] << 8); }
I64 DU32(U8 *b) { return b[0] + (b[1] << 8) + (b[2] << 16) + (b[3] << 24); }

U0 DInitCats()
{
  d_levname[0] = "population growth";
  d_levname[1] = "passive income";
  d_levname[2] = "resource income";
  d_levname[3] = "army attack";
  d_levname[4] = "army defence";
  d_levname[5] = "conscription";
  d_levname[6] = "conscription cost";
  d_levname[7] = "maintenance cost";
  d_levname[8] = "naval attack";
  d_levname[9] = "naval defence";
  d_levname[10] = "ship cost";
  d_levname[11] = "naval speed";
  d_rcatname[0] = "Buildings";
  d_rcatname[1] = "Army";
  d_rcatname[2] = "Formations";
  d_rcatname[3] = "Population";
  d_rcatname[4] = "Efficiency";
  d_rcatname[5] = "Misc";
}

/* The province index a claimed id refers to, or -1. Linear over 1,600 entries
 * done 88 times at load; not worth an index. */
I64 DProvByPid(I64 pid)
{
  I64 i;
  for (i = 0; i < w_np; i++)
    if (w_pid[i] == pid) return i;
  return -1;
}

Bool DLoad(U8 *path = "game.odd")
{
  U8 *b;
  I64 size = 0, i, k, off, nr, np, nc, cnt, pid, ix;

  DInitCats;
  b = WReadEither(path, &size);
  if (!b) return FALSE;
  if (b[0] != 'O' || b[1] != 'D' || b[2] != 'T' || b[3] != 'D')
  {
    Free(b);
    return FALSE;
  }
  nr = DU16(b + 6);
  np = DU16(b + 8);
  nc = DU16(b + 10);
  if (nr > DT_MAXR) nr = DT_MAXR;
  if (np > DT_MAXPOL) np = DT_MAXPOL;
  if (nc > DT_MAXCL) nc = DT_MAXCL;
  d_nres = nr;
  d_npol = np;

  off = 12;
  for (i = 0; i < nr; i++)
  {
    for (k = 0; k < 11; k++) d_rid[i][k] = b[off + k];
    d_rid[i][11] = 0;
    for (k = 0; k < 23; k++) d_rname[i][k] = b[off + 12 + k];
    d_rname[i][23] = 0;
    d_rcost[i] = DU16(b + off + 36);
    d_rcat[i]  = b[off + 38];
    d_rndep[i] = b[off + 39];
    for (k = 0; k < DT_DEPS; k++)
      d_rdep[i][k] = DU16(b + off + 40 + k * 2);
    d_done[i] = 0;
    off += DT_RREC;
  }

  for (i = 0; i < np; i++)
  {
    for (k = 0; k < 15; k++) d_pid[i][k] = b[off + k];
    d_pid[i][15] = 0;
    for (k = 0; k < 23; k++) d_pname[i][k] = b[off + 16 + k];
    d_pname[i][23] = 0;
    for (k = 0; k < 9; k++) d_pcat[i][k] = b[off + 40 + k];
    d_pcat[i][9] = 0;
    d_pcost[i]  = DU16(b + off + 50);
    d_pturns[i] = b[off + 52];
    d_active[i] = 0;
    /* Signed: half the levers are negative, and a policy that reads -25% as
     * +65511% would be quite a policy. */
    for (k = 0; k < DT_NLEV; k++)
    {
      d_plev[i][k] = DU16(b + off + 54 + k * 2);
      if (d_plev[i][k] > 32767) d_plev[i][k] -= 65536;
    }
    off += DT_PREC;
  }

  d_ncl = 0;
  d_nclp = 0;
  for (i = 0; i < nc; i++)
  {
    d_clcid[d_ncl] = DU16(b + off);
    cnt = DU16(b + off + 2);
    off += 4;
    d_clfirst[d_ncl] = d_nclp;
    d_cln[d_ncl] = 0;
    for (k = 0; k < cnt; k++)
    {
      pid = DU32(b + off);
      off += 4;
      ix = DProvByPid(pid);
      if (ix >= 0 && d_nclp < DT_MAXCLP)
      {
        d_clprov[d_nclp] = ix;
        d_nclp++;
        d_cln[d_ncl] = d_cln[d_ncl] + 1;
      }
    }
    d_ncl++;
  }

  Free(b);
  return TRUE;
}

/* A node can be started when everything it depends on is done. */
Bool DResAvailable(I64 i)
{
  I64 k;
  if (i < 0 || i >= d_nres || d_done[i]) return FALSE;
  for (k = 0; k < d_rndep[i]; k++)
    if (d_rdep[i][k] < d_nres && !d_done[d_rdep[i][k]]) return FALSE;
  return TRUE;
}

I64 DClaimsOf(I64 cid)
{
  I64 i;
  for (i = 0; i < d_ncl; i++)
    if (d_clcid[i] == cid) return i;
  return -1;
}

/* What a country earns towards research each turn.
 *
 * The desktop game's shape: a flat base plus the square root of what is put
 * in, so doubling the economy does not double the pace. Integer sqrt, because
 * this is a research tick and not a physics engine. */
I64 DResearchRate(I64 income_tenths)
{
  I64 v = income_tenths / 2, r = 0;
  while ((r + 1) * (r + 1) <= v) r++;
  return 1 + r;
}

#endif
