#!/usr/bin/env python3
"""Build ISO 3166 alpha-3 to alpha-2 mapping by querying Wikipedia API."""

import json
import sys
import urllib.request
import urllib.parse

COUNTRIES_JSON = sys.argv[1] if len(sys.argv) > 1 else "data/countries.json"

with open(COUNTRIES_JSON) as f:
    data = json.load(f)

isos = set()
for v in data.values():
    iso = v.get("iso_a3", "")
    if iso and iso != "-99":
        isos.add(iso)

# Try to get alpha-2 from Wikipedia API for each
# Wikipedia stores ISO 3166-1 alpha-2 in infobox
mapping = {}

for iso3 in sorted(isos):
    params = {
        "action": "query",
        "prop": "pageprops",
        "titles": iso3,
        "format": "json"
    }
    url = "https://en.wikipedia.org/w/api.php?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": "OpenDoctrines/1.0"})
    try:
        resp = urllib.request.urlopen(req, timeout=5)
        text = resp.read().decode()
        # Parse the response
        import re
        match = re.search(r'"pageprops"\s*:\s*{[^}]*"iso3166code"\s*:\s*"([A-Z]{2})"', text)
        if match:
            mapping[iso3] = match.group(1).lower()
        else:
            mapping[iso3] = None
    except:
        mapping[iso3] = None

# Print as Python dict
print("ISO_MAP = {")
for iso3, iso2 in sorted(mapping.items()):
    print(f'    "{iso3}": {json.dumps(iso2)}', end="")
    print(",")
print("}")
