#!/usr/bin/env python3
"""
Fetch the flag symbols in data/symbols/ from Wikimedia Commons, and record what
each one is licensed under.

    python3 tools/download_symbols.py            # fetch, convert, write licences
    python3 tools/download_symbols.py anchor gear   # just these

WHY ONLY SOME SYMBOLS COME FROM COMMONS

The game rasterises these with nanosvg (src/renderer/nanosvg.h), which
implements a subset of SVG and IGNORES the rest -- no gradients, no filters, no
<mask>, no <use>, no <text>, no CSS. Nothing errors; the file just renders as
something other than the picture. Measured, at the size a flag symbol is
actually drawn:

    circle_stars  <use>                 rendered COMPLETELY BLANK
    rose          detail                rendered a solid blob, 90% of the canvas
    fasces        gradient + filter     rendered unreadable, from 126 KB of art
    eagle         detail (117 KB)       rendered as noise

So Commons is the right source for GEOMETRIC symbols -- crosses, stars, the
crescent, an anchor -- and the wrong source for PICTORIAL ones. The pictorial
symbols in data/symbols/ are the project's own simplified silhouettes, which is
not a stopgap: a silhouette authored for 32px beats a rasteriser's best attempt
at heraldic linework every time. Anything added to PICKS below must be checked
by rendering it, not by looking at it on Commons.

WHAT IS DONE TO EACH FILE

  * recoloured white   -- the game draws symbols as white silhouettes onto a
                          coloured flag. Fills AND strokes: a black stroke in an
                          otherwise white symbol is the bug this exists to avoid.
  * refitted           -- into viewBox "-100 -100 200 200", scaled to fit and
                          centred, never stretched.
  * sanitised          -- <script> and on* handlers removed. nanosvg would
                          ignore them, but a file in data/ is one other tools may
                          open, and it arrived off the internet.

None of that is authorship, so no licence changes because of it. Obligations are
written to data/licenses/symbols.json; run this and commit the result together.

RATE LIMIT: Commons returns HTTP 429 if asked too quickly, and says so plainly.
PAUSE below is deliberate -- do not remove it to make this faster.
"""

import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request
from xml.etree import ElementTree

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TOOLS_DIR)
SYMBOLS_DIR = os.path.join(PROJECT_ROOT, "data", "symbols")
LICENSES = os.path.join(PROJECT_ROOT, "data", "licenses", "symbols.json")

API = "https://commons.wikimedia.org/w/api.php"
UA = "OpenDoctrines-symbols/1.0 (https://github.com/Pr1nted/OpenDoctrines)"
PAUSE = 1.5
BOX = 200.0

# Licences the project ships, and what each obliges. Same list and same purpose
# as ACCEPTED in tools/audit_flag_licenses.py: to be told about a new obligation
# rather than to accept one quietly.
ACCEPTED = {
    "Public domain": "none",
    "PD": "none",
    "CC0": "none",
    "CC BY 1.0": "attribution", "CC BY 2.0": "attribution",
    "CC BY 2.5": "attribution", "CC BY 3.0": "attribution",
    "CC BY 4.0": "attribution",
    "CC BY-SA 1.0": "attribution + share-alike on the image",
    "CC BY-SA 2.0": "attribution + share-alike on the image",
    "CC BY-SA 2.5": "attribution + share-alike on the image",
    "CC BY-SA 3.0": "attribution + share-alike on the image",
    "CC BY-SA 4.0": "attribution + share-alike on the image",
}

# symbol name in data/symbols  ->  Commons file. Every one of these was checked
# by rendering it through nanosvg first; see the note at the top of this file.
PICKS = {
    "anchor":         "Anchor pictogram.svg",
    "cross_latin":    "Christian cross.svg",
    "cross_maltese":  "Maltese Cross simplest.svg",
    "cross_nordic":   "Nordic cross.svg",
    "cross_pattee":   "Cross-Pattee-Heraldry.svg",
    "cross_saltir":   "Saltire cross icon.svg",
    "crossed_swords": "Crossed swords symbol.svg",
    "star5":          "Five Pointed Star Solid.svg",
    "star6":          "Regular hexagram.svg",
    "star_4":         "Four points star.svg",
    "star_of_david":  "Star of David.svg",
    "sun":            "Sun symbol (bold).svg",
    # Censored in game; see data/symbols/eagle_nazi.svg and Game::buildRebelFlag.
    "eagle_nazi":     "Reichsadler.svg",
}

