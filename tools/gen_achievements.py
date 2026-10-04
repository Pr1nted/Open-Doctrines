#!/usr/bin/env python3
"""Generate every copy of the achievement catalog from the one that is edited.

    python3 tools/gen_achievements.py            # write everything
    python3 tools/gen_achievements.py --check    # fail if anything has drifted
    python3 tools/gen_achievements.py --icons    # also redraw the icons (slow-ish)
    python3 tools/gen_achievements.py --unifico ../Unifico   # and the launcher's copy

tools/achievements/catalog.json is the source. From it:

  src/achievements/AchievementCatalog.gen.h   compiled into the game. A table in
                                               the BINARY, not a file in data/:
                                               an achievement whose condition
                                               lived in a shipped JSON file could
                                               be earned with a text editor.
  net/src/achievements/catalog.gen.ts          the Worker's clock and tier table
  docs/steam/achievements.md + .csv            what to type into Steamworks
  docs/steam/achievements-loc.vdf              the localised names, for Steam's
                                               localisation upload
  docs/steam/achievements/*.png                256x256 achieved/unachieved icons
  data/icons/achievements.png                  the 64px atlas the game draws
  <unifico>/src/gen/catalog.gen.h              the launcher's copy, same table

--check is in tests/run_all.sh, so editing the catalog without regenerating
fails there rather than shipping a game and a service that disagree about
what an achievement requires.
"""

import argparse
import csv
import hashlib
import io
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CATALOG = os.path.join(HERE, "achievements", "catalog.json")
LANG = os.path.join(ROOT, "data", "lang")

# Icon keyword -> a Noto Emoji codepoint. Noto Emoji is under the SIL OFL, so
# the icons it draws are redistributable; see docs/steam/achievements.md.
ICON_GLYPHS = {
    "crown": "\U0001F451", "hourglass": "\u231B", "flags": "\U0001F6A9", "mouse": "\U0001F42D",
    "elephant": "\U0001F418", "eye": "\U0001F441", "bolt": "\u26A1", "book": "\U0001F4D6",
    "globe": "\U0001F30D", "floppy": "\U0001F4BE", "box": "\U0001F4E6", "rocket": "\U0001F680",
    "trophy": "\U0001F3C6", "swords": "\u2694", "angry": "\U0001F620", "shield": "\U0001F6E1",
    "flag": "\U0001F3F4", "whiteflag": "\U0001F3F3", "skull": "\U0001F480", "tombstone": "\U0001FAA6",
    "helmet": "\U0001FA96", "poster": "\U0001F4E2", "pitchfork": "\U0001F531", "cannon": "\U0001F4A5",
    "fire": "\U0001F525", "plane": "\u2708", "flask": "\U0001F9EA", "atom": "\u2622",
    "virus": "\U0001F9A0", "facepalm": "\U0001F926", "handshake": "\U0001F91D", "dove": "\U0001F54A",
    "flower": "\U0001F33C", "scroll": "\U0001F4DC", "envelope": "\u2709", "pin": "\U0001F4CD",
    "coins": "\U0001F4B0", "empty": "\U0001F4B8", "scale": "\u2696", "factory": "\U0001F3ED",
    "hammer": "\U0001F528", "obelisk": "\U0001F5FF", "map": "\U0001F5FA", "people": "\U0001F465",
    "tower": "\U0001F5FC", "castle": "\U0001F3F0", "compass": "\U0001F9ED", "ship": "\U0001F6A2",
    "anchor": "\u2693", "brush": "\U0001F58C", "film": "\U0001F3AC", "cross": "\u271D",
}

CATEGORIES = ["meta", "war", "artillery", "diplomacy", "economy", "empire",
              "science", "society", "navy", "social", "creator"]

# Steam's own language names for the languages this game ships. Languages Steam
# does not support (Esperanto, Latin, Kyrgyz and so on) are simply absent from
# the Steam file; the game and the launcher still show them translated.
STEAM_LANGS = {
    "en": "english", "ar": "arabic", "bg": "bulgarian", "zh": "schinese", "cs": "czech",
    "da": "danish", "nl": "dutch", "fi": "finnish", "fr": "french", "de": "german",
    "el": "greek", "hu": "hungarian", "it": "italian", "ja": "japanese", "ko": "koreana",
    "nb": "norwegian", "pl": "polish", "pt": "portuguese", "ro": "romanian",
    "sv": "swedish", "tr": "turkish", "uk": "ukrainian", "vi": "vietnamese", "es": "spanish",
}


