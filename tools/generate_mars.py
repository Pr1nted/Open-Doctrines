#!/usr/bin/env python3
"""Build data/STDmaps/mars.odmap -- Mars with an ocean, and states worth playing.

    python3 tools/generate_mars.py            # download if needed, then build
    python3 tools/generate_mars.py --keep     # keep the working rasters

WHY THIS IS NOT A SCENARIO

tools/generate_scenario.py reassigns the Earth base map's provinces to different
owners; the geometry never changes, which is what lets saves and mods survive
across every era. Mars shares none of that. It is a new BASE map: its own
coastline, its own provinces, its own countries, and therefore its own copy of
every file the loader unpacks (see `needed[]` in src/Game_Loading.cpp).

MapGenerator cannot help either. It is built around Natural Earth shapefiles --
land polygons, admin boundaries, populated places -- and no such vector data
exists for a Mars that has an ocean, because that Mars does not exist. So the
geometry here is derived from a raster instead: real elevation, thresholded.

WHERE THE ELEVATION COMES FROM

MOLA MEGDR, the Mars Orbiter Laser Altimeter's gridded topography, 16 pixels
per degree, from the NASA Planetary Data System. NASA data is public domain,
which is the same standard every other input to this project's maps has to
meet (see NOTICE.md and tools/check_data_licences.py). The file is ~33 MB and
is NOT committed: this script fetches it, and only the derived map is shipped.

SEA LEVEL, AND WHY -1500 m

The datum is the areoid, so "sea level" is a design choice. Mars is bimodal --
the northern lowlands sit kilometres below the southern highlands -- so the
choice is really "how much of the north is ocean".

  -3600 m   18% water. A thin northern sea; barely a coastline.
  -2500 m   31% water.
  -1500 m   40% water.  <-- chosen
  -1000 m   44% water. The southern highlands start drowning and the map
                       loses the structure that makes it readable.

At -1500 the basins flood and the highlands do not, which is the whole of the
worldbuilding: every *Planitia* becomes a sea and keeps its name (Hellas,
Utopia, Chryse, Argyre, Isidis), and every *Terra* stays land and becomes a
country. Valles Marineris floods into an inland waterway a thousand km long.
Elysium becomes an island. None of that was arranged; it is what the real
topography does when you pour water on it.
"""

import io, json, os, sys, urllib.request, zipfile
from collections import Counter, deque

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "data", "STDmaps", "mars.odmap")
WORK = os.path.join(ROOT, "build", "mars")
MOLA_URL = ("https://pds-geosciences.wustl.edu/mgs/mgs-m-mola-5-megdr-l3-v1/"
            "mgsl_300x/meg016/megt90n000eb.img")
MOLA_SHAPE = (2880, 5760)

SEA_LEVEL = -1500
PART_W, PART_H = 4096, 2048        # the partition is computed here...
MAP_W, MAP_H   = 8192, 4096        # ...and published at the size Earth uses
N_PROVINCES = 1700
SEED = 19650714                    # Mariner 4 reached Mars on this date

