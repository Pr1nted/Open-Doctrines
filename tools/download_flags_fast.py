#!/usr/bin/env python3
"""Fast flag downloader — uses direct Wikimedia URLs (no API), skips existing real flags.

Constructs download URLs using the MD5 hash pattern:
  https://upload.wikimedia.org/wikipedia/commons/{hash[0]}/{hash[0:2]}/{filename}

This bypasses the Wikimedia API entirely, avoiding rate limits from API calls.
Only the actual file download is rate-limited (handled with retries).

Usage:
    python3 tools/download_flags_fast.py
    python3 tools/download_flags_fast.py --delay 0.2
    python3 tools/download_flags_fast.py --force  # re-download even real flags
"""

import hashlib, json, os, sys, time, urllib.request, urllib.error

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
    "IRN": "Flag of the Islamic Republic of Iran.svg", "KOR": "Flag of South Korea.svg",
    "AFG": "Flag of Afghanistan (2013–2021).svg", "TUN": "Flag of Tunisia.svg",
    "IRQ": "Flag of Iraq.svg", "SYR": "Flag of Syria (2025-).svg",
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
    "ESH": "Flag of the Sahrawi Arab Democratic Republic.svg",
    "MRT": "Flag of Mauritania.svg",
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
    "HND": "Flag of Honduras (1949–2022, 2026–present).svg",
    "BFA": "Flag of Burkina Faso.svg",
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
    "CIV": "Flag of Côte d'Ivoire.svg", "SLE": "Flag of Sierra Leone.svg",
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
    "BOL": "Flag of Bolivia.svg",
    "MOZ": "Flag of Mozambique.svg",
    "COM": "Flag of the Comoros.svg", "MDG": "Flag of Madagascar.svg",
    "FJI": "Flag of Fiji.svg", "VUT": "Flag of Vanuatu.svg",
    "WSM": "Flag of Samoa.svg", "ZWE": "Flag of Zimbabwe.svg",
    "NAM": "Flag of Namibia.svg", "CHL": "Flag of Chile.svg",
    "BWA": "Flag of Botswana.svg", "PRY": "Flag of Paraguay.svg",
    "NCL": "Flag of FLNKS.svg", "MUS": "Flag of Mauritius.svg",
    "ARG": "Flag of Argentina.svg", "ZAF": "Flag of South Africa.svg",
    "SWZ": "Flag of Eswatini.svg", "LSO": "Flag of Lesotho.svg",
    "NZL": "Flag of New Zealand.svg", "URY": "Flag of Uruguay.svg",
    "ATF": "Flag of the French Southern and Antarctic Lands.svg",
    "FLK": "Flag of the Falkland Islands.svg",
    "SGS": "Flag of South Georgia and the South Sandwich Islands.svg",
}


def get_download_url(filename):
    """Construct direct Wikimedia Commons download URL using MD5 hash."""
    fname = filename.replace(" ", "_")
    m = hashlib.md5(fname.encode()).hexdigest()
    from urllib.parse import quote
    safe_name = quote(fname, safe="")
    return f"https://upload.wikimedia.org/wikipedia/commons/{m[0]}/{m[0:2]}/{safe_name}"


