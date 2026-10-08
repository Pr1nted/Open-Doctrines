#!/usr/bin/env bash
# Map scripts against the game's mechanics, end to end.
#
# Nothing else runs a script against a real world: script_expr_test covers the
# expression parser and script_commands_test the mod command registry, and
# neither ever resolves `country.USA.goods.fuel` or sets it. Every reference
# and every `set` target lives in ScriptEngine::resolveRef/setRef, against
# Game state, and was checked by nobody -- which is how the documented
# `country.USA.at_war_with RUS` form went unnoticed as never working.
#
# So: put tests/data/script_mechanics.txt into a copy of the Modern Day map,
# load it in the dedicated server with the goods economy on, and read what the
# script printed. The script checks itself and prints "ok ..." or "FAIL ...";
# this fails on any FAIL, on any script error, and on a missing final "done"
# (a script that stopped early would otherwise pass by saying nothing).
#
#   tests/script_mechanics_test.sh [build-dir]
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"

srv="$build/OpenDoctrinesServer"
[ -x "$srv" ] || srv="$build/Release/OpenDoctrinesServer.exe"
[ -x "$srv" ] || srv="$build/Release/OpenDoctrinesServer"
if [ ! -x "$srv" ]; then
    echo "  no dedicated server binary under $build -- skipping"
    exit 0
fi
PYBIN="$("$root/tools/find_python.sh" 2>/dev/null || echo python3)"

work="$build/scriptmechanics"
rm -rf "$work" && mkdir -p "$work"
data="$work/data"
mkdir -p "$data/saves"
for entry in "$root/data"/*; do
    name="$(basename "$entry")"
    [ "$name" = "saves" ] && continue
    [ "$name" = "STDmaps" ] && continue
    ln -s "$entry" "$data/$name" 2>/dev/null || true
done
if [ ! -e "$root/data/STDmaps/map.odmap" ]; then
    echo "  no map.odmap under $root/data/STDmaps -- skipping"
    exit 0
fi

# The server takes a map by ID from its map list, never by path, so the test
# map gets a map directory of its own: with no maps_index.json there, the
# list is a scan of the .odmap files present, and this is the only one.
mkdir -p "$data/STDmaps"
map="$data/STDmaps/script_mechanics.odmap"
"$PYBIN" - "$root" "$map" <<'PY'
import sys, zipfile
root, out = sys.argv[1], sys.argv[2]
src = f"{root}/data/STDmaps/map.odmap"
script = open(f"{root}/tests/data/script_mechanics.txt", encoding="utf-8").read()
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        if item.filename.startswith("scripts/") and item.filename != "scripts/":
            continue   # only this test's script runs
        zout.writestr(item, zin.read(item.filename))
    zout.writestr("scripts/script_mechanics.txt", script)
PY

out="$work/server.log"
OD_GOODS=1 "$srv" --data "$data" --map script_mechanics --check > "$out" 2>&1
status=$?
if [ "$status" -ge 128 ]; then
    echo "  FAIL  the server was killed (status $status)"
    tail -20 "$out"
    exit 1
fi

fail=0
oks=$(grep -c '^\[SCRIPT\] script_mechanics: ok' "$out")
grep '^\[SCRIPT\] script_mechanics: ' "$out" | sed 's/^\[SCRIPT\] script_mechanics: /  /'
if grep -q '^\[SCRIPT\] script_mechanics: FAIL' "$out"; then fail=1; fi
if grep -q '^\[SCRIPT\] Failed to load' "$out"; then
    echo "  FAIL  the script reported errors:"
    grep '^\[SCRIPT\] Failed to load' "$out" | sed 's/^/        /'
    fail=1
fi
if ! grep -q '^\[SCRIPT\] script_mechanics: done$' "$out"; then
    echo "  FAIL  the script did not run to the end"
    fail=1
fi
echo "  $oks checks passed"
exit $fail