# ── the states ──────────────────────────────────────────────────────
#
# Anchored to real Martian toponyms at their real coordinates, because that is
# the difference between a map of Mars and a map of nowhere. Longitudes are
# degrees EAST, the convention MOLA uses.
#
# Note which names survive: the basins are all underwater, so the countries are
# named for highlands (Terra), volcanic provinces (Tharsis, Elysium, Alba) and
# plateaus (Solis, Hesperia, Lunae) -- and the seas carry the basin names.
# REACH is how far a state's settlement spread before the borders met, and it
# is a design decision rather than an accident of where its capital sits.
#
# Grown at one uniform rate, the map came out backwards: the states this file
# calls great powers finished smallest, because eight of them are seeded within
# one band of longitude around Tharsis and hemmed each other in, while the
# southern Terrae had open ground and ran away with it. Tharsis Union -- the
# industrial heartland the whole map is arranged around -- placed tenth, and
# Xanthe finished on twelve provinces. Reach is the dial that fixes that: a
# state expands at a cost of 1/reach per province, so 2.0 reaches twice as far.
COUNTRIES = [
    # iso   name                      lon    lat   colour     left  auth  reach
    ("THA", "Tharsis Union",          247.0,   0.0, "#b4552f", -25,  35, 1.75),
    ("OLY", "Olympian League",        226.0,  18.6, "#c9793a",  40, -30, 1.55),
    ("ALB", "Alba Commune",           250.0,  40.0, "#8d6b45", -70,  10, 1.05),
    ("TMP", "Tempe Republic",         289.0,  40.0, "#5f8a6a",  10, -45, 1.15),
    ("LUN", "Lunae Directorate",      295.0,  10.0, "#7a6ba0", -30,  60, 1.30),
    ("XAN", "Xanthe League",          310.0,   5.0, "#4f86a8",  35, -20, 1.45),
    ("MRN", "Marineris Confederation",300.0, -13.0, "#3f7d8c",  15, -55, 1.55),
    ("NOC", "Noctis Technate",        265.0,  -7.0, "#6f5f9c", -15,  70, 1.35),
    ("SOL", "Solis Protectorate",     270.0, -26.0, "#a4623f",  20,  55, 1.05),
    ("THU", "Thaumasia",              295.0, -40.0, "#8a7a4a",   0,   5, 0.85),
    ("NER", "Nereidum Shore",         316.0, -50.0, "#4e7f7a",  25, -25, 0.85),
    ("NOA", "Noachis Terra",          350.0, -45.0, "#96684f", -10,  40, 0.90),
    ("MER", "Meridiani Compact",        0.0,  -2.0, "#b0894a",  45, -35, 1.15),
    ("SAB", "Terra Sabaea",            42.0,   2.0, "#7f8f4e", -20,  25, 1.00),
    ("SYR", "Syrtis Major",            70.0,   8.0, "#4b6b3f", -45,  50, 1.15),
    ("TYR", "Tyrrhena",                90.0, -12.0, "#9a5a5a",   5,  15, 1.00),
    ("HES", "Hesperia Concord",       110.0, -22.0, "#6a7fa0",  30, -40, 1.00),
    ("CIM", "Terra Cimmeria",         145.0, -35.0, "#8c5f7a", -35,  45, 0.85),
    ("ELY", "Elysium",                147.0,  25.0, "#c08a55",  50, -50, 1.00),
    ("MEM", "Memnonia",               200.0, -10.0, "#7d6a55", -5,   20, 1.00),
    ("SIR", "Terra Sirenum",          210.0, -40.0, "#5a7d92",  -55, 30, 0.80),
    ("AON", "Aonia Terra",            260.0, -60.0, "#6b8a76",   0,  -10, 0.70),
    ("PRM", "Promethei Terra",        100.0, -65.0, "#a07b5f",  -40, 65, 0.70),
    ("AUS", "Terra Australe",           0.0, -85.0, "#8fa0ad",  -60, 80, 0.60),
    # The southern highlands are one enormous open plain, and eight states was
    # not enough of them: whoever was seeded there ran to 148 provinces on a
    # low reach purely because nobody was in the way. These break that ground
    # up. All are real Martian regions, and all are above the waterline.
    ("HEL", "Hellespontus",            40.0, -50.0, "#7f7f9c",  -15,  30, 0.95),
    ("MAL", "Malea",                   55.0, -65.0, "#9c8f7f",  -30,  55, 0.80),
    ("IAP", "Iapygia",                 65.0, -15.0, "#6f8f7f",   10, -15, 1.00),
    ("AMN", "Amenthes",               110.0,   5.0, "#8f7f6f",   25, -30, 1.05),
    ("ERI", "Eridania",               235.0, -45.0, "#7f6f8f",  -45,  20, 0.90),
    ("DAE", "Daedalia",               235.0, -20.0, "#a08f6f",   15,   0, 1.00),
    ("ICA", "Icaria",                 253.0, -43.0, "#6f8fa0",  -20,  35, 0.85),
    ("CHR", "Terra Chronium",         140.0, -58.0, "#8f9c8f",  -35,  45, 0.80),
]

