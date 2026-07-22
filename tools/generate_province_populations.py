#!/usr/bin/env python3
"""
Generate province_population_2000.json with real 2000 census data.
Each entry maps pixel-province-id → population.

For countries where we know which pixel provinces contain major cities,
we assign those cities' real populations. The rest gets distributed
proportionally by pixel area, weighted by density factors for major
countries based on province centroid coordinates.
"""
from PIL import Image
import json
import numpy as np

WIDTH = 8192
HEIGHT = 4096


def color_to_id(color):
    r = int(color[1:3], 16)
    g = int(color[3:5], 16)
    b = int(color[5:7], 16)
    return (r << 16) | (g << 8) | b


def pixel_to_lonlat(px, py):
    lon = (px / WIDTH) * 360.0 - 180.0
    lat = 90.0 - (py / HEIGHT) * 180.0
    return lon, lat


# ─── Density factor functions ───────────────────────────────────
# Each returns a multiplier for effective population density
# based on longitude/latitude of the province centroid.


def density_chn(lon, lat):
    """China: very sparse west/Tibet, dense coast + Sichuan basin."""
    # Tibet/Qinghai plateau (high altitude, sparse)
    if lat > 25 and lat < 40 and lon > 75 and lon < 103:
        if lat > 32: return 0.01  # high tibet
        elif lat > 28: return 0.03 # qinghai
        else: return 0.1  # yunnan/sichuan fringe
    # Xinjiang (desert)
    if lon < 85:
        if lat > 40: return 0.03
        else: return 0.02
    # Inner Mongolia / Gobi
    if lat > 42 and lon > 105:
        return 0.05
    if lat > 40 and lon > 105:
        return 0.08
    # Northeast (Manchuria)
    if lat > 42 and lon > 120:
        return 0.6
    if lat > 40 and lon > 120:
        return 1.0
    # North China plain (Beijing/Tianjin/Hebei/Shandong)
    if lat > 35 and lon > 113 and lon < 122:
        if lat > 38: return 4.0  # Beijing area
        return 3.0
    # Sichuan basin
    if lat > 28 and lat < 33 and lon > 103 and lon < 109:
        return 3.0
    # Yangtze delta (Shanghai/Jiangsu/Zhejiang)
    if lon > 118 and lat > 29 and lat < 33:
        return 5.0
    # Central/east general
    if lon > 110 and lat > 25:
        return 2.0
    if lon > 105 and lat > 22:
        return 1.5
    # South coast (Guangdong/Fujian)
    if lon > 110 and lat < 26:
        return 2.5
    # Yunnan
    if lon > 97 and lat < 28:
        return 0.4
    return 0.5


def density_rus(lon, lat):
    """Russia: arctic north, European Russia bonus."""
    d = 0.3
    if lat > 70:
        d = 0.01
    elif lat > 60:
        d = 0.05
    elif lat > 50:
        d = 0.3
    else:
        d = 0.8
    if lon < 50:
        d *= 2.0
    return d


def density_usa(lon, lat):
    """USA: desert west, dense east."""
    if lon < -115:
        if lat < 40:
            return 0.1
        else:
            return 0.3
    elif lon < -95:
        return 0.6
    elif lon < -80:
        return 1.0
    elif lon < -70:
        return 2.0
    else:
        return 2.5


def density_can(lon, lat):
    """Canada: very sparse north, denser south, west bonus."""
    if lat > 60:
        d = 0.01
    elif lat > 50:
        d = 0.1
    else:
        d = 1.0
    if lon < -100:
        d *= 1.5
    return d


def density_bra(lon, lat):
    """Brazil: sparse Amazon, dense southeast."""
    if lat < -25:
        return 1.5
    elif lat < -15:
        return 2.0
    elif lat < -5:
        return 0.5
    else:
        return 0.08


def density_aus(lon, lat):
    """Australia: sparse outback, dense east/southeast."""
    if lon > 135:
        if lat < -30:
            return 3.0
        return 2.0
    else:
        return 0.04


