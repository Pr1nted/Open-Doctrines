#!/usr/bin/env python3
"""
Restore states that existed on each scenario's own start date.

    python3 tools/fix_map_history.py --check          # report, change nothing
    python3 tools/fix_map_history.py                  # rewrite every map
    python3 tools/fix_map_history.py --map 1945       # just one

This is the sibling of fix_1939_history.py, which did the same job for The
Gathering Storm. Every scenario was built from one modern province raster and
one modern country table, so each inherits the same two errors in mirror image.

The early maps lost states: whoever is big today swallowed whoever was small
then. September 1945 shipped with no Japan at all -- the home islands were
simply American -- and with Syria, Lebanon, Egypt, Iraq and Austria folded into
the powers that had occupied or mandated them.

The later maps gained states that did not exist yet. October 1962 shipped with
Croatia, Slovenia, Bosnia, Macedonia and Serbia as five countries thirty years
before Yugoslavia broke up, with Czechia and Slovakia as two, with Bangladesh
nine years early and Namibia twenty-eight -- and with one Vietnam in the ninth
year of a war fought over the fact that there were two.

So this tool adds, merges, splits and renames. Every scenario's own map_date is
the only authority: "Modern Day" is dated January 2000, which is why South
Sudan and Montenegro are NOT added to it -- they were not independent yet.

HOW A PROVINCE IS CHOSEN

By WHERE IT IS, never by its id. Each entry below names a list of (lon, lat)
anchors and the tool asks the map which province covers each one.

That is not a style preference. Province ids are output of the generator, not
input to it: `rebuild_geometry()` re-cuts the province layer along each
scenario's own borders, so the ids shift whenever anything upstream of it
changes -- a different base map, a different clustering, a coastline edited by
a pixel. This table used to hold ids, and by the time anyone checked, they had
drifted. Run against freshly generated maps it proposed building Occupied Japan
out of five Venezuelan provinces and the Republic of Vietnam out of Vanuatu and
Yemen, printed as calmly as a correct result, because a stale integer is still
a valid integer. Nothing warns you; you get a plausible-looking map with Japan
in South America.

An anchor cannot fail that way. If a re-cut moves a border the anchor stays in
the same place on the Earth and resolves to whatever province now holds that
ground, and if it resolves to open sea or to a province the entry did not
expect, the tool says so and skips it.

Anchors are the DEEPEST INTERIOR POINT of the province they were derived from
-- the point furthest from any of its boundaries -- so that a province being
reshaped around the edges does not move the anchor out of it. Re-derive them
with tools/reanchor_map_history.py if the base map is ever recut so far that
one lands in the wrong country; the tool refuses silently-wrong output rather
than guessing.

Which provinces an entry SHOULD take was decided by the pixel test described
below: every province is rasterised to lon/lat and tested against a real
outline of the claimant for that year, and it moves only if at least 85% of its
land falls inside. That threshold is what keeps this honest, and it is why
several entries are deliberately partial:

  * Iraq       gets the two provinces that are Iraq. The Jordanian panhandle
               (55% inside) stays British, because it was Transjordan.
  * Tibet      gets U-Tsang and western Kham. Qinghai/Amdo (65%) stays Chinese
               -- it was Ma-clique territory, not administered from Lhasa.
  * Japan      does not get the Ryukyus. Okinawa was under US military
               government from 1945 until 1972, so American is correct there.
  * Nepal      gets both of its provinces here. fix_1939_history.py took only
               the western one on a cruder bounding-box test; the pixel test
               shows the eastern province is 98% inside Nepal.

WHAT IS NOT HERE, AND WHY

  * Montenegro 1914 has no province of its own at this resolution, so it
               cannot be restored by reassignment. 1918 does have one, and
               gets it.
  * Hejaz      independent of the Ottomans since 1916, but the Red Sea coast
               is not cut finely enough for any province to be 85% Hejaz.
  * Timor-Leste  its island is one province shared with Indonesian West Timor
               (60%), so it needs raster carving rather than reassignment.
  * Microstates  Malta, Bahrain, Singapore, Andorra and twenty-odd island
               states are smaller than one province and cannot be represented.
  * Eritrea    kept as a separate entity on the 1962 map. It was not sovereign,
               but it was a federated unit with its own assembly and flag until
               the federation was dissolved in November 1962 -- a month after
               that scenario opens.

FLAGS ARE DRAWN, NOT DOWNLOADED

Each new country gets a procedural flag built from the engine's own pattern and
symbol vocabulary (see parseFlag/parseSymbolType in src/map/CountryMap.cpp), so
nothing here adds a third-party licence obligation. Some are necessarily
approximations, and say so in their note.
"""

import json
import os
import shutil
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")

# Reused verbatim across scenarios: the same state, the same flag, only the
# province numbers differ between rasters.
PANAMA_FLAG = {"type": "quartered",
               "colors": ["#ffffff", "#d21034", "#005293", "#ffffff"],
               "symbols": [{"type": "star_5", "x": 0.25, "y": 0.28, "size": 0.18,
                            "colors": ["#005293"]},
                           {"type": "star_5", "x": 0.75, "y": 0.72, "size": 0.18,
                            "colors": ["#d21034"]}]}
