#!/usr/bin/env bash
# Real motion for the trailer: the map evolving over a whole game.
#
#   tools/trailer_timelapse.sh                       # Quick Start, 3 views
#   tools/trailer_timelapse.sh "data/saves/X.odsv"
#
# Output: $TRAILER_WORK/tl/<view>-fast.mp4, 1920x832, 30 fps.
#
# WHY THE EXTRA CONVERSION STEP. --export-timelapse writes a GIF, and the GIF
# carries a long per-frame delay: ffmpeg honours it and a 820-turn export
# decodes to SIX AND A HALF MINUTES of video with every frame repeated about
# fourteen times. Re-encoding that straight gives a file that looks like a
# still. So one frame in twenty is kept, which restores one turn per frame.
#
# AND WHY THE CROP. The Antarctic ice shelf is drawn flat grey and is the
# brightest thing on the plate -- on a dark trailer it reads as a light leak
# along the bottom edge. 1920x832 cuts it and leaves 2.31:1, which is close
# enough to the 2.39 picture that the shots barely have to scale.
#
# The GIF is an intermediate and is deleted: at 1920x960 it is 700 MB a view.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
work="${TRAILER_WORK:-/tmp/trailer-march}"
save="${1:-$root/data/saves/Quick Start.odsv}"
out="$work/tl"
mkdir -p "$out"

game=""
for c in "$root/build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
         "$root/cmake-build-debug/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
         "$root/build/OpenDoctrines" "$root/cmake-build-debug/OpenDoctrines"; do
    [ -x "$c" ] || continue
    [ -z "$game" ] || [ "$c" -nt "$game" ] && game="$c"
done
[ -n "$game" ] || { echo "build the game first: cmake --build build" >&2; exit 1; }
[ -f "$save" ] || { echo "no such save: $save" >&2; exit 1; }

# A scratch data dir, and ai/ COPIED not linked: the export only replays, but
# the same guard as tools/screenshots.sh costs nothing and model.bin has been
# overwritten by a tool that was not supposed to write it before.
data="$work/data"
mkdir -p "$data"
for entry in "$root/data"/*; do
    b="$(basename "$entry")"
    case "$b" in account.json|config.json|servers.json|mods.json) continue ;; esac
    if [ "$b" = "ai" ]; then [ -e "$data/ai" ] || cp -R "$entry" "$data/ai"; continue; fi
    [ -e "$data/$b" ] || ln -s "$entry" "$data/$b"
done

for view in political troops population; do
    echo "=== $view ==="
    OD_DATA_DIR="$data" "$game" --export-timelapse "$save" \
        "$out/$view.gif" 1920x960 "$view" --no-watermark 2>&1 | grep -iE "saved|error" | tail -1
    ffmpeg -y -v error -i "$out/$view.gif" \
        -vf "select='not(mod(n,20))',crop=1920:832:0:0,setpts=N/30/TB" -an \
        -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -r 30 "$out/$view-fast.mp4"
    rm -f "$out/$view.gif"
    printf '  -> %s  ' "$out/$view-fast.mp4"
    ffprobe -v error -show_entries stream=nb_frames -show_entries format=duration \
        -of csv=p=0 "$out/$view-fast.mp4" | tr '\n' ' '
    echo
done
