#!/usr/bin/env bash
# Install what cross-compiling the game for another Linux architecture needs, on
# an Ubuntu 22.04 x86-64 CI runner.
#
#   tools/ci_cross_deps.sh <debian-arch> <gcc-triplet>
#   tools/ci_cross_deps.sh armhf arm-linux-gnueabihf
#
# One script, used by both release-game.yml and test.yml, because the apt dance
# below is easy to get subtly wrong and two copies would drift.
#
# MULTIARCH: the foreign architecture's -dev packages install beside the host's
# (headers in /usr/include, libraries in /usr/lib/<triplet>), and the cross gcc
# finds them. tools/cmake/linux-<arch>.cmake points CMake and pkg-config there.
#
# Two things about apt make this more than `dpkg --add-architecture`:
#
#   - armhf and riscv64 are not on archive.ubuntu.com but on ports.ubuntu.com,
#     so every source has to say which architectures it serves, or apt asks each
#     mirror for indexes it does not have.
#   - the runner image ships third-party lists (Microsoft, PPAs) with no
#     architecture restriction, and asking them for riscv64 is a 404 that fails
#     `apt-get update` outright. A build needs none of them, so they are moved
#     aside rather than patched one by one.
#
# qemu-user-static registers binfmt handlers, so anything built for the target
# -- a test, or a tool the build runs -- executes transparently.
set -euo pipefail

arch="${1:?debian architecture, e.g. armhf}"
triplet="${2:?gcc triplet, e.g. arm-linux-gnueabihf}"

if [ -d /etc/apt/sources.list.d ] && [ ! -d /etc/apt/sources.list.d.off ]; then
    sudo mv /etc/apt/sources.list.d /etc/apt/sources.list.d.off
    sudo mkdir /etc/apt/sources.list.d
fi
main=http://azure.archive.ubuntu.com/ubuntu
ports=http://ports.ubuntu.com/ubuntu-ports
{
    for s in jammy jammy-updates jammy-security; do
        echo "deb [arch=amd64,i386] $main $s main restricted universe multiverse"
        echo "deb [arch=armhf,arm64,riscv64] $ports $s main restricted universe multiverse"
    done
} | sudo tee /etc/apt/sources.list >/dev/null

sudo dpkg --add-architecture "$arch"
sudo apt-get update -qq
sudo apt-get install -y -qq \
    "g++-$triplet" pkg-config qemu-user-static \
    "libasound2-dev:$arch" "libx11-dev:$arch" "libxrandr-dev:$arch" "libxi-dev:$arch" \
    "libgl1-mesa-dev:$arch" "libglu1-mesa-dev:$arch" "libxcursor-dev:$arch" \
    "libxinerama-dev:$arch" "libwayland-dev:$arch" "libxkbcommon-dev:$arch" \
    "libc6:$arch" "libstdc++6:$arch"

# 32-bit Arm on an x86-64 host is always emulated, but make sure the handler is
# on: Debian-family systems skip registering qemu-arm on hosts they believe can
# run armhf natively, and an arm64 guest without AArch32 (Apple Silicon) cannot.
if command -v update-binfmts >/dev/null; then
    sudo update-binfmts --enable "qemu-${arch/armhf/arm}" 2>/dev/null || true
fi
echo "cross toolchain ready: $triplet ($arch)"
