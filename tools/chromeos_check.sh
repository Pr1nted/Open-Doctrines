#!/bin/sh
# Install a .deb the way ChromeOS does, and check it would run there.
#
#   tools/chromeos_check.sh <path/to/opendoctrines_X_ARCH.deb>
#
# ChromeOS runs Linux apps in its Linux development environment ("Crostini"):
# a Debian 12 (bookworm) container, x86-64 or arm64 to match the Chromebook.
# Double-clicking a .deb in the Files app runs apt on it inside that container.
# So the honest test of a ChromeOS build is that same apt, in that same
# Debian, on a machine of the same architecture -- run here in a debian:bookworm
# container on a runner of the package's own architecture.
#
# It proves the package installs with nothing but Debian's archive behind it
# (every Depends: resolvable in bookworm), that the installed program finds
# every shared library it links, and that the launcher entry ChromeOS lists in
# its app grid is there. It does not open a window: there is no display here,
# and on a Chromebook the GPU is whatever ChromeOS hands the container, which
# the game's software-GL fallback already covers (src/util/SoftwareGlRelaunch).
set -eu

deb="${1:?a .deb to check}"
[ -f "$deb" ] || { echo "no such file: $deb"; exit 1; }
dir=$(cd "$(dirname "$deb")" && pwd)
name=$(basename "$deb")
# The official Debian image, from Google's mirror of Docker Hub rather than
# Docker Hub itself, whose anonymous pull limit answers 429 often enough on
# shared runners to fail a release for nothing. Same image, same digests.
image="${CHROMEOS_IMAGE:-mirror.gcr.io/library/debian:bookworm}"

docker run --rm -v "$dir:/pkg:ro" "$image" sh -euc '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  # The Files-app install: apt on the file, dependencies from the archive.
  apt-get install -y -qq "/pkg/'"$name"'" >/dev/null
  echo "installed in Debian $(cat /etc/debian_version) ($(dpkg --print-architecture))"

  exe=$(dpkg -L opendoctrines | grep -E "/OpenDoctrines$" | head -1)
  [ -n "$exe" ] && [ -x "$exe" ] || { echo "no OpenDoctrines executable in the package"; exit 1; }
  missing=$(ldd "$exe" | grep "not found" || true)
  if [ -n "$missing" ]; then
    echo "libraries the program needs and bookworm did not provide:"
    echo "$missing"
    exit 1
  fi
  echo "every library resolves: $exe"

  # What ChromeOS puts in the launcher: a .desktop file under
  # /usr/share/applications that names a command that exists.
  desktop=$(dpkg -L opendoctrines | grep -E "^/usr/share/applications/.*\.desktop$" | head -1)
  [ -n "$desktop" ] || { echo "no desktop entry -- ChromeOS would list nothing"; exit 1; }
  cmd=$(sed -n "s/^Exec=\([^ ]*\).*/\1/p" "$desktop" | head -1)
  command -v "$cmd" >/dev/null || [ -x "$cmd" ] || { echo "desktop entry runs $cmd, which is not installed"; exit 1; }
  echo "launcher entry: $desktop -> $cmd"

  # AND IT PLAYS. Three turns of 1939 from the installed copy, in a window,
  # on Mesa software GL under Xvfb -- what a Chromebook without GPU
  # acceleration for Linux gives the container. Found from the package list,
  # not assumed, so a data folder that moved is a failure here.
  map=$(dpkg -L opendoctrines | grep -E "/STDmaps/1939\.odmap$" | head -1)
  [ -n "$map" ] || { echo "the package has no 1939 scenario"; exit 1; }
  apt-get install -y -qq xvfb xauth libgl1-mesa-dri >/dev/null
  mkdir -p /tmp/play && cd /tmp/play
  set +e
  HOME=/tmp/play LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a -s "-screen 0 1280x800x24" \
    "$cmd" --simulate "$map" 3 ChromeOSCheck > play.log 2>&1
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