# The moon flies on the UPPER pennant and the sun on the lower; the first
# pass had them the wrong way round.
NEPAL_FLAG = {"type": "solid", "colors": ["#dc143c"],
              "symbols": [{"type": "crescent", "x": 0.5, "y": 0.30, "size": 0.26,
                           "colors": ["#ffffff"]},
                          {"type": "sun", "x": 0.5, "y": 0.70, "size": 0.28,
                           "colors": ["#ffffff"]}]}
TIBET_FLAG = {"type": "sunburst", "colors": ["#ffd700", "#ce1126", "#1e4d9b"],
              "symbols": [{"type": "disc", "x": 0.5, "y": 0.30, "size": 0.14,
                           "colors": ["#ffd700"]},
                          {"type": "mountain", "x": 0.5, "y": 0.62, "size": 0.34,
                           "colors": ["#ffffff"]}]}
# The Bogd Khaanate, not the later People's Republic: a gold field, no red.
BOGD_FLAG = {"type": "solid", "colors": ["#f2c50c"],
             "symbols": [{"type": "sun", "x": 0.5, "y": 0.5, "size": 0.40,
                          "colors": ["#c8102e"]}]}
MONTENEGRO_FLAG = {"type": "hstripes_3",
                   "colors": ["#c8102e", "#0c4076", "#ffffff"]}
# Muscat and Oman flew a plain red field until 1970, when Qaboos added the
# white and green bands and the national emblem. Nothing to draw but the field.
OMAN_FLAG = {"type": "solid", "colors": ["#c8102e"]}
OMAN_NOTE = ("The Sultanate of Muscat and Oman, which was never a British "
             "possession. Britain's protectorates on that coast were the "
             "Trucial States, Qatar, Bahrain and Kuwait -- treaties that took "
             "over the signatory's foreign relations. Muscat signed treaties "
             "of friendship and commerce instead, kept its own foreign "
             "relations, and was a sovereign sultanate throughout. The "
             "Trucial coast and the Buraimi provinces stay British, because "
             "there the map was right. The interior Imamate, in revolt from "
             "1954, is not modelled separately -- no province is mostly it.")

PANAMA_NOTE = "Independent of Colombia since 1903."
NEPAL_NOTE = ("Never colonised; the 1923 treaty only confirmed what was already "
              "true. The double pennant cannot be drawn from rectangles, so the "
              "sun and moon sit on a crimson field instead.")
TIBET_NOTE = ("De facto independent from 1913 until 1950: its own government, "
              "army and currency, and no Chinese administration. Qinghai stays "
              "Chinese -- it was never governed from Lhasa. The flag is the "
              "real one minus its snow lions, which have no equivalent symbol: "
              "twelve red and blue rays, a gold sun, a white snow mountain.")
BOGD_NOTE = ("The Bogd Khaanate, which declared independence from the Qing in "
             "1911 and was recognised as autonomous at Kyakhta in 1915. Inner "
             "Mongolia stays Chinese. The soyombo has no equivalent symbol.")

