"""Generate a thumbnail PNG from an .odmap file (from political.png)."""

import zipfile
import sys
import os
from PIL import Image

def generate_thumb(odmap_path, output_path, size=(160, 80)):
    with zipfile.ZipFile(odmap_path, 'r') as z:
        # Use political.png for a coloured country-border preview
        if 'political.png' in z.namelist():
            data = z.read('political.png')
            img = Image.open(io.BytesIO(data))
        elif 'provinces.png' in z.namelist():
            data = z.read('provinces.png')
            img = Image.open(io.BytesIO(data))
        else:
            print(f"No suitable image found in {odmap_path}")
            return False

    # Scale down
    thumb = img.resize(size, Image.NEAREST)
    thumb.save(output_path, 'PNG')
    print(f"Thumbnail saved: {output_path} ({size[0]}x{size[1]})")
    return True

if __name__ == '__main__':
    import io
    import argparse
    parser = argparse.ArgumentParser(description='Generate map thumbnail from .odmap')
    parser.add_argument('odmap', help='Path to .odmap file')
    parser.add_argument('-o', '--output', help='Output PNG path (default: thumb.png next to .odmap)')
    parser.add_argument('-s', '--size', type=int, nargs=2, default=[160, 80], help='Thumbnail size (default: 160 80)')
    args = parser.parse_args()

    out = args.output
    if not out:
        out = os.path.splitext(args.odmap)[0] + '_thumb.png'
    generate_thumb(args.odmap, out, tuple(args.size))