# Deliberately NOT taken from Commons. Kept as the project's own silhouettes
# because the Commons artwork does not survive nanosvg at symbol size -- the
# measurements are in the docstring above. Listed here so the reason is on the
# record rather than looking like an oversight.
OWN_WORK_ON_PURPOSE = {
    "circle_stars":  "Commons <use> renders blank",
    "rose":          "Commons file renders as a solid blob",
    "fasces":        "Commons file uses gradient+filter, renders unreadable",
    # No generic eagle ships at all. The hand-drawn one did not read as a bird,
    # and every Commons heraldic eagle is 100 KB+ of linework that renders as
    # noise at symbol size, so the symbol was removed rather than shipped
    # looking wrong. eagle_nazi.svg is in PICKS above and does survive, because
    # it is a silhouette. If a generic one is ever wanted it has to be drawn,
    # not downloaded.
    "crescent":      "own path with a real cut-out; Commons version is CC BY-SA for no gain",
    "crescent_star": "as crescent",
    "gear":          "own evenodd path with a real hub hole",
    "diamond":       "trivial shape; Commons version carries <use> and <text>",
    "hammer_sickle": "Commons file uses a gradient",
    "mountain":      "no usable Commons candidate",
    "sword":         "no usable Commons candidate",
    "swastika":      "own simple path; censored in game either way",
    "sun_wavy":      "Commons heraldic suns are detailed linework",
    "torch":         "Commons candidates are detailed linework",
    "tree":          "Commons candidates are detailed linework",
}


def api(params):
    params = dict(params, format="json")
    req = urllib.request.Request(f"{API}?{urllib.parse.urlencode(params)}",
                                 headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def strip_html(v):
    return re.sub(r"<[^>]*>", "", v or "").strip()


def fetch(title):
    """(svg bytes, licence dict) for one Commons file."""
    d = api({"action": "query", "titles": "File:" + title,
             "prop": "imageinfo", "iiprop": "url|extmetadata"})
    page = next(iter(d["query"]["pages"].values()))
    if "missing" in page:
        raise RuntimeError("no such file on Commons")
    ii = page["imageinfo"][0]
    meta = ii.get("extmetadata", {})
    time.sleep(PAUSE)
    req = urllib.request.Request(ii["url"], headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=40) as r:
        data = r.read()
    return data, {
        "file": title,
        "source": "https://commons.wikimedia.org/wiki/File:" + title.replace(" ", "_"),
        "license": meta.get("LicenseShortName", {}).get("value", "?"),
        "artist": strip_html(meta.get("Artist", {}).get("value", "")),
    }


def viewport(svg):
    """Source width/height, from the viewBox if it has one."""
    vb = re.search(r'viewBox\s*=\s*"([^"]+)"', svg)
    if vb:
        p = [float(x) for x in re.split(r"[ ,]+", vb.group(1).strip())]
        if len(p) == 4:
            return p[0], p[1], p[2], p[3]
    w = re.search(r'\swidth\s*=\s*"([\d.]+)', svg)
    h = re.search(r'\sheight\s*=\s*"([\d.]+)', svg)
    return 0.0, 0.0, float(w.group(1)) if w else BOX, float(h.group(1)) if h else BOX


def convert(svg_bytes, name):
    """Commons SVG -> a white, correctly framed game symbol."""
    s = svg_bytes.decode("utf-8", "replace")
    s = re.sub(r"<\?xml[^>]*\?>", "", s)
    s = re.sub(r"<!DOCTYPE[^>]*>", "", s, flags=re.I)
    s = re.sub(r"<script\b.*?</script>", "", s, flags=re.S | re.I)
    s = re.sub(r"\son\w+\s*=\s*\"[^\"]*\"", "", s, flags=re.I)
    s = re.sub(r"<!--.*?-->", "", s, flags=re.S)

    minx, miny, w, h = viewport(s)
    inner = re.sub(r"^.*?<svg[^>]*>", "", s, flags=re.S)
    inner = re.sub(r"</svg>\s*$", "", inner, flags=re.S).strip()

    # Editor cruft, and the reason it cannot simply be carried across: half of
    # Commons is Inkscape output, which uses inkscape: and sodipodi: prefixes
    # bound by xmlns declarations on the <svg> root -- the very element being
    # replaced here. Carried over, those prefixes are unbound and the file is
    # not well-formed XML at all. star_4 shipped exactly that way.
    #
    # <defs> goes too: it holds gradients and symbols for <use>, neither of
    # which nanosvg implements, so it is weight that can only mislead.
    inner = re.sub(r"<defs\b.*?</defs\s*>", "", inner, flags=re.S | re.I)
    inner = re.sub(r"<defs\b[^>]*/>", "", inner, flags=re.I)
    inner = re.sub(r"<metadata\b.*?</metadata\s*>", "", inner, flags=re.S | re.I)
    # Prefixed ELEMENTS (<inkscape:foo .../>, <sodipodi:bar>...</sodipodi:bar>).
    inner = re.sub(r"<(\w+):(\w+)\b[^>]*?/>", "", inner)
    inner = re.sub(r"<(\w+):(\w+)\b.*?</\1:\2\s*>", "", inner, flags=re.S)
    # Prefixed ATTRIBUTES, and the xmlns: declarations that bound them.
    inner = re.sub(r'\sxmlns:\w+\s*=\s*"[^"]*"', "", inner)
    inner = re.sub(r'\s\w+:\w+\s*=\s*"[^"]*"', "", inner)

    # Every colour becomes white. fill="none" is left alone -- it means "do not
    # paint this", which is a shape decision, not a colour.
    inner = re.sub(r'fill\s*=\s*"(?!none)[^"]*"', 'fill="#fff"', inner, flags=re.I)
    inner = re.sub(r'stroke\s*=\s*"(?!none)[^"]*"', 'stroke="#fff"', inner, flags=re.I)
    inner = re.sub(r'fill\s*:\s*(?!none)[^;"]+', 'fill:#fff', inner, flags=re.I)
    inner = re.sub(r'stroke\s*:\s*(?!none)[^;"]+', 'stroke:#fff', inner, flags=re.I)

    scale = BOX / max(w, h)                       # fit, never stretch
    tx = -(w * scale) / 2.0 - minx * scale
    ty = -(h * scale) / 2.0 - miny * scale
    out = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
            f'  <!-- {name}: from Wikimedia Commons, recoloured white and refitted.\n'
            f'       Regenerate with tools/download_symbols.py; licence and source\n'
            f'       are in data/licenses/symbols.json. Do not hand-edit. -->\n'
           f'  <g fill="#fff" transform="translate({tx:.3f},{ty:.3f}) '
           f'scale({scale:.6f})">\n{inner}\n  </g>\n</svg>\n')

    # Parse what is about to be written. A symbol that is not well-formed XML
    # is not a symbol, and the only reason star_4 shipped broken is that
    # nothing checked -- nanosvg fails quietly and the game draws nothing.
    try:
        ElementTree.fromstring(out)
    except ElementTree.ParseError as e:
        raise RuntimeError(f"converted SVG is not well-formed XML: {e}")
    return out


