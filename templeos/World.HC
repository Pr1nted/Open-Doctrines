#ifndef OD_WORLD_HC
#define OD_WORLD_HC

/* The world, in memory: provinces, who owns them, and what they are worth.
 *
 * Loaded from world.odw, which tools/templeos_world.py bakes out of an .odmap.
 * Nothing here talks to a host -- once this file is read the machine has
 * everything it needs to play, which is the whole point of it existing.
 *
 * The raster is province INDICES, one per pixel, run-length encoded. Colour is
 * decided at draw time from who owns the province now, so a province that
 * changes hands repaints and a click is answered exactly rather than by
 * guessing the nearest centre.
 */

#define OD_WORLD_VER  8
/* Sized to the world, not to a round number: eleven arrays of this length
 * live in the global segment, and at 4096 they were 440 KB before Rules.HC
 * and the renderer added theirs. */
#define OD_MAXP    2048
#define OD_MAXC     256
#define OD_SEA   0xFFFF        /* the index meaning "no province" */
#define OD_HDR      30
#define OD_PREC     50         /* bytes per province record */
#define OD_CREC     34         /* bytes per country record  */

U16 *w_pmap = NULL;            /* province index per pixel */
U16 *w_adj  = NULL;            /* flattened adjacency lists */
I64  w_w = 0, w_h = 0, w_np = 0, w_nc = 0, w_nadj = 0;

I64  w_pid[OD_MAXP],  w_cx[OD_MAXP],   w_cy[OD_MAXP];
I64  w_own[OD_MAXP],  w_pop[OD_MAXP],  w_army[OD_MAXP];
I64  w_inc[OD_MAXP],  w_fort[OD_MAXP], w_area[OD_MAXP];
I64  w_adjn[OD_MAXP], w_adjo[OD_MAXP];
I64  w_ind[OD_MAXP],  w_res[OD_MAXP];   /* industry level, resource bits */
I64  w_port[OD_MAXP];                   /* harbour level, 0 for inland */

/* ── THE FLEETS ──
 *
 * Hulls, not a naval map: each one sits in a port province and acts from
 * there. ships.json gives latitude and longitude, and the baker resolves
 * those to the owner's nearest harbour rather than trusting a projection. */
#define OD_MAXS    256
I64  w_ns = 0;
I64  w_scid[OD_MAXS], w_skind[OD_MAXS], w_shp[OD_MAXS], w_sprov[OD_MAXS];
I64  w_scargo[OD_MAXS];                 /* men aboard */
U8  *w_kindname[7];

/* ── THE SEA ──
 *
 * A coarse grid of the water, because the province graph is a LAND graph:
 * two harbours on the same ocean are usually not neighbours in it at all, so
 * it can neither route a fleet nor say how far it went. Cells are sixteen
 * pixels and count as navigable when at least half sea, which closes Panama
 * -- two pixels wide here -- and leaves real straits open. */
I64  w_gw = 0, w_gh = 0;
U8  *w_nav = NULL;                      /* one byte per cell */
I64  w_pcell[OD_MAXP];                  /* a province's harbour cell, or -1 */
U8   w_name[OD_MAXP][16];

I64  w_ccid[OD_MAXC], w_ccol[OD_MAXC], w_tre[OD_MAXC];
U8   w_ciso[OD_MAXC][4], w_cname[OD_MAXC][20];
U8   w_cr[OD_MAXC], w_cg[OD_MAXC], w_cb[OD_MAXC];

I64  w_turn = 0;
I64  w_me = -1;                /* the player's country id */

U0 WInitKinds()
{
  w_kindname[0] = "boat";
  w_kindname[1] = "destroyer";
  w_kindname[2] = "cruiser";
  w_kindname[3] = "submarine";
  w_kindname[4] = "carrier";
  w_kindname[5] = "battleship";
  w_kindname[6] = "frigate";
}

I64 WU16(U8 *b) { return b[0] + (b[1] << 8); }
I64 WU32(U8 *b) { return b[0] + (b[1] << 8) + (b[2] << 16) + (b[3] << 24); }

/* Country id to slot. Linear over 180 entries, called often enough that it is
 * worth saying it is deliberate: a hash would be faster and this is not where
 * the time goes. */
I64 WSlot(I64 cid)
{
  I64 i;
  for (i = 0; i < w_nc; i++)
    if (w_ccid[i] == cid) return i;
  return -1;
}

U8 *WIso(I64 cid)
{
  I64 s = WSlot(cid);
  if (s < 0) return "--";
  return w_ciso[s];
}

U8 *WCName(I64 cid)
{
  I64 s = WSlot(cid);
  if (s < 0) return "nobody";
  return w_cname[s];
}

/* A file whose name may have been shouted at: a lower-case 8.3 name written
 * elsewhere comes back upper case, because this OS's FAT driver does not read
 * the "was lower case" flag. */
U8 *WReadEither(U8 *path, I64 *size)
{
  U8 *buf, *slash, upper[64];
  I64 i, cut;

  buf = FileRead(path, size);
  if (buf) return buf;
  slash = StrLastOcc(path, "/");
  cut = 0;
  if (slash) cut = slash - path + 1;
  if (StrLen(path) >= 64) return NULL;
  StrCpy(upper, path);
  for (i = cut; upper[i]; i++)
    if (upper[i] >= 'a' && upper[i] <= 'z') upper[i] -= 32;
  if (!StrCmp(upper, path)) return NULL;
  return FileRead(upper, size);
}

