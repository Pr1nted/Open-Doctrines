#!/usr/bin/env python3
"""Generate SVG flag files for all countries and update countries.json references.

Reads countries.json (with procedural flag data from MapGenerator), generates
SVG files at data/flags/{iso}.svg, and updates flag_actual.image to point to them.

Symbol SVGs from data/symbols/ are embedded inline.
"""
import json, os, sys, re

DATA_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")
COUNTRIES_JSON = os.path.join(DATA_DIR, "countries.json")
FLAGS_DIR = os.path.join(DATA_DIR, "flags")
SYMBOLS_DIR = os.path.join(DATA_DIR, "symbols")

SVG_CACHE = {}

def load_symbol(svg_name):
    if svg_name not in SVG_CACHE:
        path = os.path.join(SYMBOLS_DIR, svg_name)
        if os.path.exists(path):
            with open(path) as f:
                content = f.read()
            # Extract inner svg content (strip outer <svg> wrapper, keep viewBox)
            m = re.search(r'<svg[^>]*>(.*)</svg>', content, re.DOTALL)
            inner = m.group(1) if m else content
            # Extract viewBox for sizing reference
            vb = re.search(r'viewBox="([^"]*)"', content)
            SVG_CACHE[svg_name] = (inner, vb.group(1) if vb else "0 0 100 100")
        else:
            SVG_CACHE[svg_name] = ("", "0 0 100 100")
    return SVG_CACHE[svg_name]

def embed_symbol(sym, w, h):
    """Return SVG element string for a FlagSymbol."""
    stype = sym.get("type", "").lower()
    colors = sym.get("colors", [])
    col = colors[0] if colors else "#ffffff"
    cx = sym.get("x", 0.5) * w
    cy = sym.get("y", 0.5) * h
    sz = sym.get("size", 0.3) * min(w, h)
    count = sym.get("count", 0)
    text = sym.get("text", "")
    r = sz / 2

    # Map symbol type to SVG file name
    sym_to_svg = {
        "star": "star5.svg", "star_5": "star5.svg",
        "star_6": "star6.svg", "star_7": "star7.svg",
        "star_of_david": "star_of_david.svg", "star_4": "star_4.svg",
        "crescent": "crescent.svg", "crescent_star": "crescent_star.svg",
        "sun": "sun.svg", "sun_rays": "sun_wavy.svg",
        "cross_latin": "cross_latin.svg", "cross_saltir": "cross_saltir.svg",
        "cross_maltese": "cross_maltese.svg", "cross_nordic": "cross_nordic.svg",
        "cross_pattee": "cross_pattee.svg",
        "diamond": "diamond.svg", "gear": "gear.svg",
        "hammer_sickle": "hammer_sickle.svg", "swastika": "swastika.svg",
        "sword": "sword.svg", "crossed_swords": "crossed_swords.svg",
        "mountain": "mountain.svg", "tree": "tree.svg",
        "anchor": "anchor.svg", "eagle": "eagle.svg",
        "fasces": "fasces.svg", "rose": "rose.svg",
        "torch": "torch.svg", "circle_stars": "circle_stars.svg",
    }
    svg_name = sym_to_svg.get(stype, "")
    if svg_name:
        inner, vb = load_symbol(svg_name)
        if inner:
            # Parse viewBox for scaling
            vb_parts = list(map(float, vb.split()))
            svg_w = vb_parts[2] if len(vb_parts) > 2 else 100
            svg_h = vb_parts[3] if len(vb_parts) > 3 else 100
            scale = r * 2 / max(svg_w, svg_h)
            tx = cx - svg_w * scale / 2
            ty = cy - svg_h * scale / 2
            return f'<g transform="translate({tx:.1f},{ty:.1f}) scale({scale:.4f})" fill="{col}">{inner}</g>'

    # Procedural fallbacks for symbols without SVG
    if stype == "circle":
        return f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="{col}"/>'
    elif stype == "disc":
        c2 = colors[1] if len(colors) > 1 else "#333333"
        r2 = r * 3 / 5
        return f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="{col}"/><circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r2:.1f}" fill="{c2}"/>'
    elif stype == "triangle":
        return f'<polygon points="{cx:.1f},{cy - r:.1f} {cx - r:.1f},{cy + r:.1f} {cx + r:.1f},{cy + r:.1f}" fill="{col}"/>'
    elif stype == "square" or stype == "rect":
        return f'<rect x="{cx - r:.1f}" y="{cy - r:.1f}" width="{r*2:.1f}" height="{r*2:.1f}" fill="{col}"/>'
    elif stype == "text_block" or stype == "text" or stype == "textblock":
        return f'<text x="{cx:.1f}" y="{cy:.1f}" font-size="{r:.1f}" fill="{col}" text-anchor="middle" dominant-baseline="central">{text}</text>'
    elif stype == "star" and count > 1:
        # Circle of stars
        parts = []
        for i in range(count):
            angle = 2 * 3.14159 * i / count
            sx = cx + r * 0.7 * __import__('math').cos(angle)
            sy = cy + r * 0.7 * __import__('math').sin(angle)
            sr = r * 0.3
            inner_svg, _= load_symbol("star5.svg")
            parts.append(f'<g transform="translate({sx:.1f},{sy:.1f}) scale({sr*2/100:.4f})" fill="{col}">{inner_svg}</g>')
        return "\n".join(parts)
    return ""

