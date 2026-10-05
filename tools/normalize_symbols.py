#!/usr/bin/env python3
"""Normalize all symbol SVGs to viewBox='-100 -100 200 200'.

Approach: use the existing viewBox (or width/height) to compute a simple
scale+translate transform. Does NOT compute path-data bounding boxes,
which are unreliable for SVGs with nested transforms.

THAT LAST SENTENCE IS WHY --check AND --fit EXIST

Normalising from the declared canvas moves a symbol's box; it cannot move the
artwork inside it. So a source file that drew its emblem in a corner of an
oversized canvas stays exactly as wrong after normalising, and nothing said
so: cross_saltir covered 56% of its own box and hammer_sickle 25%, against
90-100% for their siblings, and both simply drew small on the flag for as long
as anyone cared to look. Normalising is not framing.

    python3 tools/normalize_symbols.py            # normalise, as before
    python3 tools/normalize_symbols.py --check    # report coverage, fail if under
    python3 tools/normalize_symbols.py --fit      # normalise, then reframe to 92%

--check and --fit measure by rasterising through the game's own nanosvg (see
tools/svg_fill.py for why it is the only measurement worth trusting), so they
need a C++ compiler or a built SymbolFill target. Plain normalising does not.
"""

import os, sys, re
import xml.etree.ElementTree as ET

try:
    import svg_fill
except ImportError:                                  # run from the project root
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import svg_fill

SVG_NS = "http://www.w3.org/2000/svg"
CANONICAL_VB = "-100 -100 200 200"

# Everything in data/symbols/ reaches at least this much of its own box, and
# anything that does not is a framing accident -- unless it is listed below.
MIN_FILL = 0.78

# Symbols that really are drawn smaller, with the reason, so that --check does
# not nag and nobody "fixes" them into blobs.
#
# All three are dense: measured by ink -- the share of the canvas actually
# painted white -- they sit at 24-26%, which is where star5 (26%), sun (26%)
# and cross_maltese (25%) sit while spanning 92-100% of their boxes. They are
# already the same visual weight as their siblings; scaled to 92% they would
# carry roughly twice the white of anything else in the set. For the rose that
# is not theoretical, it is the failure tools/generate_symbols.py records:
# petals that meet rasterise as one round blob.
DELIBERATELY_SMALL = {
    "rose.svg":     "five discs that must not touch; already 26% ink, denser than star5",
    "gear.svg":     "solid rim and hub; 24% ink at 70% span, same weight as cross_maltese",
    "swastika.svg": "22-wide strokes; 24% ink, and censored in game either way",
}

def q(tag):
    return f"{{{SVG_NS}}}{tag}"

def is_tag(elem, local_name):
    return elem.tag == f"{{{SVG_NS}}}{local_name}" or elem.tag == local_name


def parse_svg(text):
    """Parse an SVG, KEEPING comments.

    ElementTree throws comments away by default, and this script round-trips
    every symbol through it -- so it used to quietly strip the note each file
    carries about where its artwork came from, why it is drawn the way it is,
    and which tool regenerates it. Those notes are the only record of a
    licence obligation for the Commons symbols, and normalising is no reason
    to lose them.
    """
    return ET.fromstring(text, parser=ET.XMLParser(target=ET.TreeBuilder(insert_comments=True)))


