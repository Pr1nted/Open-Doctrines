#!/usr/bin/env python3
"""Normalize all symbol SVGs to viewBox='-100 -100 200 200'.

Approach: use the existing viewBox (or width/height) to compute a simple
scale+translate transform. Does NOT compute path-data bounding boxes,
which are unreliable for SVGs with nested transforms.
"""

import os, sys, re
import xml.etree.ElementTree as ET

SVG_NS = "http://www.w3.org/2000/svg"
CANONICAL_VB = "-100 -100 200 200"

def q(tag):
    return f"{{{SVG_NS}}}{tag}"

def is_tag(elem, local_name):
    return elem.tag == f"{{{SVG_NS}}}{local_name}" or elem.tag == local_name

def strip_units(s):
    """Remove unit suffix (mm, px, in, pt, cm)."""
    i = 0
    while i < len(s) and (s[i].isdigit() or s[i] in '.-'):
        i += 1
    return s[:i]

def convert_mm_to_px(v, unit):
    """Convert physical units to pixels (96 DPI)."""
    if unit == "mm": return v * 96.0 / 25.4
    if unit == "in": return v * 96.0
    if unit == "pt": return v * 96.0 / 72.0
    if unit == "cm": return v * 96.0 / 2.54
    return v

def get_viewbox(svg_tag):
    """Extract viewBox from SVG tag. Returns (vx, vy, vw, vh) or None."""
    m = re.search(r'viewBox="([^"]*)"', svg_tag)
    if m:
        parts = m.group(1).split()
        if len(parts) == 4:
            try:
                return tuple(float(p) for p in parts)
            except ValueError:
                pass
    return None

def get_dimensions(svg_tag):
    """Extract width/height from SVG tag, converting units. Returns (w, h) or None."""
    wm = re.search(r'width="([^"]*)"', svg_tag)
    hm = re.search(r'height="([^"]*)"', svg_tag)
    if wm and hm:
        wv = wm.group(1)
        hv = hm.group(1)
        w_num = float(strip_units(wv))
        h_num = float(strip_units(hv))
        w_unit = wv[len(strip_units(wv)):]
        h_unit = hv[len(strip_units(hv)):]
        w_px = convert_mm_to_px(w_num, w_unit)
        h_px = convert_mm_to_px(h_num, h_unit)
        return (w_px, h_px)
    return None

