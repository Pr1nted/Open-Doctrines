#!/usr/bin/env python3
"""Download official flag SVGs from Wikimedia Commons for all countries.

Reads countries.json, downloads each country's flag from Wikipedia/Wikimedia,
saves to data/flags/{iso}.svg, and updates countries.json with the reference.

Falls back to procedural generation for individual countries if download fails.
"""
import json, os, sys, time, urllib.request, urllib.error, re

DATA_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")
COUNTRIES_JSON = os.path.join(DATA_DIR, "countries.json")
FLAGS_DIR = os.path.join(DATA_DIR, "flags")

FILENAMES = {
    "GRL": "Flag of Greenland.svg", "CAN": "Flag of Canada.svg",
    "RUS": "Flag of Russia.svg", "NOR": "Flag of Norway.svg",
    "USA": "Flag of the United States.svg", "FIN": "Flag of Finland.svg",
    "SWE": "Flag of Sweden.svg", "ISL": "Flag of Iceland.svg",
    "FRO": "Flag of the Faroe Islands.svg",
    "GBR": "Flag of the United Kingdom.svg", "ALA": "Flag of Åland.svg",
    "EST": "Flag of Estonia.svg", "LVA": "Flag of Latvia.svg",
    "DNK": "Flag of Denmark.svg", "LTU": "Flag of Lithuania.svg",
    "BLR": "Flag of Belarus.svg", "KAZ": "Flag of Kazakhstan.svg",
    "IRL": "Flag of Ireland.svg", "DEU": "Flag of Germany.svg",
    "POL": "Flag of Poland.svg", "IMN": "Flag of the Isle of Man.svg",
    "NLD": "Flag of the Netherlands.svg",
    "CHN": "Flag of the People's Republic of China.svg",
    "UKR": "Flag of Ukraine.svg", "MNG": "Flag of Mongolia.svg",
    "BEL": "Flag of Belgium.svg", "FRA": "Flag of France.svg",
    "CZE": "Flag of the Czech Republic.svg", "LUX": "Flag of Luxembourg.svg",
    "SVK": "Flag of Slovakia.svg", "AUT": "Flag of Austria.svg",
    "HUN": "Flag of Hungary.svg", "MDA": "Flag of Moldova.svg",
    "ROU": "Flag of Romania.svg", "CHE": "Flag of Switzerland.svg",
    "ITA": "Flag of Italy.svg", "SVN": "Flag of Slovenia.svg",
    "HRV": "Flag of Croatia.svg", "SRB": "Flag of Serbia.svg",
    "UZB": "Flag of Uzbekistan.svg", "JPN": "Flag of Japan.svg",
    "BIH": "Flag of Bosnia and Herzegovina.svg",
    "BGR": "Flag of Bulgaria.svg", "ESP": "Flag of Spain.svg",
    "GEO": "Flag of Georgia.svg", "KGZ": "Flag of Kyrgyzstan.svg",
    "PRK": "Flag of North Korea.svg", "TKM": "Flag of Turkmenistan.svg",
    "ALB": "Flag of Albania.svg", "MKD": "Flag of North Macedonia.svg",
    "PRT": "Flag of Portugal.svg", "TUR": "Flag of Turkey.svg",
    "AZE": "Flag of Azerbaijan.svg", "GRC": "Flag of Greece.svg",
    "ARM": "Flag of Armenia.svg", "TJK": "Flag of Tajikistan.svg",
    "IRN": "Flag of Iran.svg", "KOR": "Flag of South Korea.svg",
    "AFG": "Flag of Afghanistan.svg", "TUN": "Flag of Tunisia.svg",
    "IRQ": "Flag of Iraq.svg", "SYR": "Flag of Syria.svg",
    "DZA": "Flag of Algeria.svg", "PAK": "Flag of Pakistan.svg",
    "MAR": "Flag of Morocco.svg",
    "XNC": "Flag of the Turkish Republic of Northern Cyprus.svg",
    "IND": "Flag of India.svg", "CYP": "Flag of Cyprus.svg",
    "LBN": "Flag of Lebanon.svg", "JOR": "Flag of Jordan.svg",
    "ISR": "Flag of Israel.svg", "LBY": "Flag of Libya.svg",
    "MEX": "Flag of Mexico.svg", "PSE": "Flag of Palestine.svg",
    "SAU": "Flag of Saudi Arabia.svg", "EGY": "Flag of Egypt.svg",
    "NPL": "Flag of Nepal.svg", "KWT": "Flag of Kuwait.svg",
    "MMR": "Flag of Myanmar.svg", "BTN": "Flag of Bhutan.svg",
    "ESH": "Flag of Western Sahara.svg", "MRT": "Flag of Mauritania.svg",
    "BHS": "Flag of the Bahamas.svg", "BGD": "Flag of Bangladesh.svg",
    "OMN": "Flag of Oman.svg", "TWN": "Flag of Taiwan.svg",
    "QAT": "Flag of Qatar.svg", "ARE": "Flag of the United Arab Emirates.svg",
    "MLI": "Flag of Mali.svg", "NER": "Flag of Niger.svg",
    "TCD": "Flag of Chad.svg", "VNM": "Flag of Vietnam.svg",
    "CUB": "Flag of Cuba.svg", "HKG": "Flag of Hong Kong.svg",
    "LAO": "Flag of Laos.svg", "SDN": "Flag of Sudan.svg",
    "PHL": "Flag of the Philippines.svg", "THA": "Flag of Thailand.svg",
    "HTI": "Flag of Haiti.svg", "DOM": "Flag of the Dominican Republic.svg",
    "YEM": "Flag of Yemen.svg", "JAM": "Flag of Jamaica.svg",
    "BLZ": "Flag of Belize.svg", "PRI": "Flag of Puerto Rico.svg",
    "ERI": "Flag of Eritrea.svg", "GTM": "Flag of Guatemala.svg",
    "CPV": "Flag of Cape Verde.svg", "SEN": "Flag of Senegal.svg",
    "HND": "Flag of Honduras.svg", "BFA": "Flag of Burkina Faso.svg",
    "NIC": "Flag of Nicaragua.svg", "ETH": "Flag of Ethiopia.svg",
    "KHM": "Flag of Cambodia.svg", "SLV": "Flag of El Salvador.svg",
    "NGA": "Flag of Nigeria.svg", "GMB": "Flag of the Gambia.svg",
    "CMR": "Flag of Cameroon.svg", "GNB": "Flag of Guinea-Bissau.svg",
    "DJI": "Flag of Djibouti.svg", "GIN": "Flag of Guinea.svg",
    "COL": "Flag of Colombia.svg", "BEN": "Flag of Benin.svg",
    "VEN": "Flag of Venezuela.svg", "SOM": "Flag of Somalia.svg",
    "XSO": "Flag of Somaliland.svg",
    "TTO": "Flag of Trinidad and Tobago.svg", "CRI": "Flag of Costa Rica.svg",
    "GHA": "Flag of Ghana.svg", "TGO": "Flag of Togo.svg",
    "CAF": "Flag of the Central African Republic.svg",
    "CIV": "Flag of Ivory Coast.svg", "SLE": "Flag of Sierra Leone.svg",
    "LKA": "Flag of Sri Lanka.svg", "PAN": "Flag of Panama.svg",
    "LBR": "Flag of Liberia.svg", "GUY": "Flag of Guyana.svg",
    "MYS": "Flag of Malaysia.svg", "SUR": "Flag of Suriname.svg",
    "IDN": "Flag of Indonesia.svg",
    "COD": "Flag of the Democratic Republic of the Congo.svg",
    "BRA": "Flag of Brazil.svg", "BRN": "Flag of Brunei.svg",
    "KEN": "Flag of Kenya.svg", "UGA": "Flag of Uganda.svg",
    "GNQ": "Flag of Equatorial Guinea.svg",
    "COG": "Flag of the Republic of the Congo.svg",
    "GAB": "Flag of Gabon.svg",
    "STP": "Flag of São Tomé and Príncipe.svg",
    "ECU": "Flag of Ecuador.svg", "PER": "Flag of Peru.svg",
    "TZA": "Flag of Tanzania.svg", "RWA": "Flag of Rwanda.svg",
    "PNG": "Flag of Papua New Guinea.svg", "BDI": "Flag of Burundi.svg",
    "AGO": "Flag of Angola.svg", "SLB": "Flag of the Solomon Islands.svg",
    "PYF": "Flag of French Polynesia.svg", "ZMB": "Flag of Zambia.svg",
    "AUS": "Flag of Australia.svg", "MWI": "Flag of Malawi.svg",
    "BOL": "Flag of Bolivia.svg", "MOZ": "Flag of Mozambique.svg",
    "COM": "Flag of the Comoros.svg", "MDG": "Flag of Madagascar.svg",
    "FJI": "Flag of Fiji.svg", "VUT": "Flag of Vanuatu.svg",
    "WSM": "Flag of Samoa.svg", "ZWE": "Flag of Zimbabwe.svg",
    "NAM": "Flag of Namibia.svg", "CHL": "Flag of Chile.svg",
    "BWA": "Flag of Botswana.svg", "PRY": "Flag of Paraguay.svg",
    "NCL": "Flag of New Caledonia.svg", "MUS": "Flag of Mauritius.svg",
    "ARG": "Flag of Argentina.svg", "ZAF": "Flag of South Africa.svg",
    "SWZ": "Flag of Eswatini.svg", "LSO": "Flag of Lesotho.svg",
    "NZL": "Flag of New Zealand.svg", "URY": "Flag of Uruguay.svg",
    "ATF": "Flag of the French Southern and Antarctic Lands.svg",
    "FLK": "Flag of the Falkland Islands.svg",
    "SGS": "Flag of South Georgia and the South Sandwich Islands.svg",
}