# Doctrines each state opens with. Checked against data/policies.json by
# tools/check_policies.py, which also refuses a pair that conflicts.
POLICIES = {
    "THA": ["state_industry", "rationalisation"],
    "OLY": ["deregulation", "free_trade", "free_press"],
    "ALB": ["collective_agriculture", "full_employment"],
    "TMP": ["free_press", "judicial_independence", "decentralization"],
    "LUN": ["censorship", "internal_passports"],
    "XAN": ["free_trade", "merchant_marine"],
    "MRN": ["free_press", "minority_rights", "decentralization"],
    "NOC": ["technocracy", "civil_service_reform"],
    "SOL": ["conscription", "national_unity"],
    "THU": ["infrastructure_programme"],
    "NER": ["merchant_marine", "free_trade"],
    "NOA": ["traditionalism", "agrarian_priority"],
    "MER": ["flat_tax", "foreign_investment"],
    "SAB": ["land_reform", "worker_rights"],
    "SYR": ["state_industry", "conscription"],
    "TYR": ["consumer_economy"],
    "HES": ["free_trade", "professional_army"],
    "CIM": ["national_unity", "secret_police"],
    "ELY": ["free_trade", "naval_supremacy", "free_press"],
    "MEM": ["decentralization"],
    "SIR": ["collective_agriculture", "worker_rights"],
    "AON": ["freedom_of_worship", "general_amnesty"],
    "PRM": ["autarky", "fortress_doctrine"],
    "AUS": ["autarky", "strategic_stockpile"],
    "HEL": ["merchant_marine", "free_trade"],
    "MAL": ["autarky", "traditionalism"],
    "IAP": ["free_trade", "consumer_economy"],
    "AMN": ["deregulation", "free_press"],
    "ERI": ["collective_agriculture", "conscription"],
    "DAE": ["rationalisation", "technocracy"],
    "ICA": ["agrarian_priority", "traditionalism"],
    "CHR": ["national_unity", "fortress_doctrine"],
}

# Where the settlers came from, which is what "ethnicity" means on a world
# nobody evolved on. Mars-born is the majority everywhere and rising.
FOUNDERS = ["Terran", "Lunar", "Cyclerborn", "Belt-Contract"]


# ── elevation ───────────────────────────────────────────────────────
def fetch_mola():
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, os.path.basename(MOLA_URL))
    if os.path.exists(path) and os.path.getsize(path) == MOLA_SHAPE[0]*MOLA_SHAPE[1]*2:
        return path
    print("  fetching MOLA topography (~33 MB, public domain, NASA PDS)")
    req = urllib.request.Request(MOLA_URL, headers={"User-Agent": "OpenDoctrines/mars"})
    with urllib.request.urlopen(req, timeout=300) as r, open(path, "wb") as f:
        f.write(r.read())
    return path


def load_elevation(path, w, h):
    raw = np.fromfile(path, dtype=">i2").reshape(MOLA_SHAPE).astype(np.float32)
    return np.array(Image.fromarray(raw).resize((w, h), Image.BILINEAR))


# ── provinces ───────────────────────────────────────────────────────
def shifted(a, ax, sh, fill):
    """Roll, wrapping in longitude and NOT over the poles."""
    s = np.roll(a, sh, axis=ax)
    if ax == 0:
        if sh == 1: s[0, :] = fill
        else:       s[-1, :] = fill
    return s


