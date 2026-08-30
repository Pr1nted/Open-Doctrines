#!/usr/bin/env bash
# Retake every image in README.md and on the store page, from one command.
#
# WHY A SCRATCH DATA DIRECTORY
#
# The shots have to be of a KNOWN install, not of this machine's. A developer's
# data/ has their saves in the world browser, their mods in the mod menu and
# their signed-in name on the account screen -- so shots taken against it vary
# by whoever ran it last, and one of them eventually publishes a screenshot with
# their own account in the corner. This builds a throwaway data directory
# instead: bulk assets symlinked, everything a screenshot can see set up here.
#
# It also means the mod menu has mods in it. An empty mod menu is a truthful
# picture of a feature nobody can see working, which is the least useful
# possible image of the thing the SDK exists for.
#
#   tools/screenshots.sh                images -> docs/img
#   tools/screenshots.sh out/           images -> out/
#   tools/screenshots.sh --clean        throw the scratch directory away
#
# The world the in-game shots are taken in is simulated, not hand-played, so
# re-running this produces the same kind of picture rather than whatever
# happened to be on screen. See --simulate in main.cpp.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$root/.screenshots"
out="${1:-$root/docs/img}"

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$work"
    echo "removed $work"
    exit 0
fi
mkdir -p "$out"
out="$(cd "$out" && pwd)"

# Newest build wins, same rule and same reason as tools/playtest.sh: preferring
# a directory by name silently photographs a binary that predates the change.
game=""
for candidate in "$root/cmake-build-debug/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
                 "$root/build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
                 "$root/build/OpenDoctrines" \
                 "$root/cmake-build-debug/OpenDoctrines"; do
    [ -x "$candidate" ] || continue
    [ -z "$game" ] || [ "$candidate" -nt "$game" ] && game="$candidate"
done
[ -n "$game" ] && [ -x "$game" ] || { echo "build the game first: cmake --build build" >&2; exit 1; }
echo "using $game"

# ---- the scratch install ----------------------------------------------------
data="$work/data"
mkdir -p "$data/saves" "$data/mods"
for entry in "$root/data"/*; do
    base="$(basename "$entry")"
    case "$base" in account.json|config.json|servers.json|saves|mods|mods.json|tools) continue ;; esac
    [ -e "$data/$base" ] || ln -s "$entry" "$data/$base"
done

# Signed out, on no account service, with the music silent -- the "now playing"
# toast is charming in the game and clutter in a screenshot.
#
# gdtl is on because the tour's five translation-layer shots skip themselves
# when it is off, and a skip here looks exactly like a feature with no images.
# A build made without -DOD_ENABLE_GDTL=ON still skips them, which is right:
# there is nothing to photograph.
cat > "$data/config.json" <<'EOF'
{
  "accountIssuer": "",
  "accountAgreed": true,
  "fullscreen": false,
  "musicVolume": 0,
  "nowPlayingToast": false,
  "showFps": false,
  "showZoom": false,
  "debugMode": false,
  "gdtl": true
}
EOF

# ---- mods, so the mod menu has something in it ------------------------------
# The SDK's own examples, which the test suite already rebuilds from source, so
# these are the same artefacts a modder would get rather than props.
mods=0
for m in "$root/sdk/examples/hello-panel/hello-panel.odmod" \
         "$root/sdk/rust/examples/hello_panel/hello-panel-rust.odmod" \
         "$root/sdk/zig/examples/hello-panel/hello-panel-zig.odmod" \
         "$root/sdk/go/examples/hello-panel/hello-panel-go.odmod"; do
    [ -f "$m" ] || continue
    cp "$m" "$data/mods/"
    mods=$((mods + 1))
done
if [ "$mods" -eq 0 ]; then
    echo "note: no example .odmod found -- the mod menu will photograph empty." >&2
    echo "      build them first:  tests/run_all.sh" >&2
fi

# ---- the world the in-game shots are taken in -------------------------------
# Simulated rather than played: a hand-played save is whatever the last session
# left behind, and these images are meant to be reproducible.
save_name="Screenshot World"
if [ ! -f "$data/saves/$save_name.odsv" ]; then
    turns="${OD_SHOT_TURNS:-12}"
    echo "simulating $turns turns for the in-game shots (a few minutes)..."
    OD_DATA_DIR="$data" "$game" --simulate "$root/data/STDmaps/1939.odmap" "$turns" "$save_name" \
        2>&1 | grep -E '^\[SIM\]' || true
    [ -f "$data/saves/$save_name.odsv" ] || {
        echo "the simulation did not leave a save behind" >&2; exit 1; }
else
    echo "reusing $data/saves/$save_name.odsv"
fi

# ---- the tour ---------------------------------------------------------------
OD_DATA_DIR="$data" "$game" --screenshots "$out" "$save_name.odsv" 2>&1 | grep -E '^\[SHOT\]'

echo
echo "images in $out:"
ls -1 "$out"/*.png 2>/dev/null | sed 's|^|  |'