def elements(parent):
    """Child ELEMENTS, skipping comments (whose .tag is not a string)."""
    return [c for c in list(parent) if isinstance(c.tag, str)]

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

    try:
        root = parse_svg(text)
    except ET.ParseError as e:
        # One unreadable symbol must not take the other 27 down with it.
        print(f"  SKIP (not well-formed XML): {os.path.basename(filepath)} -- {e}")
        return False
    # The root's own attributes, not the whole serialised document: a file's
    # explanatory comment is free to mention viewBox, and a regex over the
    # document would read the prose.
    svg_tag_str = "<svg " + " ".join(f'{k}="{v}"' for k, v in root.attrib.items()) + ">"

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
        # Already canonical: strip width/height, which the viewBox supersedes,
        # and otherwise LEAVE THE CONTENT ALONE.
        #
        # This branch used to try to undo "wrapping transforms from previous
        # runs", and it did it by deleting them -- one rule for a <g transform>
        # holding another, one for a single wrap whose scale was under 0.5,
        # "wrong" being guessed from the number alone. Both rules were wrong,
        # and not subtly: a scale under 0.5 is what refitting a big source
        # canvas legitimately produces, so one pass over pristine data/symbols/
        # deleted the transform that positioned cross_latin (0.3546),
        # cross_pattee (0.3704), star_of_david (0.2887) and star_4, leaving the
        # artwork in source coordinates far outside the viewBox -- four symbols
        # rasterising to nothing -- and shrank eagle_nazi (0.1542) from 98% of
        # its canvas to 45%. Nothing said so, because nothing rasterised them.
        #
        # A transform cannot be judged by its scale, and discarding one can
        # only ever be right by luck. Leftovers from an older format now show
        # up as bad coverage under --check, where a person can look at the
        # symbol and decide, instead of being guessed at here.
        if "width" in root.attrib: del root.attrib["width"]
        if "height" in root.attrib: del root.attrib["height"]
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
    children = elements(root)
    for c in children:
        if is_tag(c, "g") and c.get("transform") and not elements(c):
            root.remove(c)  # empty group with transform — should not happen but just in case

    # Wrap ALL content (except defs) in a <g> with the normalization transform
    comments = [c for c in list(root) if not isinstance(c.tag, str)]
    defs_children = [c for c in elements(root) if is_tag(c, "defs")]
    content_children = [c for c in elements(root) if not is_tag(c, "defs") and not is_tag(c, "namedview")]

    if not content_children:
        print(f"  SKIP (no content): {fname}")
        return False

    # Remove everything, add defs back, wrap content
    for c in list(root):
        root.remove(c)

    # Comments stay at the top, where they were written to be read.
    for c in comments:
        root.append(c)

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
    if not isinstance(tag, str):            # comment (or PI): write it back out
        return f"{indent}<!--{elem.text}-->"
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

# ── framing ────────────────────────────────────────────────────────────────
#
# Normalising moves the box. This moves the artwork inside it.

# The one transform shape both this script and tools/download_symbols.py emit.
TRANSFORM_RE = re.compile(r'^\s*translate\(\s*(-?[\d.eE+-]+)\s*[, ]\s*(-?[\d.eE+-]+)\s*\)'
                          r'\s*scale\(\s*(-?[\d.eE+-]+)\s*\)\s*$')


def apply_fit(filepath, bbox, target=svg_fill.TARGET_FILL):
    """Reframe an already-canonical symbol so its artwork spans `target`.

    Composes into the wrapping <g>'s transform instead of adding a second one.
    A fit written as an extra layer would not survive: normalize_svg() reads a
    <g transform> holding another <g transform> as output from an older version
    of this script and unwraps it, so the reframing would be thrown away the
    next time this ran -- silently, and only for the files that needed it most.
    """
    k, cx, cy = svg_fill.correction(bbox, target=target)
    if abs(k - 1.0) < 0.005 and abs(cx) < 0.25 and abs(cy) < 0.25:
        return False, "already framed"

    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        root = parse_svg(f.read())

    content = [c for c in elements(root) if not is_tag(c, "defs")]
    wrapped = [c for c in content if is_tag(c, "g") and c.get("transform")]

    if len(content) == 1 and is_tag(content[0], "g") and not wrapped:
        # A plain group, e.g. from tools/generate_symbols.py: give it one.
        content[0].set("transform", f"translate({-k * cx:.6f},{-k * cy:.6f}) scale({k:.6f})")
    elif len(wrapped) == 1 and len(content) == 1:
        g = wrapped[0]
        m = TRANSFORM_RE.match(g.get("transform"))
        if not m:
            return False, f"transform is not translate+scale, reframe by hand: {g.get('transform')}"
        tx, ty, s = float(m.group(1)), float(m.group(2)), float(m.group(3))
        # translate(-k*c) scale(k) composed onto translate(t) scale(s).
        g.set("transform",
              f"translate({k * (tx - cx):.6f},{k * (ty - cy):.6f}) scale({k * s:.6f})")
    else:
        # Several top-level shapes and nothing to compose into. Wrapping them
        # would produce the nested pair normalize_svg() unwraps, so say so
        # rather than write something that will not survive.
        return False, f"{len(content)} top-level elements and no single group to reframe"

    with open(filepath, "w", encoding="utf-8") as f:
        f.write(serialize_xml(root) + "\n")
    return True, f"k={k:.4f} centre=({cx:.2f},{cy:.2f})"


