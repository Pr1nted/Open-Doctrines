#!/usr/bin/env python3
"""
Record the licence of every flag the game ships.

WHY THIS EXISTS

`download_flags_fast.py` pulls 180-odd SVGs off Wikimedia Commons and
`prerender_problematic_flags.py` turns them into the PNGs that actually go into
map.odmap. Most national flags there are public domain -- a plain design, or a
government work -- but not all of them are, and "flags are public domain" is a
folk belief rather than a fact. A few renderings on Commons are CC-BY-SA, which
is perfectly usable and costs one line of attribution, and which is a licence
breach if nobody writes that line.

So: ask Commons what each file is actually under, write it down, and fail if
something arrives under terms the project has not agreed to ship.

    python3 tools/audit_flag_licenses.py            # fetch and write
    python3 tools/audit_flag_licenses.py --check    # fail if the file is stale

Outputs, both regenerated wholesale:
    data/licenses/flags.json   machine-readable, one entry per flag
    data/licenses/FLAGS.md     the attribution list, shipped with the game

A PNG inherits the licence of the SVG it was rendered from -- rasterising is
not authorship -- so one entry per source file covers both.
"""

import json
import os
import sys
import time
import urllib.parse
import urllib.request

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TOOLS_DIR)
DATA_DIR = os.path.join(PROJECT_ROOT, "data")
LICENSES_DIR = os.path.join(DATA_DIR, "licenses")
OUT_JSON = os.path.join(LICENSES_DIR, "flags.json")
OUT_MD = os.path.join(LICENSES_DIR, "FLAGS.md")

API = "https://commons.wikimedia.org/w/api.php"
UA = "OpenDoctrines-flag-license-audit/1.0 (https://github.com/; licence compliance)"
BATCH = 50          # the API's own cap on titles per query
RETRIES = 4

# Licences the project has decided it will ship, and what each one obliges.
# Anything not on this list stops the audit: the point is to be told about a
# new obligation, not to have one quietly accepted.
ACCEPTED = {
    "Public domain":            "none",
    "PD":                       "none",
    "CC0":                      "none",
    "CC BY 1.0":                "attribution",
    "CC BY 2.0":                "attribution",
    "CC BY 2.5":                "attribution",
    "CC BY 3.0":                "attribution",
    "CC BY 4.0":                "attribution",
    "CC BY-SA 1.0":             "attribution + share-alike on the image",
    "CC BY-SA 2.0":             "attribution + share-alike on the image",
    "CC BY-SA 2.5":             "attribution + share-alike on the image",
    "CC BY-SA 3.0":             "attribution + share-alike on the image",
    "CC BY-SA 4.0":             "attribution + share-alike on the image",
    # Open Government Licence - Oman, the terms Omani government works carry
    # under that country's Open Data Policy. Permits re-use for any purpose
    # including commercial, on attribution alone. Currently only Flag of Oman.
    # https://www.oman.om/en/home-footer-level/faq/open-government-licence-oman
    "OGL-om 1.0":               "attribution",
}


def strip_html(value):
    """extmetadata returns little HTML fragments for Artist and Credit."""
    if not value:
        return ""
    out, depth = [], 0
    for ch in value:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    text = "".join(out)
    for entity, ch in (("&amp;", "&"), ("&lt;", "<"), ("&gt;", ">"),
                       ("&quot;", '"'), ("&#039;", "'"), ("&nbsp;", " ")):
        text = text.replace(entity, ch)
    return " ".join(text.split())


def fetch(titles):
    """imageinfo/extmetadata for up to BATCH titles. Returns {title: meta}."""
    params = {
        "action": "query",
        "format": "json",
        "formatversion": "2",
        "prop": "imageinfo",
        "iiprop": "extmetadata",
        "titles": "|".join(titles),
    }
    url = f"{API}?{urllib.parse.urlencode(params)}"
    last = None
    for attempt in range(RETRIES):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.loads(r.read().decode("utf-8"))
        except Exception as e:                      # network, 429, malformed
            last = e
            time.sleep(2 ** attempt)
    raise RuntimeError(f"Commons query failed after {RETRIES} tries: {last}")


def audit(filenames):
    """iso -> record. One network round trip per BATCH files."""
    by_title = {}
    for iso, fname in filenames.items():
        by_title.setdefault(f"File:{fname}", []).append(iso)

    titles = sorted(by_title)
    records = {}
    for i in range(0, len(titles), BATCH):
        chunk = titles[i:i + BATCH]
        print(f"  {i + 1}-{i + len(chunk)} of {len(titles)}...")
        data = fetch(chunk)
        # Commons normalises some titles (underscores, capitalisation); follow it.
        normalised = {n["from"]: n["to"] for n in data.get("query", {}).get("normalized", [])}
        pages = {p.get("title"): p for p in data.get("query", {}).get("pages", [])}

        for title in chunk:
            page = pages.get(normalised.get(title, title))
            isos = by_title[title]
            if not page or "imageinfo" not in page:
                for iso in isos:
                    records[iso] = {
                        "file": title[len("File:"):],
                        "license": "UNKNOWN",
                        "artist": "",
                        "credit": "",
                        "url": "",
                        "note": "not found on Commons — check the filename in "
                                "download_flags_fast.py",
                    }
                continue
            meta = page["imageinfo"][0].get("extmetadata", {})

            def field(key):
                return strip_html(meta.get(key, {}).get("value", ""))

            license_name = field("LicenseShortName") or "UNKNOWN"
            for iso in isos:
                records[iso] = {
                    "file": title[len("File:"):],
                    "license": license_name,
                    "artist": field("Artist"),
                    "credit": field("Credit"),
                    "url": meta.get("LicenseUrl", {}).get("value", ""),
                    "obligation": ACCEPTED.get(license_name, "REVIEW"),
                }
        time.sleep(0.4)
    return records