# map -> iso -> spec.  Province lists come from the pixel test described above.
PLAN = {
    "1914.odmap": {
        "PAN": dict(name="Panama", color="#3f6fb5", treasury=5.0,
                    compass={"left": 15, "auth": 25}, flag=PANAMA_FLAG,
                    at=[(-80.9692, 8.2837), (-82.4634, 8.7231), (-77.7173, 8.1958)], note=PANAMA_NOTE),
        "NPL": dict(name="Nepal", color="#b03050", treasury=4.0,
                    compass={"left": 10, "auth": 70}, flag=NEPAL_FLAG,
                    at=[(83.5181, 28.3228), (86.6821, 27.2681)], note=NEPAL_NOTE),
        "TIB": dict(name="Tibet", color="#c8a02c", treasury=3.0,
                    compass={"left": 0, "auth": 60}, flag=TIBET_FLAG,
                    at=[(81.145, 31.2671), (83.6938, 30.564), (95.0317, 31.3989),
                               (86.5503, 32.5415)], note=TIBET_NOTE),
        "MNG": dict(name="Bogd Khanate of Mongolia", color="#c9a227", treasury=3.0,
                    compass={"left": 20, "auth": 80}, flag=BOGD_FLAG,
                    at=[(107.8198, 47.7026), (94.0649, 47.9663), (100.7007, 47.3071),
                               (103.7329, 48.4937), (96.7456, 45.9888), (114.6753, 48.8452),
                               (106.106, 43.0884), (102.8101, 45.3296), (92.3511, 46.3843),
                               (100.7007, 43.7476), (109.27, 44.2749)],
                    note=BOGD_NOTE),
    },
    "1918.odmap": {
        "PAN": dict(name="Panama", color="#3f6fb5", treasury=5.0,
                    compass={"left": 15, "auth": 25}, flag=PANAMA_FLAG,
                    at=[(-80.9692, 8.2837), (-82.4634, 8.7231), (-77.7173, 8.1958)], note=PANAMA_NOTE),
        "NPL": dict(name="Nepal", color="#b03050", treasury=4.0,
                    compass={"left": 10, "auth": 70}, flag=NEPAL_FLAG,
                    at=[(83.5181, 28.3228), (86.6821, 27.2681)], note=NEPAL_NOTE),
        "TIB": dict(name="Tibet", color="#c8a02c", treasury=3.0,
                    compass={"left": 0, "auth": 60}, flag=TIBET_FLAG,
                    at=[(81.145, 31.2671), (83.6938, 30.564), (95.0317, 31.3989),
                               (86.5503, 32.5415)], note=TIBET_NOTE),
        "MNG": dict(name="Bogd Khanate of Mongolia", color="#c9a227", treasury=3.0,
                    compass={"left": 20, "auth": 80}, flag=BOGD_FLAG,
                    at=[(107.8198, 47.7026), (94.0649, 47.9663), (100.7007, 47.3071),
                               (103.7329, 48.4937), (96.7456, 45.9888), (114.6753, 48.8452),
                               (106.106, 43.0884), (102.8101, 45.3296), (92.3511, 46.3843),
                               (100.7007, 43.7476), (109.27, 44.2749)],
                    note=BOGD_NOTE),
        "MNE": dict(name="Kingdom of Montenegro", color="#b0453a", treasury=2.0,
                    compass={"left": 5, "auth": 65}, flag=MONTENEGRO_FLAG,
                    at=[(19.0942, 42.561)],
                    note="A sovereign kingdom until it was voted into Yugoslavia "
                         "in November 1918, six weeks after this scenario opens. "
                         "Austro-Hungarian occupation since 1916 did not end the "
                         "state; the king and government were in exile."),
        "NJD": dict(name="Emirate of Nejd and Hasa", color="#2e7d4f", treasury=2.0,
                    compass={"left": 10, "auth": 85},
                    flag={"type": "solid", "colors": ["#006c35"],
                          "symbols": [{"type": "crossed_swords", "x": 0.5, "y": 0.5,
                                       "size": 0.40, "colors": ["#ffffff"]}]},
                    at=[(51.1304, 20.5444), (50.3833, 23.4448), (44.231, 26.9604),
                               (44.8022, 23.2251), (46.7798, 17.688)],
                    note="Ibn Saud's emirate, which took Riyadh in 1902 and "
                         "Hasa from the Ottomans in 1913. Britain recognised it "
                         "as independent at Darin in 1915 -- allied, subsidised, "
                         "and never a British possession. The map had the whole "
                         "Arabian interior as British."),
        "HJZ": dict(name="Kingdom of Hejaz", color="#c0392b", treasury=3.0,
                    compass={"left": 5, "auth": 75},
                    flag={"type": "hstripes_3",
                          "colors": ["#000000", "#007a3d", "#ffffff"],
                          "symbols": [{"type": "triangle", "x": 0.16, "y": 0.5,
                                       "size": 0.45, "colors": ["#ce1126"]}]},
                    at=[(40.7153, 21.7749), (42.5171, 18.3032)],
                    note="Hussein bin Ali proclaimed it in 1916 and it was the "
                         "Arab Revolt's own state, recognised by Britain and "
                         "France; by October 1918 its forces were in Damascus. "
                         "It stayed independent until Ibn Saud took it in 1925."),
        "UKR": dict(name="Ukrainian State", color="#3b7dd8", treasury=9.0,
                    compass={"left": 10, "auth": 75},
                    flag={"type": "hstripes_2", "colors": ["#0057b7", "#ffd700"]},
                    at=[(37.1118, 48.6255), (27.7075, 49.2407), (30.6519, 47.8345),
                               (26.8286, 50.7788)],
                    note="Skoropadskyi's Hetmanate, which governed Ukraine under "
                         "German protection from April 1918. The map already had "
                         "these provinces as German-occupied; this makes the "
                         "client state itself visible. Northern Chernihiv "
                         "71%) straddles the Russian border and stays put."),
    },
    "1945.odmap": {
        "JPN": dict(name="Occupied Japan", color="#bc3b3b", treasury=18.0,
                    compass={"left": 0, "auth": 40},
                    flag={"type": "solid", "colors": ["#ffffff"],
                          "symbols": [{"type": "disc", "x": 0.5, "y": 0.5,
                                       "size": 0.42, "colors": ["#bc002d"]}]},
                    at=[(133.1323, 34.9146), (137.7026, 35.8374), (139.7681, 37.1558),
                        (141.2183, 40.2319), (141.0425, 42.7808), (142.4048, 43.6597),
                        (143.9429, 43.4839), (129.9243, 32.981), (131.2427, 32.8491)],
                    note="The scenario shipped with no Japan at all: the home "
                         "islands belonged to the United States. Japan kept its "
                         "government, emperor and civil administration under "
                         "SCAP, exactly as Allied Occupied Germany is already "
                         "modelled here. Korea and the Ryukyus "
                         "stay American -- both were under direct US military "
                         "government -- and the Kurils stay Soviet."),
        "PAN": dict(name="Panama", color="#3f6fb5", treasury=6.0,
                    compass={"left": 15, "auth": 25}, flag=PANAMA_FLAG,
                    at=[(-80.9692, 8.2837), (-82.4634, 8.7231), (-77.7173, 8.1958)], note=PANAMA_NOTE),
        "OMN": dict(name="Sultanate of Muscat and Oman", color="#a83232",
                    treasury=3.0, compass={"left": 0, "auth": 85},
                    flag=OMAN_FLAG,
                    at=[(58.2056, 22.1265), (54.6021, 18.0396), (56.0083, 19.314),
                               (53.02, 18.5669)], note=OMAN_NOTE),
        "EGY": dict(name="Kingdom of Egypt", color="#1e8449", treasury=22.0,
                    compass={"left": 5, "auth": 45},
                    flag={"type": "solid", "colors": ["#0f7d3d"],
                          "symbols": [{"type": "crescent_star", "x": 0.5, "y": 0.5,
                                       "size": 0.45, "colors": ["#ffffff"]}]},
                    at=[(32.3657, 27.8833), (28.2788, 26.6528), (32.2339, 25.2905),
                               (30.52, 29.8608)],
                    note="Independent since 1922 and a founder member of the UN "
                         "in June 1945, though British troops remained in the "
                         "Canal Zone. The Sudan was an Anglo-Egyptian "
                         "condominium and stays British."),
        "IRQ": dict(name="Kingdom of Iraq", color="#8e5a2b", treasury=16.0,
                    compass={"left": 0, "auth": 50},
                    flag={"type": "hstripes_3",
                          "colors": ["#000000", "#ffffff", "#007a3d"],
                          "symbols": [{"type": "star_7", "x": 0.30, "y": 0.5,
                                       "size": 0.22, "colors": ["#ce1126"]},
                                      {"type": "star_7", "x": 0.44, "y": 0.5,
                                       "size": 0.22, "colors": ["#ce1126"]}]},
                    at=[(42.5171, 32.8931), (45.6372, 31.1353)],
                    note="Independent and a League member since 1932. The stars "
                         "stand in for the hoist trapezoid, which has no pattern "
                         "here."),
        "SYR": dict(name="Syrian Republic", color="#2e8b57", treasury=8.0,
                    compass={"left": 10, "auth": 55},
                    flag={"type": "hstripes_3",
                          "colors": ["#007a3d", "#ffffff", "#000000"],
                          "symbols": [{"type": "star_5", "x": 0.38, "y": 0.5,
                                       "size": 0.16, "colors": ["#ce1126"]},
                                      {"type": "star_5", "x": 0.5, "y": 0.5,
                                       "size": 0.16, "colors": ["#ce1126"]},
                                      {"type": "star_5", "x": 0.62, "y": 0.5,
                                       "size": 0.16, "colors": ["#ce1126"]}]},
                    at=[(37.4194, 34.563), (38.8257, 35.9692), (37.0239, 36.0132),
                               (40.7593, 36.5405), (39.2651, 34.519), (36.4966, 32.937)],
                    note="Independence was proclaimed in 1944 and recognised by "
                         "the UN Charter in June 1945; the French bombarded "
                         "Damascus in May 1945 trying to reverse it and were "
                         "forced out. Calling Syria French in September 1945 has "
                         "it backwards. The eastern desert strip (81% inside) "
                         "straddles the Iraqi border and stays put."),
        "LBN": dict(name="Lebanon", color="#c0392b", treasury=5.0,
                    compass={"left": 15, "auth": 35},
                    flag={"type": "hstripes_3",
                          "colors": ["#ed1c24", "#ffffff", "#ed1c24"],
                          "symbols": [{"type": "tree", "x": 0.5, "y": 0.5,
                                       "size": 0.30, "colors": ["#00a850"]}]},
                    at=[(36.1011, 34.2114)],
                    note="Independent since 1943 and a UN founder member."),
        "AUT": dict(name="Occupied Austria", color="#d95f5f", treasury=6.0,
                    compass={"left": 0, "auth": 35},
                    flag={"type": "hstripes_3",
                          "colors": ["#ed2939", "#ffffff", "#ed2939"]},
                    at=[(12.4585, 47.2192), (14.48, 47.1313), (15.7544, 47.9224),
                               (13.5571, 47.9224)],
                    note="Re-established as a state in April 1945 with Renner's "
                         "provisional government, and occupied in four zones like "
                         "Germany but separately from it. The Allies had "
                         "committed to a restored Austria at Moscow in 1943."),
        "NPL": dict(name="Nepal", color="#b03050", treasury=4.0,
                    compass={"left": 10, "auth": 70}, flag=NEPAL_FLAG,
                    at=[(83.5181, 28.3228), (86.6821, 27.2681)], note=NEPAL_NOTE),
        "TIB": dict(name="Tibet", color="#c8a02c", treasury=3.0,
                    compass={"left": 0, "auth": 60}, flag=TIBET_FLAG,
                    at=[(81.145, 31.2671), (83.6938, 30.564), (95.0317, 31.3989),
                               (91.604, 29.6411), (86.5503, 32.5415)], note=TIBET_NOTE),
        "YEM": dict(name="Kingdom of Yemen", color="#a93226", treasury=3.0,
                    compass={"left": 5, "auth": 75},
                    flag={"type": "solid", "colors": ["#ce1126"],
                          "symbols": [{"type": "sword", "x": 0.5, "y": 0.5,
                                       "size": 0.42, "colors": ["#ffffff"]}]},
                    at=[(50.9985, 16.9849), (44.0991, 13.4253), (45.4175, 13.9966),
                               (48.6694, 14.7876)],
                    note="The Mutawakkilite Kingdom, independent of the "
                         "Ottomans since 1918 and a UN member from 1947. It is "
                         "on the 1939 map and was missing here. Aden and the "
                         "Hadhramaut stay British, which is correct until 1967."),
    },
    "1962.odmap": {
        "RVN": dict(name="Republic of Vietnam", color="#e0b83c", treasury=7.0,
                    compass={"left": 40, "auth": 70},
                    flag={"type": "hstripes_3",
                          "colors": ["#ffcd00", "#da251d", "#ffcd00"]},
                    at=[(108.479, 12.8979), (107.8638, 15.7104), (105.7983, 10.1294),
                               (107.4683, 11.1401)],
                    note="The map shipped one Vietnam. There were two, divided "
                         "at the 17th parallel by the 1954 Geneva Accords and "
                         "eight years into a war over it. Quang Nam "
                         "straddles the line and is 76% southern, so it goes "
                         "south. The three red stripes are approximated by a "
                         "single band."),
        "OMN": dict(name="Sultanate of Muscat and Oman", color="#a83232",
                    treasury=4.0, compass={"left": 0, "auth": 85},
                    flag=OMAN_FLAG,
                    at=[(58.2056, 22.1265), (56.0962, 19.4458), (56.4038, 23.7964),
                               (54.6021, 18.0396), (53.02, 18.5669)], note=OMAN_NOTE),
    },
    "1939.odmap": {
        "SVK": dict(name="Slovak Republic", color="#5b8fd6", treasury=7.0,
                    compass={"left": -10, "auth": 85},
                    flag={"type": "hstripes_3",
                          "colors": ["#ffffff", "#0b4ea2", "#ee1c25"],
                          "symbols": [{"type": "cross_pattee", "x": 0.34, "y": 0.5,
                                       "size": 0.26, "colors": ["#ee1c25"]}]},
                    at=[(20.2808, 48.8013)],
                    note="Independent from 14 March 1939 -- a German client "
                         "state, but a separate one, and it joined the invasion "
                         "of Poland as a belligerent in its own right. The map "
                         "had it as Hungarian. Southern Slovakia (80% "
                         "inside) really was Hungarian after the First Vienna "
                         "Award, so it stays."),
    },
    "map.odmap": {},
}