PROCEDURAL_ENTRIES = {
    "BLC", "UNC", "XNC", "XSO", "PSE", "ESH", "HKG", "PRI",
    "ALA", "FRO", "IMN", "ATF", "FLK", "SGS", "NCL", "PYF",
}

# Import procedural flag generator for fallback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util as _iutil
_gen_spec = _iutil.spec_from_file_location("generate_flag_svgs",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "generate_flag_svgs.py"))
_gen_mod = _iutil.module_from_spec(_gen_spec)
_gen_spec.loader.exec_module(_gen_mod)
_generate_flag_svg = _gen_mod.generate_flag_svg

def is_valid_svg(path):
    if not os.path.exists(path):
        return False
    with open(path) as f:
        data = f.read(500)
    if not data:
        return False
    if data.startswith("<?xml") or data.startswith("<svg"):
        # Has proper SVG header — likely real
        return len(data) > 500
    return False

def get_wikimedia_url(filename, retries=4):
    if filename is None:
        return None
    import urllib.parse
    safe = urllib.parse.quote(f"File:{filename}", safe=":/")
    url = f"https://commons.wikimedia.org/w/api.php?action=query&titles={safe}&prop=imageinfo&iiprop=url&format=json"
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "OpenDoctrines/1.0"})
            with urllib.request.urlopen(req, timeout=15) as resp:
                data = json.loads(resp.read())
                for pid, page in data.get("query", {}).get("pages", {}).items():
                    if pid != "-1":
                        ii = page.get("imageinfo", [])
                        if ii:
                            return ii[0].get("url")
            return None
        except urllib.error.HTTPError as e:
            if e.code == 429:
                wait = min(2 ** attempt * 3, 30)
                print(f"    API rate limited, waiting {wait}s...")
                time.sleep(wait)
            else:
                if attempt < retries - 1:
                    time.sleep(1)
                else:
                    print(f"    API error: {e}")
                    return None
        except Exception as e:
            if attempt < retries - 1:
                time.sleep(1)
            else:
                print(f"    API error: {e}")
                return None
    print(f"    API exhausted retries")
    return None