def download_flag(url, retries=4):
    """Download SVG from URL. Returns bytes or None."""
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={
                "User-Agent": "OpenDoctrines/1.0 (data generation tool)"
            })
            with urllib.request.urlopen(req, timeout=20) as resp:
                data = resp.read()
                # Check for SVG content (handle BOM and extra whitespace)
                stripped = data.lstrip()
                if stripped.startswith((b"<svg", b"<?xml", b"\xef\xbb\xbf<svg", b"\xef\xbb\xbf<?xml")):
                    return data
                # Try to decode and check as text
                try:
                    text = data.decode("utf-8").strip()
                    if text.startswith(("<svg", "<?xml")):
                        return data
                    # Check with BOM
                    text2 = data.decode("utf-8-sig").strip()
                    if text2.startswith(("<svg", "<?xml")):
                        return data
                except:
                    pass
                print(f"    Not SVG ({len(data)} bytes): {data[:80]}")
                return None
        except urllib.error.HTTPError as e:
            if e.code == 429:
                wait = min(2 ** attempt * 2, 20)
                print(f"    Rate limited, waiting {wait}s...")
                time.sleep(wait)
            elif e.code == 404:
                return None  # File not found
            else:
                if attempt < retries - 1:
                    time.sleep(1)
                else:
                    print(f"    HTTP {e.code}")
                    return None
        except Exception as e:
            if attempt < retries - 1:
                time.sleep(1)
            else:
                print(f"    Error: {e}")
                return None
    return None


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Fast flag downloader (no API)")
    parser.add_argument("--delay", type=float, default=0.3,
                        help="Delay between downloads in seconds (default: 0.3)")
    parser.add_argument("--force", action="store_true",
                        help="Re-download even if real SVG exists")
    args = parser.parse_args()

    os.makedirs(FLAGS_DIR, exist_ok=True)

    # data/countries.json exists only DURING a pipeline run -- step 20 sweeps it
    # -- so requiring it made this tool unusable standalone, which is when you
    # actually want to re-fetch a flag. Fall back to the base map's own copy.
    # (prerender_problematic_flags.py and generate_scenario.py had the same
    # break; the pattern is that a build artifact is not a dependency.)
    countries = None
    if os.path.exists(COUNTRIES_JSON):
        with open(COUNTRIES_JSON) as f:
            countries = json.load(f)
    else:
        import zipfile
        odmap = os.path.join(DATA_DIR, "STDmaps", "map.odmap")
        if os.path.exists(odmap):
            with zipfile.ZipFile(odmap) as z:
                if "countries.json" in z.namelist():
                    countries = json.loads(z.read("countries.json"))
            print(f"  countries.json taken from {os.path.relpath(odmap)}")
    if countries is None:
        print(f"ERROR: no countries.json in data/ or in STDmaps/map.odmap")
        sys.exit(1)

    downloaded = 0
    skipped_real = 0
    skipped_procedural = 0
    not_found = 0
    failed = 0

    # Alternative filenames — tried as fallback if the primary FILENAMES entry 404s
    ALT_FILENAMES = {
        "TWN": "Flag of the Republic of China.svg",
        "GMB": "Flag of The Gambia.svg",
        "MMR": "Flag of Myanmar (Burma).svg",
        "COD": "Flag of the Democratic Republic of the Congo (1997-2003).svg",
        "SLE": "Flag of the Republic of Sierra Leone.svg",
        "AFG": "Flag of Afghanistan.svg",
    }

    for cid, entry in sorted(countries.items(), key=lambda x: int(x[0])):
        iso = entry.get("iso_a3", "")
        name = entry.get("name", "Unknown")
        if not iso or iso == "-99":
            continue

        svg_path = os.path.join(FLAGS_DIR, f"{iso}.svg")

        # Skip if already a real SVG and not forcing
        if not args.force:
            if os.path.exists(svg_path) and os.path.getsize(svg_path) > 500:
                skipped_real += 1
                continue

        filenames_to_try = []
        if iso in FILENAMES:
            filenames_to_try.append(FILENAMES[iso])
        if iso in ALT_FILENAMES:
            alt = ALT_FILENAMES[iso]
            if alt not in filenames_to_try:
                filenames_to_try.append(alt)

        if not filenames_to_try:
            skipped_procedural += 1
            continue

        downloaded_ok = False
        for filename in filenames_to_try:
            url = get_download_url(filename)
            print(f"  [{iso}] {name} ({filename})")

            data = download_flag(url)
            if data is not None:
                with open(svg_path, "wb") as f:
                    f.write(data)
                downloaded += 1
                sz = len(data)
                print(f"    OK ({sz} bytes)")
                downloaded_ok = True
                break
            else:
                if filename != filenames_to_try[-1]:
                    print(f"    Trying next filename...")

        if not downloaded_ok:
            print(f"    NOT FOUND")
            not_found += 1
            continue

        time.sleep(args.delay)

    real = sum(1 for f in os.listdir(FLAGS_DIR)
               if f.endswith(".svg") and os.path.getsize(os.path.join(FLAGS_DIR, f)) > 500)
    print(f"\nDone: {downloaded} downloaded, {skipped_real} already real, "
          f"{skipped_procedural} no mapping, {not_found} not found, {failed} failed")
    print(f"Total real SVGs: {real}/{sum(1 for f in os.listdir(FLAGS_DIR) if f.endswith('.svg'))}")


if __name__ == "__main__":
    main()