# Countries whose name is anachronistic for their own scenario's date. The
# state existed; it was called something else. Applied after any merge, so a
# merged survivor can be renamed to the union it actually was.
RENAME = {
    "1962.odmap": {
        "SRB": "Yugoslavia",
        "CZE": "Czechoslovakia",
        "VNM": "Democratic Republic of Vietnam",
        "MMR": "Burma",
        "LKA": "Ceylon",
        "BEN": "Dahomey",
        "BFA": "Upper Volta",
        "TZA": "Tanganyika",
        "MYS": "Federation of Malaya",
        "COD": "Republic of the Congo (Leopoldville)",
        "COG": "Republic of the Congo (Brazzaville)",
    },
    "map.odmap": {
        # January 2000: Serbia and Montenegro were one state, and it held the
        # Montenegrin province the map already gives to "Serbia".
        "SRB": "FR Yugoslavia",
    },
}

# Entities on the map that were not states at the scenario's date. Everything
# they hold goes to whoever actually governed it. map -> [(iso, into, why)]
#
# The line drawn here is sovereignty OR a separate government of its own. That
# is why Suriname, the Faroes, Aland and Eritrea survive the cut and Greenland
# does not: Greenland was an ordinary Danish county until home rule in 1979,
# whereas the other three each had their own assembly and executive in 1962.
MERGE = {
    "1962.odmap": [
        ("HRV", "SRB", "Republic of the SFRY, no separate foreign policy."),
        ("SVN", "SRB", "Republic of the SFRY."),
        ("BIH", "SRB", "Republic of the SFRY."),
        ("MKD", "SRB", "Republic of the SFRY."),
        ("SVK", "CZE", "The CSSR was a unitary state in 1962; Slovakia had no "
                       "government of its own between 1960 and 1968."),
        ("BGD", "PAK", "East Pakistan until 1971."),
        ("NAM", "ZAF", "South West Africa, administered by South Africa under a "
                       "lapsed League mandate."),
        ("GRL", "DNK", "A Danish county from 1953; home rule only came in 1979."),
        ("VUT", "GBR", "The New Hebrides, an Anglo-French condominium until "
                       "1980, with no government of its own."),
        ("PSE", "JOR", "There was no State of Palestine in 1962. Jordan had "
                       "annexed the West Bank in 1950 and Gaza was under "
                       "Egyptian military administration; this single province "
                       "is mostly the West Bank."),
    ],
}

