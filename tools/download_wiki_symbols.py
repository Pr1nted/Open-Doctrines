"""Download real SVG symbols from Wikipedia Commons with rate-limiting."""
import os, sys, urllib.request, shutil, time

SYMBOLS_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "data", "flags", "symbols")
os.makedirs(SYMBOLS_DIR, exist_ok=True)

WIKI_URL = "https://upload.wikimedia.org/wikipedia/commons"

# Use thumbnail URLs (less likely to 429, and WP suggested them)
symbols = {
    "star5.svg":           f"{WIKI_URL}/1/1e/Star_5_point.svg",
    "star6.svg":           f"{WIKI_URL}/4/44/Star_of_David.svg",
    "star_of_david.svg":   f"{WIKI_URL}/4/44/Star_of_David.svg",
    "crescent.svg":        f"{WIKI_URL}/4/4a/Crescent.svg",
    "crescent_star.svg":   f"{WIKI_URL}/e/e2/Star_and_Crescent.svg",
    "hammer_sickle.svg":   f"{WIKI_URL}/e/ee/Hammer_and_sickle.svg",
    "cross_latin.svg":     f"{WIKI_URL}/b/b7/Christian_cross.svg",
    "cross_maltese.svg":   f"{WIKI_URL}/3/30/Maltese_cross.svg",
    "cross_saltir.svg":    f"{WIKI_URL}/3/3e/Saltire.svg",
    "cross_nordic.svg":    f"{WIKI_URL}/b/ba/Nordic_cross.svg",
    "sun.svg":             f"{WIKI_URL}/0/0c/Sun_symbol.svg",
    "sun_wavy.svg":        f"{WIKI_URL}/b/b5/Sunburst_flag_icon.svg",
    "gear.svg":            f"{WIKI_URL}/f/fc/Gear_icon.svg",
    "sword.svg":           f"{WIKI_URL}/5/5d/Sword_icon.svg",
    "crossed_swords.svg":  f"{WIKI_URL}/7/7d/Crossed_swords.svg",
    "mountain.svg":        f"{WIKI_URL}/4/4b/Mountain_icon.svg",
    "tree.svg":            f"{WIKI_URL}/3/34/Tree_icon.svg",
    "diamond.svg":         f"{WIKI_URL}/8/85/Diamond_shape.svg",
    "eagle.svg":           f"{WIKI_URL}/a/a2/Coat_of_arms_of_Germany.svg",
    "fasces.svg":          f"{WIKI_URL}/f/f8/Fasces.svg",
    "torch.svg":           f"{WIKI_URL}/f/fc/Torch_icon.svg",
    "rose.svg":            f"{WIKI_URL}/d/d3/Rose_icon.svg",
    "anchor.svg":          f"{WIKI_URL}/6/67/Anchor_icon.svg",
}

headers = {
    'User-Agent': 'OpenDoctrines/1.0 (data generation tool; vlad@opendoctrines.org)'
}

# Try thumbnail pattern: use png/large for most icons
def try_download(name, url, path):
    # Try original URL first
    try:
        req = urllib.request.Request(url, headers=headers)
        resp = urllib.request.urlopen(req, timeout=15)
        data = resp.read()
        if len(data) > 50:
            with open(path, 'wb') as f:
                f.write(data)
            print(f"  [OK]   {name} ({len(data)} bytes)")
            return True
    except urllib.error.HTTPError as e:
        if e.code == 404:
            print(f"  [404]  {name} — trying thumbnail...")
        else:
            print(f"  [ERR]  {name} — {e}")

    # Try as thumbnail from Wikimedia thumbnail service
    # Pattern: https://upload.wikimedia.org/wikipedia/commons/thumb/<hash>/<filename>/256px-<filename>
    parts = url.rstrip('/').split('/')
    hash_dir = parts[-2]  # e.g., "1/1e"
    filename = parts[-1]
    thumb_url = f"{WIKI_URL}/thumb/{hash_dir}/{filename}/256px-{filename}"
    try:
        time.sleep(1)
        req = urllib.request.Request(thumb_url, headers=headers)
        resp = urllib.request.urlopen(req, timeout=15)
        data = resp.read()
        if len(data) > 50:
            with open(path, 'wb') as f:
                f.write(data)
            print(f"  [OK]   {name} (thumb, {len(data)} bytes)")
            return True
    except Exception as e2:
        print(f"  [FAIL] {name} — {e2}")

    return False

downloaded = 0
failed = 0
for name, url in symbols.items():
    path = os.path.join(SYMBOLS_DIR, name)
    if try_download(name, url, path):
        downloaded += 1
    else:
        failed += 1
    time.sleep(0.5)  # Rate limit: 2 requests/second max

skip_count = 0
total = len(symbols)
print(f"\n{downloaded} downloaded, {failed} failed, {skip_count} skipped  ({total} total)")
if failed > 0:
    print("Note: keeping generated fallback SVGs for failed downloads")
