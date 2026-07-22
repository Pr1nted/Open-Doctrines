#!/usr/bin/env python3
"""
Generate relations.json — diplomatic relations between countries.

War     = active armed conflict with territorial capture (not frozen/obsolete)
Ally    = mutual defense pact / formal alliance
Guarantee = one-sided defense guarantee (not reciprocated)
NonAgress = non-aggression pact / strategic partnership (no mutual defense)

Uses historical data from ~2000 AD.
"""

import json, os

DATA_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")
OUT = os.path.join(DATA_DIR, "relations.json")

RELEVANT = [
    "AFG", "AGO", "ALB", "AND", "ARE", "ARG", "ARM", "ATG", "AUS", "AUT",
    "AZE", "BDI", "BEL", "BEN", "BFA", "BGD", "BGR", "BHR", "BHS", "BIH",
    "BLR", "BLZ", "BOL", "BRA", "BRN", "BTN", "BWA", "CAF", "CAN", "CHE",
    "CHL", "CHN", "CIV", "CMR", "COD", "COG", "COL", "CRI", "CUB", "CYP",
    "CZE", "DEU", "DJI", "DNK", "DOM", "DZA", "ECU", "EGY", "ERI", "ESH",
    "ESP", "EST", "ETH", "FIN", "FJI", "FRA", "GAB", "GBR", "GEO", "GHA",
    "GIN", "GMB", "GNB", "GNQ", "GRC", "GTM", "GUY", "HND", "HRV", "HTI",
    "HUN", "IDN", "IND", "IRL", "IRN", "IRQ", "ISL", "ISR", "ITA", "JAM",
    "JOR", "JPN", "KAZ", "KEN", "KGZ", "KHM", "KOR", "KWT", "LAO", "LBN",
    "LBR", "LBY", "LKA", "LSO", "LTU", "LUX", "LVA", "MAR", "MDA", "MDG",
    "MDV", "MEX", "MKD", "MLI", "MLT", "MMR", "MNE", "MNG", "MOZ", "MRT",
    "MUS", "MWI", "MYS", "NAM", "NER", "NGA", "NIC", "NLD", "NOR", "NPL",
    "NZL", "OMN", "PAK", "PAN", "PER", "PHL", "PNG", "POL", "PRK", "PRT",
    "PRY", "PSE", "QAT", "ROU", "RUS", "RWA", "SAU", "SDN", "SEN", "SGP",
    "SLE", "SLV", "SOM", "SRB", "SSD", "SUR", "SVK", "SVN", "SWE", "SWZ",
    "SYR", "TCD", "TGO", "THA", "TJK", "TKM", "TLS", "TON", "TUN", "TUR",
    "TWN", "TZA", "UGA", "UKR", "URY", "USA", "UZB", "VAT", "VEN", "VNM",
    "VUT", "WSM", "ALA", "XKV", "XNC", "XSO", "YEM", "ZAF", "ZMB", "ZWE",
    # Territories with diplomatic significance
    "FLK",  # Falkland Islands (claimed by ARG, controlled by GBR)
    "SGS",  # South Georgia & S Sandwich Is. (claimed by ARG, controlled by GBR)
    "COM",  # Comoros (Mayotte claimed from FRA)
    "CPV",  # Cape Verde (African Union, PALOP)
    "TTO",  # Trinidad & Tobago (CARICOM)
    "SLB",  # Solomon Islands (Pacific)
    "NCL",  # New Caledonia (French overseas, independence movement)
    "STP",  # Sao Tome & Principe (African Union, PALOP)
]

# ── Wars: active armed conflicts with territorial capture, ongoing as of ~2000 ──
WARS = {
    # Israeli-Palestinian conflict (ongoing territorial dispute)
    ("ISR", "PSE"): True,
    ("PSE", "ISR"): True,
    # Israel-Hezbollah (occupied Shebaa farms, active skirmishes)
    ("ISR", "LBN"): True,
    ("LBN", "ISR"): True,
    # Nagorno-Karabakh (active 1988-1994, ceasefire but no treaty)
    ("ARM", "AZE"): True,
    ("AZE", "ARM"): True,
    # Ethiopia-Eritrea border war (active 1998-2000)
    ("ETH", "ERI"): True,
    ("ERI", "ETH"): True,
    # Somalia-Somaliland (de facto separation, no recognition)
    ("SOM", "XSO"): True,
    ("XSO", "SOM"): True,
    # Sudan-South Sudan (Second Sudanese Civil War active 1983-2005)
    ("SDN", "SSD"): True,
    ("SSD", "SDN"): True,
    # DRC-Rwanda (Second Congo War active 1998-2003)
    ("COD", "RWA"): True,
    ("RWA", "COD"): True,
    # India-Pakistan (Kargil War 1999, Kashmir dispute)
    ("IND", "PAK"): True,
    ("PAK", "IND"): True,
}