def density_ind(lon, lat):
    """India: Ganges plain densest, Deccan plateau moderate, deserts and hills sparse."""
    # Himalayan north
    if lat > 32:
        return 0.05
    if lat > 30:
        return 0.15
    # Ganges plain (Uttar Pradesh, Bihar, West Bengal)
    if lat > 24 and lat < 30 and lon > 77 and lon < 90:
        d = 3.0
        if lon > 80 and lon < 89: d = 4.0  # Bihar/UP core
        if lon >= 89: d = 5.0  # Bengal delta
        return d
    # Rajasthan/Thar desert (west)
    if lon < 73:
        return 0.2
    if lon < 76:
        if lat > 26: return 0.15  # desert
        return 0.4  # gujarat
    # Punjab/Haryana (northwest)
    if lat > 28 and lon < 78:
        return 2.5
    # Gujarat peninsula
    if lon < 73 and lat < 24:
        return 0.6
    # Central India (Madhya Pradesh)
    if lat > 22 and lon > 76 and lon < 82:
        return 0.8
    # Deccan plateau (Maharashtra, Telangana, Karnataka)
    if lat > 16 and lat < 22:
        if lon < 78: return 0.7  # west maharashtra
        return 0.9  # east
    # Coastal
    if lon > 78 and lat > 8 and lat < 16:
        return 1.5  # east coast (Chennai/AP)
    if lon < 78 and lat > 8 and lat < 16:
        return 1.2  # west coast (Kerala/Goa)
    # Far south
    if lat < 10:
        return 1.0
    # Northeast states (Assam etc.)
    if lon > 90:
        return 0.6
    return 0.5


def density_tur(lon, lat):
    """Turkey: coastal dense, interior plateau sparse."""
    if lat > 42:
        return 0.1      # Black Sea mountains
    elif lat > 41:
        return 0.6      # inland Black Sea
    elif lat > 40:
        return 0.8      # Marmara
    else:
        d = 0.3         # central anatolia baseline
        if lon > 26 and lon < 30:
            d = 1.5     # Aegean coast
        elif lon >= 30 and lon < 32:
            d = 1.2     # inner aegean
        elif lon >= 32 and lon < 36:
            d = 0.5     # central
        elif lon >= 36 and lon < 38:
            d = 0.4     # central east
        elif lon >= 38 and lon < 40:
            d = 0.3     # east
        elif lon >= 40 and lon < 42:
            d = 0.2     # further east
        else:
            d = 0.1     # far east (van)
        if lat < 37:
            d *= 1.3    # mediterranean coast bonus
        return d


def density_jpn(lon, lat):
    """Japan: Pacific coast dense, Sea of Japan side & north sparse."""
    if lat < 33.5:
        return 1.5        # Kyushu/Shikoku
    elif lat < 35.0:
        if lon < 133.5:
            return 0.6    # Chugoku west
        else:
            return 1.8    # Kansai/Osaka
    elif lat < 37.0:
        if lon < 138:
            return 1.0    # Sea of Japan coast (Kanazawa)
        else:
            return 2.5    # Tokai/Kanto (Nagoya)
    elif lat < 40.0:
        if lon < 140:
            return 0.4    # Sea of Japan side
        else:
            return 1.5    # Tohoku south
    elif lat < 42.0:
        return 0.3        # Tohoku north
    else:
        return 0.08       # Hokkaido


DENSITY_MAP = {
    'CHN': density_chn,
    'RUS': density_rus,
    'USA': density_usa,
    'CAN': density_can,
    'BRA': density_bra,
    'AUS': density_aus,
    'IND': density_ind,
    'JPN': density_jpn,
    'TUR': density_tur,
}