Bool WLoad(U8 *path = "world.odw")
{
  U8 *b;
  I64 size = 0, i, p, total, n, ix, off, runs, k;

  WInitKinds;
  b = WReadEither(path, &size);
  if (!b) return FALSE;
  if (b[0] != 'O' || b[1] != 'D' || b[2] != 'T' || b[3] != 'W')
  {
    "$$FG,RED$$%s is not a world file$$FG$$\n", path;
    Free(b);
    return FALSE;
  }
  /* ── THE VERSION, CHECKED ──
   *
   * Record sizes changed between versions and the header did not, so an older
   * file parses its header perfectly, reports the right province count, and
   * then reads every province and every country from the wrong offset. It
   * looks like a working load until nothing can find its own country. */
  if (WU16(b + 4) != OD_WORLD_VER)
  {
    "$$FG,RED$$%s is version %d, this wants %d -- re-bake it$$FG$$\n",
        path, WU16(b + 4), OD_WORLD_VER;
    Free(b);
    return FALSE;
  }
  w_w    = WU16(b + 6);
  w_h    = WU16(b + 8);
  w_np   = WU16(b + 10);
  w_nc   = WU16(b + 12);
  runs   = WU32(b + 14);
  w_nadj = WU32(b + 18);
  if (w_np > OD_MAXP) w_np = OD_MAXP;
  if (w_nc > OD_MAXC) w_nc = OD_MAXC;

  total = w_w * w_h;
  w_pmap = MAlloc(total * 2);
  i = OD_HDR;
  p = 0;
  while (p < total && i + 2 < size)
  {
    n  = b[i];
    ix = WU16(b + i + 1);
    i += 3;
    while (n > 0 && p < total)
    {
      w_pmap[p] = ix;
      p++;
      n--;
    }
  }

  off = OD_HDR + runs;
  for (p = 0; p < w_np; p++)
  {
    w_pid[p]  = WU32(b + off);
    w_cx[p]   = WU16(b + off + 4);
    w_cy[p]   = WU16(b + off + 6);
    w_own[p]  = WU16(b + off + 8);
    w_pop[p]  = WU32(b + off + 10);
    w_army[p] = WU32(b + off + 14);      /* 32-bit: garrisons reach millions */
    w_inc[p]  = WU16(b + off + 18);
    w_adjn[p] = b[off + 20];
    w_fort[p] = b[off + 21];
    w_adjo[p] = WU32(b + off + 22);
    w_area[p] = WU32(b + off + 26);
    w_ind[p]  = b[off + 30];
    w_res[p]  = b[off + 31];
    w_port[p] = b[off + 32];
    for (k = 0; k < 15; k++)
      w_name[p][k] = b[off + 34 + k];
    w_name[p][15] = 0;
    off += OD_PREC;
  }

  w_adj = MAlloc(w_nadj * 2 + 2);
  for (p = 0; p < w_nadj; p++)
  {
    w_adj[p] = WU16(b + off);
    off += 2;
  }

  for (p = 0; p < w_nc; p++)
  {
    w_ccid[p] = WU16(b + off);
    w_ccol[p] = b[off + 2];
    w_ciso[p][0] = b[off + 4];
    w_ciso[p][1] = b[off + 5];
    w_ciso[p][2] = b[off + 6];
    w_ciso[p][3] = 0;
    w_tre[p] = WU16(b + off + 8);
    w_cr[p] = b[off + 10];
    w_cg[p] = b[off + 11];
    w_cb[p] = b[off + 12];
    for (k = 0; k < 19; k++)
      w_cname[p][k] = b[off + 14 + k];
    w_cname[p][19] = 0;
    off += OD_CREC;
  }

  /* the sea grid and the fleets, after the countries */
  w_gw = WU16(b + 24);
  w_gh = WU16(b + 26);
  w_ns = WU16(b + 22);
  if (w_ns > OD_MAXS) w_ns = OD_MAXS;
  for (p = 0; p < w_ns; p++)
  {
    w_scid[p]  = WU16(b + off);
    w_skind[p] = b[off + 2];
    w_shp[p]   = b[off + 3];
    w_sprov[p] = WU16(b + off + 4);
    w_scargo[p] = 0;
    off += 6;
  }

  w_nav = MAlloc(w_gw * w_gh + 1);
  for (p = 0; p < w_gw * w_gh; p++)
    w_nav[p] = b[off + p];
  off += w_gw * w_gh;

  for (p = 0; p < w_np; p++) w_pcell[p] = -1;
  k = WU16(b + 28);
  for (p = 0; p < k; p++)
  {
    i = WU16(b + off);
    if (i >= 0 && i < w_np) w_pcell[i] = WU16(b + off + 2);
    off += 4;
  }

  Free(b);
  return TRUE;
}

/* Give back what WLoad took. Loading twice without this leaks a megabyte and
 * a half a time -- and, worse, loading a second time without reloading at all
 * would start the next game from the last one's ruins. */
U0 WFree()
{
  if (w_pmap) Free(w_pmap);
  if (w_adj) Free(w_adj);
  if (w_nav) Free(w_nav);
  w_pmap = NULL;
  w_adj = NULL;
  w_nav = NULL;
}

/* The province under a point, or -1 for sea. Exact: this reads the raster, it
 * does not measure distance to a centre. */
I64 WAt(I64 x, I64 y)
{
  I64 ix;
  if (x < 0 || y < 0 || x >= w_w || y >= w_h) return -1;
  ix = w_pmap[y * w_w + x];
  if (ix == OD_SEA) return -1;
  return ix;
}

I64 WNeighbour(I64 ix, I64 k)
{
  if (ix < 0 || ix >= w_np || k < 0 || k >= w_adjn[ix]) return -1;
  return w_adj[w_adjo[ix] + k];
}

#endif
