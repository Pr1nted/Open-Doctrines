#!/usr/bin/env python3
"""
Generate minorities.json with per-province ethnic composition.
No generic "Other" groups — only named nationalities/ethnicities.
Same group name always gets the same color.
"""
import json, hashlib, random, re

ETHNIC_MAP = {
    # North America
    "USA": [("White American", 60), ("African American", 13), ("Hispanic American", 18), ("Asian American", 6), ("Native American", 2), ("Arab American", 1)],
    "CAN": [("English Canadian", 32), ("French Canadian", 22), ("Chinese Canadian", 5), ("South Asian Canadian", 6), ("Indigenous Canadian", 5), ("Black Canadian", 4), ("Filipino Canadian", 3), ("Arab Canadian", 2), ("Mixed Canadian", 6)],
    "MEX": [("Mestizo Mexican", 62), ("Indigenous Mexican", 20), ("White Mexican", 15), ("Black Mexican", 2), ("Asian Mexican", 1)],
    "GTM": [("Mestizo Guatemalan", 60), ("Maya", 38), ("Xinca", 1), ("Garifuna", 1)],
    "BLZ": [("Mestizo Belizean", 50), ("Creole Belizean", 25), ("Maya", 10), ("Garifuna", 6), ("White Belizean", 5), ("Indian Belizean", 4)],
    "SLV": [("Mestizo Salvadoran", 86), ("White Salvadoran", 10), ("Indigenous Salvadoran", 3), ("Black Salvadoran", 1)],
    "HND": [("Mestizo Honduran", 85), ("Indigenous Honduran", 7), ("White Honduran", 5), ("Black Honduran", 2), ("Garifuna", 1)],
    "NIC": [("Mestizo Nicaraguan", 65), ("White Nicaraguan", 15), ("Black Nicaraguan", 10), ("Indigenous Nicaraguan", 5), ("Chinese Nicaraguan", 3), ("Korean Nicaraguan", 2)],
    "CRI": [("White Costa Rican", 65), ("Mestizo Costa Rican", 20), ("Black Costa Rican", 5), ("Indigenous Costa Rican", 3), ("Chinese Costa Rican", 3), ("Nicaraguan", 2), ("Mixed Costa Rican", 2)],
    "PAN": [("Mestizo Panamanian", 60), ("White Panamanian", 15), ("Black Panamanian", 10), ("Indigenous Panamanian", 8), ("Chinese Panamanian", 5), ("Asian Panamanian", 2)],
    # Caribbean
    "CUB": [("White Cuban", 64), ("Mixed Cuban", 27), ("Black Cuban", 9)],
    "DOM": [("Mixed Dominican", 70), ("White Dominican", 15), ("Black Dominican", 10), ("Asian Dominican", 3), ("Indigenous Dominican", 2)],
    "HTI": [("Black Haitian", 90), ("Mixed Haitian", 8), ("White Haitian", 1), ("Arab Haitian", 1)],
    "JAM": [("Black Jamaican", 90), ("Mixed Jamaican", 6), ("White Jamaican", 2), ("Indian Jamaican", 1), ("Chinese Jamaican", 1)],
    "BHS": [("Black Bahamian", 85), ("White Bahamian", 10), ("Mixed Bahamian", 3), ("Asian Bahamian", 2)],
    "BRB": [("Black Barbadian", 85), ("White Barbadian", 5), ("Mixed Barbadian", 5), ("Indian Barbadian", 3), ("Chinese Barbadian", 2)],
    "TTO": [("Indian Trinidadian", 35), ("African Trinidadian", 35), ("Mixed Trinidadian", 22), ("White Trinidadian", 3), ("Chinese Trinidadian", 2), ("Syrian Trinidadian", 1), ("Portuguese Trinidadian", 1), ("Spanish Trinidadian", 1)],
    "GRD": [("Black Grenadian", 85), ("Mixed Grenadian", 8), ("White Grenadian", 3), ("Indian Grenadian", 2), ("Chinese Grenadian", 2)],
    "VCT": [("Black Vincentian", 80), ("Mixed Vincentian", 15), ("White Vincentian", 3), ("Indian Vincentian", 2)],
    "DMA": [("Black Dominican", 85), ("Mixed Dominican", 8), ("White Dominican", 3), ("Kalinago", 2), ("Indian Dominican", 2)],
    "LCA": [("Black Saint Lucian", 80), ("Mixed Saint Lucian", 12), ("White Saint Lucian", 3), ("Indian Saint Lucian", 3), ("Chinese Saint Lucian", 2)],
    "ATG": [("Black Antiguan", 85), ("Mixed Antiguan", 8), ("White Antiguan", 3), ("Indian Antiguan", 2), ("Portuguese Antiguan", 2)],
    "KNA": [("Black Kittitian", 90), ("Mixed Kittitian", 5), ("White Kittitian", 3), ("Indian Kittitian", 2)],
    "TCA": [("Black Turks Islander", 85), ("White Turks Islander", 10), ("Mixed Turks Islander", 5)],
    "VGB": [("Black British Virgin Islander", 80), ("White British Virgin Islander", 10), ("Mixed British Virgin Islander", 5), ("Indian British Virgin Islander", 3), ("Asian British Virgin Islander", 2)],
    "CYM": [("Black Caymanian", 50), ("White Caymanian", 30), ("Mixed Caymanian", 10), ("Indian Caymanian", 5), ("Asian Caymanian", 5)],
    "BMU": [("Black Bermudian", 55), ("White Bermudian", 30), ("Mixed Bermudian", 10), ("Asian Bermudian", 3), ("Indian Bermudian", 2)],
    "PRI": [("White Puerto Rican", 70), ("Mixed Puerto Rican", 20), ("Black Puerto Rican", 8), ("Indigenous Puerto Rican", 2)],
    "VIR": [("Black US Virgin Islander", 75), ("White US Virgin Islander", 15), ("Mixed US Virgin Islander", 8), ("Asian US Virgin Islander", 2)],
    # South America
    "BRA": [("White Brazilian", 44), ("Mixed Brazilian", 43), ("Black Brazilian", 8), ("Asian Brazilian", 2), ("Indigenous Brazilian", 2), ("Arab Brazilian", 1)],
    "ARG": [("White Argentine", 80), ("Mestizo Argentine", 15), ("Indigenous Argentine", 3), ("Asian Argentine", 1), ("Arab Argentine", 1)],
    "COL": [("Mestizo Colombian", 50), ("White Colombian", 25), ("Mixed Colombian", 14), ("Black Colombian", 5), ("Indigenous Colombian", 3), ("Zambo Colombian", 2), ("Arab Colombian", 1)],
    "PER": [("Mestizo Peruvian", 60), ("Quechua", 22), ("White Peruvian", 6), ("Aymara", 4), ("Black Peruvian", 2), ("Japanese Peruvian", 2), ("Chinese Peruvian", 2), ("Asháninka", 1), ("Awajún", 1)],
    "CHL": [("White Chilean", 53), ("Mestizo Chilean", 40), ("Mapuche", 5), ("Aymara", 1), ("Rapa Nui", 1)],
    "BOL": [("Mestizo Bolivian", 55), ("Quechua", 25), ("Aymara", 15), ("White Bolivian", 3), ("Guarani", 1), ("Chiquitano", 1)],
    "ECU": [("Mestizo Ecuadorian", 65), ("Indigenous Ecuadorian", 25), ("White Ecuadorian", 5), ("Black Ecuadorian", 3), ("Montubio", 2)],
    "PRY": [("Mestizo Paraguayan", 75), ("White Paraguayan", 15), ("Indigenous Paraguayan", 5), ("Japanese Paraguayan", 3), ("Korean Paraguayan", 2)],
    "VEN": [("Mestizo Venezuelan", 50), ("White Venezuelan", 40), ("Black Venezuelan", 5), ("Indigenous Venezuelan", 3), ("Italian Venezuelan", 1), ("Portuguese Venezuelan", 1)],
    "URY": [("White Uruguayan", 80), ("Mestizo Uruguayan", 10), ("Black Uruguayan", 5), ("Indigenous Uruguayan", 3), ("Asian Uruguayan", 2)],
    "GUY": [("Indo-Guyanese", 44), ("Afro-Guyanese", 30), ("Mixed Guyanese", 16), ("Indigenous Guyanese", 7), ("Chinese Guyanese", 2), ("Portuguese Guyanese", 1)],
    "SUR": [("Hindustani Surinamese", 27), ("Maroon", 21), ("Javanese Surinamese", 15), ("Mixed Surinamese", 14), ("Indigenous Surinamese", 5), ("Chinese Surinamese", 5), ("Creole Surinamese", 4), ("Dutch Surinamese", 3), ("Lebanese Surinamese", 2), ("Portuguese Surinamese", 2), ("Japanese Surinamese", 1), ("Korean Surinamese", 1)],
    # Western Europe
    "GBR": [("English", 74), ("Scottish", 8), ("Welsh", 5), ("Indian", 3), ("Polish", 2), ("Pakistani", 2), ("Black British", 2), ("Irish", 2), ("Bangladeshi", 1), ("Chinese", 1), ("Romanian", 1), ("Arab", 1), ("Mixed British", 2)],
    "IRL": [("Irish", 82), ("Polish", 4), ("Black Irish", 2), ("White Irish", 9), ("Chinese", 1), ("Indian", 1), ("Filipino", 1)],
    "FRA": [("French", 80), ("Algerian", 5), ("Moroccan", 4), ("Portuguese", 3), ("Italian", 3), ("Tunisian", 2), ("Spanish", 2), ("Turkish", 1), ("Mixed French", 2)],
    "DEU": [("German", 78), ("Turkish", 5), ("Polish", 3), ("Italian", 2), ("Russian", 2), ("Romanian", 2), ("Greek", 2), ("Syrian", 2), ("Afghan", 1), ("Iraqi", 1), ("Chinese", 1), ("Mixed German", 2)],
    "ITA": [("Italian", 85), ("Romanian", 3), ("Albanian", 2), ("Moroccan", 2), ("Chinese", 2), ("Ukrainian", 2), ("Filipino", 1), ("Indian", 1), ("Egyptian", 1), ("Mixed Italian", 1)],
    "ESP": [("Spanish", 70), ("Catalan", 8), ("Galician", 4), ("Basque", 3), ("Valencian", 2), ("Moroccan", 3), ("Romanian", 2), ("Ecuadorian", 2), ("Colombian", 2), ("British", 2), ("German", 1), ("Mixed Spanish", 1)],
    "PRT": [("Portuguese", 82), ("Brazilian", 4), ("Ukrainian", 3), ("Cape Verdean", 3), ("Angolan", 2), ("Mozambican", 1), ("Chinese", 1), ("Romani", 1), ("Black Portuguese", 1), ("Mixed Portuguese", 1)],
    "NLD": [("Dutch", 74), ("Turkish", 3), ("Moroccan", 3), ("Indonesian", 2), ("Surinamese", 2), ("Polish", 2), ("German", 2), ("Chinese", 2), ("British", 2), ("Iraqi", 1), ("Afghan", 1), ("Iranian", 1), ("Somali", 1), ("Mixed Dutch", 2)],
    "BEL": [("Flemish", 55), ("Walloon", 32), ("French Belgian", 3), ("Dutch Belgian", 2), ("German Belgian", 1), ("Moroccan", 3), ("Turkish", 2), ("Italian", 1), ("Polish", 1)],
    "CHE": [("Swiss German", 58), ("Swiss French", 23), ("Swiss Italian", 8), ("Romansh", 1), ("German Swiss", 3), ("Italian Swiss", 2), ("Portuguese Swiss", 2), ("French Swiss", 2), ("Spanish Swiss", 1), ("Turkish Swiss", 1), ("Serbian Swiss", 1), ("Kosovar Swiss", 1)],
    "AUT": [("Austrian", 78), ("German", 4), ("Turkish", 3), ("Serbian", 3), ("Bosnian", 2), ("Romanian", 2), ("Hungarian", 2), ("Croatian", 1), ("Polish", 1), ("Slovak", 1), ("Czech", 1), ("Mixed Austrian", 2)],
    "LUX": [("Luxembourgish", 52), ("Portuguese", 15), ("French", 7), ("Italian", 4), ("Belgian", 3), ("German", 3), ("Spanish", 2), ("British", 2), ("Chinese", 2), ("Indian", 2), ("Cape Verdean", 2)],
    "MCO": [("French", 60), ("Italian", 15), ("Monegasque", 8), ("British", 5), ("German", 3), ("Swiss", 3), ("American", 2), ("Russian", 2), ("Greek", 2)],
    "LIE": [("Liechtensteiner", 45), ("German", 20), ("Italian", 10), ("Austrian", 8), ("Swiss", 5), ("Turkish", 3), ("Spanish", 3), ("Portuguese", 2), ("French", 2), ("British", 2)],
    "AND": [("Andorran", 40), ("Spanish", 30), ("French", 10), ("Portuguese", 8), ("British", 4), ("German", 3), ("Moroccan", 2), ("Romanian", 2), ("Russian", 1)],
    "GIB": [("Gibraltarian", 65), ("British", 15), ("Spanish", 8), ("Moroccan", 5), ("Indian", 3), ("Portuguese", 2), ("Italian", 2)],
    "MLT": [("Maltese", 88), ("Italian", 2), ("English", 2), ("Arab", 2), ("Filipino", 1), ("Indian", 1), ("Chinese", 1), ("Russian", 1), ("Mixed Maltese", 2)],
    # Northern Europe
    "SWE": [("Swedish", 76), ("Finnish", 3), ("Iraqi", 3), ("Polish", 2), ("Syrian", 2), ("Somali", 2), ("Iranian", 2), ("Turkish", 1), ("Bosnian", 1), ("German", 1), ("Danish", 1), ("Norwegian", 1), ("Chilean", 1), ("Thai", 1), ("Mixed Swedish", 3)],
    "NOR": [("Norwegian", 80), ("Polish", 3), ("Swedish", 2), ("Somali", 2), ("Iraqi", 2), ("Pakistani", 2), ("Lithuanian", 2), ("German", 1), ("Danish", 1), ("Vietnamese", 1), ("Turkish", 1), ("Iranian", 1), ("Mixed Norwegian", 2)],
    "DNK": [("Danish", 82), ("Turkish", 2), ("Polish", 2), ("Syrian", 2), ("German", 2), ("Iraqi", 1), ("Lebanese", 1), ("Pakistani", 1), ("Bosnian", 1), ("Somali", 1), ("Iranian", 1), ("Swedish", 1), ("Norwegian", 1), ("Mixed Danish", 2)],
    "FIN": [("Finnish", 84), ("Swedish Finnish", 5), ("Russian", 3), ("Estonian", 2), ("Somali", 1), ("Iraqi", 1), ("Chinese", 1), ("Vietnamese", 1), ("Mixed Finnish", 2)],
    "ISL": [("Icelandic", 84), ("Polish", 5), ("Danish", 3), ("Swedish", 2), ("Lithuanian", 2), ("German", 1), ("Norwegian", 1), ("American", 1), ("Mixed Icelandic", 1)],
    "FRO": [("Faroese", 85), ("Danish", 8), ("Icelandic", 2), ("Norwegian", 2), ("Filipino", 1), ("British", 1), ("Swedish", 1)],
    "GGY": [("Guernseyman", 50), ("British", 30), ("French", 8), ("Portuguese", 5), ("Irish", 3), ("Spanish", 2), ("Polish", 2)],
    "JEY": [("Jerseyman", 45), ("British", 30), ("Portuguese", 8), ("French", 6), ("Irish", 3), ("Polish", 3), ("Spanish", 2), ("Romanian", 2), ("German", 1)],
    "IMN": [("Manx", 40), ("British", 35), ("Irish", 8), ("Scottish", 5), ("Portuguese", 3), ("Polish", 3), ("Latvian", 2), ("South African", 2), ("German", 1), ("French", 1)],
    "ALA": [("Finnish", 85), ("Swedish", 10), ("Russian", 3), ("Estonian", 2)],
    "SJM": [("Norwegian", 90), ("Russian", 5), ("Polish", 3), ("German", 2)],
    # Eastern Europe
    "RUS": [("Russian", 75), ("Tatar", 4), ("Ukrainian", 3), ("Bashkir", 2), ("Chuvash", 2), ("Chechen", 2), ("Armenian", 2), ("Avar", 1), ("Mordvin", 1), ("Udmurt", 1), ("Mari", 1), ("Kazakh", 1), ("Azeri", 1), ("Belarusian", 1), ("German", 1), ("Jewish", 1), ("Korean", 1)],
    "UKR": [("Ukrainian", 78), ("Russian", 17), ("Romanian", 1), ("Belarusian", 1), ("Hungarian", 1), ("Bulgarian", 1), ("Polish", 1)],
    "POL": [("Polish", 94), ("German", 1), ("Ukrainian", 1), ("Belarusian", 1), ("Russian", 1), ("Silesian", 1), ("Mixed Polish", 1)],
    "CZE": [("Czech", 88), ("Moravian", 4), ("Slovak", 2), ("Ukrainian", 1), ("Polish", 1), ("German", 1), ("Hungarian", 1), ("Romani", 1), ("Mixed Czech", 1)],
    "SVK": [("Slovak", 80), ("Hungarian", 9), ("Romani", 3), ("Czech", 2), ("Ruthenian", 2), ("Ukrainian", 1), ("Polish", 1), ("German", 1), ("Mixed Slovak", 1)],
    "HUN": [("Hungarian", 83), ("Romani", 5), ("German", 3), ("Slovak", 2), ("Romanian", 2), ("Croatian", 1), ("Serbian", 1), ("Ukrainian", 1), ("Polish", 1), ("Mixed Hungarian", 1)],
    "ROU": [("Romanian", 82), ("Hungarian", 6), ("Romani", 3), ("German", 2), ("Ukrainian", 2), ("Turkish", 1), ("Lipovan", 1), ("Greek", 1), ("Mixed Romanian", 2)],
    "BGR": [("Bulgarian", 75), ("Turkish", 8), ("Romani", 5), ("Macedonian", 2), ("Pomak", 3), ("Greek", 1), ("Armenian", 1), ("Russian", 1), ("Serbian", 1), ("Jewish", 1), ("Vlach", 1), ("Mixed Bulgarian", 1)],
    "SRB": [("Serbian", 80), ("Hungarian", 4), ("Bosniak", 2), ("Romani", 2), ("Croatian", 1), ("Slovak", 1), ("Montenegrin", 1), ("Romanian", 1), ("Macedonian", 1), ("Bulgarian", 1), ("Ruthenian", 1), ("Albanian", 1), ("Yugoslav", 1), ("Mixed Serbian", 3)],
    "HRV": [("Croatian", 89), ("Serbian", 5), ("Bosniak", 1), ("Italian", 1), ("Hungarian", 1), ("Czech", 1), ("Romani", 1), ("Mixed Croatian", 1)],
    "SVN": [("Slovene", 82), ("Serbian", 3), ("Croatian", 2), ("Bosniak", 2), ("Hungarian", 1), ("Albanian", 1), ("Macedonian", 1), ("Montenegrin", 1), ("Italian", 1), ("Romani", 1), ("German", 1), ("Ukrainian", 1), ("Mixed Slovene", 1)],
    "BIH": [("Bosniak", 50), ("Serb", 31), ("Croat", 15), ("Romani", 2), ("Albanian", 1), ("Montenegrin", 1)],
    "MKD": [("Macedonian", 62), ("Albanian", 25), ("Turkish", 4), ("Romani", 3), ("Serbian", 2), ("Vlach", 1), ("Bosniak", 1), ("Mixed Macedonian", 2)],
    "ALB": [("Albanian", 82), ("Greek", 3), ("Romani", 2), ("Macedonian", 2), ("Montenegrin", 1), ("Serbian", 1), ("Bulgarian", 1), ("Egyptian", 1), ("Bosniak", 1), ("Aromanian", 1), ("Jewish", 1)],
    "MNE": [("Montenegrin", 45), ("Serbian", 30), ("Bosniak", 10), ("Albanian", 5), ("Croatian", 3), ("Romani", 2), ("Bulgarian", 2), ("Macedonian", 1), ("Greek", 1), ("Italian", 1)],
    "XKX": [("Albanian", 88), ("Serbian", 6), ("Bosniak", 3), ("Romani", 1), ("Turkish", 1), ("Gorani", 1)],
    "GRC": [("Greek", 85), ("Albanian", 5), ("Macedonian", 2), ("Turkish", 2), ("Bulgarian", 1), ("Romani", 1), ("Armenian", 1), ("Jewish", 1), ("Mixed Greek", 2)],
    "CYP": [("Greek Cypriot", 70), ("Turkish Cypriot", 25), ("Armenian", 2), ("Maronite", 1), ("British", 1), ("Russian", 1)],
    "XNC": [("Turkish Cypriot", 65), ("Turkish", 15), ("Greek Cypriot", 10), ("British", 5), ("Arab", 3), ("Romanian", 2)],
    "BLR": [("Belarusian", 82), ("Russian", 8), ("Polish", 3), ("Ukrainian", 2), ("Jewish", 1), ("Tatar", 1), ("Romani", 1), ("Mixed Belarusian", 2)],
    "MDA": [("Moldovan", 66), ("Romanian", 12), ("Ukrainian", 8), ("Russian", 5), ("Gagauz", 4), ("Bulgarian", 2), ("Jewish", 1), ("Mixed Moldovan", 2)],
    "LTU": [("Lithuanian", 82), ("Polish", 7), ("Russian", 6), ("Belarusian", 2), ("Ukrainian", 1), ("Mixed Lithuanian", 2)],
    "LVA": [("Latvian", 60), ("Russian", 26), ("Belarusian", 4), ("Ukrainian", 3), ("Polish", 2), ("Lithuanian", 1), ("Jewish", 1), ("Romani", 1), ("Mixed Latvian", 2)],
    "EST": [("Estonian", 67), ("Russian", 25), ("Ukrainian", 2), ("Belarusian", 1), ("Finnish", 1), ("Latvian", 1), ("German", 1), ("Mixed Estonian", 2)],
    # Caucasus
    "GEO": [("Georgian", 85), ("Azeri", 6), ("Armenian", 5), ("Russian", 2), ("Ossetian", 1), ("Greek", 1), ("Abkhaz", 1)],
    "ARM": [("Armenian", 95), ("Yazidi", 2), ("Russian", 1), ("Assyrian", 1), ("Mixed Armenian", 1)],
    "AZE": [("Azeri", 90), ("Russian", 2), ("Armenian", 2), ("Talysh", 1), ("Lezgian", 1), ("Kurdish", 1), ("Tatar", 1), ("Mixed Azeri", 2)],
    # Middle East / North Africa
    "TUR": [("Turkish", 70), ("Kurdish", 17), ("Arab", 3), ("Circassian", 2), ("Armenian", 1), ("Greek", 1), ("Laz", 1), ("Romani", 1), ("Azeri", 1), ("Zaza", 1), ("Mixed Turkish", 2)],
    "IRN": [("Persian", 53), ("Azeri", 16), ("Kurdish", 10), ("Gilaki", 5), ("Mazandarani", 5), ("Arab", 3), ("Baloch", 2), ("Turkmen", 2), ("Lur", 2), ("Mixed Iranian", 2)],
    "IRQ": [("Arab", 70), ("Kurdish", 19), ("Turkmen", 3), ("Assyrian", 2), ("Yazidi", 2), ("Armenian", 1), ("Mixed Iraqi", 3)],
    "SYR": [("Arab", 72), ("Kurdish", 10), ("Turkmen", 3), ("Assyrian", 3), ("Circassian", 2), ("Armenian", 2), ("Alawite", 2), ("Druze", 1), ("Ismaili", 1), ("Mhallami", 1), ("Mixed Syrian", 3)],
    "LBN": [("Lebanese Arab", 78), ("Armenian", 4), ("Kurdish", 3), ("Assyrian", 2), ("Circassian", 2), ("Greek", 2), ("Syriac", 2), ("Turkmen", 2), ("Jewish", 1), ("Maronite", 3), ("Mixed Lebanese", 3)],
    "ISR": [("Jewish Israeli", 72), ("Arab Israeli", 21), ("Druze", 2), ("Bedouin", 1), ("Circassian", 1), ("Armenian", 1), ("Mixed Israeli", 2)],
    "PSE": [("Palestinian Arab", 93), ("Bedouin", 1), ("Armenian", 1), ("Greek", 1), ("Jewish", 1), ("Mixed Palestinian", 3)],
    "JOR": [("Jordanian Arab", 85), ("Circassian", 2), ("Chechen", 2), ("Armenian", 2), ("Kurdish", 2), ("Bedouin", 3), ("Syrian", 2), ("Mixed Jordanian", 2)],
    "SAU": [("Saudi", 72), ("Indian", 5), ("Pakistani", 5), ("Bangladeshi", 3), ("Filipino", 3), ("Egyptian", 3), ("Syrian", 2), ("Yemeni", 2), ("Indonesian", 2), ("Mixed Saudi", 3)],
    "YEM": [("Yemeni Arab", 85), ("Afro-Arab", 5), ("Indian", 2), ("Somali", 2), ("Turkish", 1), ("Mixed Yemeni", 5)],
    "OMN": [("Omani", 50), ("Indian", 15), ("Pakistani", 12), ("Bangladeshi", 8), ("Filipino", 3), ("Egyptian", 2), ("Iranian", 2), ("British", 2), ("Mixed Omani", 6)],
    "ARE": [("Emirati", 15), ("Indian", 25), ("Pakistani", 15), ("Bangladeshi", 8), ("Filipino", 8), ("Egyptian", 5), ("British", 5), ("American", 3), ("Iranian", 3), ("Chinese", 3), ("Syrian", 2), ("Mixed Emirati", 8)],
    "QAT": [("Qatari", 15), ("Indian", 20), ("Pakistani", 12), ("Bangladeshi", 10), ("Filipino", 8), ("Egyptian", 5), ("Syrian", 4), ("Iranian", 3), ("British", 3), ("Mixed Qatari", 20)],
    "KWT": [("Kuwaiti", 30), ("Indian", 15), ("Pakistani", 10), ("Bangladeshi", 8), ("Filipino", 5), ("Egyptian", 5), ("Syrian", 4), ("Iranian", 3), ("British", 2), ("Mixed Kuwaiti", 18)],
    "BHR": [("Bahraini", 45), ("Indian", 15), ("Pakistani", 10), ("Bangladeshi", 8), ("Filipino", 5), ("Egyptian", 3), ("Syrian", 2), ("British", 2), ("Iranian", 2), ("Mixed Bahraini", 8)],
    "EGY": [("Egyptian", 88), ("Bedouin", 3), ("Nubian", 2), ("Coptic", 3), ("Berber", 1), ("Greek", 1), ("Mixed Egyptian", 2)],
    "DZA": [("Algerian Arab", 70), ("Berber", 24), ("Tuareg", 1), ("Mixed Algerian", 5)],
    "MAR": [("Moroccan Arab", 60), ("Berber", 33), ("Tuareg", 1), ("Jewish", 1), ("Mixed Moroccan", 5)],
    "TUN": [("Tunisian Arab", 78), ("Berber", 18), ("French", 1), ("Italian", 1), ("Mixed Tunisian", 2)],
    "LBY": [("Libyan Arab", 82), ("Berber", 10), ("Tuareg", 3), ("Tebu", 2), ("Mixed Libyan", 3)],
    "SSD": [("Dinka", 33), ("Nuer", 15), ("Shilluk", 5), ("Azande", 5), ("Bari", 5), ("Jur", 4), ("Murle", 4), ("Lotuko", 3), ("Mundari", 3), ("Kakwa", 2), ("Pojulu", 2), ("Moru", 2), ("Madi", 2), ("Lugbara", 2), ("Lango", 2), ("Acholi", 2), ("Baka", 1), ("Fertit", 1), ("Toposa", 1), ("Didinga", 1), ("Boya", 1), ("Tenet", 1), ("Mixed South Sudanese", 4)],
    "ESH": [("Sahrawi", 78), ("Arab", 15), ("Berber", 3), ("Spanish", 2), ("Mixed Sahrawi", 2)],
    "MRT": [("Haratin", 35), ("Moor", 40), ("Fulani", 5), ("Soninke", 3), ("Wolof", 2), ("Bambara", 1), ("Mixed Mauritanian", 14)],
    # Sub-Saharan Africa
    "ZAF": [("Zulu", 18), ("Xhosa", 15), ("Afrikaner", 8), ("English South African", 6), ("Coloured", 8), ("Indian South African", 3), ("Sotho", 6), ("Tswana", 6), ("Pedi", 5), ("Tsonga", 4), ("Swazi", 3), ("Ndebele", 2), ("Venda", 2), ("Chinese South African", 1), ("Mixed South African", 13)],
    "NGA": [("Hausa", 23), ("Yoruba", 21), ("Igbo", 18), ("Fulani", 9), ("Ijaw", 3), ("Kanuri", 3), ("Ibibio", 3), ("Tiv", 2), ("Edo", 2), ("Nupe", 2), ("Urhobo", 2), ("Idoma", 2), ("Igala", 2), ("Mangawa", 2), ("Jukun", 1), ("Tarok", 1), ("Berom", 1), ("Kuteb", 1), ("Mixed Nigerian", 2)],
    "ETH": [("Oromo", 34), ("Amhara", 25), ("Somali", 6), ("Tigray", 6), ("Sidama", 4), ("Gurage", 3), ("Welayta", 3), ("Afar", 2), ("Hadiya", 2), ("Kaffa", 2), ("Gamo", 2), ("Slite", 1), ("Konso", 1), ("Bench", 1), ("Karo", 1), ("Dizi", 1), ("Nuer", 1), ("Anuak", 1), ("Daasanach", 1), ("Mixed Ethiopian", 4)],
    "KEN": [("Kikuyu", 17), ("Luhya", 14), ("Kalenjin", 13), ("Luo", 11), ("Kamba", 10), ("Somali", 6), ("Kisii", 6), ("Mijikenda", 5), ("Meru", 4), ("Turkana", 3), ("Maasai", 2), ("Embu", 2), ("Taita", 2), ("Pokomo", 1), ("Samburu", 1), ("Rendille", 1), ("Borana", 1), ("Orma", 1)],
    "TZA": [("Sukuma", 16), ("Nyamwezi", 6), ("Chagga", 5), ("Haya", 5), ("Hehe", 4), ("Gogo", 4), ("Makonde", 4), ("Nyakyusa", 4), ("Yao", 3), ("Pare", 3), ("Zaramo", 3), ("Luguru", 3), ("Shambala", 2), ("Ngoni", 2), ("Makua", 2), ("Ha", 2), ("Bena", 2), ("Fipa", 2), ("Mbugwe", 2), ("Kuria", 2), ("Jita", 2), ("Kwaya", 2), ("Sagara", 2), ("Zigula", 2), ("Bondei", 2), ("Rangi", 2), ("Sandawe", 2), ("Hadza", 1), ("Iraqw", 1), ("Datooga", 1), ("Maasai", 1), ("Luo", 1), ("Zinza", 1), ("Kerewe", 1), ("Tongwe", 1), ("Bende", 1), ("Pimbwe", 1), ("Wanda", 1), ("Mwera", 1), ("Ndali", 1), ("Lambya", 1), ("Safwa", 1), ("Malila", 1)],
    "UGA": [("Baganda", 16), ("Banyankole", 9), ("Basoga", 8), ("Bakonjo", 7), ("Iteso", 6), ("Langi", 6), ("Bagisu", 5), ("Acholi", 5), ("Lugbara", 4), ("Banyarwanda", 4), ("Batoro", 3), ("Karamojong", 3), ("Banyoro", 3), ("Alur", 3), ("Jopadhola", 2), ("Kakwa", 2), ("Kumam", 2), ("Sebei", 2), ("Samia", 2), ("Chiga", 2), ("Ganda", 1), ("Nkole", 1), ("Soga", 1), ("Kiga", 1), ("Gisu", 1), ("Nyole", 1), ("Kenyi", 1), ("Madi", 1), ("Mvuba", 1)],
    "RWA": [("Hutu", 84), ("Tutsi", 12), ("Twa", 2), ("Indian", 1), ("Mixed Rwandan", 1)],
    "BDI": [("Hutu", 83), ("Tutsi", 14), ("Twa", 1), ("Rwandan", 1), ("Mixed Burundian", 1)],
    "SOM": [("Somali", 83), ("Bantu Somali", 4), ("Benadiri", 3), ("Arab", 2), ("Swahili", 2), ("Galgala", 2), ("Zigua", 1), ("Oromo", 1), ("Mixed Somali", 2)],
    "XSO": [("Somali", 88), ("Arab", 5), ("Bantu Somali", 3), ("Ethiopian", 2), ("Mixed Somalilander", 2)],
    "DJI": [("Somali", 60), ("Afar", 35), ("Arab", 3), ("French", 1), ("Ethiopian", 1)],
    "SDN": [("Sudanese Arab", 65), ("Nubian", 5), ("Beja", 5), ("Fur", 3), ("Nuba", 3), ("Fallata", 2), ("Zaghawa", 2), ("Masalit", 2), ("Dinka", 2), ("Nuer", 2), ("Shilluk", 2), ("Anuak", 1), ("Berta", 1), ("Mixed Sudanese", 5)],
    "ERI": [("Tigrinya", 55), ("Tigre", 30), ("Saho", 5), ("Afar", 4), ("Bilen", 2), ("Kunama", 2), ("Nara", 1), ("Rashida", 1)],
    "GHA": [("Akan", 45), ("Mole-Dagbon", 16), ("Ewe", 13), ("Ga-Dangme", 8), ("Guan", 4), ("Gurma", 3), ("Bissa", 3), ("Mande", 3), ("Hausa", 3), ("Mixed Ghanaian", 2)],
    "CIV": [("Akan", 40), ("Mandé", 27), ("Krou", 14), ("Gur", 11), ("Senufo", 5), ("Dyula", 1), ("Mixed Ivorian", 2)],
    "MLI": [("Bambara", 33), ("Fulani", 14), ("Soninke", 10), ("Tuareg", 9), ("Senufo", 8), ("Malinke", 7), ("Dogon", 6), ("Songhai", 5), ("Sarakole", 3), ("Bozo", 2), ("Mianka", 2), ("Khassonke", 1), ("Tamasheq", 1), ("Maure", 1)],
    "BFA": [("Mossi", 40), ("Fulani", 9), ("Gurunsi", 7), ("Bobo", 7), ("Senufo", 6), ("Lobi", 5), ("Tuareg", 4), ("Marka", 3), ("Bissa", 3), ("Dagari", 3), ("Bwa", 3), ("Samo", 2), ("Nuni", 2), ("Kassena", 2), ("Lyela", 2), ("Winiama", 1), ("Nouna", 1)],
    "NER": [("Hausa", 53), ("Zarma-Songhai", 20), ("Tuareg", 10), ("Fulani", 8), ("Kanuri", 4), ("Arab", 2), ("Gurma", 1), ("Mixed Nigerien", 2)],
    "SEN": [("Wolof", 38), ("Fulani", 23), ("Serer", 14), ("Jola", 5), ("Mandinka", 4), ("Soninke", 4), ("Tukulor", 3), ("Lebou", 2), ("Bassari", 1), ("Bayot", 1), ("Mankanya", 1), ("Balanta", 1), ("Moor", 1), ("Mixed Senegalese", 2)],
    "GMB": [("Mandinka", 33), ("Fulani", 18), ("Wolof", 12), ("Jola", 10), ("Serer", 8), ("Soninke", 5), ("Aku", 4), ("Manjago", 3), ("Bambara", 2), ("Fula", 2), ("Mixed Gambian", 3)],
    "GIN": [("Fulani", 32), ("Malinke", 28), ("Soussou", 18), ("Kpelle", 4), ("Toma", 3), ("Kissi", 3), ("Konianke", 2), ("Maninka", 2), ("Mikhifore", 2), ("Baga", 1), ("Landoma", 1), ("Basari", 1), ("Mano", 1), ("Kono", 1)],
    "GNB": [("Fulani", 28), ("Balanta", 22), ("Mandinka", 14), ("Papel", 10), ("Manjaco", 7), ("Beafada", 5), ("Bissago", 4), ("Mancanha", 3), ("Jola", 2), ("Bijago", 2), ("Portuguese", 1), ("Mestiço", 1), ("Mixed Guinea-Bissauan", 1)],
    "SLE": [("Temne", 33), ("Mende", 30), ("Limba", 9), ("Kono", 5), ("Kuranko", 4), ("Fulani", 3), ("Loko", 3), ("Sherbro", 3), ("Mandingo", 3), ("Susu", 2), ("Kissi", 2), ("Yalunka", 1), ("Vai", 1), ("Krio", 1)],
    "LBR": [("Kpelle", 20), ("Bassa", 14), ("Grebo", 8), ("Gio", 8), ("Kru", 7), ("Mano", 7), ("Lorma", 5), ("Gola", 4), ("Kissi", 3), ("Vaai", 3), ("Mandingo", 3), ("Fulani", 3), ("Belle", 2), ("Krahn", 2), ("Gbandi", 2), ("Mende", 2), ("Sapo", 2), ("Gbii", 2), ("Kono", 1), ("Dey", 1), ("Mixed Liberian", 3)],
    "BEN": [("Fon", 39), ("Adja", 16), ("Yoruba", 12), ("Bariba", 9), ("Fulani", 7), ("Aizo", 4), ("Dendi", 3), ("Hausa", 3), ("Ottamari", 2), ("Mina", 2), ("Xwla", 2), ("Sahoué", 1)],
    "TGO": [("Ewe", 32), ("Kabye", 14), ("Moba", 8), ("Gourma", 7), ("Mina", 6), ("Kotokoli", 5), ("Ife", 4), ("Ane", 3), ("Akposso", 3), ("Bassar", 3), ("Peul", 2), ("Lamba", 2), ("Ntribou", 2), ("Kusasi", 2), ("Nawdeba", 2), ("Konkomb", 2), ("Bimoba", 1), ("Bissa", 1), ("Temberma", 1)],
    "CMR": [("Fang", 20), ("Bamileke", 19), ("Duala", 10), ("Fulani", 10), ("Bassa", 8), ("Tikar", 5), ("Mafa", 4), ("Mada", 4), ("Kotoko", 3), ("Masa", 3), ("Kanuri", 3), ("Arab", 2), ("Hausa", 2), ("Pygmies", 1), ("Mixed Cameroonian", 1)],
    "CAF": [("Baya", 33), ("Banda", 27), ("Mandjia", 13), ("Sara", 10), ("Mboum", 7), ("M'Baka", 4), ("Yakoma", 2), ("Peuhl", 2), ("Gbi", 1), ("Runga", 1)],
    "TCD": [("Sara", 27), ("Arab", 12), ("Kanembu", 9), ("Maba", 6), ("Hausa", 5), ("Fulani", 4), ("Gorane", 3), ("Masa", 3), ("Mararit", 3), ("Baguirmi", 2), ("Moussei", 2), ("Boulala", 2), ("Goundo", 2), ("Kaba", 2), ("Gula", 2), ("Sila", 2), ("Kenga", 2), ("Bidio", 2), ("Barma", 2), ("Zaghawa", 2), ("Lisi", 2), ("Toupouri", 2), ("Moundang", 2)],
    "GAB": [("Fang", 32), ("Punu", 15), ("Nzebi", 11), ("Mpongwe", 9), ("Teke", 8), ("Mitsogo", 5), ("Obamba", 4), ("Kota", 3), ("Vili", 3), ("Myene", 2), ("Lumbu", 2), ("Shira", 2), ("Pygmies", 2), ("French", 1), ("Portuguese", 1)],
    "COG": [("Kongo", 48), ("Teke", 17), ("Mbochi", 12), ("Sangha", 5), ("Maka", 4), ("Pygmies", 3), ("Mbamba", 3), ("Vili", 2), ("Lari", 2), ("Koungo", 2), ("Koukouya", 1), ("Ngangulu", 1)],
    "COD": [("Luba", 18), ("Kongo", 16), ("Mongo", 14), ("Rwandan", 10), ("Azande", 8), ("Bangala", 6), ("Rundi", 5), ("Twa", 3), ("Lunda", 3), ("Hutu", 3), ("Lingala", 3), ("Kwilu", 3), ("Tetela", 2), ("Kusu", 2), ("Mbudza", 2), ("Binza", 2)],
    "AGO": [("Ovimbundu", 37), ("Kimbundu", 25), ("Bakongo", 13), ("Mestiço Angolan", 3), ("White Angolan", 2), ("Chokwe", 2), ("Lunda", 2), ("Nganguela", 2), ("Nyaneka", 2), ("Humbi", 2), ("Ambo", 2), ("Herero", 1), ("Xindonga", 1), ("Kwangwa", 1), ("San", 1), ("Khoisan", 1), ("Mbangala", 1), ("Songo", 1), ("Sama", 1)],
    "ZMB": [("Bemba", 21), ("Tonga", 14), ("Chewa", 8), ("Lozi", 6), ("Nsenga", 5), ("Tumbuka", 4), ("Lunda", 4), ("Kaonde", 3), ("Luvale", 3), ("Mambwe", 3), ("Lenje", 2), ("Ila", 2), ("Ushi", 2), ("Lala", 2), ("Ngoni", 2), ("Soli", 2), ("Bisa", 2), ("Mukulu", 2), ("Lamba", 2), ("Aushi", 2), ("Chishinga", 2), ("Kunda", 1), ("Swaka", 1), ("Ambo", 1), ("Mpukushu", 1), ("Twa", 1), ("White Zambian", 1), ("Indian Zambian", 1)],
    "MWI": [("Chewa", 34), ("Lomwe", 18), ("Yao", 13), ("Ngoni", 11), ("Tumbuka", 9), ("Sena", 4), ("Tonga", 3), ("Nkhonde", 2), ("Lambya", 2), ("Sukwa", 1), ("Mang'anja", 1), ("Zulu", 1), ("Indian Malawian", 1)],
    "MOZ": [("Makua", 26), ("Tsonga", 16), ("Malawi", 11), ("Shona", 9), ("Yao", 8), ("Maravi", 7), ("Ngoni", 5), ("Mwani", 4), ("Makonde", 4), ("Chopi", 3), ("Mwera", 2), ("Tonga", 2), ("Sena", 2), ("Portuguese Mozambican", 1)],
    "MDG": [("Merina", 26), ("Betsimisaraka", 15), ("Betsileo", 12), ("Tsimihety", 7), ("Antandroy", 5), ("Antsaka", 5), ("Antanosy", 4), ("Bara", 3), ("Vezo", 2), ("Mahafaly", 2), ("Sakalava", 2), ("Masikoro", 2), ("Tanala", 2), ("Antaifasy", 2), ("Antambahoaka", 2), ("Zafisoro", 1), ("Makoa", 1), ("French", 1), ("Chinese", 1), ("Indian", 1), ("Comorian", 1)],
    "ZWE": [("Shona", 68), ("Ndebele", 16), ("Tonga", 2), ("Venda", 2), ("Kalanga", 2), ("Sotho", 2), ("Nambya", 1), ("Shangaan", 1), ("Xhosa", 1), ("Chewa", 1), ("White Zimbabwean", 1), ("Indian Zimbabwean", 1), ("Mixed Zimbabwean", 2)],
    "BWA": [("Tswana", 70), ("Kalanga", 8), ("Bushmen", 4), ("Kgalagadi", 3), ("Herero", 2), ("Mbukushu", 2), ("Yei", 2), ("Subiya", 1), ("Tswapong", 1), ("White Botswanan", 1), ("Indian Botswanan", 1), ("San", 1), ("Khoe", 1), ("Mixed Botswanan", 2)],
    "NAM": [("Ovambo", 48), ("Kavango", 10), ("Damara", 8), ("Herero", 7), ("White Namibian", 6), ("Nama", 5), ("Caprivian", 4), ("Bushmen", 3), ("Tswana", 2), ("Coloured Namibian", 2), ("Lozi", 1), ("Indian Namibian", 1), ("San", 1), ("Mixed Namibian", 2)],
    "LSO": [("Sotho", 92), ("Zulu", 3), ("Xhosa", 1), ("Indian Mosotho", 1), ("Mixed Basotho", 3)],
    "SWZ": [("Swazi", 82), ("Zulu", 10), ("White Swazi", 3), ("Tsonga", 2), ("Indian Swazi", 1), ("Mixed Swazi", 2)],
    "GNQ": [("Fang", 80), ("Bubi", 6), ("Ndowe", 5), ("Bisio", 3), ("Bujeba", 2), ("Annobonese", 2), ("Fernandino", 1), ("Spanish", 1)],
    "MUS": [("Indo-Mauritian", 48), ("Creole Mauritian", 32), ("Sino-Mauritian", 3), ("Franco-Mauritian", 2), ("Chinese", 2), ("Tamil", 2), ("Telugu", 2), ("Marathi", 2), ("Gujarati", 2), ("Bhojpuri", 2), ("Mixed Mauritian", 3)],
    "STP": [("Mestiço Santomean", 60), ("Angolar", 15), ("Forros", 10), ("Serviçal", 5), ("Tongas", 5), ("Portuguese", 5)],
    "SYC": [("Seychellois Creole", 78), ("French", 4), ("Chinese", 4), ("Indian", 4), ("English", 3), ("Malagasy", 3), ("Filipino", 2), ("Mixed Seychellois", 2)],
    "COM": [("Comorian", 88), ("Indian", 4), ("Arab", 3), ("Malagasy", 2), ("French", 1), ("Mixed Comorian", 2)],
    "CPV": [("Cape Verdean", 68), ("Portuguese", 10), ("Black Cape Verdean", 5), ("Mixed Cape Verdean", 10), ("Chinese", 3), ("Senegalese", 2), ("White Cape Verdean", 2)],
    # Indian subcontinent
    "IND": [("Hindustani", 40), ("Bengali", 8), ("Tamil", 7), ("Telugu", 7), ("Marathi", 6), ("Gujarati", 5), ("Punjabi", 4), ("Bihari", 3), ("Rajasthani", 3), ("Assamese", 2), ("Odia", 2), ("Kannada", 2), ("Malayali", 2), ("Sindhi", 1), ("Kashmiri", 1), ("Adivasi", 5), ("Mixed Indian", 2)],
    "PAK": [("Punjabi", 42), ("Pashtun", 15), ("Sindhi", 14), ("Saraiki", 8), ("Urdu", 8), ("Baloch", 4), ("Hindko", 2), ("Brahui", 2), ("Kashmiri", 1), ("Shina", 1), ("Mixed Pakistani", 3)],
    "BGD": [("Bengali", 95), ("Bihari", 1), ("Chakma", 1), ("Marma", 1), ("Santali", 1), ("Mixed Bangladeshi", 1)],
    "LKA": [("Sinhalese", 72), ("Tamil", 15), ("Moor", 9), ("Malay", 1), ("Burgher", 1), ("Mixed Sri Lankan", 2)],
    "NPL": [("Chhetri", 16), ("Brahmin", 12), ("Magar", 8), ("Tharu", 7), ("Tamang", 6), ("Newar", 5), ("Kami", 4), ("Rai", 4), ("Gurung", 3), ("Damai", 3), ("Yadav", 3), ("Musalman", 3), ("Sarki", 2), ("Limbu", 2), ("Bhote", 2), ("Sherpa", 1), ("Sunuwar", 1), ("Jirel", 1), ("Chepang", 1), ("Danuwar", 1), ("Kumal", 1), ("Darai", 1), ("Thami", 1), ("Tajpuriya", 1), ("Gangai", 1), ("Santhal", 1), ("Kisan", 1), ("Koche", 1), ("Munda", 1), ("Dhimal", 1), ("Bote", 1)],
    "BTN": [("Ngalop", 50), ("Sharchop", 25), ("Lhotshampa", 20), ("Kheng", 3), ("Bumthang", 2)],
    # East Asia
    "CHN": [("Han Chinese", 87), ("Zhuang", 2), ("Hui", 1), ("Manchu", 1), ("Uyghur", 1), ("Miao", 1), ("Yi", 1), ("Tibetan", 1), ("Mongol", 1), ("Buyei", 1), ("Korean", 1), ("Mixed Chinese", 2)],
    "TWN": [("Han Taiwanese", 93), ("Indigenous Taiwanese", 3), ("Chinese", 1), ("Japanese", 1), ("Mixed Taiwanese", 2)],
    "JPN": [("Japanese", 94), ("Chinese", 2), ("Korean", 1), ("Filipino", 1), ("Brazilian", 1), ("Mixed Japanese", 1)],
    "KOR": [("Korean", 95), ("Chinese", 2), ("Vietnamese", 1), ("Filipino", 1), ("Mixed Korean", 1)],
    "PRK": [("Korean", 98), ("Chinese", 1), ("Mixed Korean", 1)],
    "MNG": [("Mongol", 83), ("Kazakh", 5), ("Tuvan", 3), ("Buryat", 2), ("Dörvöd", 2), ("Zakhchin", 1), ("Torguud", 1), ("Khalkha", 1), ("Mixed Mongolian", 2)],
    # Southeast Asia
    "MMR": [("Bamar", 60), ("Shan", 9), ("Karen", 7), ("Rakhine", 5), ("Mon", 3), ("Chinese", 3), ("Kachin", 2), ("Indian", 2), ("Chin", 2), ("Kayin", 2), ("Kayah", 1), ("Wa", 1), ("Palung", 1), ("Mixed Burmese", 2)],
    "THA": [("Thai", 78), ("Chinese", 10), ("Malay", 3), ("Khmer", 2), ("Lao", 2), ("Karen", 1), ("Mon", 1), ("Hmong", 1), ("Mixed Thai", 2)],
    "VNM": [("Kinh", 80), ("Tay", 2), ("Thai", 2), ("Muong", 2), ("Khmer", 2), ("Hmong", 2), ("Hoa", 2), ("Nung", 1), ("Dao", 1), ("Cham", 1), ("Ede", 1), ("Gia Rai", 1), ("Ba Na", 1), ("San Diu", 1), ("Mixed Vietnamese", 2)],
    "LAO": [("Lao", 55), ("Khmu", 11), ("Hmong", 8), ("Phuan", 3), ("Tai Dam", 3), ("Lue", 2), ("Akha", 2), ("Katu", 2), ("Bru", 2), ("Ta Oi", 2), ("Ngae", 2), ("Yao", 2), ("Laven", 2), ("Katang", 2), ("Alak", 2)],
    "KHM": [("Khmer", 93), ("Chinese", 2), ("Vietnamese", 1), ("Cham", 1), ("Lao", 1), ("Mixed Cambodian", 2)],
    "IDN": [("Javanese", 38), ("Sundanese", 15), ("Malay", 10), ("Madurese", 5), ("Batak", 4), ("Bugis", 3), ("Minangkabau", 3), ("Balinese", 2), ("Chinese", 2), ("Banjarese", 2), ("Acehnese", 2), ("Sasak", 2), ("Dayak", 2), ("Papuan", 2), ("Makassar", 2), ("Cirebonese", 1), ("Lampung", 1), ("Gorontalo", 1), ("Palembang", 1), ("Mixed Indonesian", 3)],
    "PHL": [("Tagalog", 26), ("Cebuano", 13), ("Ilocano", 9), ("Bisaya", 8), ("Hiligaynon", 7), ("Bicolano", 6), ("Waray", 3), ("Kapampangan", 3), ("Pangasinan", 2), ("Maranao", 2), ("Maguindanao", 2), ("Tausug", 2), ("Chinese Filipino", 2), ("Japanese", 1), ("Spanish", 1), ("American", 1), ("Korean", 1), ("Aeta", 1), ("Igorot", 1), ("Lumad", 1), ("Moro", 1), ("Mixed Filipino", 5)],
    "MYS": [("Malay", 48), ("Chinese", 23), ("Indian", 7), ("Orang Asli", 11), ("Iban", 3), ("Bidayuh", 2), ("Kadazan", 2), ("Dusun", 2), ("Mixed Malaysian", 2)],
    "SGP": [("Chinese", 73), ("Malay", 13), ("Indian", 9), ("Eurasian", 1), ("Filipino", 1), ("Thai", 1), ("Mixed Singaporean", 2)],
    "TLS": [("Tetum", 30), ("Mambai", 15), ("Makasae", 10), ("Tukude", 8), ("Galoli", 6), ("Kemak", 5), ("Bunak", 5), ("Fataluku", 4), ("Bekais", 3), ("Lovaea", 3), ("Makalero", 3), ("Baikeno", 2), ("Portuguese", 2), ("Chinese", 2), ("Indonesian", 2)],
    "BRN": [("Malay", 65), ("Chinese", 10), ("Indigenous Bruneian", 5), ("Indian", 3), ("Filipino", 2), ("Mixed Bruneian", 15)],
    # Central Asia
    "KAZ": [("Kazakh", 58), ("Russian", 22), ("Ukrainian", 3), ("Uzbek", 3), ("German", 2), ("Tatar", 2), ("Uyghur", 2), ("Korean", 2), ("Azeri", 1), ("Turkish", 1), ("Dungan", 1), ("Kurdish", 1), ("Mixed Kazakh", 2)],
    "UZB": [("Uzbek", 78), ("Tajik", 5), ("Russian", 4), ("Kazakh", 3), ("Karakalpak", 2), ("Tatar", 2), ("Korean", 2), ("Turkmen", 1), ("Ukrainian", 1), ("Mixed Uzbek", 2)],
    "TKM": [("Turkmen", 73), ("Uzbek", 10), ("Russian", 5), ("Kazakh", 2), ("Tatar", 2), ("Baloch", 2), ("Armenian", 1), ("Azeri", 1), ("Lezgian", 1), ("Ukrainian", 1), ("Mixed Turkmen", 2)],
    "KGZ": [("Kyrgyz", 66), ("Uzbek", 15), ("Russian", 6), ("Dungan", 2), ("Ukrainian", 2), ("Tatar", 2), ("Kazakh", 1), ("Tajik", 1), ("Turkish", 1), ("German", 1), ("Korean", 1), ("Mixed Kyrgyz", 2)],
    "TJK": [("Tajik", 73), ("Uzbek", 15), ("Russian", 2), ("Kyrgyz", 2), ("Turkmen", 2), ("Tatar", 1), ("Ukrainian", 1), ("German", 1), ("Korean", 1), ("Mixed Tajik", 2)],
    # Afghanistan
    "AFG": [("Pashtun", 40), ("Tajik", 27), ("Hazara", 9), ("Uzbek", 9), ("Aimaq", 4), ("Turkmen", 3), ("Baloch", 2), ("Nuristani", 2), ("Pashai", 1), ("Kyrgyz", 1), ("Mixed Afghan", 2)],
    # Oceania
    "AUS": [("Anglo Australian", 62), ("Chinese Australian", 5), ("Indian Australian", 3), ("Vietnamese Australian", 2), ("Filipino Australian", 2), ("Italian Australian", 3), ("Greek Australian", 2), ("Aboriginal", 3), ("Pacific Islander", 2), ("Middle Eastern Australian", 2), ("African Australian", 2), ("Mixed Australian", 7)],
    "NZL": [("European New Zealander", 65), ("Maori", 15), ("Chinese New Zealander", 4), ("Indian New Zealander", 3), ("Pacific Islander", 7), ("Middle Eastern", 1), ("Mixed New Zealander", 5)],
    "PNG": [("Papuan", 58), ("Melanesian", 20), ("Highlander", 10), ("Coastal", 5), ("Chinese", 2), ("Australian", 2), ("Filipino", 1), ("Mixed Papua New Guinean", 2)],
    "FJI": [("iTaukei", 54), ("Indian Fijian", 37), ("Rotuman", 1), ("Chinese", 1), ("European Fijian", 1), ("Pacific Islander", 1), ("Mixed Fijian", 5)],
    "SLB": [("Melanesian", 90), ("Polynesian", 3), ("Micronesian", 2), ("Chinese", 1), ("European", 1), ("Gilbertese", 1), ("Mixed Solomon Islander", 2)],
    "VUT": [("Melanesian", 93), ("European", 2), ("Chinese", 1), ("Vietnamese", 1), ("French", 1), ("Mixed Ni-Vanuatu", 2)],
    "NCL": [("Kanak", 40), ("European", 30), ("Wallisian", 8), ("Tahitian", 6), ("Indonesian", 4), ("Vietnamese", 3), ("Chinese", 2), ("Mixed New Caledonian", 7)],
    "PYF": [("Polynesian", 68), ("Mixed Tahitian", 20), ("Chinese", 5), ("French", 3), ("European", 2), ("Mixed French Polynesian", 2)],
    "WSM": [("Samoan", 90), ("Mixed Samoan", 8), ("Chinese", 1), ("European", 1)],
    "TON": [("Tongan", 93), ("Chinese", 2), ("European", 1), ("Mixed Tongan", 3), ("Fijian", 1)],
    "KIR": [("I-Kiribati", 88), ("Mixed Kiribati", 7), ("Chinese", 2), ("European", 1), ("Pacific Islander", 1), ("Filipino", 1)],
    "MHL": [("Marshallese", 88), ("Mixed Marshallese", 7), ("Chinese", 2), ("European", 1), ("Filipino", 1), ("Japanese", 1)],
    "PLW": [("Palauan", 68), ("Filipino", 15), ("Chinese", 5), ("Japanese", 3), ("Korean", 2), ("Mixed Palauan", 4), ("Caroline Islander", 2), ("European", 1)],
    "FSM": [("Chuukese", 30), ("Pohnpeian", 25), ("Kosraean", 20), ("Yapese", 15), ("Mixed Micronesian", 5), ("Chinese", 2), ("Filipino", 2), ("Japanese", 1)],
    "NRU": [("Nauruan", 56), ("Mixed Nauruan", 27), ("Chinese", 8), ("Kiribati", 3), ("Fijian", 2), ("Tuvaluan", 2), ("European", 1), ("Filipino", 1)],
    "TUV": [("Tuvaluan", 83), ("Mixed Tuvaluan", 12), ("Gilbertese", 3), ("Chinese", 1), ("European", 1)],
    "COK": [("Cook Islands Maori", 73), ("Mixed Cook Islander", 17), ("Chinese", 5), ("European", 3), ("Filipino", 2)],
    "NIU": [("Niuean", 63), ("Mixed Niuean", 22), ("Chinese", 8), ("European", 5), ("Tongan", 2)],
    # Other territories
    "GRL": [("Inuit", 85), ("Danish", 10), ("Icelandic", 3), ("American", 2)],
    "ATF": [("French", 85), ("Mixed French", 10), ("Italian", 5)],
    "FLK": [("British Falklander", 85), ("Mixed Falklander", 7), ("Filipino", 3), ("Chilean", 3), ("St Helenian", 2)],
    "SGS": [("British", 75), ("Mixed South Georgian", 10), ("Filipino", 5), ("Chilean", 5), ("South African", 5)],
    "ATA": [("White", 90), ("Mixed", 5), ("Asian", 3), ("Black", 2)],
}

