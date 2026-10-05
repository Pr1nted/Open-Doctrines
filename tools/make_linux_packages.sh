#!/usr/bin/env bash
# Build the Linux packages from an already-packaged game tree.
#
#   tools/make_linux_packages.sh <tree> <version> <arch> <outdir> [kinds...]
#
#   tree     the directory tools/package.py produced (OpenDoctrines + data/)
#   version  1.2.3a -- the release version, without the leading v
#   arch     amd64 | arm64   (Debian spelling; rpm and AppImage are translated)
#   kinds    appimage deb rpm   (default: all three)
#
# WHY THESE EXIST WHEN THERE IS ALREADY A TARBALL
#
# The published binary is built on Ubuntu 22.04 and needs glibc 2.35, so
# README.md tells anyone on Ubuntu 20.04, Debian 11 or RHEL 9 to build from
# source.
#
# ONLY THE FLATPAK FIXES THAT, and this comment used to claim the AppImage did
# too. It does not. Measured 2026-09-30 on a Debian guest: the AppImage this
# script builds contains no libc and no dynamic loader, so glibc still comes
# from the HOST, and the binary inside still demands GLIBC_2.35 -- the same
# symbols, failing on the same distributions, for the same reason. An AppImage
# bundles an application's own libraries, not the C library under them; the
# usual advice is to build it on the oldest system you intend to support, and
# ours is built on the newest.
#
# So, honestly:
#   Flatpak    brings org.freedesktop.Platform, its own glibc included. Works
#              on Debian 11.
#   AppImage   one file, no install, no root, no package manager. Genuinely
#              useful, and NOT a glibc fix. It needs the same 2.35 the tarball
#              does.
#   .deb/.rpm  for people who want their package manager to own it, and to put
#              the game in the menu. Also not a glibc fix.
#
# What WOULD fix it is building the game itself against an older glibc -- in an
# ubuntu:20.04 container, say -- because the requirement is baked into the
# binary and no amount of repackaging lowers it.
#
# THEY CANNOT GO INTO DEBIAN OR FEDORA. The licence is free to play, modify and
# share NON-COMMERCIALLY, which is not OSI-approved, so the official archives
# are closed to this project (packaging/linux/README.md says the same about
# F-Droid). These are packages to download and install by hand, and saying so
# here is cheaper than someone discovering it during a submission.
#
# EVERY PACKAGE GETS THE MANAGED MARKER. GameUpdates::managedInstall() looks for
# a file called MANAGED beside the executable and, finding one, refuses to
# self-update -- see the note over it in GameUpdates.h, which names a .deb owned
# by dpkg and an AppImage specifically. The updater works by renaming the
# running binary and writing next to it, which behind a package manager does not
# merely fail: it desynchronises the package database from the disk. The plain
# zip has no marker and updates itself, which is the intended difference.
set -euo pipefail

die() { echo "make_linux_packages: $*" >&2; exit 1; }

[ $# -ge 4 ] || die "usage: $0 <tree> <version> <arch> <outdir> [appimage|deb|rpm ...]"
TREE=$(cd "$1" && pwd) || die "no such tree: $1"
VERSION="$2"
ARCH="$3"
OUT=$(mkdir -p "$4" && cd "$4" && pwd)
shift 4
KINDS=("$@")
[ ${#KINDS[@]} -gt 0 ] || KINDS=(appimage deb rpm)

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PKG="$ROOT/packaging/linux"
APPID=io.github.Pr1nted.OpenDoctrines

[ -x "$TREE/OpenDoctrines" ] || die "$TREE has no OpenDoctrines executable"
[ -d "$TREE/data" ]          || die "$TREE has no data directory"

case "$ARCH" in
    amd64) RPM_ARCH=x86_64;  APPIMAGE_ARCH=x86_64  ;;
    arm64) RPM_ARCH=aarch64; APPIMAGE_ARCH=aarch64 ;;
    *) die "arch must be amd64 or arm64, not $ARCH" ;;
esac

# ── the layout every package shares ──
#
# <prefix>/bin/opendoctrines          the launcher, which derives <prefix>
# <prefix>/lib/opendoctrines/         the game, read-only
# <prefix>/share/{applications,icons,metainfo}
#
# opendoctrines-launcher resolves the prefix from its own path, so this one
# staging tree becomes /usr for a .deb or .rpm and $APPDIR/usr for an AppImage
# with nothing rewritten. See the note at the top of that file.
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
LIB="$STAGE/usr/lib/opendoctrines"

