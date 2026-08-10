"""Generate a thumbnail PNG from an .odmap file.

Prefers the thumb.png the archive already carries. Every map built by the
pipeline or exported by the editor ships one, drawn at thumbnail scale rather
than shrunk from the full raster -- which matters, because the political
shading runs 60 map pixels inward and averaging that down to 1/16 scale drives
the whole world to its darkest value (see fill_water_speckle.build_thumb).

Falls back to redrawing the political layer for an archive with no thumb.
"""

import argparse
import io
import os
import sys
import zipfile

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from odmap_pack import political_layer   # noqa: E402


def generate_thumb(odmap_path, output_path, size=(160, 80)):
    with zipfile.ZipFile(odmap_path) as z:
        names = z.namelist()
        data = z.read("thumb.png") if "thumb.png" in names else None

    if data is not None:
        img = Image.open(io.BytesIO(data))
    else:
        try:
            img = political_layer(odmap_path)
        except (KeyError, OSError, ValueError) as e:
            print(f"No suitable image found in {odmap_path}: {e}")
            return False

    # Nearest neighbour: flat political colours, and averaging across a border
    # invents a country colour that belongs to neither side of it.
    thumb = img.resize(size, Image.NEAREST)
    thumb.save(output_path, "PNG")
    print(f"Thumbnail saved: {output_path} ({size[0]}x{size[1]})")
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate map thumbnail from .odmap")
    parser.add_argument("odmap", help="Path to .odmap file")
    parser.add_argument("-o", "--output", help="Output PNG path (default: <name>_thumb.png)")
    parser.add_argument("-s", "--size", type=int, nargs=2, default=[160, 80],
                        help="Thumbnail size (default: 160 80)")
    args = parser.parse_args()

    out = args.output or os.path.splitext(args.odmap)[0] + "_thumb.png"
    sys.exit(0 if generate_thumb(args.odmap, out, tuple(args.size)) else 1)
