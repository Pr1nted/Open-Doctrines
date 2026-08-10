#!/usr/bin/env python3
"""Inline <use> elements in SVG files for nanosvg compatibility.

nanosvg does not support <use xlink:href="#id"> elements. This script
processes SVG files and expands all <use> references inline so they
render correctly with nanosvg.

Usage:
    python3 tools/inline_svg_use.py                     # process data/flags/*.svg
    python3 tools/inline_svg_use.py --path data/flags/  # custom path
"""

import os, sys, re, copy
import xml.etree.ElementTree as ET

# Pre-rewrite copies go here, under the build output, never inside data/. See
# process_svg_file for what happened when they went beside the file instead.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKUP_ROOT = os.path.join(REPO_ROOT, "build", "svg-pre-inline")

SVG_NS = "http://www.w3.org/2000/svg"
XLINK_NS = "http://www.w3.org/1999/xlink"


def q(tag):
    return f"{{{SVG_NS}}}{tag}"


def is_tag(elem, local_name):
    """Check if elem has the given local tag name, regardless of namespace."""
    if elem.tag == f"{{{SVG_NS}}}{local_name}":
        return True
    # Also match unprefixed tags (SVG without xmlns declaration)
    if elem.tag == local_name:
        return True
    return False


def xlink_q(attr):
    return f"{{{XLINK_NS}}}{attr}"


def preprocess_svg_text(text):
    """Clean up SVG text before XML parsing.

    1. Remove <metadata>...</metadata> blocks (may contain RDF with broken namespaces)
    2. Add xmlns:xlink if xlink:href is used but not declared
    """
    # Remove metadata blocks (including multiline)
    text = re.sub(r'<metadata[^>]*>.*?</metadata>', '', text, flags=re.DOTALL)
    # Remove any remaining Clark-notation attributes ({uri}attr) which indicate
    # broken serialisation — they appear in some corrupted SVGs
    text = re.sub(r'\s*\{[^}]+\}[a-zA-Z_][a-zA-Z0-9_.-]*="[^"]*"', '', text)
    # Restore missing namespace declarations
    if 'xmlns="http://www.w3.org/2000/svg"' not in text and 'xmlns:svg' not in text:
        if 'xmlns:xlink="http://www.w3.org/1999/xlink"' in text:
            text = text.replace('xmlns:xlink', 'xmlns="http://www.w3.org/2000/svg" xmlns:xlink', 1)
        else:
            text = text.replace('<svg', '<svg xmlns="http://www.w3.org/2000/svg"', 1)
    if 'xlink:href' in text and 'xmlns:xlink' not in text:
        if 'xmlns="http://www.w3.org/2000/svg"' in text:
            text = text.replace('xmlns="http://www.w3.org/2000/svg"',
                                'xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"', 1)
        else:
            text = text.replace('<svg', '<svg xmlns:xlink="http://www.w3.org/1999/xlink"', 1)
    return text


def parse_transform(transform_str):
    if not transform_str:
        return []
    transforms = []
    pattern = r"(\w+)\s*\(([^)]*)\)"
    for m in re.finditer(pattern, transform_str):
        op = m.group(1)
        args_str = m.group(2).strip()
        args = []
        for part in re.split(r"[\s,]+", args_str):
            part = part.strip()
            if part:
                try:
                    args.append(float(part))
                except ValueError:
                    args.append(0.0)
        transforms.append((op, args))
    return transforms


def transforms_to_str(transforms):
    parts = []
    for op, args in transforms:
        args_str = ",".join(f"{a:.6f}" for a in args)
        parts.append(f"{op}({args_str})")
    return " ".join(parts)


def concat_transforms(outer, inner):
    """Flatten two transform lists. `outer` is applied first, `inner` inside it.

    SVG reads transform="A B" left to right as outermost to innermost, so a flat
    concatenation is correct -- but only if the caller passes them in that order.
    Named for that, because passing them the wrong way round is exactly the bug
    this function was part of."""
    if not outer:
        return inner
    if not inner:
        return outer
    return outer + inner


def qname(root, local):
    """`local` in the document's own namespace, so the wrapper serialises as
    <g> rather than <ns0:g> and stays a real SVG element."""
    if isinstance(root.tag, str) and root.tag.startswith("{"):
        return "{" + root.tag[1:].split("}")[0] + "}" + local
    return local


def has_use_elements(elem):
    if is_tag(elem, "use"):
        return True
    for child in elem:
        if has_use_elements(child):
            return True
    return False


def collect_all_ids(root):
    ids = {}
    for elem in root.iter():
        elem_id = elem.get("id")
        if elem_id:
            ids[elem_id] = elem
    return ids


def deep_copy_strip_ids(elem):
    new = copy.deepcopy(elem)
    for child in new.iter():
        for attr in list(child.attrib):
            local = attr.split("}")[-1] if "}" in attr else attr
            if local == "id":
                del child.attrib[attr]
    return new