# Fallback 2000 country populations (World Bank data when API unavailable)
FALLBACK_POPULATIONS = {
    'CHN': 1262645000, 'IND': 1056576000, 'USA': 282162411, 'IDN': 211540000,
    'BRA': 174504898, 'PAK': 142600000, 'RUS': 146890128, 'BGD': 129247000,
    'JPN': 126926000, 'NGA': 123178000, 'MEX': 100894000, 'DEU': 82163500,
    'VNM': 78826000, 'PHL': 76275000, 'FRA': 60912000, 'GBR': 58785600,
    'ITA': 56993742, 'TUR': 63584000, 'THA': 62400000, 'EGY': 61572000,
    'KOR': 47008000, 'IRN': 63399000, 'ESP': 40566000, 'UKR': 49115000,
    'POL': 38254000, 'MMR': 48500000, 'ZAF': 44700000, 'COL': 39844000,
    'ARG': 37057000, 'TZA': 33733000, 'SDN': 32042000, 'KEN': 31139000,
    'DZA': 30958000, 'CAN': 30769000, 'MAR': 28705000, 'PER': 25744000,
    'NPL': 23941000, 'UGA': 23865000, 'IRQ': 22977000, 'AFG': 21395000,
    'LKA': 19103000, 'ROU': 21437000, 'MOZ': 19505000, 'YEM': 18417000,
    'AGO': 18249000, 'GHA': 17711000, 'MDG': 17101000, 'AUS': 19169000,
    'PRK': 22817000, 'TWN': 22276000, 'SYR': 16489000, 'CIV': 16009000,
    'NLD': 15926000, 'CMR': 15275000, 'CHL': 15211000, 'GTM': 11725000,
    'ECU': 12335000, 'KHM': 13047000, 'ZWE': 11868000, 'SEN': 9771000,
    'BFA': 11535000, 'MWI': 11171000, 'MLI': 11012000, 'ZMB': 10551000,
    'BEL': 10263000, 'PRT': 10276000, 'CZE': 10267000, 'GRC': 10539000,
    'HUN': 10211000, 'SWE': 8872100, 'BLR': 9687000, 'BOL': 8567000,
    'DOM': 8495000, 'AZE': 8153900, 'AUT': 8113000, 'CHE': 7187000,
    'BGR': 7819000, 'HND': 6478000, 'ISR': 6287000, 'TUN': 6321000,
    'HTI': 8355000, 'JOR': 4760000, 'LAO': 5300000, 'NIC': 5073000,
    'SLV': 6275000, 'KGZ': 4950000, 'TKM': 4530000, 'SGP': 4024000,
    'DK':  5330000, 'FIN': 5176000, 'SVK': 5389000, 'NOR': 4491000,
    'HRV': 4370000, 'GEO': 4470000, 'BIH': 3815000, 'ARM': 3060000,
    'LTU': 3499000, 'LVA': 2373000, 'SVN': 1989000, 'EST': 1372000,
    'SAU': 21568000, 'ARE': 3021000, 'OMN': 2471000, 'KWT': 1975000,
    'QAT': 613000, 'BHR': 667000, 'LBN': 3772000, 'MNG': 2494000,
    'PRY': 5297000, 'URY': 3336000, 'PAN': 3049000, 'CRI': 4051000,
    'MYS': 23270000, 'SOM': 8648000, 'ETH': 65878000, 'COD': 49256000,
    'TCD': 8693000, 'NER': 11426000, 'COG': 3108000, 'GAB': 1233000,
    'MRT': 2541000, 'ERI': 3504000, 'GNB': 1226000, 'GIN': 8237000,
    'SLE': 4653000, 'LBR': 2845000, 'BEN': 7076000, 'TGO': 4812000,
    'CAF': 3710000, 'RWA': 8095000, 'BDI': 6282000, 'SSD': 6558000,
    'DJI': 723000, 'SWZ': 1062000, 'LSO': 1798000, 'BTN': 574000,
    'MDV': 290000, 'SYC': 81000, 'COM': 556000, 'CPV': 467000,
    'STP': 155000, 'MUS': 1194000, 'FJI': 808000, 'PNG': 5456000,
    'SLB': 430000, 'VUT': 193000, 'WSM': 174000, 'TON': 99800,
    'PLW': 19100, 'FSM': 110000, 'MHL': 51000, 'KIR': 83000,
    'TUV': 9400, 'NRU': 10000, 'BRN': 334000, 'BLZ': 252000,
    'BHS': 310000, 'BRB': 273000, 'GRD': 103000, 'LCA': 160000,
    'VCT': 108000, 'DMA': 71000, 'TTO': 1317000, 'GUY': 755000,
    'SUR': 479000, 'JAM': 2617000, 'CUB': 11145000, 'PRI': 3820000,
}