def normalize_svg(filepath):
    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    # Add namespace if missing
    if 'xmlns="http://www.w3.org/2000/svg"' not in text:
        text = text.replace('<svg', '<svg xmlns="http://www.w3.org/2000/svg"', 1)

    root = ET.fromstring(text)
    svg_tag_str = ET.tostring(root, encoding="unicode")

    # Get original viewBox or dimensions
    vb = get_viewbox(svg_tag_str)
    dims = None
    if vb:
        vx, vy, vw, vh = vb
    else:
        dims = get_dimensions(svg_tag_str)
        if dims:
            vw, vh = dims
            vx, vy = 0, 0
        else:
            print(f"  SKIP (no viewBox or dims): {os.path.basename(filepath)}")
            return False

    if vw <= 0 or vh <= 0:
        print(f"  SKIP (zero dims): {os.path.basename(filepath)}")
        return False

    # Compute canonical normalization
    norm_scale = 200.0 / max(vw, vh)
    cx = vx + vw / 2.0
    cy = vy + vh / 2.0
    tx = -cx * norm_scale
    ty = -cy * norm_scale

    fname = os.path.basename(filepath)

    # Check if already canonical
    if vb and abs(vw - 200) < 0.5 and abs(vh - 200) < 0.5 and abs(vx + 100) < 0.5 and abs(vy + 100) < 0.5:
        # Already canonical — remove any wrapping transform from previous runs
        # Remove width/height if present (not needed with viewBox)
        if "width" in root.attrib: del root.attrib["width"]
        if "height" in root.attrib: del root.attrib["height"]
        # Check if first child is a <g> that came from normalization (has transform)
        children = list(root)
        if children and is_tag(children[0], "g") and children[0].get("transform"):
            outer_g = children[0]
            grandkids = list(outer_g)
            if grandkids and is_tag(grandkids[0], "g") and grandkids[0].get("transform"):
                # Double-wrapped (old normalization format) — unwrap
                inner_g = grandkids[0]
                inner_kids = list(inner_g)
                for c in inner_kids:
                    root.append(c)
                root.remove(outer_g)
                print(f"  UNWRAPPED: {fname}")
            else:
                # Single wrap from old normalization — may or may not need changes
                # Check if this was anormalized file by looking at scale
                t = children[0].get("transform", "")
                if "scale" in t:
                    # Was normalized before — we might need to fix it
                    # Estimate original scale
                    m = re.search(r'scale\(([^)]+)\)', t)
                    if m:
                        old_scale = float(m.group(1).split()[0])
                        # If scale is very small (like 0.015), it was wrong
                        if old_scale < 0.5:
                            # Remove outer wrap, re-apply correct normalization
                            for c in list(outer_g):
                                root.append(c)
                            root.remove(outer_g)
                            print(f"  REMOVED BAD WRAP: {fname} (old scale={old_scale:.4f})")
                        else:
                            print(f"  ALREADY OK: {fname}")
                else:
                    print(f"  OK: {fname}")
        else:
            print(f"  OK: {fname}")
        output = serialize_xml(root)
        with open(filepath, "w", encoding="utf-8") as f:
            f.write(output)
        return True

    # Not canonical — apply normalization
    # Remove any existing width/height (viewBox handles sizing)
    for attr in ["width", "height"]:
        if attr in root.attrib:
            del root.attrib[attr]

    # Remove any existing <g> wrapper from a previous normalization run
    children = list(root)
    for c in children:
        if is_tag(c, "g") and c.get("transform") and not list(c):
            root.remove(c)  # empty group with transform — should not happen but just in case

    # Wrap ALL content (except defs) in a <g> with the normalization transform
    defs_children = [c for c in list(root) if is_tag(c, "defs")]
    content_children = [c for c in list(root) if not is_tag(c, "defs") and not is_tag(c, "namedview")]

    if not content_children:
        print(f"  SKIP (no content): {fname}")
        return False

    # Remove everything, add defs back, wrap content
    for c in list(root):
        root.remove(c)

    for d in defs_children:
        root.append(d)

    g = ET.SubElement(root, q("g"))
    g.set("transform", f"translate({tx:.6f},{ty:.6f}) scale({norm_scale:.6f})")

    for c in content_children:
        # Remove any sodipodi/namedview stuff — just skip non-SVG elements
        if not is_tag(c, "namedview"):
            g.append(c)

    root.set("viewBox", CANONICAL_VB)
    # Remove preserveAspectRatio — not needed for canonical VB
    if "preserveAspectRatio" in root.attrib:
        del root.attrib["preserveAspectRatio"]

    output = serialize_xml(root)
    with open(filepath, "w", encoding="utf-8") as f:
        f.write(output)

    print(f"  OK: {fname} orig_vb=({vx:.0f},{vy:.0f},{vw:.0f},{vh:.0f}) scale={norm_scale:.4f}")
    return True

def serialize_xml(elem, level=0, root=True):
    indent = "  " * level
    tag = elem.tag
    if tag.startswith("{"):
        local = tag.split("}")[1]
        tag_name = local
    else:
        tag_name = tag

    parts = [f"{indent}<{tag_name}"]

    if root:
        parts.append(f' xmlns="{SVG_NS}"')

    for attr_name, attr_val in sorted(elem.attrib.items()):
        if attr_name.startswith("{"):
            local = attr_name.split("}")[1]
            parts.append(f' {local}="{_escape_attr(attr_val)}"')
        else:
            parts.append(f' {attr_name}="{_escape_attr(attr_val)}"')

    children = list(elem)
    text = (elem.text or "").strip()

    if not children and not text:
        parts.append("/>")
        return "".join(parts)

    parts.append(">")

    if text:
        parts.append(_escape_text(text))

    for child in children:
        parts.append("\n")
        parts.append(serialize_xml(child, level + 1, root=False))

    parts.append(f"\n{indent}</{tag_name}>")
    return "".join(parts)

def _escape_attr(s):
    return s.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;").replace(">", "&gt;")

def _escape_text(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def main():
    symbols_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "symbols")
    if not os.path.isdir(symbols_dir):
        print(f"ERROR: {symbols_dir} not found")
        sys.exit(1)

    count = 0
    for fname in sorted(os.listdir(symbols_dir)):
        if not fname.endswith(".svg"):
            continue
        fpath = os.path.join(symbols_dir, fname)
        if normalize_svg(fpath):
            count += 1

    print(f"\nProcessed {count} symbols")

if __name__ == "__main__":
    main()