def report(symbols_dir, paths, fix=False):
    """Print what each symbol covers; reframe it too when `fix`."""
    try:
        measured = svg_fill.measure(paths)
    except svg_fill.Unavailable as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    bad = 0
    for path in paths:
        fname = os.path.basename(path)

        # nanosvg does not parse XML strictly, so it renders files that no
        # other tool can read -- a "--" typed inside a comment is enough, and
        # it is easy to type in prose. The symbol looks perfect in game and
        # every script here dies on it, so check the strict reading too.
        try:
            ET.parse(path)
        except ET.ParseError as e:
            print(f"  BROKEN: {fname} is not well-formed XML ({e})")
            bad += 1
            continue

        info = measured.get(path)
        if info is None or "error" in info:
            print(f"  BROKEN: {fname} -- {info['error'] if info else 'not measured'}")
            bad += 1
            continue

        fill, note = info["fill"], DELIBERATELY_SMALL.get(fname)
        if note:
            print(f"  {fname:<22} {fill * 100:5.1f}%  by design: {note}")
            continue
        if fill >= MIN_FILL and not fix:
            print(f"  {fname:<22} {fill * 100:5.1f}%")
            continue

        if not fix:
            print(f"  {fname:<22} {fill * 100:5.1f}%  UNDER-FILLED (want {MIN_FILL * 100:.0f}%)")
            bad += 1
            continue

        changed, why = apply_fit(path, info["bbox"])
        if not changed:
            flag = "" if why == "already framed" else "  CANNOT FIT"
            print(f"  {fname:<22} {fill * 100:5.1f}%  {why}{flag}")
            if why != "already framed" and fill < MIN_FILL:
                bad += 1
            continue
        after = svg_fill.measure([path]).get(path, {})
        print(f"  {fname:<22} {fill * 100:5.1f}% -> {after.get('fill', 0) * 100:5.1f}%  ({why})")
        # Reframing that did not reframe is worse than none: it is a silent
        # no-op on the file that needed it.
        if after.get("fill", 0) < MIN_FILL:
            print(f"    STILL UNDER-FILLED after reframing: {fname}")
            bad += 1

    if bad:
        print(f"\n{bad} symbol(s) need attention")
    return 1 if bad else 0


def main():
    symbols_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "symbols")
    if not os.path.isdir(symbols_dir):
        print(f"ERROR: {symbols_dir} not found")
        sys.exit(1)

    check = "--check" in sys.argv
    fit = "--fit" in sys.argv
    paths = [os.path.join(symbols_dir, f) for f in sorted(os.listdir(symbols_dir)) if f.endswith(".svg")]

    if check:
        print("Coverage of each symbol's own canvas:")
        sys.exit(report(symbols_dir, paths))

    count = 0
    for fpath in paths:
        if normalize_svg(fpath):
            count += 1

    print(f"\nProcessed {count} symbols")

    if fit:
        print("\nReframing to fill the canvas:")
        sys.exit(report(symbols_dir, paths, fix=True))

if __name__ == "__main__":
    main()