def generate_flag_svg(country_entry, w=200, h=133):
    """Generate a complete SVG string for a country's flag."""
    fa = country_entry.get("flag_actual", {})
    ftype = fa.get("type", "solid")
    colors = fa.get("colors", ["#cccccc"])
    symbols = fa.get("symbols", [])
    star_count = fa.get("starCount", 0)

    elements = []

    def cols(idx):
        return colors[idx] if idx < len(colors) else "#cccccc"

    def add_rect(x, y, w2, h2, c):
        elements.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w2:.1f}" height="{h2:.1f}" fill="{c}"/>')

    def add_poly(pts, c):
        elements.append(f'<polygon points="{pts}" fill="{c}"/>')

    if ftype == "solid":
        add_rect(0, 0, w, h, cols(0))
    elif ftype in ("hstripes", "hstripes_2", "hstripes_3", "hstripes_n"):
        n = len(colors)
        stripe_h = h / n
        for i in range(n):
            add_rect(0, i * stripe_h, w, stripe_h, cols(i))
    elif ftype in ("vstripes", "vstripes_2", "vstripes_3", "vstripes_n"):
        n = len(colors)
        stripe_w = w / n
        for i in range(n):
            add_rect(i * stripe_w, 0, stripe_w, h, cols(i))
    elif ftype in ("diagonal", "diagonal_l"):
        # Diagonal from top-left to bottom-right
        add_rect(0, 0, w, h, cols(0))
        add_poly(f"0,0 {w},{0} {0},{h}", cols(1))
    elif ftype == "diagonal_r":
        add_rect(0, 0, w, h, cols(0))
        add_poly(f"{w},0 {w},{h} 0,{h}", cols(1))
    elif ftype in ("triangle", "triangle_double"):
        add_rect(0, 0, w, h, cols(1) if len(colors) > 1 else "#ffffff")
        tri_w = w * 0.4
        add_poly(f"0,0 {tri_w},{h/2} 0,{h}", cols(0))
        if ftype == "triangle_double" and len(colors) > 2:
            tri_w2 = w * 0.25
            add_poly(f"0,0 {tri_w2},{h/2} 0,{h}", cols(2))
    elif ftype == "quarter":
        add_rect(0, 0, w/2, h/2, cols(0))
        add_rect(w/2, 0, w/2, h/2, cols(1) if len(colors) > 1 else "#ffffff")
        add_rect(0, h/2, w/2, h/2, cols(2) if len(colors) > 2 else "#ffffff")
        add_rect(w/2, h/2, w/2, h/2, cols(3) if len(colors) > 3 else cols(0))
    elif ftype == "saltir":
        add_rect(0, 0, w, h, cols(0))
        add_poly(f"0,0 {w},{0} {w/2},{h/2}", cols(1))
        add_poly(f"0,0 0,{h} {w/2},{h/2}", cols(2) if len(colors) > 2 else cols(1))
        add_poly(f"{w},{0} {w},{h} {w/2},{h/2}", cols(3) if len(colors) > 3 else cols(1))
        add_poly(f"0,{h} {w},{h} {w/2},{h/2}", cols(4) if len(colors) > 4 else cols(1))
    elif ftype == "canton":
        add_rect(0, 0, w, h, cols(0))
        canton_w = w * 0.4
        canton_h = h * 0.5
        add_rect(0, 0, canton_w, canton_h, cols(1) if len(colors) > 1 else "#ffffff")
    elif ftype == "pale":
        add_rect(0, 0, w, h, cols(0))
        pale_w = w / 3
        add_rect(w/2 - pale_w/2, 0, pale_w, h, cols(1))
    elif ftype == "fess":
        add_rect(0, 0, w, h, cols(0))
        fess_h = h / 3
        add_rect(0, h/2 - fess_h/2, w, fess_h, cols(1))
    elif ftype == "cross":
        add_rect(0, 0, w, h, cols(0))
        cross_w = w / 5
        cross_h = h / 5
        add_rect(w/2 - cross_w/2, 0, cross_w, h, cols(1) if len(colors) > 1 else "#ffffff")
        add_rect(0, h/2 - cross_h/2, w, cross_h, cols(1) if len(colors) > 1 else "#ffffff")
    elif ftype == "cross_nordic":
        add_rect(0, 0, w, h, cols(0))
        cx = w * 0.35
        bar_w = w / 8
        bar_h = h / 6
        add_rect(cx - bar_w/2, 0, bar_w, h, cols(1) if len(colors) > 1 else "#ffffff")
        add_rect(0, h/2 - bar_h/2, w, bar_h, cols(1) if len(colors) > 1 else "#ffffff")
    elif ftype == "sunburst":
        add_rect(0, 0, w, h, cols(0))
        # Simple sunburst: gradient
        elements.append(f'<radialGradient id="sg"><stop offset="0%" stop-color="{cols(1)}"/><stop offset="100%" stop-color="{cols(0)}"/></radialGradient>')
        add_rect(0, 0, w, h, "url(#sg)")
    elif ftype == "circle":
        add_rect(0, 0, w, h, cols(0))
        r2 = min(w, h) * 0.35
        elements.append(f'<circle cx="{w/2:.1f}" cy="{h/2:.1f}" r="{r2:.1f}" fill="{cols(1) if len(colors) > 1 else "#ffffff"}"/>')
    elif ftype == "star":
        add_rect(0, 0, w, h, cols(0))
    elif ftype == "border":
        add_rect(0, 0, w, h, cols(0))
        bw = w * 0.05
        add_rect(bw, bw, w - 2*bw, h - 2*bw, cols(1) if len(colors) > 1 else "#ffffff")
    else:
        add_rect(0, 0, w, h, "#cccccc")

    # Render symbols
    for sym in symbols:
        el = embed_symbol(sym, w, h)
        if el:
            elements.append(el)

    # Star count fallback (for simple "star" type without explicit symbols)
    if ftype == "star" and star_count > 0 and not symbols:
        sc = colors[1] if len(colors) > 1 else "#ffffff"
        inner_svg, _ = load_symbol("star5.svg")
        for i in range(min(star_count, 50)):
            angle = 2 * 3.14159 * i / star_count - 3.14159 / 2
            dist = min(w, h) * 0.25
            sx = w / 2 + dist * __import__('math').cos(angle)
            sy = h / 2 + dist * __import__('math').sin(angle) * 0.8
            sr = min(w, h) * 0.08
            elements.append(f'<g transform="translate({sx:.1f},{sy:.1f}) scale({sr*2/100:.4f})" fill="{sc}">{inner_svg}</g>')

    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">\n'
        + "\n".join(elements)
        + "\n</svg>"
    )

