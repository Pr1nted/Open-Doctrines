#!/usr/bin/env bash
# Install what the DOS build needs: the DJGPP cross compiler and CWSDPMI.
#
#   tools/dos_toolchain.sh <dir>
#
# <dir>/djgpp     the compiler (i586-pc-msdosdjgpp-*): point DJGPP_PREFIX here
# <dir>/cwsdpmi   CWSDPMI.EXE and CWSDPMI.DOC, the DPMI host shipped beside the
#                 game -- a DJGPP program needs one to run under plain DOS
#
# Both pinned and checked by hash: these end up inside a release, so what is
# downloaded has to be what was reviewed.
#
# DJGPP is andrewwutw/build-djgpp's prebuilt (gcc 12.2.0, DJGPP 2.05), the
# usual way to get the cross compiler without building gcc. CWSDPMI is r7, from
# the DJGPP archive itself (or its FAU mirror).
set -euo pipefail

DEST="${1:?usage: dos_toolchain.sh <dir>}"
mkdir -p "$DEST"
DEST="$(cd "$DEST" && pwd)"

DJGPP_URL="https://github.com/andrewwutw/build-djgpp/releases/download/v3.4/djgpp-linux64-gcc1220.tar.bz2"
DJGPP_SHA256="8464f17017d6ab1b2bb2df4ed82357b5bf692e6e2b7fee37e315638f3d505f00"

CWSDPMI_URLS=(
  "https://www.delorie.com/pub/djgpp/current/v2misc/csdpmi7b.zip"
  "https://ftp.fau.de/djgpp/current/v2misc/csdpmi7b.zip"
)
# csdpmi7b.zip (CWSDPMI r7), as first fetched in CI on 2026-10-29.
CWSDPMI_SHA256="deacda0488e1cdd7c4a9f32fab45662b34c0ed6b2d7d4d13bc07041b62004a8c"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [ ! -x "$DEST/djgpp/bin/i586-pc-msdosdjgpp-gcc" ]; then
  echo "fetching DJGPP (gcc 12.2.0)"
  curl -fsSL --retry 3 -o "$work/djgpp.tar.bz2" "$DJGPP_URL"
  echo "$DJGPP_SHA256  $work/djgpp.tar.bz2" | sha256sum -c -
  tar -xjf "$work/djgpp.tar.bz2" -C "$DEST"
fi
"$DEST/djgpp/bin/i586-pc-msdosdjgpp-gcc" --version | head -1

if [ ! -f "$DEST/cwsdpmi/CWSDPMI.EXE" ]; then
  got=""
  for u in "${CWSDPMI_URLS[@]}"; do
    if curl -fsSL --retry 3 -o "$work/csdpmi.zip" "$u"; then got="$u"; break; fi
  done
  [ -n "$got" ] || { echo "could not fetch CWSDPMI from any mirror" >&2; exit 1; }
  sum="$(sha256sum "$work/csdpmi.zip" | cut -d' ' -f1)"
  echo "CWSDPMI from $got: sha256 $sum"
  if [ "$sum" != "$CWSDPMI_SHA256" ]; then
    echo "CWSDPMI hash mismatch: expected $CWSDPMI_SHA256" >&2
    exit 1
  fi
  mkdir -p "$work/cws" "$DEST/cwsdpmi"
  unzip -q "$work/csdpmi.zip" -d "$work/cws"
  exe="$(find "$work/cws" -iname cwsdpmi.exe | head -1)"
  doc="$(find "$work/cws" -iname cwsdpmi.doc | head -1)"
  [ -n "$exe" ] && [ -n "$doc" ] || { echo "csdpmi7b.zip has no CWSDPMI.EXE/.DOC:"; find "$work/cws"; exit 1; }
  cp "$exe" "$DEST/cwsdpmi/CWSDPMI.EXE"
  cp "$doc" "$DEST/cwsdpmi/CWSDPMI.DOC"
fi
ls -l "$DEST/cwsdpmi"