# ─── City populations (census ~2000) with real-world coordinates ──
# Format: (lon, lat, name, population)
# Province IDs are resolved at runtime by nearest-province lookup.

CITIES_BY_COUNTRY = {
    'TUR': [
        (28.98, 41.01, 'İstanbul',   10018735),
        (32.87, 39.93, 'Ankara',      4007660),
        (27.14, 38.42, 'İzmir',       3370866),
        (32.49, 37.87, 'Konya',       2192076),
        (29.06, 40.18, 'Bursa',       2125405),
        (30.70, 36.90, 'Antalya',     1719751),
        (35.33, 37.00, 'Adana',       1849750),
        (37.38, 37.07, 'Gaziantep',   1561023),
        (36.34, 41.29, 'Samsun',      1209137),
        (29.97, 40.78, 'Kocaeli',     1206293),
    ],
    'GBR': [
        (-0.13, 51.51, 'London',      7172000),
        (-2.24, 53.48, 'Manchester',  2517000),
        (-1.90, 52.48, 'Birmingham',  2238600),
        (-4.25, 55.86, 'Glasgow',     1199640),
        (-2.98, 56.46, 'Edinburgh',    935620),
        (-3.19, 51.48, 'Bristol',      558000),
        (-1.32, 53.80, 'Sheffield',    530100),
        (-1.42, 54.98, 'Leeds',        495500),
        ( 1.32, 52.63, 'Norwich',      194800),
        (-5.93, 54.60, 'Belfast',      277700),
    ],
    'DEU': [
        (13.41, 52.52, 'Berlin',      3382169),
        (11.58, 48.14, 'Munich',      1211632),
        (10.00, 53.55, 'Hamburg',      1719722),
        ( 6.78, 51.23, 'Cologne',      983347),
        ( 8.68, 50.11, 'Frankfurt',    646531),
        ( 9.73, 52.38, 'Hannover',     523642),
        ( 7.07, 50.36, 'Bonn',         315829),
        (10.45, 52.02, 'Braunschweig', 245418),
        (12.10, 54.32, 'Rostock',      212198),
        (13.74, 51.05, 'Dresden',      477800),
    ],
    'ITA': [
        (12.50, 41.90, 'Rome',        2540684),
        ( 9.19, 45.46, 'Milan',       1256211),
        (14.27, 40.85, 'Naples',      2177795),
        (11.34, 44.49, 'Bologna',      381195),
        (12.34, 43.77, 'Florence',     371222),
        (11.88, 45.41, 'Venice',       268465),
        (15.81, 40.63, 'Bari',         320922),
        ( 9.11, 39.22, 'Cagliari',     155285),
        ( 7.69, 45.07, 'Turin',        908041),
        (16.58, 38.11, 'Catanzaro',     95064),
    ],
    'ESP': [
        (-3.70, 40.42, 'Madrid',      2934446),
        ( 2.17, 41.39, 'Barcelona',   1503451),
        (-0.38, 39.47, 'Valencia',     738506),
        (-5.98, 37.39, 'Seville',      684718),
        (-2.45, 42.47, 'Bilbao',       346574),
        (-4.42, 36.72, 'Málaga',       542047),
        ( 0.32, 39.47, 'Alicante',     294629),
        (-6.14, 43.36, 'Vigo',         293422),
        (-1.88, 41.65, 'Zaragoza',     635822),
        ( 3.80, 43.31, 'Donostia',     183424),
    ],
    'POL': [
        (21.01, 52.23, 'Warsaw',      1676171),
        (19.94, 50.06, 'Krakow',       739627),
        (18.65, 54.35, 'Gdansk',       462716),
        (17.03, 51.11, 'Wroclaw',      635932),
        (19.47, 51.75, 'Lodz',         756350),
        (18.67, 50.32, 'Katowice',     336975),
        (22.57, 51.25, 'Lublin',       353483),
        (16.93, 52.41, 'Poznan',       578891),
        (19.76, 49.82, 'Bialystok',    268326),
        (20.48, 53.78, 'Elblag',       126323),
    ],
    'FRA': [
        ( 2.35, 48.86, 'Paris',       2125246),
        ( 4.84, 45.76, 'Lyon',         415483),
        ( 5.37, 43.30, 'Marseille',    798430),
        ( 3.08, 50.63, 'Lille',        226041),
        ( 7.75, 48.58, 'Strasbourg',   264126),
        ( -1.55, 47.22, 'Nantes',     270252),
        (-4.48, 48.39, 'Brest',        144426),
        ( 1.44, 43.60, 'Toulouse',     390350),
        (-0.56, 44.84, 'Bordeaux',     215363),
        (-4.00, 48.21, 'Quimper',       67504),
        ( 3.88, 43.95, 'Montpellier',  210559),
        ( 7.27, 43.71, 'Nice',          342669),
        ( 4.09, 49.12, 'Reims',        183821),
        ( 5.76, 45.19, 'Grenoble',     160232),
        ( 4.83, 46.60, 'Dijon',        150104),
        (-1.15, 46.17, 'La Rochelle',   79086),
        ( 2.89, 48.56, 'Melun',         39325),
        ( 1.85, 50.95, 'Lens',          42600),
        ( 3.06, 50.37, 'Valenciennes',  41752),
        ( 4.71, 48.08, 'Troyes',        60928),
        ( 6.18, 49.12, 'Metz',          59566),
        ( 1.77, 47.39, 'Orleans',       66401),
        ( 2.31, 47.39, 'Bourges',       54107),
        ( 2.44, 46.58, 'Chateauroux',   44101),
        (-1.08, 48.00, 'Le Mans',      143250),
        ( 1.28, 45.83, 'Limoges',      135083),
    ],
    'JPN': [
        (139.69, 35.69, 'Tokyo',      8134688),
        (135.50, 34.69, 'Osaka',      2592974),
        (136.91, 35.18, 'Nagoya',     2188275),
        (140.87, 38.27, 'Sendai',      965447),
        (131.61, 33.59, 'Kitakyushu', 1013310),
        (141.35, 43.06, 'Sapporo',    1822368),
        (132.46, 34.39, 'Hiroshima',  1126060),
        (139.92, 36.05, 'Niigata',     802850),
        (137.72, 34.97, 'Kanazawa',    457904),
        (140.12, 36.57, 'Fukushima',   297358),
    ],
    'CHN': [
        (121.47, 31.23, 'Shanghai',   13735900),
        (116.41, 39.90, 'Beijing',    10171800),
        (106.55, 29.56, 'Chongqing',  30721600),
        (113.26, 23.13, 'Guangzhou',   9943000),
        (117.20, 39.13, 'Tianjin',     7843000),
        (114.31, 30.59, 'Wuhan',       7232000),
        (114.07, 22.55, 'Shenzhen',    7009000),
        (104.07, 30.67, 'Chengdu',    11040000),
        (118.78, 32.06, 'Nanjing',     5453000),
        (108.94, 34.26, 'Xi\'an',      6544000),
        (123.43, 41.80, 'Shenyang',    6420000),
        (120.15, 30.29, 'Hangzhou',    6241000),
        (117.00, 36.67, 'Jinan',       5850000),
        (113.65, 34.76, 'Zhengzhou',   6680000),
        (114.30, 30.59, 'Nanchang',    4500000),
        (102.73, 25.04, 'Kunming',     5200000),
        (106.71, 26.57, 'Guiyang',     4400000),
        ( 91.11, 29.65, 'Lhasa',       395000),
        ( 87.62, 43.83, 'Urumqi',      2030000),
    ],
    'IND': [
        (72.88, 19.08, 'Mumbai',     11937891),
        (77.21, 28.61, 'Delhi',      12791407),
        (88.36, 22.57, 'Kolkata',     4572196),
        (80.27, 13.08, 'Chennai',     4326942),
        (77.59, 12.97, 'Bangalore',   4301329),
        (78.47, 17.39, 'Hyderabad',   3637000),
        (75.79, 31.33, 'Chandigarh',  852614),
        (73.86, 15.36, 'Goa',         585000),
        (72.57, 23.03, 'Ahmedabad',   3520000),
        (80.95, 26.85, 'Lucknow',     2245000),
        (85.82, 20.30, 'Bhubaneswar',  723000),
        (76.91, 11.02, 'Kochi',        602000),
        (79.96, 11.01, 'Pondicherry',  244000),
        (83.30, 26.77, 'Varanasi',    1198000),
        (88.44, 22.58, 'Durgapur',     493000),
        (80.93, 26.84, 'Kanpur',      2728000),
    ],
    'USA': [
        (-74.01, 40.71, 'New York',   18976057),
        (-118.24, 34.05, 'Los Angeles', 11789040),
        (-87.63, 41.88, 'Chicago',     8306393),
        (-95.37, 29.76, 'Houston',     4514027),
        (-112.07, 33.45, 'Phoenix',   3719303),
        (-75.17, 39.95, 'Philadelphia', 5175258),
        (-98.49, 29.42, 'San Antonio', 2953525),
        (-117.16, 32.72, 'San Diego',  1223400),
        (-122.42, 37.77, 'San Francisco', 784876),
        (-84.39, 33.75, 'Atlanta',     432427),
        (-80.19, 25.76, 'Miami',       362470),
        (-90.07, 29.95, 'New Orleans', 484674),
        (-104.99, 39.74, 'Denver',     600158),
        (-112.07, 33.45, 'Phoenix',   3719303),
        (-97.52, 27.78, 'Corpus Christi', 277401),
        (-96.80, 32.78, 'Dallas',     1188580),
        (-95.37, 29.76, 'Houston',     4514027),
        (-81.66, 30.33, 'Jacksonville', 735167),
        (-86.16, 39.77, 'Indianapolis', 791926),
        (-76.61, 39.29, 'Baltimore',   651154),
    ],
    'RUS': [
        (37.62, 55.76, 'Moscow',          10382754),
        (30.32, 59.93, 'St. Petersburg',   4661219),
        (82.93, 55.03, 'Novosibirsk',      1425655),
        (60.60, 56.84, 'Yekaterinburg',    1293537),
        (50.15, 53.20, 'Kazan',           1105287),
        (44.01, 56.33, 'Nizhny Novgorod', 1311284),
        (39.72, 47.24, 'Rostov-on-Don',   1093323),
        (73.37, 54.99, 'Omsk',            1154116),
        (61.40, 55.17, 'Chelyabinsk',     1130125),
        (48.02, 46.35, 'Astrakhan',        520339),
    ],
}