def partition(land, n_seeds, rng):
    h, w = land.shape
    lat = 90.0 - (np.arange(h) + 0.5) * (180.0 / h)
    wt = np.cos(np.radians(lat))[:, None] * land          # equal AREA, not equal pixels
    wt = wt / wt.sum()
    picks = rng.choice(h*w, size=min(n_seeds*4, int(land.sum())), replace=False, p=wt.ravel())
    rs, cs = np.divmod(picks, w)
    minsep = max(3, int(np.sqrt(land.sum()/n_seeds) * 0.55))
    grid, keep = set(), []
    for r, c in zip(rs, cs):
        k = (r//minsep, c//minsep)
        if k in grid: continue
        grid.add(k); keep.append((r, c))
        if len(keep) >= n_seeds: break
    lab = np.full((h, w), -1, np.int32)
    for i, (r, c) in enumerate(keep):
        lab[r, c] = i
    while True:
        todo = (lab < 0) & land
        if not todo.any(): break
        grew = False
        for ax, sh in ((1,1), (1,-1), (0,1), (0,-1)):
            src = shifted(lab, ax, sh, -1)
            m = todo & (src >= 0)
            if m.any():
                lab[m] = src[m]; todo &= ~m; grew = True
        if not grew: break
    return lab


def claim_islands(lab, land, min_island=60, per_province=1600):
    """Land no seed could reach: its own province, or too small to govern."""
    h, w = lab.shape
    nxt = int(lab.max()) + 1
    kept = dropped = 0
    todo = (lab < 0) & land
    while todo.any():
        r0, c0 = divmod(int(np.argmax(todo)), w)
        comp, q, seen = [], deque([(r0, c0)]), {(r0, c0)}
        while q:
            r, c = q.popleft(); comp.append((r, c))
            for dr, dc in ((1,0), (-1,0), (0,1), (0,-1)):
                rr, cc = r+dr, (c+dc) % w
                if 0 <= rr < h and (rr, cc) not in seen and todo[rr, cc]:
                    seen.add((rr, cc)); q.append((rr, cc))
        rr = np.array([p[0] for p in comp]); cc = np.array([p[1] for p in comp])
        if len(comp) >= min_island:
            n = max(1, len(comp)//per_province)
            step = len(comp)//n
            for i in range(n):
                sl = slice(i*step, (i+1)*step if i < n-1 else len(comp))
                lab[rr[sl], cc[sl]] = nxt; nxt += 1
            kept += 1
        else:
            land[rr, cc] = False; dropped += 1
        todo = (lab < 0) & land
    lab[~land] = -1
    return kept, dropped


def absorb_runts(lab, land, min_prov=350):
    """A province too small to click is merged into the neighbour it shares the
    longest border with -- or, having no neighbour at all, is an islet and goes
    back to the sea."""
    for _ in range(8):
        ids, counts = np.unique(lab[lab >= 0], return_counts=True)
        small = ids[counts < min_prov]
        if not len(small): break
        for pid in small:
            m = lab == pid
            if not m.any(): continue
            nb = Counter()
            for ax, sh in ((1,1), (1,-1), (0,1), (0,-1)):
                src = shifted(lab, ax, sh, -1)
                for v in src[m]:
                    if v >= 0 and v != pid: nb[int(v)] += 1
            if nb: lab[m] = max(nb.items(), key=lambda kv: kv[1])[0]
            else:  lab[m] = -1; land[m] = False
    ids = np.unique(lab[lab >= 0])
    remap = np.full(int(ids.max())+1, -1, np.int32)
    remap[ids] = np.arange(len(ids), dtype=np.int32)
    return np.where(lab >= 0, remap[np.clip(lab, 0, None)], -1).astype(np.int32)


# ── who owns what ───────────────────────────────────────────────────
def province_graph(lab, nprov):
    """Neighbour sets, so countries can be grown over land rather than drawn
    with a compass -- a nearest-centroid assignment hands a state provinces on
    the far side of a sea."""
    adj = [set() for _ in range(nprov)]
    for ax, sh in ((1,1), (0,1)):
        src = shifted(lab, ax, sh, -1)
        m = (lab >= 0) & (src >= 0) & (lab != src)
        for a, b in zip(lab[m], src[m]):
            adj[int(a)].add(int(b)); adj[int(b)].add(int(a))
    return adj


def province_centres(lab, nprov, h, w):
    lat = 90.0 - (np.arange(h) + 0.5) * (180.0/h)
    lon = (np.arange(w) + 0.5) * (360.0/w)
    rows, cols = np.indices(lab.shape)
    m = lab >= 0
    ids = lab[m]
    # circular mean over longitude, or a province on the dateline lands at 180E
    ang = np.radians(lon[cols[m]])
    sx = np.bincount(ids, np.cos(ang), nprov); sy = np.bincount(ids, np.sin(ang), nprov)
    cnt = np.bincount(ids, None, nprov).astype(np.float64)
    cnt[cnt == 0] = 1
    clon = (np.degrees(np.arctan2(sy/cnt, sx/cnt))) % 360.0
    clat = np.bincount(ids, lat[rows[m]], nprov) / cnt
    return clat, clon, np.bincount(ids, None, nprov)


def assign_countries(adj, clat, clon, nprov):
    """Grow every state outward from the province holding its real coordinates,
    all at the same rate, so borders fall where the expansions meet."""
    def nearest(lat0, lon0):
        d = ((clat - lat0)**2 +
             (np.minimum(np.abs(clon - lon0), 360 - np.abs(clon - lon0))
              * np.cos(np.radians(clat)))**2)
        return int(np.argmin(d))
    import heapq
    owner = np.full(nprov, -1, np.int32)
    cost = np.full(nprov, np.inf)
    heap = []
    for ci, c in enumerate(COUNTRIES):
        p = nearest(c[3], c[2])
        step = 1.0 / c[7]
        if cost[p] > 0.0:
            cost[p] = 0.0; heapq.heappush(heap, (0.0, p, ci, step))
    while heap:
        d, p, ci, step = heapq.heappop(heap)
        if owner[p] >= 0: continue
        owner[p] = ci
        for q in adj[p]:
            if owner[q] < 0 and d + step < cost[q]:
                cost[q] = d + step
                heapq.heappush(heap, (d + step, q, ci, step))
    return owner


# ── everything a province needs to be played ────────────────────────
def derive(lab, land, elev, owner, clat, clon, area_px, nprov, rng):
    h, w = lab.shape
    # coastal: touches sea
    sea_adj = np.zeros(nprov, bool)
    for ax, sh in ((1,1), (1,-1), (0,1), (0,-1)):
        src = shifted(lab, ax, sh, -1)
        m = (lab >= 0) & (src < 0)
        for v in np.unique(lab[m]): sea_adj[int(v)] = True

    mean_elev = np.bincount(lab[lab >= 0], elev[lab >= 0], nprov) / np.maximum(area_px, 1)

    # POPULATION. Thin air is the constraint nobody escapes: every 1000 m up is
    # roughly a tenth of the pressure gone, so people live low, warm and wet.
    height = np.clip(mean_elev - SEA_LEVEL, 0, None)
    wgt = (np.exp(-height / 5500.0)
           * np.clip(np.cos(np.radians(clat)), 0.05, 1) ** 1.4
           * np.where(sea_adj, 1.7, 1.0)
           * rng.uniform(0.55, 1.45, nprov))
    wgt = np.maximum(wgt, 1e-6)
    pop = np.maximum(1200, (wgt / wgt.sum() * 180_000_000).astype(np.int64))
    return sea_adj, mean_elev, pop


def build(keep_work=False):
    rng = np.random.default_rng(SEED)
    os.makedirs(WORK, exist_ok=True)
    print("=== mars: an ocean on real topography ===")

    elev_p = load_elevation(fetch_mola(), PART_W, PART_H)
    land = elev_p >= SEA_LEVEL
    lat_row = 90.0 - (np.arange(PART_H) + 0.5) * (180.0/PART_H)
    wcos = np.cos(np.radians(lat_row))[:, None] * np.ones((1, PART_W))
    print("  sea level %d m -> %.1f%% of the globe is water"
          % (SEA_LEVEL, wcos[~land].sum() / wcos.sum() * 100))

    lab = partition(land, N_PROVINCES, rng)
    kept, dropped = claim_islands(lab, land)
    print("  islands: %d given provinces, %d specks returned to the sea" % (kept, dropped))
    lab = absorb_runts(lab, land)
    nprov = int(lab.max()) + 1
    ids, counts = np.unique(lab[lab >= 0], return_counts=True)
    print("  provinces: %d (smallest %d px, median %d px)"
          % (nprov, counts.min(), int(np.median(counts))))

    adj = province_graph(lab, nprov)
    clat, clon, area_px = province_centres(lab, nprov, PART_H, PART_W)
    owner = assign_countries(adj, clat, clon, nprov)
    orphan = int((owner < 0).sum())
    if orphan:
        # An island no state's growth could reach: give it to the nearest one
        # that exists, because a province owned by nobody is not playable.
        for p in np.where(owner < 0)[0]:
            d = ((clat - clat[p])**2 +
                 (np.minimum(np.abs(clon - clon[p]), 360 - np.abs(clon - clon[p])))**2)
            d[owner < 0] = 1e18
            owner[p] = owner[int(np.argmin(d))]
        print("  %d unreachable island province(s) attached to their nearest state" % orphan)
    sea_adj, mean_elev, pop = derive(lab, land, elev_p, owner, clat, clon, area_px, nprov, rng)

    held = Counter(owner.tolist())
    print("  states: %d  (largest %s with %d provinces)"
          % (len(held), COUNTRIES[held.most_common(1)[0][0]][1], held.most_common(1)[0][1]))

    # ── rasters, published at the size the Earth maps use ──
    big_lab  = np.repeat(np.repeat(lab,  2, 0), 2, 1)
    big_land = np.repeat(np.repeat(land, 2, 0), 2, 1)
    pid = np.where(big_lab >= 0, big_lab + 1, 0).astype(np.uint32)   # 0 is the sea
    prov_png = np.stack([(pid >> 16) & 255, (pid >> 8) & 255, pid & 255], -1).astype(np.uint8)

    ls = np.where(big_land, 2, 0).astype(np.uint8)
    ls_img = Image.fromarray(ls, mode="P")
    ls_img.putpalette([0,0,0, 0,0,0, 255,255,255] + [0,0,0]*253)

    # The thumbnail is NOT drawn here; see finish_archive(). It is the map
    # browser's picture of who owns what, tools/rebuild_map_preview.py checks it
    # against province ownership, and the only way to agree with that check is
    # to call the same function it does.

    return dict(lab=lab, land=land, nprov=nprov, owner=owner, clat=clat, clon=clon,
                area_px=area_px, pop=pop, sea_adj=sea_adj, mean_elev=mean_elev,
                prov_png=prov_png, ls_img=ls_img, rng=rng)


def group_colour(name):
    h = 2166136261
    for ch in name.encode(): h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return [60 + (h & 127), 60 + ((h >> 8) & 127), 60 + ((h >> 16) & 127)]


def pack(b):
    lab, owner, nprov, pop = b["lab"], b["owner"], b["nprov"], b["pop"]
    clat, clon, rng = b["clat"], b["clon"], b["rng"]
    sea_adj, mean_elev, area_px = b["sea_adj"], b["mean_elev"], b["area_px"]
    iso_of  = [c[0] for c in COUNTRIES]
    name_of = [c[1] for c in COUNTRIES]
    cid_of  = {iso: i + 1 for i, iso in enumerate(iso_of)}

    provinces, population, compass, minorities, resources = {}, {}, {}, {}, {}
    ports, armies, mcolors = {}, {}, {}
    per_country = Counter()
    for p in range(nprov):
        ci  = int(owner[p]); iso = iso_of[ci]
        pid = p + 1
        per_country[ci] += 1
        provinces[str(pid)] = {
            "color": "#%06x" % pid, "country_id": cid_of[iso], "id": pid,
            "iso_a3": iso, "name": "%s #%d" % (name_of[ci], per_country[ci]),
        }
        population[str(pid)] = int(pop[p])
        left, auth = COUNTRIES[ci][5], COUNTRIES[ci][6]
        compass[str(pid)] = {"left": int(np.clip(left + rng.integers(-22, 23), -100, 100)),
                             "auth": int(np.clip(auth + rng.integers(-22, 23), -100, 100))}

        # WHERE PEOPLE CAME FROM, which is what ancestry means on a world nobody
        # evolved on. Mars-born everywhere and rising; the founder communities
        # are strongest where the settlement charters were.
        born = float(rng.uniform(54, 88))
        rest = 100.0 - born
        picks = list(rng.choice(FOUNDERS, size=int(rng.integers(2, 4)), replace=False))
        shares = rng.dirichlet(np.ones(len(picks))) * rest
        ent = [{"n": "Mars-born", "p": round(born, 1)}]
        for g, s in zip(picks, shares):
            if s >= 0.4: ent.append({"n": str(g), "p": round(float(s), 1)})
        minorities[str(pid)] = ent
        for e in ent: mcolors.setdefault(e["n"], group_colour(e["n"]))

        # RESOURCES. No oil and no rubber: both are the remains of a biosphere
        # and Mars never had one. Metal instead, everywhere -- the planet is
        # iron oxide to a first approximation -- and a little gold where the
        # highlands are old.
        h_km = max(0.0, (mean_elev[p] - SEA_LEVEL) / 1000.0)
        metal = float(np.clip(rng.normal(11 + 1.6 * h_km, 5), 0.4, 60))
        gold  = float(max(0.0, rng.normal(1.1 + 0.35 * h_km, 1.6)))
        gems  = float(max(0.0, rng.normal(0.25, 0.7)))
        lvl = int(np.clip(np.searchsorted([25_000, 90_000, 260_000, 700_000], pop[p]), 0, 4))
        resources[str(pid)] = {
            "oil": {"a": 0.0, "b": 0.0},
            "gold": {"a": round(gold, 1), "b": round(gold * 0.33, 1)},
            "rubber": {"a": 0.0, "b": 0.0},
            "gemstones": {"a": round(gems, 1), "b": round(gems * 0.3, 1)},
            "metal": {"a": round(metal, 1), "b": round(metal * 0.34, 1)},
            "industry": {"level": lvl, "income": round(0.4 * lvl, 1),
                         "specialization": "Metal", "resourceIncome": round(0.12 * lvl, 2),
                         "popIncome": 0.0, "popModifier": 1.02},
            "fortification": 0,
        }
        if sea_adj[p] and pop[p] > 40_000:
            ports[str(pid)] = {"level": 1 + int(pop[p] > 200_000) + int(pop[p] > 600_000)}

    # ── armies, spread over each state's own population ──
    total_pop = pop.sum()
    for ci, iso in enumerate(iso_of):
        mine = np.where(owner == ci)[0]
        if not len(mine): continue
        strength = int(pop[mine].sum() * 0.011)          # ~1.1% under arms
        share = pop[mine] / pop[mine].sum()
        for p, s in zip(mine, share):
            n = int(strength * s)
            if n >= 500:
                armies.setdefault(str(p + 1), []).append(
                    {"country_id": cid_of[iso], "count": n})

    # ── a few hulls each, berthed where the ports are ──
    ships = []
    for ci, iso in enumerate(iso_of):
        mine = [p for p in np.where(owner == ci)[0] if str(p + 1) in ports]
        if not mine: continue
        n = int(np.clip(len(mine) // 6, 1, 9))
        for p in rng.choice(mine, size=min(n, len(mine)), replace=False):
            t = str(rng.choice(["destroyer", "destroyer", "boat", "carrier"], p=[.5, .25, .2, .05]))
            ships.append({"country_id": cid_of[iso], "type": t,
                          "lat": round(float(clat[p]), 5),
                          "lon": round(float(((clon[p] + 180) % 360) - 180), 5),
                          "health": 100,
                          "crew": {"destroyer": 320, "boat": 60, "carrier": 1800}[t]})

    countries = {}
    for ci, (iso, name, lon0, lat0, col, l, a, _reach) in enumerate(COUNTRIES):
        r, g, bl = int(col[1:3], 16), int(col[3:5], 16), int(col[5:7], 16)
        flag = {"type": "hstripes_3", "colors": [
            "#%02x%02x%02x" % (max(r-55, 0), max(g-55, 0), max(bl-55, 0)), col,
            "#%02x%02x%02x" % (min(r+60, 255), min(g+60, 255), min(bl+60, 255))]}
        countries[str(cid_of[iso])] = {
            "id": cid_of[iso], "iso_a3": iso, "name": name, "color": col,
            "flag_actual": flag, "flag_censored": dict(flag),
            "treasury": round(float(pop[owner == ci].sum()) / 2.2e6, 1) if (owner == ci).any() else 5.0,
        }
    return dict(provinces=provinces, population=population, compass=compass,
                minorities=minorities, mcolors=mcolors, resources=resources,
                ports=ports, armies=armies, ships=ships, countries=countries,
                iso_of=iso_of, cid_of=cid_of)


# Who gets on with whom. Written rather than generated: a diplomatic web that
# means something is the difference between a world and a list of colours.
RELATIONS = {
    "alliances": [("OLY", "XAN"), ("XAN", "MER"), ("TMP", "MRN"),
                  ("SYR", "SAB"), ("PRM", "AUS"), ("SIR", "ALB")],
    "wars":      [("THA", "NOC"), ("SOL", "MRN")],
    "non_aggression": [("THA", "OLY"), ("HES", "CIM"), ("ELY", "TYR"), ("NOA", "MER")],
}
# The Marineris waterway is the one thing everybody upstream of it wants.
CLAIM_TARGETS = [("THA", "NOC"), ("SOL", "MRN"), ("NOC", "MRN"), ("LUN", "XAN")]


def write_odmap(b, d):
    prov_by_iso = {}
    for pid, p in d["provinces"].items():
        prov_by_iso.setdefault(p["iso_a3"], []).append(int(pid))

    relations = {}
    def rel(a, x, key):
        relations.setdefault(a, {}).setdefault(x, {})[key] = True
        relations.setdefault(x, {}).setdefault(a, {})[key] = True
    for a, x in RELATIONS["alliances"]:       rel(a, x, "ally")
    for a, x in RELATIONS["wars"]:            rel(a, x, "war")
    for a, x in RELATIONS["non_aggression"]:  rel(a, x, "non_aggression")

    claims = {}
    for by, on in CLAIM_TARGETS:
        claims.setdefault(by, []).extend(sorted(prov_by_iso.get(on, []))[:14])

    files = {
        "provinces.json": d["provinces"],
        "countries.json": d["countries"],
        "population.json": d["population"],
        "political_compass.json": d["compass"],
        "minorities.json": d["minorities"],
        "minority_colors.json": d["mcolors"],
        "resources.json": d["resources"],
        "ports.json": d["ports"],
        "armies.json": d["armies"],
        "ships.json": d["ships"],
        "relations.json": relations,
        "claims.json": claims,
        "country_compass.json": {iso: {"left": c[5], "auth": c[6]}
                                 for iso, c in zip(d["iso_of"], COUNTRIES)},
        "starting_policies.json": {"starting_policies":
                                   {iso: POLICIES.get(iso, []) for iso in d["iso_of"]}},
        "starting_minority_policies.json": {
            iso: {g: [1, 1, 1, 1, 1, 1] for g in ["Mars-born"] + FOUNDERS}
            for iso in d["iso_of"]},
        "metadata.json": {
            "author": "OpenDoctrines", "has_scripts": False, "license": "CC-BY-4.0",
            "map_date": "March 2247 AD", "name": "Mars",
            "description": ("Real Martian topography with the basins flooded. Every "
                            "sea is a named basin -- Hellas, Utopia, Chryse, Argyre -- "
                            "and every state stands on the highlands between them."),
        },
    }
    with open(os.path.join(ROOT, "data", "policies.json")) as f:
        policies_text = f.read()

    png = {}
    for name, arr in (("provinces.png", Image.fromarray(b["prov_png"], "RGB")),
                      ("land_sea.png", b["ls_img"])):
        buf = io.BytesIO(); arr.save(buf, "PNG", optimize=True); png[name] = buf.getvalue()

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for n, obj in files.items():
            z.writestr(n, json.dumps(obj, separators=(",", ":")))
        z.writestr("policies.json", policies_text)
        for n, raw in png.items():
            z.writestr(n, raw)
    print("  wrote %s (%.1f MB)" % (os.path.relpath(OUT, ROOT), os.path.getsize(OUT)/1e6))

    thumb_path = os.path.join(ROOT, "data", "STDmaps", "mars_thumb.png")
    finish_archive(thumb_path)
    idx_path = os.path.join(ROOT, "data", "STDmaps", "maps_index.json")
    with open(idx_path) as f: idx = json.load(f)
    idx = [e for e in idx if e.get("filename") != "mars.odmap"]
    idx.append({"name": "Mars", "filename": "mars.odmap", "author": "OpenDoctrines",
                "description": files["metadata.json"]["description"],
                "license": "CC-BY-4.0", "thumbnail": "mars_thumb.png",
                "hasScripts": False})
    with open(idx_path, "w") as f: json.dump(idx, f, indent=2); f.write("\n")
    print("  registered in maps_index.json")


def finish_archive(thumb_path):
    """Draw the browser preview, and rewrite the archive the shared way.

    NOT A HAND-ROLLED THUMBNAIL, and the first version here was one: a shaded
    relief, with a comment claiming the browser painted owners over it. It does
    not. thumb.png IS what the browser shows before a world is loaded, and
    tools/rebuild_map_preview.py checks it against province ownership -- a
    relief map disagrees on 131,070 pixels of 131,072 and failed the suite on
    all four platforms. Redrawing it as flat owner colours still drifted 95%,
    because build_thumb downsamples the ids FIRST and then runs a border
    gradient at thumbnail scale. There is no approximating that; the only way
    to agree with the check is to call what the check calls.

    EFFORT_FAST, deliberately. Zopfli belongs in tools/shrink_maps.py "and
    nowhere else" (odmap_pack.py) -- generation writes fast and the compaction
    pass squeezes afterwards, which is why a freshly generated map is expected
    to read as "not compact" until that pass runs.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fill_water_speckle import build_thumb
    from odmap_pack import EFFORT_FAST, layer_png, province_ids, read_members, write_odmap

    members, dirs = read_members(OUT)
    thumb = build_thumb(province_ids(members),
                        json.loads(members["provinces.json"]),
                        json.loads(members["countries.json"]))
    members["thumb.png"] = layer_png(thumb, EFFORT_FAST)
    write_odmap(OUT, members, dirs, EFFORT_FAST)
    thumb.save(thumb_path, optimize=True)
    print("  preview drawn by build_thumb, the way every tool that rewrites a map draws it")


def main(argv):
    b = build("--keep" in argv)
    d = pack(b)
    write_odmap(b, d)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