def main():
    if not os.path.exists(COUNTRIES_JSON):
        print(f"ERROR: {COUNTRIES_JSON} not found. Run MapGenerator first.")
        sys.exit(1)
    os.makedirs(FLAGS_DIR, exist_ok=True)

    with open(COUNTRIES_JSON) as f:
        countries = json.load(f)

    updated = 0
    skipped = 0
    for cid, entry in countries.items():
        name = entry.get("name", "Unknown")
        iso = entry.get("iso_a3", "")
        if not iso or iso == "-99":
            print(f"  Skip {cid} ({name}): no ISO code")
            skipped += 1
            continue

        fa = entry.get("flag_actual", {})
        # If image already set and we have a backup of the original data, use it
        if "image" in fa and "_procedural" in fa:
            procedural_data = fa["_procedural"]
            # Regenerate from backup
            entry["flag_actual"] = dict(procedural_data)  # restore original
        elif "image" in fa:
            # Already has image reference but no backup — regenerate from SVG file if exists
            if not entry.get("flag_actual", {}).get("type"):
                # We lost the procedural data, can't regenerate — set placeholder
                print(f"  Skip {iso} ({name}): already has image reference, no procedural data")
                skipped += 1
                continue

        svg_path = os.path.join(FLAGS_DIR, f"{iso}.svg")

        try:
            svg = generate_flag_svg(entry)
        except Exception as e:
            print(f"  ERROR generating flag for {iso} ({name}): {e}")
            skipped += 1
            continue

        with open(svg_path, "w") as f:
            f.write(svg)

        # Update countries.json to reference the SVG file
        # Keep a backup of the original procedural data for re-generation
        fa = entry.get("flag_actual", {})
        if "type" in fa and "_procedural" not in entry.get("flag_censored", {}):
            # Save backup before overwriting
            backup = dict(fa)
            # Remove any existing image to keep it clean
            backup.pop("image", None)
            img_name = f"flags/{iso}.svg"
            entry["flag_actual"] = {"image": img_name, "_procedural": backup}
            entry["flag_censored"] = {"image": img_name, "censored": True, "_procedural": backup}
        elif "_procedural" in fa:
            # Already has backup, just update image reference
            img_name = f"flags/{iso}.svg"
            entry["flag_actual"]["image"] = img_name
            entry["flag_censored"]["image"] = img_name
        updated += 1
        if updated <= 5 or updated % 20 == 0:
            print(f"  [{updated}] {iso} ({name}): {svg_path} ({len(svg)} bytes)")

    with open(COUNTRIES_JSON, "w") as f:
        json.dump(countries, f, indent=2)

    # Remove stale flag files (for countries that no longer exist)
    expected = {f"{entry.get('iso_a3','')}.svg" for entry in countries.values() if entry.get("iso_a3","") and entry.get("iso_a3","") != "-99"}
    for fname in os.listdir(FLAGS_DIR):
        if fname.endswith(".svg") and fname not in expected:
            os.remove(os.path.join(FLAGS_DIR, fname))
            print(f"  Removed stale: {fname}")

    print(f"\nDone: {updated} flags generated, {skipped} skipped")
    print(f"Flags directory: {FLAGS_DIR}")

if __name__ == "__main__":
    main()
