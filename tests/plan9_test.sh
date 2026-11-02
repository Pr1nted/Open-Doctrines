#!/bin/bash
# Does the Plan 9 edition play the game the FreeRTOS edition plays?
#
#   tests/plan9_test.sh              on this machine: needs only cc
#   tests/plan9_test.sh --9front ISO  and inside 9front, under QEMU
#
# On this machine, without Plan 9, two things are checked:
#
#   1. The shared core is written in the C that 9front's compilers take: no
#      declaration after a statement, none in a for, no compound literal, no
#      inline. gcc in gnu89 mode refuses each of those, so it stands in for
#      6c here.
#   2. The core prints the same game through plan9/od9fmt.c -- the printf the
#      Plan 9 build uses instead of a libc's -- as through the host's own:
#      tests/freertos/play.txt, byte for byte against play.expected.
#
# With --9front, tools/plan9_9front.py boots the ISO in QEMU, builds plan9/
# there with mk and 6c, plays the same script, and the transcript it brings
# back is held to the same file. That is what CI runs (.github/workflows/plan9.yml).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
PY="$("$root/tools/find_python.sh" 2>/dev/null || echo python3)"
command -v cc >/dev/null 2>&1 || { echo "skip: no cc"; exit 0; }
out="$root/build/plan9"
mkdir -p "$out"
fail=0
exp="$root/tests/freertos/play.expected"

"$PY" "$root/tools/plan9_package.py" --data "$out/gen" > /dev/null || exit 1

if cc -std=gnu89 -Wdeclaration-after-statement -Werror -fsyntax-only \
        -I"$root/freertos/core" -I"$root/templeos" \
        "$root/freertos/core/od_world.c" "$root/freertos/core/od_rules.c" \
        "$root/freertos/core/od_shell.c" > "$out/c89.log" 2>&1; then
    echo "ok    core: C that kencc compiles (gnu89, no declarations after statements)"
else
    echo "FAILED: the core uses C that 9front's compilers do not take:"
    head -20 "$out/c89.log"
    fail=1
fi

# od_host.c's main, with the core's vsnprintf/snprintf/sprintf replaced by od9fmt's.
fmt=(-Dvsnprintf=od9_vsnprintf -Dsnprintf=od9_snprintf -Dsprintf=od9_sprintf -U_FORTIFY_SOURCE)
# The transcript driver (tools/freertos_play.py) waits on the program's pipe
# with select(), which Windows only offers for sockets -- the same reason
# tests/freertos_test.sh skips there. The compile check above still runs.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) posix_host=0 ;;
    *) posix_host=1 ;;
esac
if [ "$posix_host" = 0 ]; then
    echo "skip  od9fmt: the transcript driver needs a POSIX host"
elif cc -std=c99 -O2 -Wall -Wextra -Werror -include "$root/plan9/od9fmt.h" "${fmt[@]}" \
        -I"$root/freertos/core" -I"$root/templeos" -I"$root/plan9" \
        "$root/freertos/core/od_world.c" "$root/freertos/core/od_rules.c" \
        "$root/freertos/core/od_shell.c" "$root/freertos/host/od_host.c" \
        "$root/plan9/od9fmt.c" "$out/gen/od_world_data.c" -o "$out/od_fmt_host" > "$out/fmt.log" 2>&1; then
    "$PY" "$root/tools/freertos_play.py" "$root/tests/freertos/play.txt" -- "$out/od_fmt_host" > "$out/fmt.txt"
    if diff -u "$exp" "$out/fmt.txt" > "$out/fmt.diff"; then
        echo "ok    od9fmt: $(grep -c '^> ' "$out/fmt.txt") commands, transcript as expected"
    else
        echo "FAILED: the core prints a different game through od9fmt:"
        head -40 "$out/fmt.diff"
        fail=1
    fi
else
    echo "FAILED: the core with od9fmt does not build:"
    head -20 "$out/fmt.log"
    fail=1
fi

if [ "${1:-}" = --9front ]; then
    iso="${2:?--9front needs the 9front ISO}"
    if "$PY" "$root/tools/plan9_9front.py" --iso "$iso" --work "$out/guest" \
            --transcript "$out/9front.txt"; then
        if diff -u "$exp" "$out/9front.txt" > "$out/9front.diff"; then
            echo "ok    9front: $(grep -c '^> ' "$out/9front.txt") commands, transcript as expected"
        else
            echo "FAILED: 9front played a different game:"
            head -40 "$out/9front.diff"
            fail=1
        fi
    else
        echo "FAILED: the 9front guest did not bring back a transcript (log: $out/guest/serial.log)"
        tail -30 "$out/guest/serial.log" 2>/dev/null
        fail=1
    fi
fi
exit $fail