mkdir -p "$LIB" "$STAGE/usr/bin" \
         "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/metainfo" \
         "$STAGE/usr/share/icons/hicolor/256x256/apps"

cp "$TREE/OpenDoctrines" "$LIB/"
cp -r "$TREE/data" "$LIB/data"
printf '%s\n' "$VERSION" > "$LIB/VERSION"

# The marker, worded as CMakeLists.txt words it for the installers it builds.
cat > "$LIB/MANAGED" <<'EOF'
This copy of OpenDoctrines was put here by an installer, a store or a
package manager, so the in-game updater will not replace it. Update it
the same way you installed it.

Delete this file only if you moved the game somewhere you manage
yourself and want it to update itself again.
EOF

install -m755 "$PKG/opendoctrines-launcher" "$STAGE/usr/bin/opendoctrines"
install -m644 "$PKG/$APPID.desktop"      "$STAGE/usr/share/applications/$APPID.desktop"
install -m644 "$PKG/$APPID.metainfo.xml" "$STAGE/usr/share/metainfo/$APPID.metainfo.xml"
# THE ICON COMES FROM THE REPOSITORY, like the three files above it.
#
# It used to be taken from $TREE, and that tree is whatever was handed in. A
# BUILD tree has data/Icon in it, which is why the preflight packages stage
# passed; a RELEASE tree does not, because tools/package.py ships only what is
# in DATA_ALLOWLIST and the icon is a packaging input rather than game content.
# So every release died here --
#
#   install: cannot stat '.../incoming/OpenDoctrines-linux-x64/data/Icon/icon.png'
#
# -- and no release has ever carried a .deb, .rpm, AppImage or Flatpak. The
# alternative, adding Icon to DATA_ALLOWLIST, would put 640 KB of build input
# into every player's download to satisfy a packaging step.
#
# $TREE first so a tree that does carry one keeps deciding, then the repository.
ICON="$TREE/data/Icon/icon.png"
[ -f "$ICON" ] || ICON="$ROOT/data/Icon/icon.png"
[ -f "$ICON" ] || die "no icon at $TREE/data/Icon/icon.png or $ROOT/data/Icon/icon.png"
install -m644 "$ICON" \
        "$STAGE/usr/share/icons/hicolor/256x256/apps/$APPID.png"

SIZE_KB=$(du -sk "$STAGE" | cut -f1)
echo "staged $((SIZE_KB / 1024)) MB for $VERSION ($ARCH)"

have() { command -v "$1" >/dev/null 2>&1; }

for kind in "${KINDS[@]}"; do
case "$kind" in

deb)
    have fpm || die "fpm is not installed (gem install --no-document fpm)"
    # The runtime libraries raylib's X11 backend dlopens or links. Named
    # explicitly rather than left to fpm, because a .deb that installs and then
    # cannot open a window is worse than one that refuses to install.
    fpm -s dir -t deb -C "$STAGE" \
        --name opendoctrines \
        --version "$VERSION" \
        --architecture "$ARCH" \
        --maintainer 'Pr1nted <111568178+Pr1nted@users.noreply.github.com>' \
        --url 'https://github.com/Pr1nted/Open-Doctrines' \
        --description 'OpenDoctrines -- a grand strategy game of doctrines, economies and war' \
        --license 'See LICENSE: free to play, modify and share non-commercially' \
        --depends libc6 --depends libx11-6 --depends libxrandr2 --depends libxi6 \
        --depends libxcursor1 --depends libxinerama1 --depends libgl1 \
        --depends libasound2 --depends libxkbcommon0 \
        --package "$OUT/opendoctrines_${VERSION}_${ARCH}.deb" \
        usr
    echo "  deb: $(basename "$OUT/opendoctrines_${VERSION}_${ARCH}.deb")"
    ;;

rpm)
    have fpm || die "fpm is not installed (gem install --no-document fpm)"
    # No explicit --depends here: rpmbuild reads the ELF and generates
    # dependencies on the SONAMEs it actually finds, which is both more accurate
    # than a hand list and correct across Fedora, openSUSE and RHEL, whose
    # package names for these libraries do not agree with each other.
    fpm -s dir -t rpm -C "$STAGE" \
        --name opendoctrines \
        --version "$VERSION" \
        --architecture "$RPM_ARCH" \
        --maintainer 'Pr1nted <111568178+Pr1nted@users.noreply.github.com>' \
        --url 'https://github.com/Pr1nted/Open-Doctrines' \
        --description 'OpenDoctrines -- a grand strategy game of doctrines, economies and war' \
        --license 'Non-commercial; see LICENSE' \
        --package "$OUT/opendoctrines-${VERSION}.${RPM_ARCH}.rpm" \
        usr
    echo "  rpm: $(basename "$OUT/opendoctrines-${VERSION}.${RPM_ARCH}.rpm")"
    ;;