def load():
    d = json.load(open(CATALOG, encoding="utf-8"))
    out = []
    ids = set()
    for i, a in enumerate(d["achievements"]):
        a = dict(a)
        assert a["id"] not in ids, f"duplicate id {a['id']}"
        ids.add(a["id"])
        assert a["cat"] in CATEGORIES, f"{a['id']}: unknown category {a['cat']}"
        assert a["icon"] in ICON_GLYPHS, f"{a['id']}: unknown icon {a['icon']}"
        for r in a.get("requires", []):
            assert r in ids, f"{a['id']} requires {r}, which must come earlier in the catalog"
        a.setdefault("requires", [])
        a.setdefault("hidden", False)
        a.setdefault("minSessionMinutes", 0)
        a["steam"] = "ACH_" + a["id"].upper()
        out.append(a)
    return out


def catalog_hash(achs):
    canon = json.dumps([{k: a[k] for k in ("id", "rule", "minHours", "minSessionMinutes", "requires")}
                        for a in achs], sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canon.encode()).hexdigest()[:16]


def cstr(s):
    return json.dumps(s, ensure_ascii=False)


def icon_order(achs):
    seen = []
    for a in achs:
        if a["icon"] not in seen:
            seen.append(a["icon"])
    return seen


def gen_cpp(achs, namespace_comment):
    icons = icon_order(achs)
    lines = [
        "// GENERATED by tools/gen_achievements.py from tools/achievements/catalog.json.",
        "// Do not edit: edit the catalog and regenerate. --check in tests/run_all.sh",
        "// fails when this file and the catalog disagree.",
        "//",
        namespace_comment,
        "#pragma once",
        "",
        "#include <cstddef>",
        "",
        "namespace odach {",
        "",
        "struct Def {",
        "    const char* id;          // stable forever: grants name it",
        "    const char* steam;       // Steamworks API name",
        "    const char* name;        // English; translated through T() when drawn",
        "    const char* desc;",
        "    const char* cat;",
        "    const char* stat;        // what Achievements.cpp measures",
        "    double      gte;         // claimed when stat >= gte",
        "    int         minHours;    // service clock: hours since first play session",
        "    int         minSessionMinutes;",
        "    bool        hidden;      // description withheld until earned",
        "    const char* prereqs;     // comma-separated ids, may be empty",
        "    int         icon;        // tile in data/icons/achievements.png",
        "};",
        "",
        f"inline constexpr int kIconTiles = {len(icons)};",
        "inline constexpr int kIconTilePx = 64;",
        "",
        f'inline constexpr const char* kCatalogHash = "{catalog_hash(achs)}";',
        "",
        "inline constexpr Def kCatalog[] = {",
    ]
    for a in achs:
        r = a["rule"]
        lines.append(
            f"    {{{cstr(a['id'])}, {cstr(a['steam'])}, {cstr(a['name'])}, {cstr(a['desc'])}, "
            f"{cstr(a['cat'])}, {cstr(r['stat'])}, {float(r['gte'])!r}, {a['minHours']}, "
            f"{a['minSessionMinutes']}, {'true' if a['hidden'] else 'false'}, "
            f"{cstr(','.join(a['requires']))}, {icons.index(a['icon'])}}},")
    lines += [
        "};",
        "",
        "inline constexpr int kCatalogCount = int(sizeof(kCatalog) / sizeof(kCatalog[0]));",
        "",
        "inline constexpr const char* kCategories[] = {",
        "    " + ", ".join(cstr(c) for c in CATEGORIES),
        "};",
        f"inline constexpr int kCategoryCount = {len(CATEGORIES)};",
        "",
        "}  // namespace odach",
        "",
    ]
    return "\n".join(lines)


def gen_ts(achs):
    rows = []
    for a in achs:
        rows.append("    { id: %s, steam: %s, hidden: %s, minHours: %d, minSessionMinutes: %d, requires: %s }," % (
            json.dumps(a["id"]), json.dumps(a["steam"]), "true" if a["hidden"] else "false",
            a["minHours"], a["minSessionMinutes"], json.dumps(a["requires"])))
    return "\n".join([
        "// GENERATED by tools/gen_achievements.py from tools/achievements/catalog.json.",
        "// Do not edit. The game compiles the same catalog; the two must agree on",
        "// every id, tier and clock, which is why neither is written by hand.",
        "",
        "export interface CatalogEntry {",
        "    id: string;",
        "    steam: string;",
        "    hidden: boolean;",
        "    minHours: number;",
        "    minSessionMinutes: number;",
        "    requires: string[];",
        "}",
        "",
        f'export const CATALOG_HASH = "{catalog_hash(achs)}";',
        "",
        "export const CATALOG: CatalogEntry[] = [",
        *rows,
        "];",
        "",
    ])


