#!/bin/sh
# Build, run and package the game inside a BSD VM -- the `run` of every job in
# .github/workflows/bsd-game.yml.
#
#   tools/bsd_game_build.sh <artifact> <expected ELF machine> [extra cmake args...]
#   tools/bsd_game_build.sh OpenDoctrines-netbsd-arm64 aarch64 -DOD_ENABLE_MODS=OFF
#
# One script for FreeBSD, OpenBSD and NetBSD on every architecture, so the VM
# steps in that workflow differ only in how each system installs packages. It
# was three copies of this inline, and the notes below had to be repeated in
# each to stay true. Plain sh: it is what all three systems run.
#
# OD_ACHIEVEMENT_KEYS comes from the environment (a repository variable; the
# VM does not inherit the runner's environment, so the workflow passes it on
# the command line).
set -eu

artifact="${1:?artifact name, e.g. OpenDoctrines-freebsd-amd64}"
want="${2:?expected machine, e.g. x86_64}"
shift 2

uname -a
jobs=$(getconf NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOD_ACCOUNT_ISSUER=https://opendoctrines-net.opendoctrines.workers.dev \
  "-DOD_ACHIEVEMENT_KEYS=${OD_ACHIEVEMENT_KEYS:-}" \
  -DOD_DISCORD_APP_ID=1547303703370014830 \
  "$@"
cmake --build build --target OpenDoctrines -j "$jobs"

# The binary is the architecture this job is named for. Under emulation, an
# image of the wrong architecture booting quietly would publish an x86 build
# under an Arm name, and nothing after this would notice.
python3 - build/OpenDoctrines "$want" <<'EOF'
import sys
path, want = sys.argv[1], sys.argv[2]
elf = {62: "x86_64", 183: "aarch64"}
b = open(path, "rb").read(64)
got = elf.get(int.from_bytes(b[18:20], "little")) if b[:4] == b"\x7fELF" else None
print(f"{path}: {got} (want {want})")
sys.exit(0 if got == want else 1)
EOF

# AND THE HEADLESS SERVER, so something can actually be RUN here.
# The game itself needs an OpenGL window and there is no display in a VM on a
# CI runner, so a build that is only ever compiled is all a BSD job could
# otherwise claim. The server is the same engine with raylib stubbed out:
# --check loads a shipped map, builds a world and exits, which exercises the
# map reader, the save layer, the AI tables and the text layer odseal feeds.
cmake --build build --target OpenDoctrinesServer -j "$jobs"
# < /dev/null IS LOAD-BEARING, and this cost two runs to see.
#
# The dedicated server has an interactive console that reads stdin, and
# vmactions feeds this whole script to the remote shell ON stdin. So the server
# sat there and ATE THE REST OF THE SCRIPT: packaging never ran, the shell
# reached end-of-input with nothing left to do, and the step exited 0. The
# failure surfaced two steps later as "does not carry the account service" --
# a complaint about a directory that was never created. Running this as a file
# rather than an inline block does not change that: the server still inherits
# whatever stdin the shell had.
./build/OpenDoctrinesServer --map 1939 --no-tunnel --check < /dev/null

# Echoed either side, because this step once did not run at all and nothing
# said so: the job went green here and failed in the NEXT step with "does not
# carry the account service", which is a report about a directory that was
# never created.
echo "=== packaging ==="
python3 tools/package.py --binary build/OpenDoctrines --out "dist/$artifact" --zip
echo "=== packaged ==="
ls -lh dist/
test -d "dist/$artifact" || { echo "no packaged tree"; exit 1; }