# FRA_REGIONS: used as overrides — if a city finds province P,
# the region population for P replaces the city population.
FRA_REGIONS = {
    # province_id will be resolved at runtime; keyed by city name
    'Paris':       10320883,   # Île-de-France
    'Lyon':         1579786,
    'Marseille':    1832739,
    'Lille':        2328658,   # Nord-Pas-de-Calais
    'Strasbourg':   2380894,   # Alsace
    'Nantes':       1500157,   # Pays de la Loire
    'Brest':        1637929,   # Bretagne
    'Toulouse':     1628209,   # Aquitaine
    'Bordeaux':     1426756,   # Aquitaine (nearby)
    'Quimper':      1093027,   # Bretagne (nearby)
    'Montpellier':  1370772,   # Languedoc
    'Nice':         1279208,   # PACA (nearby)
    'Reims':        1243206,   # Champagne
    'Grenoble':     1155202,   # Rhône-Alpes (nearby)
    'Dijon':        1118998,   # Bourgogne
    'La Rochelle':  1063219,   # Poitou-Charentes
    'Melun':        1061745,   # Île-de-France (nearby)
    'Lens':         2328658,   # Nord
    'Valenciennes': 2328658,   # Nord
    'Troyes':       1061745,   # Champagne (nearby)
    'Metz':         1243206,   # Lorraine
    'Orleans':      1370772,   # Centre
    'Bourges':      1370772,   # Centre
    'Chateauroux':  1370772,   # Centre (nearby)
    'Le Mans':      1500157,   # Pays de la Loire (nearby)
    'Limoges':      1628209,   # Limousin (nearby)
}