# Minimal name normalizations: only spelling/terminology fixes and variant mapping
NAME_ALIASES = {
    "Roma": "Romani",
    "Gypsy": "Romani",
    "Kurd": "Kurdish",
    "Pygmy": "Pygmies",
    "Pygnies": "Pygmies",
    "Bamar": "Burmese",
    "Mestico": "Mestiço",
}

def normalize(name):
    return NAME_ALIASES.get(name, name)

GENERIC_PATTERNS = re.compile(r'\b(Other|Generic|Misc)\b', re.I)

def should_keep(name):
    name_clean = name.strip()
    if GENERIC_PATTERNS.search(name_clean):
        return False
    return True

def color_from_name(name):
    h = int(hashlib.md5(name.encode()).hexdigest()[:6], 16)
    r = (h & 0xFF) % 200 + 30
    g = ((h >> 8) & 0xFF) % 200 + 30
    b = ((h >> 16) & 0xFF) % 200 + 30
    return [r, g, b]

def main():
    data_dir = '/Users/vladyavdoshenko/CLionProjects/OpenDoctrines/data'
    with open(f'{data_dir}/provinces.json') as f:
        provs = json.load(f)
    result = {}
    colors = {}
    by_iso = {}
    for pid_str, entry in provs.items():
        iso = entry.get('iso_a3', '')
        if not iso:
            continue
        if iso not in by_iso:
            by_iso[iso] = []
        by_iso[iso].append(int(pid_str))

    # Continent-based fallback for unlisted countries
    REGION_FALLBACK = {
        "EUR": [("European", 80), ("Arab", 3), ("Chinese", 2), ("Indian", 2), ("Turkish", 2), ("Mixed European", 11)],
        "AFR": [("African", 60), ("Arab", 10), ("Fulani", 5), ("Hausa", 5), ("Yoruba", 3), ("Igbo", 3), ("Somali", 2), ("Amhara", 2), ("Oromo", 2), ("Berber", 2), ("Mixed African", 6)],
        "ASI": [("Han Chinese", 25), ("Indian", 15), ("Arab", 10), ("Malay", 8), ("Japanese", 5), ("Korean", 4), ("Turkish", 3), ("Persian", 3), ("Punjabi", 3), ("Bengali", 3), ("Javanese", 3), ("Vietnamese", 3), ("Thai", 3), ("Burmese", 2), ("Filipino", 2), ("Kazakh", 2), ("Uzbek", 2), ("Mixed Asian", 4)],
        "AME": [("European American", 35), ("Mestizo American", 25), ("African American", 10), ("Indigenous American", 10), ("Asian American", 5), ("Mixed American", 15)],
        "OCE": [("European Oceanian", 45), ("Pacific Islander", 20), ("Asian Oceanian", 10), ("Maori", 5), ("Indigenous Oceanian", 5), ("Mixed Oceanian", 15)],
        "ANT": [("American", 18), ("Russian", 15), ("Chinese", 10), ("British", 10), ("Australian", 8), ("Argentine", 8), ("Chilean", 5), ("South African", 5), ("French", 5), ("Japanese", 4), ("Indian", 3), ("Korean", 3), ("Mixed nationality", 6)],
    }

    # Approximate continent mapping from ISO alpha-3 (rough)
    def iso_to_region(iso3):
        if iso3 == "UNC": return "ANT"
        euro = {"GBR","FRA","DEU","ITA","ESP","PRT","NLD","BEL","CHE","AUT","POL","CZE","SVK","HUN","ROU","BGR","GRC","HRV","SRB","BIH","SVN","MKD","ALB","MNE","XKX","UKR","BLR","MDA","LTU","LVA","EST","RUS","FIN","SWE","NOR","DNK","ISL","IRL","LUX","MLT","CYP","MCO","AND","LIE","SMR","VAT","GIB","GGY","JEY","IMN","FRO","ALA","SJM","XNC"}
        afric = {"ZAF","NGA","ETH","EGY","KEN","TZA","AGO","DZA","MAR","SDN","SSD","GHA","CMR","CIV","MOZ","MDG","UGA","ZWE","SEN","BFA","MLI","NER","MWI","ZMB","SOM","TCD","RWA","BDI","GIN","BEN","TGO","SLE","LBR","CAF","COG","ERI","MRT","GMB","GAB","BWA","LSO","NAM","SWZ","GNQ","GNB","MUS","DJI","STP","SYC","COM","CPV","COD","LBY","TUN","ESH","XSO"}
        asia = {"CHN","JPN","KOR","PRK","MNG","TWN","IND","PAK","BGD","LKA","NPL","BTN","MMR","THA","VNM","LAO","KHM","IDN","PHL","MYS","SGP","TLS","BRN","KAZ","UZB","TKM","KGZ","TJK","IRN","IRQ","AFG","SAU","YEM","OMN","ARE","QAT","KWT","BHR","LBN","JOR","SYR","ISR","PSE","TUR","AZE","GEO","ARM","MYS","MMR"}
        americas = {"USA","CAN","MEX","GTM","BLZ","SLV","HND","NIC","CRI","PAN","CUB","JAM","HTI","DOM","BHS","BRB","TTO","GRD","VCT","DMA","LCA","ATG","KNA","TCA","VGB","CYM","BMU","COL","VEN","GUY","SUR","ECU","PER","BRA","BOL","PRY","CHL","ARG","URY","FLK","PRI","VIR","GRL","ATF"}
        oceania = {"AUS","NZL","PNG","FJI","SLB","VUT","NCL","PYF","WSM","TON","KIR","MHL","PLW","FSM","NRU","TUV","COK","NIU","GUM","ASM"}
        if iso3 in euro: return "EUR"
        if iso3 in afric: return "AFR"
        if iso3 in asia: return "ASI"
        if iso3 in americas: return "AME"
        if iso3 in oceania: return "OCE"
        return None

    for iso, pids in by_iso.items():
        groups = ETHNIC_MAP.get(iso, None)
        if groups is None:
            region = iso_to_region(iso)
            groups = REGION_FALLBACK.get(region, [("European", 100)])
        # Normalize and filter
        cleaned = []
        for name, base in groups:
            nname = normalize(name)
            if not should_keep(nname):
                continue
            cleaned.append((nname, base))
        if not cleaned:
            cleaned = [("European", 100)]
        groups = cleaned

        for pid in pids:
            rng = random.Random(pid * 1000 + 777)
            raw = []
            for name, base in groups:
                dev = rng.uniform(-max(base * 0.3, 2), max(base * 0.3, 2))
                pct = max(1.0, base + dev)
                raw.append((name, pct))
            total = sum(p for _, p in raw)
            entries = [{"n": name, "p": round(p / total * 100, 1)} for name, p in raw]
            entries = [e for e in entries if e["p"] >= 0.5]
            result[str(pid)] = entries
            for e in entries:
                if e["n"] not in colors:
                    colors[e["n"]] = color_from_name(e["n"])

    out_path = f'{data_dir}/minorities.json'
    with open(out_path, 'w') as f:
        json.dump(result, f, separators=(',', ':'))
    col_path = f'{data_dir}/minority_colors.json'
    with open(col_path, 'w') as f:
        json.dump(colors, f, separators=(',', ':'))
    print(f"Saved {out_path} ({len(result)} provinces, {len(colors)} unique groups)")
    print(f"Saved {col_path} ({len(colors)} colors)")

if __name__ == '__main__':
    main()
