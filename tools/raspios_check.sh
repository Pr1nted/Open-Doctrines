#!/bin/sh
# Install an OpenGL ES (Raspberry Pi OS) .deb the way a Pi owner would, and
# play it.
#
#   tools/raspios_check.sh <opendoctrines_*.deb> <arm64|armhf>
#
# Raspberry Pi OS is Debian bookworm plus Raspberry Pi's own apt repository
# (archive.raspberrypi.com), and that is what the container here is: Debian
# bookworm for the package's architecture with that repository added and
# upgraded from, so a library Raspberry Pi replaces is Raspberry Pi's version.
# armhf runs under QEMU (docker/setup-qemu-action in the workflow): an Arm64
# runner cannot run 32-bit Arm code itself.
#
# What it holds the package to, as tools/chromeos_check.sh does for ChromeOS:
#   it installs with apt, pulling every dependency it declares;
#   every library the program links resolves;
#   it links OpenGL ES (libGLESv2), not desktop GL -- the whole point of this
#     build, which a Pi's GPU (GL 3.1 / GLES 3.1) needs;
#   the installed game plays three turns of 1939, on Mesa's software GLES
#     under Xvfb.
set -eu
deb="${1:?a .deb to check}"
arch="${2:?arm64 or armhf}"
[ -f "$deb" ] || { echo "no such file: $deb"; exit 1; }
case "$arch" in
    arm64) platform=linux/arm64 ;;
    armhf) platform=linux/arm/v7 ;;
    *) echo "arch must be arm64 or armhf, not $arch"; exit 1 ;;
esac
dir=$(cd "$(dirname "$deb")" && pwd)
name=$(basename "$deb")
image="${RASPIOS_IMAGE:-mirror.gcr.io/library/debian:bookworm}"

docker run --rm --platform "$platform" -v "$dir:/pkg:ro" "$image" sh -euc '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq ca-certificates curl gnupg >/dev/null
  curl -fsSL https://archive.raspberrypi.com/debian/raspberrypi.gpg.key \
    | gpg --dearmor -o /usr/share/keyrings/raspberrypi-archive-keyring.gpg
  echo "deb [signed-by=/usr/share/keyrings/raspberrypi-archive-keyring.gpg] http://archive.raspberrypi.com/debian/ bookworm main" \
    > /etc/apt/sources.list.d/raspi.list
  apt-get update -qq
  apt-get upgrade -y -qq >/dev/null
  apt-get install -y -qq "/pkg/'"$name"'" >/dev/null
  echo "installed in Debian $(cat /etc/debian_version) + archive.raspberrypi.com ($(dpkg --print-architecture))"

  exe=$(dpkg -L opendoctrines | grep -E "/OpenDoctrines$" | head -1)
  [ -n "$exe" ] && [ -x "$exe" ] || { echo "no OpenDoctrines executable in the package"; exit 1; }
  missing=$(ldd "$exe" | grep "not found" || true)
  if [ -n "$missing" ]; then
    echo "libraries the program needs and Raspberry Pi OS did not provide:"
    echo "$missing"
    exit 1
  fi
  echo "every library resolves: $exe"
  ldd "$exe" | grep -q "libGLESv2" || { echo "it does not link OpenGL ES"; exit 1; }
  if ldd "$exe" | grep -qE "libOpenGL\.so|libGL\.so"; then
    echo "it links desktop GL, which a Pi cannot give it"; exit 1
  fi
  echo "links OpenGL ES, not desktop GL"

  map=$(dpkg -L opendoctrines | grep -E "/STDmaps/1939\.odmap$" | head -1)
  [ -n "$map" ] || { echo "the package has no 1939 scenario"; exit 1; }
  # Launched as the desktop menu launches it, through the entry the package installs.
  desktop=$(dpkg -L opendoctrines | grep -E "^/usr/share/applications/.*\.desktop$" | head -1)
  [ -n "$desktop" ] || { echo "no desktop entry -- the Pi menu would list nothing"; exit 1; }
  cmd=$(sed -n "s/^Exec=\([^ ]*\).*/\1/p" "$desktop" | head -1)
  command -v "$cmd" >/dev/null || [ -x "$cmd" ] || { echo "desktop entry runs $cmd, which is not installed"; exit 1; }
  echo "launcher entry: $desktop -> $cmd"
  apt-get install -y -qq xvfb xauth libgl1-mesa-dri libgles2 libegl1 >/dev/null
  mkdir -p /tmp/play && cd /tmp/play
  set +e
  HOME=/tmp/play xvfb-run -a -s "-screen 0 1280x800x24" \
    "$cmd" --simulate "$map" 3 RaspiosCheck > play.log 2>&1
  rc=$?
  set -e
  grep -aE "^\[SIM\]" play.log || true
  if [ "$rc" -ne 0 ] || ! grep -qa "^\[SIM\] turn 3/3" play.log; then
    echo "the installed game did not play three turns (exit $rc):"
    tail -30 play.log
    exit 1
  fi
  echo "played three turns of 1939 from the installed package"
'