# Provinces that belong to a state already on the map but were given to the
# wrong one. map -> [(province ids, from iso, to iso, why)]
REASSIGN = {
    "1914.odmap": [
        # Found by tools/check_map_history.py, not by eye. Germany had the
        # third largest colonial empire in 1914 and the scenario gave all of
        # it to whoever took it off them AFTER the war they had not yet lost.
        ([(17.3364, -24.5435), (18.2593, -19.314)], "GBR", "GER",
         "German South-West Africa. South Africa only invaded in 1915 and the "
         "mandate came in 1920."),
        ([(12.1948, 4.7681)], "FRA", "GER",
         "Kamerun. The Entente campaign began in August 1914 and the partition "
         "was 1916."),
        ([(34.7388, -2.5708), (31.9263, -9.2065), (37.3315, -8.6353),
          (35.6177, -5.1636)], "GBR", "GER",
         "German East Africa. Lettow-Vorbeck held it until the armistice; "
         "Tanganyika became a British mandate in 1920. The southwest, between "
         "Lake Tanganyika and Lake Nyasa and the largest of the four at "
         "11,848 px, was missed by the first pass and left British; it "
         "measures 77% inside the real colony."),
        ([(29.5093, -2.3511), (30.4321, -1.6479), (29.8608, -3.7134),
          (30.2563, -3.0542)], "BEL", "GER",
         "Ruanda-Urundi, the northwestern districts of German East Africa and "
         "not Belgian in July 1914. Belgian forces took them in 1916 and the "
         "mandate followed in 1922, so the map was running eight years ahead. "
         "All four measure 94% or better inside German East Africa."),
        ([(45.813, 9.646)], "GBR", "ITA", "Italian Somaliland."),
        ([(-0.1978, 8.8989)], "GBR", "GER", "Togoland, German until the 1914 campaign."),
        ([(-15.2271, 22.6099)], "FRA", "ESP",
         "Rio de Oro. The Spanish Sahara was Spanish, not French."),
        ([(21.687, 45.8569), (23.1812, 46.9995)], "ROU", "AUH",
         "Transylvania, Austro-Hungarian until 1918. Romania only gained it "
         "at Trianon, four years after this scenario opens."),
        ([(94.0649, 47.9663), (114.6753, 48.8452), (113.6206, 46.0767)], "CHN", "MNG",
         "Outer Mongolia. The first pass used bounding boxes and missed these; "
         "the easternmost measures 96% inside the real border, and an earlier "
         "rule here moved it the other way on a hand-traced polygon that was "
         "wrong."),
    ],
    "1918.odmap": [
        ([(27.8394, 47.8345), (28.7183, 46.6919), (28.8501, 47.6587)], "RUS", "ROU",
         "Bessarabia. The Moldavian Democratic Republic voted union with "
         "Romania in April 1918 and Romanian troops had held it since January; "
         "Soviet Russia had no presence there in October."),
        ([(20.896, 54.6899), (21.5991, 54.7778)], "RUS", "GER",
         "East Prussia and Memel. The modern raster has this as Kaliningrad, so "
         "the scenario handed Russia a province that was German from 1871 until "
         "1945 -- leaving the German Empire with no East Prussia at all."),
        ([(94.0649, 47.9663), (114.6753, 48.8452), (113.6206, 46.0767)], "CHN", "MNG",
         "As 1914, including the province an earlier rule here moved out of "
         "Mongolia; it measures 87% inside the real border."),
        ([(31.5747, 38.5181)], "ITA", "OTT",
         "Isparta and Konya, in central Anatolia, which this rule used to hand "
         "to ITALY. It was labelled 'Tripolitania and Cyrenaica' and named two "
         "province ids; the ids were Anatolian. Libya was already Italian from "
         "the scenario file, so the entry did nothing anyone would notice "
         "except quietly give Italy the middle of Turkey -- and it shipped "
         "that way. Italy did occupy Antalya and Konya, but from March 1919, "
         "five months after this scenario opens. The direction is reversed so "
         "that the maps already carrying the mistake are corrected, and a map "
         "generated fresh -- where that ground is Ottoman to begin with -- "
         "skips it."),
        ([(49.9438, 10.5249)], "GBR", "ITA", "Italian Somaliland."),
        ([(33.4204, 51.3501)], "GER", "UKR",
         "Left out of the Ukrainian State by a hand-traced polygon that put it "
         "at 71%; measured against the real border it is wholly Ukrainian."),
        # Found by bracketing October 1918 between the 1914 and 1920 snapshots:
        # where both agree and we differ, it is a real finding.
        ([(-15.2271, 22.6099)], "FRA", "ESP",
         "Rio de Oro. The Spanish Sahara was Spanish in both 1914 and 1920, so "
         "it was Spanish in between; the map had it French."),
        ([(143.064, 50.0317)], "RUS", "JPN",
         "Karafuto, southern Sakhalin, Japanese since the Treaty of Portsmouth "
         "in 1905."),
    ],
    "1939.odmap": [
        ([(29.0698, 46.2085)], "SOV", "ROU",
         "Bessarabia, Romanian from 1918 until the Soviet ultimatum of June "
         "1940. PARTIAL: this province is 67% Bessarabia and 33% the Moldavian "
         "ASSR east of the Dniester, which really was Soviet. Romania is much "
         "the closer answer, but the province cannot be split without carving."),
        ([(25.4224, 53.064)], "SOV", "POL",
         "The Kresy around Brest and Grodno -- 99% inside Poland's 1939 border, "
         "and Polish until the Soviet invasion of 17 September."),
        ([(20.896, 54.6899), (21.5991, 54.7778)], "SOV", "GER",
         "East Prussia and Memel, as 1918. Germany's easternmost province "
         "otherwise stopped at 18.8E, so the Reich was drawn without the "
         "exclave the Polish Corridor was famous for separating."),
        ([(106.106, 43.0884), (102.8101, 45.3296), (106.3257, 49.021),
          (103.6011, 43.0444)], "CHN", "MNG",
         "Outer Mongolia. fix_1939_history.py chose these by bounding box and "
         "missed four provinces that are 94-100% inside the real border."),
        # check_map_history.py measured this at 87% Mongolia against the real
        # 1938 border. My hand-traced polygon said 68% and I moved it out of
        # Mongolia on that basis, which was wrong. Put it back.
        ([(113.6206, 46.0767)], "CHN", "MNG", "Southeastern Mongolia, 87% inside the real border."),
        # 1939 note: the [720] MNG->CHN rule that used to sit below is gone.
        ([(47.9663, 6.7017), (44.1431, 3.1421), (42.2534, 0.6372)], "GBR", "ITA",
         "Italian Somaliland, Italian since the 1880s and not British until "
         "the 1941 campaign."),
        ([(49.021, 16.3257), (51.438, 17.8198)], "GBR", "YEM",
         "The Yemeni highlands. Aden and the Hadhramaut stay British, which is "
         "correct, but these two are the Mutawakkilite Kingdom."),
        ([(42.0776, 39.8804)], "TUR", "SOV",
         "Soviet Armenia. Turkey's eastern border was settled at Kars in 1921."),
        ([(40.188, 37.9028), (37.4634, 37.9468)], "TUR", "FRA",
         "Northern Syria under the French mandate. Turkey gained only Hatay, "
         "in June 1939, not the whole Jazira."),
        ([(24.7192, 49.6362)], "SOV", "POL",
         "More of the Kresy: Polish until the Soviet invasion of 17 September."),
        ([(-77.146, -3.8013)], "PER", "ECU",
         "Ecuadorian Amazon. Peru took it in the 1941 war and the Rio Protocol "
         "of 1942, two years after this scenario opens."),
    ],
    "1945.odmap": [
        ([(-13.5132, 25.3345)], "FRA", "ESP", "Spanish Sahara, not French."),
        ([(12.5903, 25.9497)], "GBR", "FRA",
         "The Fezzan, under French military administration from 1943 until "
         "Libyan independence."),
    ],
    "1962.odmap": [
        ([(44.0112, 9.8657), (46.0767, 8.9868), (47.9663, 10.2612),
          (45.813, 10.2612), (47.5708, 8.7671)], "GBR", "SOM",
         "British Somaliland joined Italian Somaliland to form independent "
         "Somalia on 1 July 1960, two years before this scenario opens."),
    ],
}