def translations():
    out = {}
    if not os.path.isdir(LANG):
        return out
    for f in sorted(os.listdir(LANG)):
        if f.endswith(".json") and not f.endswith(".names.json"):
            code = f[:-5]
            try:
                out[code] = json.load(open(os.path.join(LANG, f), encoding="utf-8"))
            except ValueError:
                pass
    return out


def gen_steam_md(achs):
    lines = [
        "# Steam achievements",
        "",
        "GENERATED by `tools/gen_achievements.py` from `tools/achievements/catalog.json`.",
        "",
        "Steamworks has no bulk import for achievements, so each row below is typed into",
        "**App Admin → Stats & Achievements → Achievements** once, when the app id exists",
        "(see `docs/steam/README.md`). The API name must be copied exactly: the game sets",
        "achievements by that string (`src/platform/SteamBridge.cpp`), and a typo there",
        "fails silently — Steam accepts `SetAchievement` for a name it does not know and",
        "returns false, which nothing shows the player.",
        "",
        "Icons: `docs/steam/achievements/<API name>.png` (achieved) and `<API name>_locked.png`",
        "(unachieved), 256x256 as Steam asks. Drawn with Noto Emoji (SIL OFL 1.1).",
        "",
        "Set **Hidden** where the table says so. Steam only ever receives an achievement",
        "that the account service has signed, so the Steam collection is exactly as",
        "meaningful as the one in the game.",
        "",
        "Localised names and descriptions are in `achievements-loc.vdf`, for the",
        "localisation upload on the same page.",
        "",
        "| # | API name | Name | Description | Hidden |",
        "|---|---|---|---|---|",
    ]
    for i, a in enumerate(achs, 1):
        lines.append(f"| {i} | `{a['steam']}` | {a['name']} | {a['desc']} | {'yes' if a['hidden'] else ''} |")
    lines.append("")
    return "\n".join(lines)


def gen_steam_csv(achs):
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(["api_name", "display_name", "description", "hidden", "icon", "icon_locked"])
    for a in achs:
        w.writerow([a["steam"], a["name"], a["desc"], int(a["hidden"]),
                    f"achievements/{a['steam']}.png", f"achievements/{a['steam']}_locked.png"])
    return buf.getvalue()


def vdf_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def gen_steam_vdf(achs, tr):
    lines = ['"lang"', "{"]
    for code, steam in STEAM_LANGS.items():
        table = tr.get(code, {}) if code != "en" else {}
        lines += [f'\t"{steam}"', "\t{", '\t\t"Tokens"', "\t\t{"]
        for a in achs:
            name = table.get(a["name"]) or a["name"] if code != "en" else a["name"]
            desc = table.get(a["desc"]) or a["desc"] if code != "en" else a["desc"]
            lines.append(f'\t\t\t"{a["steam"]}_NAME"\t"{vdf_escape(name)}"')
            lines.append(f'\t\t\t"{a["steam"]}_DESC"\t"{vdf_escape(desc)}"')
        lines += ["\t\t}", "\t}"]
    lines += ["}", ""]
    return "\n".join(lines)


