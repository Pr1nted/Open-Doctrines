#!/usr/bin/env python3
"""Generate icon.icns (macOS) and icon.ico (Windows) from icon.png source."""
import subprocess, shutil, os, tempfile
from PIL import Image

SRC = os.path.join(os.path.dirname(__file__), '..', 'data', 'Icon', 'icon.png')
DIST_ICO = os.path.join(os.path.dirname(__file__), '..', 'data', 'Icon')
DIST_MAC = os.path.join(os.path.dirname(__file__), '..', 'dist', 'OpenDoctrines.app', 'Contents', 'Resources')

img = Image.open(SRC).convert('RGBA')
assert img.size == (1024, 1024), f'Expected 1024x1024 source, got {img.size}'

# ── macOS .icns via iconset ──
iconset = tempfile.mkdtemp(suffix='.iconset')
sizes = {
    'icon_16x16': 16, 'icon_16x16@2x': 32,
    'icon_32x32': 32, 'icon_32x32@2x': 64,
    'icon_128x128': 128, 'icon_128x128@2x': 256,
    'icon_256x256': 256, 'icon_256x256@2x': 512,
    'icon_512x512': 512, 'icon_512x512@2x': 1024,
}
for name, sz in sizes.items():
    resized = img.resize((sz, sz), Image.LANCZOS)
    resized.save(os.path.join(iconset, f'{name}.png'))

icns_path = os.path.join(DIST_MAC, 'icon.icns')
subprocess.run(['iconutil', '--convert', 'icns', iconset, '--output', icns_path], check=True)
# Also copy to data/Icon/ for build script access
icns_data = os.path.join(DIST_ICO, 'icon.icns')
shutil.copy2(icns_path, icns_data)
print(f'Created {icns_path}')
print(f'Copied to {icns_data}')

# ── Windows .ico (multi-size) ──
ico_sizes = [16, 32, 48, 64, 128, 256]
ico_frames = []
for sz in ico_sizes:
    resized = img.resize((sz, sz), Image.LANCZOS)
    ico_frames.append(resized)
ico_path = os.path.join(DIST_ICO, 'icon.ico')
ico_frames[0].save(ico_path, format='ICO', append_images=ico_frames[1:],
                   sizes=[(s, s) for s in ico_sizes])
print(f'Created {ico_path}')
