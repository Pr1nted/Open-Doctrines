#!/usr/bin/env bash
# Render the trailer's 3-D shot.  See tools/trailer_blender.py for the scene.
#
#   tools/trailer_blender.sh                      # defaults: 170 frames, 1920x804
#   tools/trailer_blender.sh 240 docs/trailer/assets/plates/globe-names.png
#
# Blender is not on PATH on macOS, which is why this exists rather than a bare
# command in the docs: the binary lives inside the .app bundle.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
work="${TRAILER_WORK:-/tmp/trailer-march}"
frames="${1:-170}"
screen="${2:-$work/tl/political-fast.mp4}"

blender=""
for c in /Applications/Blender.app/Contents/MacOS/Blender \
         "$HOME/Applications/Blender.app/Contents/MacOS/Blender" \
         "$(command -v blender || true)"; do
    [ -n "$c" ] && [ -x "$c" ] && { blender="$c"; break; }
done
[ -n "$blender" ] || { echo "Blender not found" >&2; exit 1; }

[ -f "$screen" ] || { echo "no screen source: $screen" >&2
                      echo "generate one: see tools/trailer_timelapse.sh" >&2; exit 1; }

out="$work/blender"
rm -rf "$out"; mkdir -p "$out"
"$blender" --background --python "$root/tools/trailer_blender.py" -- \
    --out "$out" --screen "$screen" --frames "$frames" \
    --width 1920 --height 804 --samples 64

# The PNG sequence is the render; the mp4 is what the cut reads.
ffmpeg -y -v error -framerate 30 -i "$out/f%04d.png" \
    -c:v libx264 -preset medium -crf 17 -pix_fmt yuv420p "$work/fly-desk.mp4"
rm -rf "$out"
echo "-> $work/fly-desk.mp4  ($frames frames)"
