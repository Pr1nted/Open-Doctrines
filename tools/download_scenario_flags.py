#!/usr/bin/env python3
"""
Download the historical flags scenario powers need, from Wikimedia Commons.

Scenario powers are not modern countries -- there is no `flags/GER.png` for the
German Reich, no Austria-Hungary, no Ottoman Empire -- so without this they fall
back to procedural stripes, which looks like a placeholder because it is one.

Reuses `download_flags_fast.py` wholesale: the same MD5-path URL construction
(no API, no rate limit), the same SVG sniffing, the same retry policy. Files
land in `data/flags/` under their scenario code, so every step downstream
already handles them: `inline_svg_use.py` resolves their `<use>` elements,
`prerender_problematic_flags.py` rasterises them, and `audit_flag_licenses.py`
records what each is under and refuses anything not on the accepted list.

    python3 tools/download_scenario_flags.py
    python3 tools/download_scenario_flags.py --force

The mapping is tools/data/scenario_flags.json.
"""

import json
import os
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TOOLS_DIR)
FLAGS_DIR = os.path.join(PROJECT_ROOT, "data", "flags")
MAPPING = os.path.join(TOOLS_DIR, "data", "scenario_flags.json")

sys.path.insert(0, TOOLS_DIR)
from download_flags_fast import get_download_url, download_flag   # noqa: E402

API = "https://commons.wikimedia.org/w/api.php"
UA = "OpenDoctrines-scenario-flags/1.0"


def resolve_via_api(filename):
    """Ask Commons for the real file URL.

    The MD5-path trick assumes the title you have is the canonical one. It is
    not, for a handful of these: some are redirects, and some differ from the
    obvious spelling by a character that does not survive being guessed. The
    API normalises and follows redirects, so it answers where the fast path
    only 404s. Slower, and used only after the fast path has failed.
    """
    import json as _json
    import urllib.parse
    import urllib.request
    params = {
        "action": "query", "format": "json", "formatversion": "2",
        "prop": "imageinfo", "iiprop": "url", "redirects": "1",
        "titles": "File:" + filename,
    }
    url = f"{API}?{urllib.parse.urlencode(params)}"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=30) as r:
            data = _json.loads(r.read())
        for page in data.get("query", {}).get("pages", []):
            info = page.get("imageinfo")
            if info:
                return info[0].get("url")
    except Exception:
        pass
    return None


def main(argv):
    force = "--force" in argv
    delay = 0.3
    if "--delay" in argv:
        delay = float(argv[argv.index("--delay") + 1])

    with open(MAPPING, encoding="utf-8") as f:
        doc = json.load(f)
    if doc.get("schema") != 1:
        print(f"{MAPPING}: unsupported schema {doc.get('schema')!r}")
        return 1

    os.makedirs(FLAGS_DIR, exist_ok=True)
    flags = doc["flags"]
    ok = skipped = failed = 0
    missing = []

    print(f"Fetching {len(flags)} historical flags into data/flags/ ...")
    for code, filename in sorted(flags.items()):
        dest = os.path.join(FLAGS_DIR, code + ".svg")
        if os.path.exists(dest) and not force:
            skipped += 1
            continue
        data = download_flag(get_download_url(filename))
        if not data:
            real = resolve_via_api(filename)
            if real:
                data = download_flag(real)
                if data:
                    print(f"  {code:14} (via API) {real.rsplit('/', 1)[-1]}")
        if data:
            with open(dest, "wb") as f:
                f.write(data)
            print(f"  {code:14} {filename}  ({len(data):,} bytes)")
            ok += 1
        else:
            print(f"  {code:14} FAILED  {filename}")
            missing.append((code, filename))
            failed += 1
        time.sleep(delay)

    print(f"\n{ok} downloaded, {skipped} already present, {failed} failed")
    if missing:
        # Two very different causes, and the message used to blame the wrong one.
        # A 404 means the filename is wrong. A 429 means Wikimedia is rate
        # limiting this host, which it does readily after a few hundred files,
        # and the same name will work later untouched. Both leave the power on
        # procedural stripes, so say which it was.
        rate_limited = any(resolve_via_api(f) for _, f in missing)
        print("\nThese have no file yet, and their powers fall back to a "
              "procedural pattern until they do:")
        for code, filename in missing:
            exists = resolve_via_api(filename) is not None
            why = ("exists on Commons — this was rate limiting, rerun later"
                   if exists else "NOT FOUND on Commons — fix the filename")
            print(f"    {code:14} {filename}\n        {why}")
        if rate_limited:
            print("\nWikimedia returns 429 after a few hundred files from one "
                  "host. Wait a few minutes and rerun; nothing needs changing.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