def inline_svg_text(svg_text, filepath_hint=""):
    """Process SVG text content, inlining all <use> elements.
    Returns (inlined_text, count_inlined, count_unresolved).
    """
    # Preprocess
    svg_text = preprocess_svg_text(svg_text)

    # Parse
    root = ET.fromstring(svg_text)

    # Collect all IDs
    ids = collect_all_ids(root)

    total_inlined = 0
    max_passes = 100

    for _pass in range(max_passes):
        use_to_resolve = None
        use_parent = None
        use_index = -1

        # Find first resolvable <use> element
        for parent in root.iter():
            children = list(parent)
            for i, child in enumerate(children):
                if is_tag(child, "use"):
                    href = child.get(xlink_q("href")) or child.get("href") or ""
                    if href and href.startswith("#") and href[1:] in ids:
                        use_to_resolve = child
                        use_parent = parent
                        use_index = i
                        break
            if use_to_resolve is not None:
                break

        # No more resolvable <use> elements
        if use_to_resolve is None:
            break

        href = use_to_resolve.get(xlink_q("href")) or use_to_resolve.get("href") or ""
        ref_id = href[1:]
        ref_elem = ids[ref_id]

        # Per the SVG spec, <use x y transform="T" href="#r"/> is equivalent to
        #     <g transform="T translate(x,y)"> {deep clone of #r, keeping its
        #                                        own transform} </g>
        # so the flattened list is  T, translate(x,y), then the referenced
        # element's own transform -- outermost first.
        #
        # This was built the other way round: the x/y translate was inserted
        # BEFORE the use element's own transform, and the referenced element's
        # transform was placed outermost of all. Whenever both a use offset and
        # a referenced transform existed the result was wrong, which is why the
        # East German emblem sat in the upper-left corner and Iran's takbir
        # bands were displaced. Flags with only one of the two looked fine,
        # which is why this survived 183 flags.
        # Build the <g> the spec says a <use> is equivalent to, and put the clone
        # inside it. Wrapping rather than merging keeps two things right that
        # merging got wrong:
        #
        #  - transform order. T then translate(x,y) on the wrapper, the
        #    referenced element's own transform left untouched inside it. Merging
        #    them into one list had the composition backwards, which put the East
        #    German emblem in a corner.
        #
        #  - EVERY OTHER ATTRIBUTE. Only transform/x/y were carried over, so
        #    stroke, fill, opacity and the rest were silently dropped. Nepal
        #    draws its blue border as <use href="#a" stroke="#003893" .../> over
        #    the crimson field, so the whole border vanished -- which is why that
        #    file ended up hand-patched with a duplicate blue path instead.
        #    Attributes go on the wrapper, so anything the referenced element
        #    sets itself still wins, which is what inheritance does.
        wrapper = ET.Element(qname(root, "g"))
        for attr, value in use_to_resolve.attrib.items():
            local = attr.split("}")[-1] if "}" in attr else attr
            if local in ("href", "x", "y", "transform", "id"):
                continue
            wrapper.set(attr, value)

        use_transforms = parse_transform(use_to_resolve.get("transform", ""))
        x_str = use_to_resolve.get("x")
        y_str = use_to_resolve.get("y")
        if x_str or y_str:
            tx = float(x_str) if x_str else 0.0
            ty = float(y_str) if y_str else 0.0
            use_transforms.append(("translate", [tx, ty]))
        if use_transforms:
            wrapper.set("transform", transforms_to_str(use_transforms))

        use_id = use_to_resolve.get("id")
        if use_id:
            wrapper.set("id", use_id)

        wrapper.append(deep_copy_strip_ids(ref_elem))

        use_parent.remove(use_to_resolve)
        use_parent.insert(use_index, wrapper)

        total_inlined += 1

    # After inlining, check for remaining <use> elements
    remaining = 0
    if has_use_elements(root):
        remaining = sum(1 for _ in root.iter() if isinstance(_.tag, str) and is_tag(_, "use"))

    output = serialize_xml(root)
    return output, total_inlined, remaining


def process_svg_file(filepath):
    """Process a single SVG file. Returns (inlined_count, unresolved_count) or raises.

    Keeps the pre-inlining file, because this rewrites in place and when the
    transform composition turned out to be wrong there was no way to see it or
    undo it without re-downloading every flag from Commons -- the only evidence
    was an emblem in the wrong corner. A copy costs nothing and makes the next
    inliner bug diffable.

    UNDER build/, NOT beside the file. These backups used to land in
    data/flags/.orig/, which is inside the directory that ships: 1.4 MB of
    superseded artwork in every installer and, until it was excluded, in the
    package every browser visitor waited on before the menu drew. They are
    developer scaffolding for one rewrite, and git already keeps the real
    history, so they belong with the build output.
    """
    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        original = f.read()

    if "<use" in original:
        orig_dir = os.path.join(BACKUP_ROOT, os.path.basename(os.path.dirname(filepath)))
        os.makedirs(orig_dir, exist_ok=True)
        keep = os.path.join(orig_dir, os.path.basename(filepath))
        if not os.path.exists(keep):
            with open(keep, "w", encoding="utf-8") as f:
                f.write(original)

    if "<use" not in original and "<use " not in original:
        return 0, 0

    result_text, inlined, remaining = inline_svg_text(original, filepath)

    with open(filepath, "w", encoding="utf-8") as f:
        f.write(result_text)

    return inlined, remaining