# ── Alliances: mutual defense pacts ──
ALLIANCES = {
    # NATO (as of 2000)
    ("USA", "GBR"): True, ("USA", "FRA"): True, ("USA", "DEU"): True,
    ("USA", "ITA"): True, ("USA", "CAN"): True, ("USA", "ESP"): True,
    ("USA", "NLD"): True, ("USA", "BEL"): True, ("USA", "PRT"): True,
    ("USA", "DNK"): True, ("USA", "NOR"): True, ("USA", "GRC"): True,
    ("USA", "TUR"): True, ("USA", "POL"): True, ("USA", "CZE"): True,
    ("USA", "HUN"): True, ("USA", "ISL"): True, ("USA", "LUX"): True,
    ("GBR", "FRA"): True, ("GBR", "DEU"): True, ("GBR", "ITA"): True,
    ("GBR", "CAN"): True, ("GBR", "ESP"): True, ("GBR", "NLD"): True,
    ("GBR", "BEL"): True, ("GBR", "PRT"): True, ("GBR", "DNK"): True,
    ("GBR", "NOR"): True, ("GBR", "GRC"): True, ("GBR", "TUR"): True,
    ("GBR", "POL"): True, ("GBR", "CZE"): True, ("GBR", "HUN"): True,
    ("GBR", "ISL"): True, ("GBR", "LUX"): True,
    ("FRA", "DEU"): True, ("FRA", "ITA"): True, ("FRA", "ESP"): True,
    ("FRA", "NLD"): True, ("FRA", "BEL"): True,
    ("DEU", "FRA"): True, ("DEU", "ITA"): True, ("DEU", "NLD"): True,
    ("DEU", "BEL"): True, ("DEU", "POL"): True, ("DEU", "CZE"): True,
    ("ITA", "FRA"): True, ("ITA", "DEU"): True, ("ITA", "ESP"): True,
    ("CAN", "USA"): True, ("CAN", "GBR"): True, ("CAN", "FRA"): True,
    ("ESP", "FRA"): True, ("ESP", "ITA"): True,
    ("NLD", "BEL"): True, ("NLD", "DEU"): True,
    ("BEL", "NLD"): True, ("BEL", "FRA"): True,
    ("POL", "CZE"): True, ("POL", "HUN"): True,
    ("CZE", "POL"): True, ("CZE", "SVK"): True,
    ("HUN", "POL"): True, ("HUN", "ROU"): True,
    ("ROU", "HUN"): True, ("ROU", "BGR"): True,
    ("LTU", "LVA"): True, ("LTU", "EST"): True, ("LVA", "EST"): True,
    ("LTU", "POL"): True, ("LVA", "POL"): True, ("EST", "FIN"): True,
    ("SVK", "CZE"): True, ("SVN", "ITA"): True,
    ("HRV", "HUN"): True, ("HRV", "ITA"): True,
    ("ALB", "ITA"): True, ("ALB", "GRC"): True,
    ("MNE", "HRV"): True, ("MNE", "ITA"): True,
    ("MKD", "GRC"): True, ("MKD", "ALB"): True,
    ("BGR", "ROU"): True, ("BGR", "GRC"): True,
    # CSTO / Russia allies
    ("RUS", "BLR"): True, ("RUS", "KAZ"): True, ("RUS", "KGZ"): True,
    ("RUS", "TJK"): True, ("RUS", "ARM"): True,
    ("BLR", "RUS"): True, ("KAZ", "RUS"): True,
    ("KGZ", "RUS"): True, ("TJK", "RUS"): True,
    ("ARM", "RUS"): True,
    # China — Pakistan (all-weather friendship)
    ("CHN", "PAK"): True, ("PAK", "CHN"): True,
    # China — North Korea (1961 mutual defense treaty)
    ("CHN", "PRK"): True, ("PRK", "CHN"): True,
    # China — Myanmar, Laos, Cambodia (close ties, not formal alliances but treated as such)
    ("CHN", "MMR"): True, ("CHN", "LAO"): True, ("CHN", "KHM"): True,
    # India — Russia
    ("IND", "RUS"): True, ("RUS", "IND"): True,
    # GCC / Arab League
    ("SAU", "ARE"): True, ("SAU", "KWT"): True, ("SAU", "QAT"): True,
    ("SAU", "OMN"): True, ("SAU", "BHR"): True,
    ("ARE", "SAU"): True, ("KWT", "SAU"): True,
    ("QAT", "SAU"): True, ("OMN", "SAU"): True,
    ("BHR", "SAU"): True,
    ("EGY", "SAU"): True, ("EGY", "JOR"): True, ("EGY", "ARE"): True,
    ("JOR", "EGY"): True, ("JOR", "SAU"): True,
    # Comoros — Arab League / African Union
    ("COM", "SAU"): True, ("COM", "ARE"): True, ("COM", "EGY"): True,
    ("COM", "FRA"): True,  # former colonial power, Mayotte dispute
    ("COM", "TZA"): True,  # Indian Ocean neighbor
    # Cape Verde — PALOP / African Union
    ("CPV", "PRT"): True, ("CPV", "BRA"): True,
    ("CPV", "SEN"): True, ("CPV", "GMB"): True,
    # Sao Tome & Principe — PALOP / African Union
    ("STP", "PRT"): True, ("STP", "BRA"): True,
    ("STP", "AGO"): True, ("STP", "GAB"): True,
    # ECOWAS
    ("NGA", "GHA"): True, ("NGA", "SEN"): True, ("NGA", "CIV"): True,
    ("GHA", "NGA"): True, ("SEN", "MLI"): True,
    # East African Community
    ("KEN", "TZA"): True, ("KEN", "UGA"): True,
    ("TZA", "KEN"): True, ("UGA", "KEN"): True,
    ("RWA", "UGA"): True, ("UGA", "RWA"): True,
    # South America / Mercosur
    ("BRA", "ARG"): True, ("BRA", "URY"): True, ("BRA", "CHL"): True,
    ("ARG", "BRA"): True, ("ARG", "URY"): True, ("ARG", "CHL"): True,
    ("CHL", "ARG"): True, ("CHL", "PER"): True,
    ("PER", "CHL"): True, ("PER", "COL"): True,
    ("COL", "PER"): True, ("COL", "BRA"): True,
    # Trinidad & Tobago — CARICOM / Americas
    ("TTO", "USA"): True, ("TTO", "GBR"): True,
    ("TTO", "CAN"): True, ("TTO", "BRA"): True,
    # ASEAN
    ("IDN", "MYS"): True, ("IDN", "SGP"): True, ("IDN", "THA"): True,
    ("MYS", "SGP"): True, ("MYS", "IDN"): True, ("MYS", "BRN"): True,
    ("SGP", "MYS"): True, ("SGP", "IDN"): True,
    ("THA", "MYS"): True, ("THA", "LAO"): True, ("THA", "KHM"): True,
    ("VNM", "LAO"): True, ("VNM", "KHM"): True,
    ("PHL", "USA"): True, ("PHL", "JPN"): True,
    # ANZUS
    ("AUS", "NZL"): True, ("AUS", "USA"): True, ("AUS", "GBR"): True,
    ("NZL", "AUS"): True, ("NZL", "USA"): True,
    ("PNG", "AUS"): True, ("FJI", "AUS"): True,
    # Solomon Islands — Australia (defense cooperation)
    ("SLB", "AUS"): True, ("AUS", "SLB"): True,
    # New Caledonia — France (overseas territory defense)
    ("NCL", "FRA"): True, ("FRA", "NCL"): True,
    # Europe
    ("FRA", "DEU"): True, ("FRA", "GBR"): True,
    ("DEU", "AUT"): True, ("DEU", "CHE"): True,
    ("AUT", "DEU"): True, ("CHE", "DEU"): True,
    ("SWE", "NOR"): True, ("SWE", "FIN"): True, ("SWE", "DNK"): True,
    ("NOR", "SWE"): True, ("NOR", "DNK"): True,
    ("FIN", "SWE"): True, ("FIN", "EST"): True,
    ("DNK", "SWE"): True, ("DNK", "NOR"): True,
    ("ISL", "DNK"): True, ("ISL", "NOR"): True,
    ("IRL", "GBR"): True,
    ("SRB", "GRC"): True, ("SRB", "RUS"): True,
    ("RUS", "SRB"): True,
    # Cyprus-Greece (defense cooperation)
    ("CYP", "GRC"): True, ("GRC", "CYP"): True,
    # Moldova-Romania (special relationship)
    ("MDA", "ROU"): True, ("ROU", "MDA"): True,
    # Middle East
    ("IRN", "SYR"): True, ("SYR", "IRN"): True,
    ("TUR", "AZE"): True, ("AZE", "TUR"): True,
    ("TUR", "PAK"): True, ("PAK", "TUR"): True,
    ("SAU", "PAK"): True, ("PAK", "SAU"): True,
    # South Asia
    ("IND", "BGD"): True, ("BGD", "IND"): True,
    ("NPL", "IND"): True, ("IND", "NPL"): True,
    ("BTN", "IND"): True, ("IND", "BTN"): True,
}