# CITIES_BY_COUNTRY is the master list; CITY_MAP is computed at runtime after
# province coordinates are loaded.


def compute_centroids(img):
    """Return dict: color_id → (centroid_x, centroid_y, pixel_count)."""
    img_arr = np.array(img, dtype=np.uint8)
    h, w = img_arr.shape[:2]

    centroids = {}

    for y in range(h):
        row = img_arr[y, :, :]
        r = row[:, 0].astype(np.int64)
        g = row[:, 1].astype(np.int64)
        b = row[:, 2].astype(np.int64)
        a = row[:, 3]
        mask = a > 0
        if not np.any(mask):
            continue
        colors = (r[mask] << 16) | (g[mask] << 8) | b[mask]
        xs = np.where(mask)[0].astype(np.float64)
        unique_c, inverse = np.unique(colors, return_inverse=True)
        counts = np.bincount(inverse)
        sum_x = np.bincount(inverse, weights=xs)
        sum_y = counts * y

        for i, c in enumerate(unique_c):
            c = int(c)
            if c not in centroids:
                centroids[c] = [0.0, 0.0, 0]
            centroids[c][0] += float(sum_x[i])
            centroids[c][1] += float(sum_y[i])
            centroids[c][2] += int(counts[i])

    result = {}
    for c, (sx, sy, n) in centroids.items():
        result[c] = (sx / n, sy / n, n)
    return result