def main():
    wanted = sys.argv[1:] or list(PICKS)
    unknown = [w for w in wanted if w not in PICKS]
    if unknown:
        print(f"not in PICKS: {', '.join(unknown)}", file=sys.stderr)
        return 2

    records, problems = {}, []
    for name in wanted:
        title = PICKS[name]
        try:
            raw, lic = fetch(title)
        except Exception as e:
            print(f"  FAIL  {name:15s} {e}")
            problems.append(name)
            time.sleep(PAUSE)
            continue

        obligation = ACCEPTED.get(lic["license"])
        if obligation is None:
            # Not a licence the project has agreed to ship. Refuse the file
            # rather than write it and hope somebody notices the JSON later.
            print(f"  STOP  {name:15s} unknown licence {lic['license']!r} -- not written")
            problems.append(name)
            time.sleep(PAUSE)
            continue

        out = convert(raw, name)
        with open(os.path.join(SYMBOLS_DIR, name + ".svg"), "w", encoding="utf-8") as f:
            f.write(out)
        lic["obligation"] = obligation
        lic["modifications"] = ("Recoloured white and refitted to the -100 -100 200 200 "
                                "symbol viewBox. Neither is authorship.")
        records[name + ".svg"] = lic
        print(f"  ok    {name:15s} {len(out):6d} B  {lic['license']}")
        time.sleep(PAUSE)

    if records:
        existing = {}
        if os.path.exists(LICENSES):
            with open(LICENSES) as f:
                existing = json.load(f)
        existing.setdefault("$comment",
            "Provenance for data/symbols/. Regenerate with tools/download_symbols.py. "
            "Symbols listed under own_work are the project's own and carry no third-party "
            "obligation; see that tool for why they are not taken from Commons.")
        existing["source"] = "Wikimedia Commons, where a file is not own work"
        existing.setdefault("symbols", {}).update(records)
        existing["own_work"] = {
            "$comment": "Project's own SVGs. The reason each is not sourced from Commons "
                        "is recorded in tools/download_symbols.py (OWN_WORK_ON_PURPOSE).",
            "files": {k: v for k, v in sorted(OWN_WORK_ON_PURPOSE.items())},
        }
        with open(LICENSES, "w", encoding="utf-8") as f:
            json.dump(existing, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print(f"\nwrote {len(records)} licence record(s) to {LICENSES}")

    if problems:
        print(f"\n{len(problems)} symbol(s) not written: {', '.join(problems)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