def download_svg(url, retries=4):
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "OpenDoctrines/1.0"})
            with urllib.request.urlopen(req, timeout=20) as resp:
                data = resp.read()
                if data.strip().startswith((b"<svg", b"<?xml")):
                    return data
                try:
                    text = data.decode("utf-8").strip()
                    if text.startswith(("<svg", "<?xml")):
                        return data
                except:
                    pass
                print(f"    Not SVG: {data[:100]}")
                return None
        except urllib.error.HTTPError as e:
            if e.code == 429:
                wait = min(2 ** attempt * 3, 30)
                print(f"    Download rate limited, waiting {wait}s...")
                time.sleep(wait)
            else:
                if attempt < retries - 1:
                    time.sleep(1)
                else:
                    print(f"    Download error: {e}")
                    return None
        except Exception as e:
            if attempt < retries - 1:
                time.sleep(1)
            else:
                print(f"    Download error: {e}")
                return None
    print(f"    Download exhausted retries")
    return None

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Download flags from Wikimedia Commons")
    parser.add_argument("--best-effort", action="store_true",
                        help="Skip countries with existing real SVGs (>500 bytes), "
                             "longer delays, best-effort only")
    args = parser.parse_args()

    if not os.path.exists(COUNTRIES_JSON):
        print(f"ERROR: {COUNTRIES_JSON} not found")
        sys.exit(1)
    os.makedirs(FLAGS_DIR, exist_ok=True)

    with open(COUNTRIES_JSON) as f:
        countries = json.load(f)

    downloaded = 0
    skipped = 0
    failed = 0
    procedural = 0

    for cid, entry in sorted(countries.items(), key=lambda x: int(x[0])):
        iso = entry.get("iso_a3", "")
        name = entry.get("name", "Unknown")
        if not iso or iso == "-99":
            continue

        svg_path = os.path.join(FLAGS_DIR, f"{iso}.svg")

        # In best-effort mode, skip countries that already have real SVGs
        if args.best_effort and is_valid_svg(svg_path):
            skipped += 1
            continue

        # Skip if already a valid real SVG (>500 bytes)
        if is_valid_svg(svg_path):
            downloaded += 1
            continue

        filename = FILENAMES.get(iso)
        if filename is None:
            if iso in PROCEDURAL_ENTRIES or iso in ("BLC", "UNC"):
                # Generate simple placeholder
                if os.path.exists(svg_path) and os.path.getsize(svg_path) > 50:
                    continue
                svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 133">'
                       f'<rect width="200" height="133" fill="#888"/>'
                       f'<text x="100" y="70" font-size="14" fill="#fff" '
                       f'text-anchor="middle" dominant-baseline="central">{name}</text></svg>')
                with open(svg_path, "w") as f:
                    f.write(svg)
                entry["flag_actual"] = {"image": f"flags/{iso}.svg"}
                entry["flag_censored"] = {"image": f"flags/{iso}.svg", "censored": True}
                print(f"  [{iso}] {name}: placeholder")
                continue
            print(f"  [{iso}] {name}: no filename mapping, skipping")
            skipped += 1
            continue

        print(f"  [{iso}] {name}: {filename}")

        direct_url = get_wikimedia_url(filename)
        if not direct_url:
            alt = filename.replace("Flag of the ", "Flag of ")
            if alt != filename:
                direct_url = get_wikimedia_url(alt)
            if not direct_url:
                print(f"    NOT FOUND — procedural fallback")
                svg_data = _generate_flag_svg(entry, w=200, h=133).encode("utf-8")
                if not svg_data:
                    failed += 1
                    continue
                procedural += 1
                with open(svg_path, "wb") as f:
                    f.write(svg_data)
                entry["flag_actual"] = {"image": f"flags/{iso}.svg"}
                entry["flag_censored"] = {"image": f"flags/{iso}.svg", "censored": True}
                downloaded += 1
                print(f"    OK (procedural, {len(svg_data)} bytes)")
                continue

        svg_data = download_svg(direct_url)
        if not svg_data:
            print(f"    DOWNLOAD FAILED — procedural fallback")
            svg_data = _generate_flag_svg(entry, w=200, h=133).encode("utf-8")
            if not svg_data:
                failed += 1
                continue
            procedural += 1

        with open(svg_path, "wb") as f:
            f.write(svg_data)

        entry["flag_actual"] = {"image": f"flags/{iso}.svg"}
        entry["flag_censored"] = {"image": f"flags/{iso}.svg", "censored": True}
        downloaded += 1
        sz = len(svg_data)
        print(f"    OK ({sz} bytes)")

        # Delay between requests to avoid rate limiting
        time.sleep(3.0 if args.best_effort else 1.0)

    with open(COUNTRIES_JSON, "w") as f:
        json.dump(countries, f, indent=2)

    expected = {f"{entry.get('iso_a3','')}.svg" for entry in countries.values()
                if entry.get("iso_a3","") and entry.get("iso_a3","") != "-99"}
    for fname in os.listdir(FLAGS_DIR):
        if fname.endswith(".svg") and fname not in expected:
            os.remove(os.path.join(FLAGS_DIR, fname))
            print(f"  Removed stale: {fname}")

    real = sum(1 for f in os.listdir(FLAGS_DIR)
               if f.endswith(".svg") and is_valid_svg(os.path.join(FLAGS_DIR, f)))
    print(f"\nDone: {downloaded} OK, {skipped} skipped, {failed} failed, {procedural} procedural fallbacks, ~{real} real SVGs")

if __name__ == "__main__":
    main()