# ── Guarantees: one-sided defense guarantees ──
GURANTEES = {
    # USA guarantees: Japan (Article 5), South Korea, Israel, Taiwan
    ("USA", "JPN"): True, ("USA", "KOR"): True, ("USA", "ISR"): True,
    ("USA", "TWN"): True,
    # France guarantees: former African colonies (defense agreements)
    ("FRA", "CIV"): True, ("FRA", "SEN"): True, ("FRA", "GAB"): True,
    ("FRA", "DJI"): True, ("FRA", "TCD"): True, ("FRA", "MLI"): True,
    ("FRA", "BEN"): True, ("FRA", "NER"): True, ("FRA", "BFA"): True,
    ("FRA", "MRT"): True,
    # Turkey guarantees: Northern Cyprus
    ("TUR", "XNC"): True,
    # UK guarantees: Brunei, Falkland Islands, South Georgia
    ("GBR", "BRN"): True,
    ("GBR", "FLK"): True,
    ("GBR", "SGS"): True,
    # Sweden guarantees: Åland (demilitarized autonomous status)
    ("SWE", "ALA"): True,
}

# ── Non-aggression pacts / strategic partnerships ──
NON_AGRESSION = {
    # Russia-China: strategic partnership, not mutual defense
    ("RUS", "CHN"): True, ("CHN", "RUS"): True,
    # India-China (various border agreements, though tensions exist)
    ("IND", "CHN"): True, ("CHN", "IND"): True,
}


