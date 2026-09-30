#!/usr/bin/env bash
# ONE LAUNCHER STANDS BETWEEN FOUR PACKAGES AND A LOST SAVE.
#
# packaging/linux/opendoctrines-launcher is what the Flatpak, the .deb, the
# .rpm and the AppImage all start. It exists because every one of those installs
# the game somewhere read-only while the game keeps config.json, saves/ and the
# trained AI model INSIDE its data directory -- so the launcher copies the
# shipped tree into XDG_DATA_HOME once and points OD_DATA_DIR at the copy.
#
# Two things it has to get right, and both fail silently:
#
#   1. It must find its own prefix. It used to hard-code /app, which is the
#      Flatpak's and nobody else's; a .deb built from that file starts and
#      cannot save. The prefix is derived from the script's own path now, and
#      "derived" is a claim worth testing against each layout.
#
#   2. On a version change it must refresh what the release shipped and keep
#      what the PLAYER made. Getting that backwards deletes an evening, or
#      leaves a new release running last release's maps.
#
# No game binary is needed: a stub that prints OD_DATA_DIR proves the contract,
# and this runs anywhere with a POSIX shell.
set -uo pipefail

checks=0; failed=0
ok() {
    checks=$((checks + 1))
    if [ "$1" = "1" ]; then printf '  ok    %s\n' "$2"
    else printf '  FAIL  %s%s\n' "$2" "${3:+  [$3]}"; failed=$((failed + 1)); fi
}
eq() { [ "$2" = "$3" ] && ok 1 "$1" || ok 0 "$1" "got '$2', wanted '$3'"; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LAUNCHER="$ROOT/packaging/linux/opendoctrines-launcher"
[ -f "$LAUNCHER" ] || { echo "no launcher at $LAUNCHER"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Build a fake install at an arbitrary prefix and run the launcher from it.
make_install() {   # make_install <prefix> <version>
    local prefix="$1" version="$2"
    mkdir -p "$prefix/bin" "$prefix/lib/opendoctrines/data"
    cp "$LAUNCHER" "$prefix/bin/opendoctrines"; chmod +x "$prefix/bin/opendoctrines"
    printf '#!/bin/sh\nprintf "%%s" "$OD_DATA_DIR"\n' \
        > "$prefix/lib/opendoctrines/OpenDoctrines"
    chmod +x "$prefix/lib/opendoctrines/OpenDoctrines"
    printf '%s\n' "$version" > "$prefix/lib/opendoctrines/VERSION"
    printf 'shipped\n'       > "$prefix/lib/opendoctrines/data/tips.json"
}

echo
echo "== every package's prefix, from the same file =="
# The layouts are genuinely different roots; the launcher may assume none.
for layout in app usr appdir; do
    case $layout in
        app)    prefix="$TMP/$layout/app" ;;          # Flatpak
        usr)    prefix="$TMP/$layout/usr" ;;          # .deb / .rpm
        appdir) prefix="$TMP/$layout/AppDir/usr" ;;   # AppImage
    esac
    make_install "$prefix" 1.0.0
    home="$TMP/$layout/home"
    got=$(XDG_DATA_HOME="$home" "$prefix/bin/opendoctrines" 2>/dev/null)
    eq "$layout: the game is started with a writable OD_DATA_DIR" \
       "$got" "$home/opendoctrines/data"
    [ -f "$home/opendoctrines/data/tips.json" ] \
        && ok 1 "$layout: the shipped tree was seeded" \
        || ok 0 "$layout: the shipped tree was seeded"
done

echo
echo "== an update keeps what the player made =="
prefix="$TMP/upd/usr"; home="$TMP/upd/home"
make_install "$prefix" 1.0.0
XDG_DATA_HOME="$home" "$prefix/bin/opendoctrines" >/dev/null 2>&1

d="$home/opendoctrines/data"
mkdir -p "$d/saves" "$d/mods" "$d/ai"
echo 'an evening' > "$d/saves/world.odsv"
echo '{"vol":7}'  > "$d/config.json"
echo 'my mod'     > "$d/mods/mine.odmod"
echo 'trained'    > "$d/ai/model.bin"

# The release ships: a new version, a changed file and a new one.
printf '%s\n' 1.1.0     > "$prefix/lib/opendoctrines/VERSION"
printf 'refreshed\n'    > "$prefix/lib/opendoctrines/data/tips.json"
printf 'a new map\n'    > "$prefix/lib/opendoctrines/data/newmap.odmap"
XDG_DATA_HOME="$home" "$prefix/bin/opendoctrines" >/dev/null 2>&1

eq "a changed shipped file is refreshed"  "$(cat "$d/tips.json")"        "refreshed"
eq "a new shipped file arrives"           "$(cat "$d/newmap.odmap")"     "a new map"
eq "the player's save survives"           "$(cat "$d/saves/world.odsv")" "an evening"
eq "the player's config survives"         "$(cat "$d/config.json")"      '{"vol":7}'
eq "the player's mods survive"            "$(cat "$d/mods/mine.odmod")"  "my mod"
eq "the trained model survives"           "$(cat "$d/ai/model.bin")"     "trained"
eq "and the stamp records the new version" \
   "$(cat "$d/.installed-version")" "1.1.0"

echo
echo "== a second run with no version change copies nothing =="
touch -t 200001010000 "$d/tips.json"
before=$(ls -l "$d/tips.json")
XDG_DATA_HOME="$home" "$prefix/bin/opendoctrines" >/dev/null 2>&1
eq "an unchanged version leaves the tree alone" "$(ls -l "$d/tips.json")" "$before"

echo
echo "== a broken install says so rather than starting nothing =="
bad="$TMP/bad/usr"; mkdir -p "$bad/bin"
cp "$LAUNCHER" "$bad/bin/opendoctrines"; chmod +x "$bad/bin/opendoctrines"
if XDG_DATA_HOME="$TMP/bad/home" "$bad/bin/opendoctrines" >/dev/null 2>&1; then
    ok 0 "a prefix with no game exits non-zero"
else
    ok 1 "a prefix with no game exits non-zero"
fi

echo
echo "$checks checks, $failed failed"
[ "$failed" -eq 0 ]
