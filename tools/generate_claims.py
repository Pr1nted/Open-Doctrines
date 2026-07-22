#!/usr/bin/env python3
"""Generate territorial claims based on real-world disputes using geographic matching."""
import json, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from province_geo import load_province_centroids, load_provinces_iso, find_province

DATA_DIR = "data"
PROVINCES_JSON = os.path.join(DATA_DIR, "provinces.json")
COUNTRIES_JSON = os.path.join(DATA_DIR, "countries.json")
OUT_CLAIMS = os.path.join(DATA_DIR, "claims.json")

with open(PROVINCES_JSON) as f:
    provinces = json.load(f)
with open(COUNTRIES_JSON) as f:
    countries = json.load(f)

# Build mappings
cid_to_iso = {}
for k, v in countries.items():
    cid_to_iso[int(k)] = v.get("iso_a3", k)

iso_pids = {}
for pid_str, p in provinces.items():
    cid = p.get("country_id", 0)
    if cid <= 0:
        continue
    iso = cid_to_iso.get(cid)
    if iso:
        iso_pids.setdefault(iso, []).append(int(pid_str))

def provinces_of(iso):
    return set(iso_pids.get(iso, []))

# Geographic matching helper
centroids = load_province_centroids(DATA_DIR)
prov_iso = load_provinces_iso(DATA_DIR)

def near(target_lon, target_lat, iso_filter=None):
    return find_province(target_lon, target_lat, centroids, iso_filter, prov_iso)

claims = {}

# Taiwan claims mainland China (all CHN provinces)
chn_pids = provinces_of("CHN")
if chn_pids:
    claims["TWN"] = sorted(chn_pids)
    print(f"Taiwan claims {len(chn_pids)} China provinces")

# China claims Taiwan
twn_pids = provinces_of("TWN")
if twn_pids:
    claims["CHN"] = sorted(twn_pids)
    print(f"China claims {len(twn_pids)} Taiwan provinces")

# Korea mutual claims
kor_pids = provinces_of("KOR")
prk_pids = provinces_of("PRK")
if kor_pids and prk_pids:
    claims.setdefault("PRK", []).extend(kor_pids)
    claims.setdefault("KOR", []).extend(prk_pids)
    print(f"North Korea claims {len(kor_pids)} South Korea provinces")
    print(f"South Korea claims {len(prk_pids)} North Korea provinces")

# Cyprus claims N. Cyprus (cid=77)
ncy_pids = [int(ps) for ps, p in provinces.items() if p.get('country_id') == 77]
cyp_pids = provinces_of("CYP")
if ncy_pids and cyp_pids:
    claims.setdefault("CYP", []).extend(ncy_pids)
    print(f"Cyprus claims {len(ncy_pids)} N. Cyprus provinces")

# Somalia claims Somaliland (cid=167)
sl_pids = [int(ps) for ps, p in provinces.items() if p.get('country_id') == 167]
som_pids = provinces_of("SOM")
if sl_pids and som_pids:
    claims.setdefault("SOM", []).extend(sl_pids)
    print(f"Somalia claims {len(sl_pids)} Somaliland provinces")

# Kashmir: geographic matching
# India claims Pakistan-administered Kashmir (Gilgit-Baltistan/Azad Kashmir)
for lon, lat, label in [(74.3, 35.4, "Gilgit-Baltistan"), (74.5, 35.6, "Gilgit")]:
    pid = near(lon, lat, "PAK")
    if pid:
        claims.setdefault("IND", []).append(pid)
        print(f"India claims PAK province {pid} ({label})")
# Pakistan claims India-administered Kashmir (Jammu & Kashmir)
pid = near(75.0, 33.8, "IND")
if pid:
    claims.setdefault("PAK", []).append(pid)
    print(f"Pakistan claims IND province {pid} (Jammu & Kashmir)")

# India claims Aksai Chin (from China)
pid = near(80.7, 31.7, "CHN")
if pid:
    claims.setdefault("IND", []).append(pid)
    print(f"India claims CHN province {pid} (Aksai Chin)")

# China claims Arunachal Pradesh (from India)
pid = near(95.6, 28.3, "IND")
if pid:
    claims.setdefault("CHN", []).append(pid)
    print(f"China claims IND province {pid} (Arunachal Pradesh)")

# Afghanistan claims Pashtun areas (Durand Line)
for lon, lat, label in [(67.0, 30.5, "N Balochistan"), (73.0, 35.4, "Gilgit/KPK"), (72.0, 35.3, "KPK")]:
    pid = near(lon, lat, "PAK")
    if pid:
        claims.setdefault("AFG", []).append(pid)
        print(f"Afghanistan claims PAK province {pid} ({label})")