def _get_ns_decls(elem):
    """Collect namespace declarations needed by this element and its children."""
    needed = {}
    # Check if elem uses SVG namespace
    if elem.tag.startswith(f"{{{SVG_NS}}}"):
        needed["xmlns"] = SVG_NS
    # Check xlink usage in attributes
    for attr_name in elem.attrib:
        if attr_name.startswith(f"{{{XLINK_NS}}}"):
            needed["xmlns:xlink"] = XLINK_NS
    # Check xlink usage in children
    for child in elem.iter():
        if child.tag.startswith(f"{{{XLINK_NS}}}"):
            needed["xmlns:xlink"] = XLINK_NS
        for attr_name in child.attrib:
            if attr_name.startswith(f"{{{XLINK_NS}}}"):
                needed["xmlns:xlink"] = XLINK_NS
    return needed


def serialize_xml(elem, level=0, root=True):
    indent = "  " * level
    tag = elem.tag

    if tag.startswith("{"):
        ns_uri = tag.split("}")[0][1:]
        local = tag.split("}")[1]
        if ns_uri == SVG_NS:
            tag_name = local
        elif ns_uri == XLINK_NS:
            tag_name = f"xlink:{local}"
        else:
            tag_name = local
    else:
        tag_name = tag

    parts = [f"{indent}<{tag_name}"]

    # Add namespace declarations on root element
    if root:
        ns_decls = _get_ns_decls(elem)
        for prefix, uri in sorted(ns_decls.items()):
            parts.append(f' {prefix}="{_escape_attr(uri)}"')

    for attr_name, attr_val in sorted(elem.attrib.items()):
        if attr_name.startswith("{"):
            ns_uri = attr_name.split("}")[0][1:]
            local = attr_name.split("}")[1]
            if ns_uri == XLINK_NS:
                attr_full = f"xlink:{local}"
            else:
                attr_full = attr_name
        else:
            attr_full = attr_name
        parts.append(f' {attr_full}="{_escape_attr(attr_val)}"')

    children = list(elem)
    text = elem.text or ""

    if not children and not text.strip():
        parts.append("/>")
        return "".join(parts)

    parts.append(">")

    if text.strip():
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
    import argparse
    parser = argparse.ArgumentParser(description="Inline <use> elements in SVG files")
    parser.add_argument("--path", default=None, help="Path to flag directory")
    parser.add_argument("--check", action="store_true", help="Only check which files have <use>")
    parser.add_argument("--verbose", action="store_true", help="Verbose output")
    args = parser.parse_args()

    flags_dir = args.path
    if not flags_dir:
        flags_dir = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "data", "flags"
        )

    if not os.path.isdir(flags_dir):
        print(f"ERROR: {flags_dir} not found")
        sys.exit(1)

    inlined = 0
    clean = 0
    errors = 0
    unresolved = 0

    for fname in sorted(os.listdir(flags_dir)):
        if not fname.endswith(".svg"):
            continue
        fpath = os.path.join(flags_dir, fname)

        if args.check:
            with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                content = f.read()
            if "<use" in content and "<use " in content:
                count = content.count("<use ") + content.count("<use\n") + content.count("<use\r")
                # subtract self-closing <use/>
                count -= content.count("<use/>") + content.count("<use />")
                if count > 0:
                    print(f"  {count:3d} use(s): {fname}")
                    unresolved += 1
                else:
                    clean += 1
            else:
                clean += 1
            continue

        try:
            inc, rem = process_svg_file(fpath)
            if inc > 0:
                if args.verbose:
                    rem_str = f" ({rem} remaining)" if rem > 0 else ""
                    print(f"  INLINED {inc}{rem_str}: {fname}")
                inlined += 1
            elif rem > 0:
                print(f"  UNRESOLVED ({rem}): {fname}")
                unresolved += 1
            else:
                clean += 1
        except ET.ParseError as e:
            print(f"  PARSE ERROR: {fname}: {e}")
            errors += 1
        except Exception as e:
            print(f"  ERROR: {fname}: {e}")
            import traceback
            traceback.print_exc()
            errors += 1

    total = inlined + clean + errors + unresolved
    if args.check:
        print(f"\nChecked {total} files: {unresolved} have <use>, {clean} clean, {errors} errors")
    else:
        print(f"\nDone: {inlined} inlined, {clean} clean, {unresolved} unresolved, {errors} errors (total {total})")


if __name__ == "__main__":
    main()