def main():
    data_dir = '/Users/vladyavdoshenko/CLionProjects/OpenDoctrines/data'

    with open(f'{data_dir}/provinces.json') as f:
        provs = json.load(f)
    with open(f'{data_dir}/population_2000.json') as f:
        wb_pops = json.load(f)

    # Pixel counts + centroids from image
    img = Image.open(f'{data_dir}/provinces.png').convert('RGBA')
    colors = img.getcolors(maxcolors=1000000)
    pixel_counts = {}
    for count, rgba in colors:
        r, g, b, a = rgba
        if a > 0:
            pixel_counts[(r << 16) | (g << 8) | b] = count

    print("Computing province centroids...")
    centroids_by_color = compute_centroids(img)
    print(f"  Found centroids for {len(centroids_by_color)} colors")

    # province_id → pixel_count, centroid (lon/lat)
    prov_pixel_count = {}
    prov_centroids = {}
    for pid_str, entry in provs.items():
        pid = entry['id']
        color = entry.get('color', '')
        if color:
            enc = color_to_id(color)
            prov_pixel_count[pid] = pixel_counts.get(enc, 0)
            if enc in centroids_by_color:
                cx, cy, _ = centroids_by_color[enc]
                prov_centroids[pid] = pixel_to_lonlat(cx, cy)
            else:
                prov_centroids[pid] = None

    # Build result
    result = {}

    # Group provinces by ISO
    by_iso = {}
    for pid_str, entry in provs.items():
        iso = entry.get('iso_a3', '')
        cid = entry.get('country_id', 0)
        if not iso:
            continue
        if iso not in by_iso:
            by_iso[iso] = {'country_id': cid, 'provinces': {}}
        pid = entry['id']
        by_iso[iso]['provinces'][pid] = {
            'px': prov_pixel_count.get(pid, 0),
            'centroid': prov_centroids.get(pid),
        }

    # ─── Resolve city→province by nearest centroid lookup ───────────
    # Multiple cities may map to the same province; their populations accumulate.
    city_resolved = {}  # iso → {province_id: (names_str, total_pop)}
    for iso, cities in CITIES_BY_COUNTRY.items():
        if iso not in by_iso:
            continue
        provs_in_country = by_iso[iso]['provinces']
        resolved = {}  # pid → [names_list, total_pop]
        for lon, lat, name, pop in cities:
            best_pid = None
            best_dist = float('inf')
            for pid, info in provs_in_country.items():
                centroid = info['centroid']
                if centroid is None:
                    continue
                c_lon, c_lat = centroid
                dist = (c_lon - lon) ** 2 + (c_lat - lat) ** 2
                if dist < best_dist:
                    best_dist = dist
                    best_pid = pid
            if best_pid is not None:
                # Use FRA_REGIONS override if available
                if iso == 'FRA' and name in FRA_REGIONS:
                    pop = FRA_REGIONS[name]
                if best_pid in resolved:
                    resolved[best_pid][0].append(name)
                    resolved[best_pid][1] += pop
                else:
                    resolved[best_pid] = [[name], pop]
        # Flatten to (name_str, total_pop) tuples for downstream compatibility
        city_resolved[iso] = {pid: ("+".join(names), total) for pid, (names, total) in resolved.items()}

    # Print resolved city mappings
    for iso, resolved in city_resolved.items():
        if not resolved:
            continue
        entries = sorted(resolved.items(), key=lambda x: -x[1][1])
        print(f"\n{iso} resolved city→province (accumulated):")
        for pid, (name, pop) in entries[:5]:
            centroid = by_iso[iso]['provinces'][pid]['centroid']
            c_lon, c_lat = centroid if centroid else (0, 0)
            print(f"  prov {pid:>5}: {name:>25}  pop={pop:>10,}  center=({c_lon:.1f},{c_lat:.1f})")
        if len(entries) > 5:
            print(f"  ... and {len(entries) - 5} more")

    for iso, data in by_iso.items():
        provinces = data['provinces']
        if not provinces:
            continue

        # Unclaimed territory gets minimal population
        if iso == "UNC" or iso == "BLC":
            for pid in provinces:
                result[str(pid)] = 0
            print(f"  {iso}: set {len(provinces)} provinces to 0 (unclaimed)")
            continue

        country_pop = wb_pops.get(iso, 0) if wb_pops else 0
        if country_pop == 0:
            country_pop = FALLBACK_POPULATIONS.get(iso, 0)
            if country_pop > 0:
                print(f"  {iso}: using fallback population {country_pop:,}")

        # Get resolved city mapping for this country
        resolved_cities = city_resolved.get(iso, {})

        # Compute total city population
        city_total = sum(pop for _, (_, pop) in resolved_cities.items())

        # Determine density function for this country
        density_fn = DENSITY_MAP.get(iso, None)

        # Compute effective pixels for ALL provinces
        total_effective = 0
        prov_effective = {}
        for pid, info in provinces.items():
            raw_px = info['px']
            if density_fn is not None and info['centroid'] is not None:
                cx, cy = info['centroid']
                lon, lat = cx, cy  # already lon/lat
                df = density_fn(lon, lat)
                effective = raw_px * df
            else:
                effective = raw_px
            prov_effective[pid] = effective
            total_effective += effective

        remaining_pop = country_pop - city_total

        for pid, info in provinces.items():
            if total_effective > 0 and remaining_pop > 0 and info['px'] > 0:
                effective = prov_effective[pid]
                pop = int(remaining_pop * effective / total_effective)
            elif info['px'] > 0:
                pop = max(info['px'] * 50, 1000)
            else:
                pop = 0
            # Add city pop on top
            if pid in resolved_cities:
                pop += resolved_cities[pid][1]
            result[str(pid)] = max(pop, 1000) if info['px'] > 0 else 0

    # Save
    out_path = f'{data_dir}/province_population_2000.json'
    with open(out_path, 'w') as f:
        json.dump(result, f, indent=2)

    print(f"\nSaved {out_path} ({len(result)} provinces, total pop: {sum(result.values()):,})")

    # Verify a few countries
    for iso in ['TUR', 'FRA', 'CHN', 'USA', 'RUS', 'IND', 'JPN']:
        if iso not in by_iso:
            continue
        provs_in = by_iso[iso]['provinces']
        resolved = city_resolved.get(iso, {})
        city_pop = sum(pop for _, (_, pop) in resolved.items())
        total_pop = sum(result.get(str(p), 0) for p in provs_in)
        expected = wb_pops.get(iso, 0) if wb_pops else 0
        print(f"\n{iso}: expected={expected:>12,}  actual={total_pop:>12,}  city={city_pop:>12,}")
        for pid, (name, pop) in sorted(resolved.items(), key=lambda x: -x[1][1])[:3]:
            print(f"  {name:>15} → prov {pid}: {pop:>10,}")


if __name__ == '__main__':
    main()
