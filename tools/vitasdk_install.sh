#!/usr/bin/env bash
# Install the pinned VitaSDK (the PlayStation Vita homebrew toolchain).
#
#     tools/vitasdk_install.sh [prefix]        # default: $HOME/vitasdk
#
# Then:  export VITASDK=<prefix>; export PATH="$VITASDK/bin:$PATH"
#
# PINNED, NOT "LATEST". VitaSDK publishes immutable snapshots and a channel
# manifest naming the one a release series is built on; this takes the core of
# the stable 2026.08 series (vitasdk/autobuilds, channels.json) by its tag and
# checks the archive against the hash that release published, so a CI run
# today and one next year build with the same compiler and the same libc. The
# bootstrap archive is the whole toolchain -- gcc, newlib, the Sce stubs and
# vita-elf-create / vita-make-fself / vita-mksfoex -- and nothing more is
# installed: SDL3 is built from source by the game's own CMakeLists.txt, as
# on Linux and Windows.
#
# Moving the pin means changing both lines below together.
set -euo pipefail

TAG=sdk-snapshot-20260825.611.1
SHA256=3d5be8ad0fae5de872c13328f3508a59b9e1eec504269eec64e47d06de78b098

prefix=${1:-$HOME/vitasdk}
url="https://github.com/vitasdk/autobuilds/releases/download/$TAG/vitasdk-bootstrap-x86_64-linux-gnu.tar.bz2"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "VitaSDK $TAG -> $prefix"
curl -fsSL --retry 3 -o "$tmp/vitasdk.tar.bz2" "$url"
echo "$SHA256  $tmp/vitasdk.tar.bz2" | sha256sum -c -

# The archive's top directory is vitasdk/.
mkdir -p "$prefix"
tar -xjf "$tmp/vitasdk.tar.bz2" -C "$tmp"
cp -a "$tmp/vitasdk/." "$prefix/"
"$prefix/bin/arm-vita-eabi-gcc" --version | head -1
