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

Provinces have no names, only pixels, so they can only be identified by where
they are. Every province is rasterised to lon/lat and tested against a real
outline of the claimant for that year; it moves only if at least 85% of its
land falls inside. That threshold is what keeps this honest, and it is why
several entries are deliberately partial:

  * Iraq       gets the two provinces that are Iraq. The Jordanian panhandle
               (369, 55%) stays British, because it was Transjordan.
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
                    provinces=[195, 196, 197], note=PANAMA_NOTE),
        "NPL": dict(name="Nepal", color="#b03050", treasury=4.0,
                    compass={"left": 10, "auth": 70}, flag=NEPAL_FLAG,
                    provinces=[594, 595], note=NEPAL_NOTE),
        "TIB": dict(name="Tibet", color="#c8a02c", treasury=3.0,
                    compass={"left": 0, "auth": 60}, flag=TIBET_FLAG,
                    provinces=[151, 152, 166, 167], note=TIBET_NOTE),
        "MNG": dict(name="Bogd Khanate of Mongolia", color="#c9a227", treasury=3.0,
                    compass={"left": 20, "auth": 80}, flag=BOGD_FLAG,
                    provinces=[170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180],
                    note=BOGD_NOTE),
    },
    "1918.odmap": {
        "PAN": dict(name="Panama", color="#3f6fb5", treasury=5.0,
                    compass={"left": 15, "auth": 25}, flag=PANAMA_FLAG,
                    provinces=[200, 201, 202], note=PANAMA_NOTE),
        "NPL": dict(name="Nepal", color="#b03050", treasury=4.0,
                    compass={"left": 10, "auth": 70}, flag=NEPAL_FLAG,
                    provinces=[609, 610], note=NEPAL_NOTE),
        "TIB": dict(name="Tibet", color="#c8a02c", treasury=3.0,
                    compass={"left": 0, "auth": 60}, flag=TIBET_FLAG,
                    provinces=[156, 157, 171, 172], note=TIBET_NOTE),
        "MNG": dict(name="Bogd Khanate of Mongolia", color="#c9a227", treasury=3.0,
                    compass={"left": 20, "auth": 80}, flag=BOGD_FLAG,
                    provinces=[175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185],
                    note=BOGD_NOTE),
        "MNE": dict(name="Kingdom of Montenegro", color="#b0453a", treasury=2.0,
                    compass={"left": 5, "auth": 65}, flag=MONTENEGRO_FLAG,
                    provinces=[1049],
                    note="A sovereign kingdom until it was voted into Yugoslavia "
                         "in November 1918, six weeks after this scenario opens. "
                         "Austro-Hungarian occupation since 1916 did not end the "
                         "state; the king and government were in exile."),
        "NJD": dict(name="Emirate of Nejd and Hasa", color="#2e7d4f", treasury=2.0,
                    compass={"left": 10, "auth": 85},
                    flag={"type": "solid", "colors": ["#006c35"],
                          "symbols": [{"type": "crossed_swords", "x": 0.5, "y": 0.5,
                                       "size": 0.40, "colors": ["#ffffff"]}]},
                    provinces=[617, 618, 619, 623, 627],
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
                    provinces=[621, 626],
                    note="Hussein bin Ali proclaimed it in 1916 and it was the "
                         "Arab Revolt's own state, recognised by Britain and "
                         "France; by October 1918 its forces were in Damascus. "
                         "It stayed independent until Ibn Saud took it in 1925."),
        "UKR": dict(name="Ukrainian State", color="#3b7dd8", treasury=9.0,
                    compass={"left": 10, "auth": 75},
                    flag={"type": "hstripes_2", "colors": ["#0057b7", "#ffd700"]},
                    provinces=[674, 675, 677, 678],
                    note="Skoropadskyi's Hetmanate, which governed Ukraine under "
                         "German protection from April 1918. The map already had "
                         "these provinces as German-occupied; this makes the "
                         "client state itself visible. Northern Chernihiv (676, "
                         "71%) straddles the Russian border and stays put."),
    },
    "1945.odmap": {
        "JPN": dict(name="Occupied Japan", color="#bc3b3b", treasury=18.0,
                    compass={"left": 0, "auth": 40},
                    flag={"type": "solid", "colors": ["#ffffff"],
                          "symbols": [{"type": "disc", "x": 0.5, "y": 0.5,
                                       "size": 0.42, "colors": ["#bc002d"]}]},
                    provinces=[1237, 1238, 1239, 1240, 1241, 1243, 1244, 1245,
                               1246, 1247],
                    note="The scenario shipped with no Japan at all: the home "
                         "islands belonged to the United States. Japan kept its "
                         "government, emperor and civil administration under "
                         "SCAP, exactly as Allied Occupied Germany is already "
                         "modelled here. Korea (1160) and the Ryukyus (1242) "
                         "stay American -- both were under direct US military "
                         "government -- and the Kurils stay Soviet."),
        "PAN": dict(name="Panama", color="#3f6fb5", treasury=6.0,
                    compass={"left": 15, "auth": 25}, flag=PANAMA_FLAG,
                    provinces=[182, 183, 184], note=PANAMA_NOTE),
        "EGY": dict(name="Kingdom of Egypt", color="#1e8449", treasury=22.0,
                    compass={"left": 5, "auth": 45},
                    flag={"type": "solid", "colors": ["#0f7d3d"],
                          "symbols": [{"type": "crescent_star", "x": 0.5, "y": 0.5,
                                       "size": 0.45, "colors": ["#ffffff"]}]},
                    provinces=[382, 383, 385, 386],
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
                    provinces=[368, 370],
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
                    provinces=[317, 318, 320, 321, 322, 323],
                    note="Independence was proclaimed in 1944 and recognised by "
                         "the UN Charter in June 1945; the French bombarded "
                         "Damascus in May 1945 trying to reverse it and were "
                         "forced out. Calling Syria French in September 1945 has "
                         "it backwards. The eastern desert strip (319, 81%) "
                         "straddles the Iraqi border and stays put."),
        "LBN": dict(name="Lebanon", color="#c0392b", treasury=5.0,
                    compass={"left": 15, "auth": 35},
                    flag={"type": "hstripes_3",
                          "colors": ["#ed1c24", "#ffffff", "#ed1c24"],
                          "symbols": [{"type": "tree", "x": 0.5, "y": 0.5,
                                       "size": 0.30, "colors": ["#00a850"]}]},
                    provinces=[324],
                    note="Independent since 1943 and a UN founder member."),
        "AUT": dict(name="Occupied Austria", color="#d95f5f", treasury=6.0,
                    compass={"left": 0, "auth": 35},
                    flag={"type": "hstripes_3",
                          "colors": ["#ed2939", "#ffffff", "#ed2939"]},
                    provinces=[646, 647, 648, 649],
                    note="Re-established as a state in April 1945 with Renner's "
                         "provisional government, and occupied in four zones like "
                         "Germany but separately from it. The Allies had "
                         "committed to a restored Austria at Moscow in 1943."),
        "NPL": dict(name="Nepal", color="#b03050", treasury=4.0,
                    compass={"left": 10, "auth": 70}, flag=NEPAL_FLAG,
                    provinces=[597, 598], note=NEPAL_NOTE),
        "TIB": dict(name="Tibet", color="#c8a02c", treasury=3.0,
                    compass={"left": 0, "auth": 60}, flag=TIBET_FLAG,
                    provinces=[150, 151, 165, 166, 167], note=TIBET_NOTE),
        "YEM": dict(name="Kingdom of Yemen", color="#a93226", treasury=3.0,
                    compass={"left": 5, "auth": 75},
                    flag={"type": "solid", "colors": ["#ce1126"],
                          "symbols": [{"type": "sword", "x": 0.5, "y": 0.5,
                                       "size": 0.42, "colors": ["#ffffff"]}]},
                    provinces=[616, 618, 619, 620],
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
                    provinces=[1513, 1514, 1518, 1519],
                    note="The map shipped one Vietnam. There were two, divided "
                         "at the 17th parallel by the 1954 Geneva Accords and "
                         "eight years into a war over it. Quang Nam (1514) "
                         "straddles the line and is 76% southern, so it goes "
                         "south. The three red stripes are approximated by a "
                         "single band."),
    },
    "1939.odmap": {
        "SVK": dict(name="Slovak Republic", color="#5b8fd6", treasury=7.0,
                    compass={"left": -10, "auth": 85},
                    flag={"type": "hstripes_3",
                          "colors": ["#ffffff", "#0b4ea2", "#ee1c25"],
                          "symbols": [{"type": "cross_pattee", "x": 0.34, "y": 0.5,
                                       "size": 0.26, "colors": ["#ee1c25"]}]},
                    provinces=[646],
                    note="Independent from 14 March 1939 -- a German client "
                         "state, but a separate one, and it joined the invasion "
                         "of Poland as a belligerent in its own right. The map "
                         "had it as Hungarian. Southern Slovakia (648, 80% "
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
        ([390, 391], "GBR", "GER",
         "German South-West Africa. South Africa only invaded in 1915 and the "
         "mandate came in 1920."),
        ([307], "FRA", "GER",
         "Kamerun. The Entente campaign began in August 1914 and the partition "
         "was 1916."),
        ([381, 383, 384], "GBR", "GER",
         "German East Africa. Lettow-Vorbeck held it until the armistice; "
         "Tanganyika became a British mandate in 1920."),
        ([376], "GBR", "ITA", "Italian Somaliland."),
        ([406], "GBR", "GER", "Togoland, German until the 1914 campaign."),
        ([289], "FRA", "ESP",
         "Rio de Oro. The Spanish Sahara was Spanish, not French."),
        ([892, 894], "ROU", "AUH",
         "Transylvania, Austro-Hungarian until 1918. Romania only gained it "
         "at Trianon, four years after this scenario opens."),
        ([171, 175, 181], "CHN", "MNG",
         "Outer Mongolia. The first pass used bounding boxes and missed these; "
         "181 measures 96% inside the real border, and an earlier rule here "
         "moved it the other way on a hand-traced polygon that was wrong."),
    ],
    "1918.odmap": [
        ([1044, 1045, 1046], "RUS", "ROU",
         "Bessarabia. The Moldavian Democratic Republic voted union with "
         "Romania in April 1918 and Romanian troops had held it since January; "
         "Soviet Russia had no presence there in October."),
        ([1014, 1015], "RUS", "GER",
         "East Prussia and Memel. The modern raster has this as Kaliningrad, so "
         "the scenario handed Russia a province that was German from 1871 until "
         "1945 -- leaving the German Empire with no East Prussia at all."),
        ([176, 180, 186], "CHN", "MNG",
         "As 1914, including the province an earlier rule here moved out of "
         "Mongolia; it measures 87% inside the real border."),
        ([856, 860], "OTT", "ITA",
         "Tripolitania and Cyrenaica. Italy took Libya from the Ottomans in "
         "the 1911-12 war, six years before this scenario opens."),
        ([389], "GBR", "ITA", "Italian Somaliland."),
        ([676], "GER", "UKR",
         "Left out of the Ukrainian State by a hand-traced polygon that put it "
         "at 71%; measured against the real border it is wholly Ukrainian."),
        # Found by bracketing October 1918 between the 1914 and 1920 snapshots:
        # where both agree and we differ, it is a real finding.
        ([304], "FRA", "ESP",
         "Rio de Oro. The Spanish Sahara was Spanish in both 1914 and 1920, so "
         "it was Spanish in between; the map had it French."),
        ([1007], "RUS", "JPN",
         "Karafuto, southern Sakhalin, Japanese since the Treaty of Portsmouth "
         "in 1905."),
    ],
    "1939.odmap": [
        ([984], "SOV", "ROU",
         "Bessarabia, Romanian from 1918 until the Soviet ultimatum of June "
         "1940. PARTIAL: this province is 67% Bessarabia and 33% the Moldavian "
         "ASSR east of the Dniester, which really was Soviet. Romania is much "
         "the closer answer, but the province cannot be split without carving."),
        ([964], "SOV", "POL",
         "The Kresy around Brest and Grodno -- 99% inside Poland's 1939 border, "
         "and Polish until the Soviet invasion of 17 September."),
        ([1009, 1010], "SOV", "GER",
         "East Prussia and Memel, as 1918. Germany's easternmost province "
         "otherwise stopped at 18.8E, so the Reich was drawn without the "
         "exclave the Polish Corridor was famous for separating."),
        ([157, 158, 161, 163], "CHN", "MNG",
         "Outer Mongolia. fix_1939_history.py chose these by bounding box and "
         "missed four provinces that are 94-100% inside the real border."),
        # check_map_history.py measured this at 87% Mongolia against the real
        # 1938 border. My hand-traced polygon said 68% and I moved it out of
        # Mongolia on that basis, which was wrong. Put it back.
        ([720], "CHN", "MNG", "Southeastern Mongolia, 87% inside the real border."),
        # 1939 note: the [720] MNG->CHN rule that used to sit below is gone.
        ([364, 365, 366], "GBR", "ITA",
         "Italian Somaliland, Italian since the 1880s and not British until "
         "the 1941 campaign."),
        ([596, 598], "GBR", "YEM",
         "The Yemeni highlands. Aden and the Hadhramaut stay British, which is "
         "correct, but these two are the Mutawakkilite Kingdom."),
        ([1060], "TUR", "SOV",
         "Soviet Armenia. Turkey's eastern border was settled at Kars in 1921."),
        ([1061, 1066], "TUR", "FRA",
         "Northern Syria under the French mandate. Turkey gained only Hatay, "
         "in June 1939, not the whole Jazira."),
        ([985], "SOV", "POL",
         "More of the Kresy: Polish until the Soviet invasion of 17 September."),
        ([846], "PER", "ECU",
         "Ecuadorian Amazon. Peru took it in the 1941 war and the Rio Protocol "
         "of 1942, two years after this scenario opens."),
    ],
    "1945.odmap": [
        ([287], "FRA", "ESP", "Spanish Sahara, not French."),
        ([377], "GBR", "FRA",
         "The Fezzan, under French military administration from 1943 until "
         "Libyan independence."),
    ],
    "1962.odmap": [
        ([487, 488, 489, 490, 491], "GBR", "SOM",
         "British Somaliland joined Italian Somaliland to form independent "
         "Somalia on 1 July 1960, two years before this scenario opens."),
    ],
}

# Historically load-bearing, and cheap to state.
RELATIONS = {
    "1918.odmap": {"UKR": {"GER": {"ally": True}}},
    "1945.odmap": {"JPN": {"USA": {"war": False}}},
}


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
            for pid in spec["provinces"]:
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
                  f"{len(spec['provinces'])} provinces ({src})")

        for pids, src, dst, _why in REASSIGN.get(name, []):
            if dst not in have:
                print(f"  WARN  cannot move to {dst}: not on this map", file=sys.stderr)
                continue
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