def draw_icons(achs, out_atlas, steam_dir):
    from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageOps
    font_path = os.environ.get("OD_EMOJI_FONT", os.path.join(HERE, "achievements", "NotoEmoji.ttf"))
    if not os.path.exists(font_path):
        sys.exit(f"icons need Noto Emoji at {font_path} (OD_EMOJI_FONT overrides); "
                 "it is https://github.com/google/fonts/tree/main/ofl/notoemoji")

    gold = (201, 162, 39)
    ground = (12, 15, 22)
    sea = (22, 32, 46)

    def medal(size, glyph, unlocked):
        s = size * 4                       # supersample, then downscale: cheap antialiasing
        im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        pad = s * 0.04
        ring = gold if unlocked else (102, 109, 124)
        d.ellipse([pad, pad, s - pad, s - pad], fill=ring)
        inner = s * 0.10
        d.ellipse([inner, inner, s - inner, s - inner], fill=sea if unlocked else (20, 24, 32))
        # A faint compass rose behind the glyph: the site's map-table motif.
        import math
        c = s / 2
        rose = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        rd = ImageDraw.Draw(rose)
        for k in range(8):
            ang = k * math.pi / 4 + math.pi / 8
            r = s * (0.37 if k % 2 == 0 else 0.25)
            w = s * 0.035
            tip = (c + r * math.cos(ang), c + r * math.sin(ang))
            left = (c + w * math.cos(ang + math.pi / 2), c + w * math.sin(ang + math.pi / 2))
            right = (c + w * math.cos(ang - math.pi / 2), c + w * math.sin(ang - math.pi / 2))
            rd.polygon([left, tip, right], fill=(ring[0], ring[1], ring[2], 38))
        im = Image.alpha_composite(im, rose)
        d = ImageDraw.Draw(im)
        font = ImageFont.truetype(font_path, int(s * 0.46))
        glyph_layer = Image.new("L", (s, s), 0)
        gd = ImageDraw.Draw(glyph_layer)
        bbox = gd.textbbox((0, 0), glyph, font=font)
        gw, gh = bbox[2] - bbox[0], bbox[3] - bbox[1]
        gd.text(((s - gw) / 2 - bbox[0], (s - gh) / 2 - bbox[1]), glyph, font=font, fill=255)
        colour = (232, 228, 218) if unlocked else (120, 126, 140)
        tint = Image.new("RGBA", (s, s), colour + (255,))
        shadow = Image.new("RGBA", (s, s), (0, 0, 0, 160))
        im.paste(shadow, (s // 60, s // 40), glyph_layer.filter(ImageFilter.GaussianBlur(s / 80)))
        im.paste(tint, (0, 0), glyph_layer)
        return im.resize((size, size), Image.LANCZOS)

    icons = icon_order(achs)
    tile = 64
    atlas = Image.new("RGBA", (tile * len(icons), tile * 2), (0, 0, 0, 0))
    for i, key in enumerate(icons):
        atlas.paste(medal(tile, ICON_GLYPHS[key], True), (i * tile, 0))
        atlas.paste(medal(tile, ICON_GLYPHS[key], False), (i * tile, tile))
    os.makedirs(os.path.dirname(out_atlas), exist_ok=True)
    # Quantised: the launcher embeds this atlas, and 256 colours of gold, slate
    # and off-white are indistinguishable from truecolour at 64px.
    atlas.quantize(colors=256, method=Image.FASTOCTREE, dither=Image.NONE).save(out_atlas, optimize=True)

    os.makedirs(steam_dir, exist_ok=True)
    for a in achs:
        medal(256, ICON_GLYPHS[a["icon"]], True).convert("RGB").save(
            os.path.join(steam_dir, f"{a['steam']}.png"), optimize=True)
        medal(256, ICON_GLYPHS[a["icon"]], False).convert("RGB").save(
            os.path.join(steam_dir, f"{a['steam']}_locked.png"), optimize=True)
    return atlas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--icons", action="store_true")
    ap.add_argument("--unifico", default=os.environ.get("UNIFICO_DIR", ""))
    args = ap.parse_args()

    achs = load()
    tr = translations()
    outputs = {
        os.path.join(ROOT, "src", "achievements", "AchievementCatalog.gen.h"):
            gen_cpp(achs, "// The game's copy. The launcher has the same table, generated the same way."),
        os.path.join(ROOT, "net", "src", "achievements", "catalog.gen.ts"): gen_ts(achs),
        os.path.join(ROOT, "docs", "steam", "achievements.md"): gen_steam_md(achs),
        os.path.join(ROOT, "docs", "steam", "achievements.csv"): gen_steam_csv(achs),
        os.path.join(ROOT, "docs", "steam", "achievements-loc.vdf"): gen_steam_vdf(achs, tr),
    }
    if args.unifico:
        outputs[os.path.join(args.unifico, "src", "gen", "catalog.gen.h")] = gen_cpp(
            achs, "// Unifico's copy. Open Doctrines compiles the same table; regenerate both together.")

    drift = []
    for path, text in outputs.items():
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old == text:
            continue
        # The Steam localisation file follows the translations, which change
        # far more often than the catalog does. It is rewritten on every normal
        # run, but its drift is not a reason to fail CI.
        if not (args.check and path.endswith("achievements-loc.vdf")):
            drift.append(os.path.relpath(path, ROOT))
        if not args.check:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)

    atlas = os.path.join(ROOT, "data", "icons", "achievements.png")
    if args.icons and not args.check:
        draw_icons(achs, atlas, os.path.join(ROOT, "docs", "steam", "achievements"))
        if args.unifico:
            import shutil
            dst = os.path.join(args.unifico, "assets", "achievements.png")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(atlas, dst)
    elif os.path.exists(atlas):
        # The PNG header, not Pillow: --check runs in CI, which has no Pillow.
        import struct
        with open(atlas, "rb") as fh:
            head = fh.read(24)
        w, h = struct.unpack(">II", head[16:24])
        if w != 64 * len(icon_order(achs)) or h != 128:
            drift.append("data/icons/achievements.png (run with --icons)")

    if args.check:
        if drift:
            print("achievement catalog drift -- run python3 tools/gen_achievements.py:")
            for d in drift:
                print("  " + d)
            return 1
        print(f"achievements: {len(achs)} in catalog, generated copies agree")
        return 0
    print(f"achievements: {len(achs)}; wrote {len(drift)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