def render_markdown(records):
    needs_attr = {
        iso: r for iso, r in records.items()
        if r.get("obligation") not in ("none", None) or r["license"] == "UNKNOWN"
    }
    free = {iso: r for iso, r in records.items() if iso not in needs_attr}

    lines = [
        "# Flag artwork",
        "",
        "Generated by `tools/audit_flag_licenses.py` from the Wikimedia Commons",
        "API. Do not edit by hand — rerun the tool.",
        "",
        "The PNGs in `flags/` inside `map.odmap` are rasterisations of these SVGs.",
        "Rasterising is not authorship, so each PNG is under the same terms as the",
        "file it came from.",
        "",
        f"{len(records)} files: {len(free)} public domain or equivalent, "
        f"{len(needs_attr)} requiring attribution or review.",
        "",
    ]

    if needs_attr:
        lines += [
            "## Attribution required",
            "",
            "| Country | File | Licence | Author |",
            "|---|---|---|---|",
        ]
        for iso in sorted(needs_attr):
            r = needs_attr[iso]
            author = r.get("artist") or r.get("credit") or "unattributed on Commons"
            lines.append(f"| {iso} | {r['file']} | {r['license']} | {author} |")
        lines.append("")

    lines += [
        "## Public domain or equivalent",
        "",
        "No attribution obligation. Listed for provenance.",
        "",
        "| Country | File | Licence |",
        "|---|---|---|",
    ]
    for iso in sorted(free):
        r = free[iso]
        lines.append(f"| {iso} | {r['file']} | {r['license']} |")
    lines.append("")
    return "\n".join(lines)


def main(argv):
    check = "--check" in argv

    sys.path.insert(0, TOOLS_DIR)
    try:
        from download_flags_fast import FILENAMES
    except ImportError as e:
        print(f"cannot import the flag list: {e}")
        return 1
    # Historical flags for scenario powers live in their own mapping but ship in
    # the same archives under the same terms, so they are audited the same way.
    FILENAMES = dict(FILENAMES)
    try:
        with open(os.path.join(TOOLS_DIR, "data", "scenario_flags.json"),
                  encoding="utf-8") as f:
            FILENAMES.update(json.load(f)["flags"])
    except (FileNotFoundError, KeyError, json.JSONDecodeError):
        pass

    if check:
        # --check must not hit the network: it runs in tests. It asserts the
        # audit exists and covers the current flag list, which is the failure
        # that actually happens (a flag added, the audit not rerun).
        if not os.path.exists(OUT_JSON):
            print(f"MISSING {OUT_JSON} — run: python3 tools/audit_flag_licenses.py")
            return 1
        with open(OUT_JSON) as f:
            recorded = json.load(f)
        missing = sorted(set(FILENAMES) - set(recorded.get("flags", {})))
        stale = sorted(
            iso for iso, r in recorded.get("flags", {}).items()
            if iso in FILENAMES and r.get("file") != FILENAMES[iso]
        )
        if missing or stale:
            if missing:
                print(f"{len(missing)} flag(s) have no recorded licence: {', '.join(missing)}")
            if stale:
                print(f"{len(stale)} flag(s) changed file since the audit: {', '.join(stale)}")
            print("run: python3 tools/audit_flag_licenses.py")
            return 1
        review = [iso for iso, r in recorded["flags"].items()
                  if r.get("obligation") == "REVIEW" or r.get("license") == "UNKNOWN"]
        if review:
            print(f"{len(review)} flag(s) under terms the project has not accepted: "
                  f"{', '.join(sorted(review))}")
            return 1
        print(f"flag licences: {len(recorded['flags'])} recorded, all accepted")
        return 0

    print(f"Auditing {len(FILENAMES)} flags against the Commons API...")
    records = audit(FILENAMES)

    os.makedirs(LICENSES_DIR, exist_ok=True)
    with open(OUT_JSON, "w") as f:
        json.dump({
            "$comment": "Generated by tools/audit_flag_licenses.py. Do not edit.",
            "source": "Wikimedia Commons, prop=imageinfo&iiprop=extmetadata",
            "flags": dict(sorted(records.items())),
        }, f, indent=1)
        f.write("\n")
    with open(OUT_MD, "w") as f:
        f.write(render_markdown(records))
    print(f"Wrote {OUT_JSON} and {OUT_MD}")

    review = sorted(iso for iso, r in records.items()
                    if r.get("obligation") == "REVIEW" or r["license"] == "UNKNOWN")
    if review:
        print()
        print(f"{len(review)} flag(s) are under terms not in the accepted list. "
              "Either add the licence to ACCEPTED once you have read it, or "
              "replace the artwork:")
        for iso in review:
            print(f"    {iso:5} {records[iso]['license']:24} {records[iso]['file']}")
        return 1
    print(f"All {len(records)} flags are under accepted terms.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