# DRC claims Uganda border (Lake Albert)
pid = near(30.5, -0.6, "UGA")
if pid:
    claims.setdefault("COD", []).append(pid)
    print(f"DRC claims UGA province {pid} (Lake Albert)")
# Uganda claims DRC border (Lake Albert)
pid = near(29.3, 2.9, "COD")
if pid:
    claims.setdefault("UGA", []).append(pid)
    print(f"Uganda claims COD province {pid} (Lake Albert)")

# Eritrea claims Ethiopian border (Badme area)
pid = near(38.0, 13.5, "ETH")
if pid:
    claims.setdefault("ERI", []).append(pid)
    print(f"Eritrea claims ETH province {pid} (Badme)")
# Ethiopia claims Eritrean border
for lon, lat, label in [(38.1, 16.8, "Eritrea border"), (39.0, 15.3, "Eritrea border")]:
    pid = near(lon, lat, "ERI")
    if pid:
        claims.setdefault("ETH", []).append(pid)
        print(f"Ethiopia claims ERI province {pid} ({label})")

# Armenia-Azerbaijan (Nagorno-Karabakh)
arm_pids = provinces_of("ARM")
aze_pids = provinces_of("AZE")
if len(aze_pids) > 1 and arm_pids:
    nk_claims = aze_pids - {max(aze_pids)}
    claims.setdefault("ARM", []).extend(nk_claims)
    claims.setdefault("AZE", []).extend(arm_pids)
    print(f"Armenia claims {len(nk_claims)} Azerbaijan provinces (Nagorno-Karabakh)")
    print(f"Azerbaijan claims {len(arm_pids)} Armenia provinces")

# Cambodia-Thailand (Preah Vihear)
tha_pids = provinces_of("THA")
khm_pids = provinces_of("KHM")
if tha_pids and khm_pids:
    tha_claims = sorted(khm_pids)[:2]
    khm_claims = sorted(tha_pids)[:2]
    claims.setdefault("THA", []).extend(tha_claims)
    claims.setdefault("KHM", []).extend(khm_claims)
    print(f"Thailand claims {len(tha_claims)} Cambodia provinces (Preah Vihear)")
    print(f"Cambodia claims {len(khm_claims)} Thailand provinces (Preah Vihear)")

# Japan claims Kuril Islands / Northern Territories and Southern Sakhalin (from Russia)
# Etorofu/Iturup: lon=147.8, lat=45.1
# Kunashiri/Kunashir: lon=146.0, lat=44.2
# Southern Sakhalin/Karafuto: lon=143.0, lat=50.2
for lon, lat, label in [(147.8, 45.1, "Iturup/Etorofu"), (146.0, 44.2, "Kunashir/Kunashiri"), (143.0, 50.2, "S Sakhalin/Karafuto")]:
    pid = near(lon, lat, "RUS")
    if pid:
        claims.setdefault("JPN", []).append(pid)
        print(f"Japan claims RUS province {pid} ({label})")

# Argentina claims Falkland Islands (cid=250, iso=FLK)
arg_pids = provinces_of("ARG")
flk_pids = [int(ps) for ps, p in provinces.items() if p.get('country_id') == 250]
if flk_pids and arg_pids:
    claims.setdefault("ARG", []).extend(flk_pids)
    print(f"Argentina claims {len(flk_pids)} Falkland Islands provinces")

# Morocco claims Western Sahara
es_pids = provinces_of("ESH")
if es_pids:
    claims.setdefault("MAR", []).extend(es_pids)
    print(f"Morocco claims {len(es_pids)} W Sahara provinces")

# Israel-Palestine mutual claims
isr_pids = provinces_of("ISR")
pse_pids = provinces_of("PSE")
if isr_pids and pse_pids:
    claims.setdefault("PSE", []).extend(isr_pids)
    claims.setdefault("ISR", []).extend(pse_pids)
    print(f"Palestine claims {len(isr_pids)} Israel provinces")
    print(f"Israel claims {len(pse_pids)} Palestine provinces")

# Remove duplicates
for iso in claims:
    seen = set()
    deduped = []
    for pid in claims[iso]:
        if pid not in seen:
            seen.add(pid)
            deduped.append(pid)
    claims[iso] = sorted(deduped)

with open(OUT_CLAIMS, "w") as f:
    json.dump(claims, f, indent=2, separators=(",", ":"))

total = sum(len(v) for v in claims.values())
print(f"Wrote {OUT_CLAIMS} with {total} total claims")