# Historically load-bearing, and cheap to state.
RELATIONS = {
    "1918.odmap": {"UKR": {"GER": {"ally": True}}},
    "1945.odmap": {"JPN": {"USA": {"war": False}}},
}


def province_raster(work):
    """id-per-pixel for the map being edited, as a numpy array."""
    import numpy as np
    from PIL import Image
    a = np.array(Image.open(os.path.join(work, "provinces.png")).convert("RGB"),
                 dtype=np.uint32)
    return a[:, :, 0] << 16 | a[:, :, 1] << 8 | a[:, :, 2]


def resolve(anchors, pid_arr, label):
    """Anchors -> the province ids covering them, in order and without repeats.

    An anchor over open sea, or two anchors that have landed in the same
    province because a re-cut merged them, are both reported rather than
    swallowed: they mean the anchor needs re-deriving, and a table that quietly
    does less than it says is how this went wrong the first time.
    """
    h, w = pid_arr.shape
    out, seen = [], set()
    for lon, lat in anchors:
        x = int((lon + 180.0) / 360.0 * w) % w
        y = min(h - 1, max(0, int((90.0 - lat) / 180.0 * h)))
        pid = int(pid_arr[y, x])
        if pid == 0:
            print(f"  WARN  {label}: anchor {lon},{lat} is open sea on this map",
                  file=sys.stderr)
            continue
        if pid in seen:
            print(f"  WARN  {label}: anchors {lon},{lat} and an earlier one "
                  f"both land in province {pid}", file=sys.stderr)
            continue
        seen.add(pid)
        out.append(pid)
    return out


