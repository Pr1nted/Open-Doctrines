#!/bin/bash
# Build the TempleOS release: bake the data, compile the binary IN the guest,
# and package what somebody actually needs to play.
#
# ── WHY THE VM IS PART OF THE BUILD ──
#
# The binary is machine code for TempleOS, produced by TempleOS's own compiler.
# There is no cross-compiler and writing one is not on the table, so the build
# host boots the guest, runs Cmp inside it, and takes the result back out. That
# is slow and it is honest: the artifact is what that machine produced.
#
#     tools/templeos_release.sh            # build into dist/
#     tools/templeos_release.sh --data     # just re-bake the data files
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
ver="$(tr -d ' \n' < VERSION)"
out="$root/dist/templeos"
map="${OD_MAP:-data/STDmaps/map.odmap}"
vm="$root/templeos/vm.sh"

say() { printf '\n== %s ==\n' "$*"; }

say "baking the world from $map"
python3 tools/templeos_world.py "$map" templeos/world.odw --width 1024 --height 512
python3 tools/templeos_data.py  "$map" templeos/game.odd
python3 tools/templeos_font.py  build-web/_deps/raylib-src/src/rtext.c templeos/font.odf
python3 tools/templeos_sync.py

if [ "${1:-}" = "--data" ]; then
    echo "data only; stopping"
    exit 0
fi

# The output directory exists before anything writes near it: the first
# version of this pointed a screenshot at "$out/.." before $out existed, and
# the file landed AS the directory it was supposed to sit beside.
mkdir -p "$out"

say "compiling inside TempleOS"
"$vm" stop >/dev/null 2>&1 || true
sleep 2
"$vm" push templeos/*.HC templeos/*.HH templeos/world.odw templeos/game.odd \
             templeos/font.odf >/dev/null
"$vm" run >/dev/null
# The boot menu, the tour prompt, then the compile. The waits are generous
# because this is an emulated 1990s machine and a short wait here means a
# keystroke lands in the wrong prompt and the build silently does nothing.
sleep 28; "$vm" key 2
sleep 40; "$vm" key n
sleep 10; "$vm" type 'Cmp("ODBIN.HC",,"ODBIN.BIN");'; "$vm" key ret
sleep 150
"$vm" shot "$root/dist/templeos-build.png" >/dev/null 2>&1 || true
"$vm" stop >/dev/null

say "collecting"
if ! "$vm" pull ODBIN.BIN "$out"; then
    echo "no binary came back. dist/templeos-build.png shows what the guest"
    echo "was doing -- most often the compile never started because a"
    echo "keystroke landed in the boot menu or the tour prompt."
    exit 1
fi
cp templeos/world.odw templeos/game.odd templeos/font.odf "$out/"
cp templeos/*.HC templeos/*.HH "$out/" 2>/dev/null || true

cat > "$out/READ.ME" <<EOF
Open Doctrines $ver -- TempleOS build

Copy every file here into D:/Home on a TempleOS machine, then:

    #include "RUN"
    U0 (*f)(U8 *w) = ODStart;
    (*f)("world.odw");

ODBIN.BIN is the game, compiled ahead of time by TempleOS's own compiler.
The .HC files are its source; they are included so the build can be
reproduced and because this OS is source-first by temperament.

world.odw  the world: 1,632 provinces, who owns them, the sea
game.odd   research, policies, claims, shells, who lives where
font.odf   raylib's bitmap font, the same glyphs the desktop game draws

Needs a linear-framebuffer mode through the Bochs DISPI registers, which
QEMU, Bochs and VirtualBox provide and real hardware does not.
EOF

zip="$root/dist/open-doctrines-templeos-$ver.zip"
rm -f "$zip"
(cd "$out/.." && zip -qr "$zip" templeos)
say "built $zip"
ls -la "$zip"
echo
echo "tag it with:  git tag templeos-v$ver && git push origin templeos-v$ver"
