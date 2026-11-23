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
# THE RUNNER'S OWN RELEASE, not a hardcoded one. riscv64 cross-builds on 24.04
# (noble) and the rest on 22.04 (jammy): jammy's riscv64 port never got the
# ncurses security update the amd64 and armhf ports did (libtinfo6 0.1 vs 0.3),
# and multiarch needs one version of each shared library across architectures,
# so Mesa's LLVM dependency could not be installed at all ("held broken
# packages"). Noble's riscv64 port is complete.
. /etc/os-release
codename="${VERSION_CODENAME:?no VERSION_CODENAME in /etc/os-release}"
{
    for s in "$codename" "$codename-updates" "$codename-security"; do
        echo "deb [arch=amd64,i386] $main $s main restricted universe multiverse"
        echo "deb [arch=armhf,arm64,riscv64] $ports $s main restricted universe multiverse"
    done
} | sudo tee /etc/apt/sources.list >/dev/null

sudo dpkg --add-architecture "$arch"
sudo apt-get update -qq
sudo apt-get install -y -qq \
    "g++-$triplet" pkg-config qemu-user-static \
    "libasound2-dev:$arch" "libx11-dev:$arch" "libxrandr-dev:$arch" "libxi-dev:$arch" \
    "libgl1-mesa-dev:$arch" "libglu1-mesa-dev:$arch" "libgles-dev:$arch" "libxcursor-dev:$arch" \
    "libxinerama-dev:$arch" "libwayland-dev:$arch" "libxkbcommon-dev:$arch" "libudev-dev:$arch" \
    "libc6:$arch" "libstdc++6:$arch"

# A cross-built target binary runs on this x86-64 host through qemu's binfmt
# handler. On the CI runners update-binfmts/systemd-binfmt did NOT register the
# qemu handlers (binfmt_misc held only llvm's), so riscv64 hit "Exec format
# error". The multiarch container writes the handlers straight into binfmt_misc,
# which is the mechanism docker/setup-qemu-action uses and the one that works
# here. Best-effort and non-fatal: a build-only cross job never executes a
# target binary, and the job that runs the suite fails at the first exec if this
# did not take -- so this must not fail a pure cross-compile.
qemu_name="qemu-${arch/armhf/arm}"
if command -v docker >/dev/null; then
    sudo docker run --rm --privileged multiarch/qemu-user-static \
        --reset -p yes >/dev/null 2>&1 || true
fi
if [ -e "/proc/sys/fs/binfmt_misc/$qemu_name" ]; then
    echo "cross toolchain ready: $triplet ($arch); binfmt $qemu_name registered"
else
    echo "cross toolchain ready: $triplet ($arch); binfmt $qemu_name NOT" \
         "registered (fine for a build-only job; a run job will fail at exec)"
fi