def apply_map(name, check):
    path = os.path.join(MAPS_DIR, name)
    if not os.path.exists(path):
        print(f"  missing {path}", file=sys.stderr)
        return 1

    work = tempfile.mkdtemp(prefix="odmaphist_")
    try:
        with zipfile.ZipFile(path) as z:
            entries = z.namelist()
            z.extractall(work)

        def rd(fn):
            p = os.path.join(work, fn)
            return json.load(open(p)) if os.path.exists(p) else None

        countries = rd("countries.json")
        provinces = rd("provinces.json")
        compass = rd("country_compass.json") or {}
        relations = rd("relations.json") or {}
        armies = rd("armies.json") or {}
        ships = rd("ships.json")
        claims = rd("claims.json") or {}
        starting = rd("starting_policies.json") or {}
        meta = rd("metadata.json") or {}

        print(f"\n=== {name}  {meta.get('name','?')}  [{meta.get('map_date','?')}] ===")

        pid_arr = province_raster(work)
        have = {v["iso_a3"]: int(k) for k, v in countries.items()}
        next_id = max(int(k) for k in countries if int(k) < 60000) + 1
        moved = 0

        for iso, spec in PLAN.get(name, {}).items():
            if iso in have:
                print(f"  skip  {iso} already present")
                continue

            cid = next_id
            next_id += 1
            countries[str(cid)] = {
                "id": cid, "iso_a3": iso, "name": spec["name"],
                "color": spec["color"],
                "flag_actual": spec["flag"],
                "flag_censored": {"type": "solid", "colors": [spec["color"]],
                                  "censored": True},
                "treasury": spec["treasury"],
            }
            compass[iso] = spec["compass"]
            have[iso] = cid

            took = {}
            pids = resolve(spec["at"], pid_arr, iso)
            for pid in pids:
                pv = provinces.get(str(pid))
                if pv is None:
                    print(f"  WARN  {iso}: province {pid} does not exist",
                          file=sys.stderr)
                    continue
                took[pv["iso_a3"]] = took.get(pv["iso_a3"], 0) + 1
                pv["country_id"] = cid
                pv["iso_a3"] = iso
                # An army standing in a province that has changed hands belongs
                # to whoever owns the ground, not to the country it was filed
                # under -- otherwise every new state opens under occupation.
                for unit in armies.get(str(pid), []):
                    unit["country_id"] = cid
                moved += 1
            src = ", ".join(f"{n} from {k}" for k, n in sorted(took.items()))
            print(f"  add   {iso:4s} {spec['name']:26s} cid {cid:4d}  "
                  f"{len(pids)} provinces ({src})")

        for anchors, src, dst, _why in REASSIGN.get(name, []):
            if dst not in have:
                print(f"  WARN  cannot move to {dst}: not on this map", file=sys.stderr)
                continue
            pids = resolve(anchors, pid_arr, f"{src}->{dst}")
            n = 0
            for pid in pids:
                pv = provinces.get(str(pid))
                if pv is None or pv["iso_a3"] != src:
                    continue
                pv["country_id"] = have[dst]
                pv["iso_a3"] = dst
                for unit in armies.get(str(pid), []):
                    unit["country_id"] = have[dst]
                n += 1
                moved += 1
            print(f"  move  {n} provinces {src} -> {dst}")

        for iso, into, _why in MERGE.get(name, []):
            if iso not in have:
                print(f"  skip  {iso} not on this map")
                continue
            if into not in have:
                print(f"  WARN  cannot merge {iso}: {into} is not on this map",
                      file=sys.stderr)
                continue
            src_id, dst_id = have[iso], have[into]
            n = 0
            for pid, pv in provinces.items():
                if pv["country_id"] != src_id:
                    continue
                pv["country_id"] = dst_id
                pv["iso_a3"] = into
                for unit in armies.get(pid, []):
                    unit["country_id"] = dst_id
                n += 1
            for sh in (ships or []):
                if sh.get("country_id") == src_id:
                    sh["country_id"] = dst_id
            del countries[str(src_id)]
            compass.pop(iso, None)
            relations.pop(iso, None)
            for peer in relations.values():
                peer.pop(iso, None)
            claims.pop(iso, None)
            starting.get("starting_policies", {}).pop(iso, None)
            del have[iso]
            moved += n
            print(f"  merge {iso:4s} -> {into:4s}  {n} provinces")

        for iso, new_name in RENAME.get(name, {}).items():
            cid = have.get(iso)
            if cid is None:
                continue
            old = countries[str(cid)]["name"]
            if old == new_name:
                continue
            countries[str(cid)]["name"] = new_name
            print(f"  name  {iso:4s} {old!r} -> {new_name!r}")

        for iso, rel in RELATIONS.get(name, {}).items():
            if iso in have:
                relations.setdefault(iso, {}).update(rel)

        # An army filed under someone other than the owner of the ground it is
        # standing on. The engine has no notion of a turn-zero occupation, so
        # this reads as a foreign army loose inside a country's borders. Every
        # instance is an artefact of a province changing hands without its
        # garrison following -- fix_1939_history.py left eleven, which is why
        # 1939 is in the table above.
        adopted = 0
        for pid, units in armies.items():
            pv = provinces.get(pid)
            if pv is None:
                continue
            for unit in units:
                if unit["country_id"] != pv["country_id"]:
                    unit["country_id"] = pv["country_id"]
                    adopted += 1
        if adopted:
            print(f"  army  {adopted} garrisons handed to the province owner")

        # Country-keyed entries for countries that are not on the map. These
        # predate this tool -- Modern Day has relations with Singapore, Bahrain
        # and South Sudan, none of which it contains -- and a lookup on one
        # returns a country id that does not exist.
        stale = []
        for fn, obj in (("country_compass.json", compass),
                        ("relations.json", relations),
                        ("claims.json", claims),
                        ("starting_policies.json",
                         starting.get("starting_policies", {}))):
            for iso in sorted(set(obj) - set(have)):
                del obj[iso]
                stale.append(f"{fn.split('.')[0]}/{iso}")
        for peer in relations.values():
            for iso in sorted(set(peer) - set(have)):
                del peer[iso]
        if stale:
            print(f"  prune {len(stale)} dangling entries: {', '.join(stale)}")

        print(f"  {moved} provinces reassigned, {len(countries)} countries")
        if check:
            return 0

        for fn, obj in (("countries.json", countries),
                        ("provinces.json", provinces),
                        ("country_compass.json", compass),
                        ("relations.json", relations),
                        ("armies.json", armies),
                        ("ships.json", ships),
                        ("claims.json", claims),
                        ("starting_policies.json", starting)):
            if obj is None:
                continue
            with open(os.path.join(work, fn), "w", encoding="utf-8") as f:
                json.dump(obj, f, separators=(",", ":"))

        # Rewritten wholesale, preserving the original entry order so a diff of
        # the archive is about the data and not about zip bookkeeping.
        tmp = path + ".tmp"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for e in entries:
                src = os.path.join(work, e)
                if e.endswith("/"):
                    z.writestr(e, b"")
                else:
                    z.write(src, e)
        os.replace(tmp, path)
        print(f"  wrote {path}")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    check = "--check" in sys.argv
    only = None
    if "--map" in sys.argv:
        only = sys.argv[sys.argv.index("--map") + 1]
        if not only.endswith(".odmap"):
            only += ".odmap"

    rc = 0
    for name in PLAN:
        if only and name != only:
            continue
        rc |= apply_map(name, check)
    if check:
        print("\n--check: nothing written")
    return rc


if __name__ == "__main__":
    sys.exit(main())