appimage)
    have appimagetool || die "appimagetool is not on PATH"
    APPDIR="$STAGE/AppDir"
    mkdir -p "$APPDIR"
    cp -r "$STAGE/usr" "$APPDIR/usr"
    # An AppImage wants the desktop file and the icon at the ROOT of the AppDir
    # as well as in share/, and an AppRun to start. AppRun execs the launcher,
    # which derives $APPDIR/usr as its prefix exactly as it derives /usr.
    cp "$APPDIR/usr/share/applications/$APPID.desktop" "$APPDIR/$APPID.desktop"
    cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APPID.png" "$APPDIR/$APPID.png"
    cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/opendoctrines" "$@"
EOF
    chmod +x "$APPDIR/AppRun"
    # THE RUNTIME IS FETCHED HERE, NOT BY APPIMAGETOOL.
    #
    # Left to itself appimagetool downloads the type2 runtime as the last step
    # of the build, and on a Debian guest with working DNS and a working
    # https -- curl pulls that exact URL, 936 KB, 200 -- its own downloader
    # reports "server returned status code 0" and the whole build dies after
    # the squashfs is already made. Whatever the cause inside appimagetool,
    # curl can do it and appimagetool cannot, so curl does it.
    #
    # Fetching it on purpose is better anyway. "continuous" is a rolling tag:
    # a package build that pulls from it is reaching for a moving artifact at
    # the end of a release, which is both unreproducible and a thing that can
    # break on a day nobody changed anything. Cached under the output
    # directory, one download serves every arch and every re-run, and
    # OD_APPIMAGE_RUNTIME points at a pinned copy where one is wanted.
    rt="${OD_APPIMAGE_RUNTIME:-}"
    if [ -z "$rt" ]; then
        # OUTSIDE THE OUTPUT DIRECTORY, because the output directory is not
        # ours to keep. tools/linux_vm_test.sh starts every run with
        # `rm -rf ~/od`, which took the cache with it -- so a "cache" added to
        # stop this reaching the network on every build was re-downloaded on
        # every build, and the whole stage quietly depended on github.com
        # being up. One gate run failed exactly there and the reason was
        # discarded; blocking github reproduces it character for character.
        #
        # The runtime is a fixed binary for an architecture, so a cache that
        # survives between runs is the correct lifetime for it.
        cache="${XDG_CACHE_HOME:-$HOME/.cache}/opendoctrines"
        mkdir -p "$cache"
        rt="$cache/appimage-runtime-$APPIMAGE_ARCH"
        if [ ! -s "$rt" ]; then
            echo "  fetching the AppImage runtime for $APPIMAGE_ARCH"
            # --retry-connrefused and --retry-all-errors as well as --retry:
            # plain --retry does not re-attempt a refused connection or a 5xx,
            # which are the two ways this actually fails.
            curl -fsSL --retry 5 --retry-delay 2 --retry-connrefused \
                 --retry-all-errors --connect-timeout 20 -o "$rt.part" \
                "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$APPIMAGE_ARCH" \
                || die "could not fetch the AppImage runtime for $APPIMAGE_ARCH.
  It is cached at $rt once fetched, so this needs the network only once per
  machine; set OD_APPIMAGE_RUNTIME to a local copy to skip it entirely."
            mv "$rt.part" "$rt"
        fi
    fi
    [ -s "$rt" ] || die "AppImage runtime is empty: $rt"

    ARCH="$APPIMAGE_ARCH" appimagetool --no-appstream --runtime-file "$rt" \
        "$APPDIR" "$OUT/OpenDoctrines-${VERSION}-${APPIMAGE_ARCH}.AppImage"
    echo "  AppImage: OpenDoctrines-${VERSION}-${APPIMAGE_ARCH}.AppImage"
    ;;

*) die "unknown package kind: $kind" ;;
esac
done

echo "packages in $OUT:"
ls -la "$OUT" | tail -n +2