def main():
    result = {}
    for iso1 in RELEVANT:
        for iso2 in RELEVANT:
            if iso1 >= iso2:
                continue
            pair = (iso1, iso2)
            pair_rev = (iso2, iso1)

            war = WARS.get(pair, False) or WARS.get(pair_rev, False)
            ally = ALLIANCES.get(pair, False) or ALLIANCES.get(pair_rev, False)
            guarantee = GURANTEES.get(pair, False) or GURANTEES.get(pair_rev, False)
            nonAgg = NON_AGRESSION.get(pair, False) or NON_AGRESSION.get(pair_rev, False)

            if war or ally or guarantee or nonAgg:
                entry = {}
                if war: entry["war"] = True
                if ally: entry["ally"] = True
                if guarantee: entry["guarantee"] = True
                if nonAgg: entry["nonAggression"] = True

                # For guarantees, store in the correct direction (protector→protected)
                if guarantee:
                    if pair in GURANTEES:
                        result.setdefault(iso1, {})[iso2] = entry
                        result.setdefault(iso2, {})  # ensure guaranteed country exists
                    elif pair_rev in GURANTEES:
                        result.setdefault(iso2, {})[iso1] = entry
                        result.setdefault(iso1, {})  # ensure guaranteed country exists
                else:
                    result.setdefault(iso1, {})[iso2] = entry
                # War/ally/nonAgg are symmetric; guarantee is one-way
                if not guarantee:
                    result.setdefault(iso2, {})[iso1] = dict(entry)

    # Propagate war through guarantee chains (bidirectional + iterative for convergence)
    # If A guarantees B, they share enemies: A fights B's enemies and B fights A's enemies
    changed = True
    while changed:
        changed = False
        for isoA, isoB in GURANTEES:
            if isoA not in result or isoB not in result:
                continue
            rel = result[isoA].get(isoB, {})
            if not rel.get("guarantee"):
                continue
            # A guarantees B — find B's enemies, A joins
            for isoC, relBC in list(result.get(isoB, {}).items()):
                if relBC.get("war") and isoA != isoC:
                    # Only add if not already present
                    existing = result.get(isoA, {}).get(isoC, {})
                    if not existing.get("war"):
                        result.setdefault(isoA, {})[isoC] = result.get(isoA, {}).get(isoC, {})
                        result[isoA][isoC]["war"] = True
                        result.setdefault(isoC, {})[isoA] = result.get(isoC, {}).get(isoA, {})
                        result[isoC][isoA]["war"] = True
                        changed = True
            # Also find A's enemies, B joins (bidirectional)
            for isoC, relAC in list(result.get(isoA, {}).items()):
                if relAC.get("war") and isoB != isoC:
                    existing = result.get(isoB, {}).get(isoC, {})
                    if not existing.get("war"):
                        result.setdefault(isoB, {})[isoC] = result.get(isoB, {}).get(isoC, {})
                        result[isoB][isoC]["war"] = True
                        result.setdefault(isoC, {})[isoB] = result.get(isoC, {}).get(isoB, {})
                        result[isoC][isoB]["war"] = True
                        changed = True

    with open(OUT, "w") as f:
        json.dump(result, f, indent=2)
    num_relations = sum(len(v) for v in result.values())
    print(f"Saved {OUT} ({len(result)} countries, {num_relations} bilateral relations)")


if __name__ == "__main__":
    main()
