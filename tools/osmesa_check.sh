#!/usr/bin/env bash
# Can this machine render OpenGL with no display at all? Compiles and runs
# tools/osmesa_check.c against OSMesa (Mesa's offscreen software rasteriser).
#
#   tools/osmesa_check.sh
#
# WHY A SKIP IS NOT A PASS HERE
#
# OSMesa is a separate package (libosmesa6-dev on Debian, mesa-libOSMesa on
# FreeBSD, etc.). On a developer's machine that does not have it, this SKIPS --
# there is nothing to test. In CI, where the environment is ours to set up and
# the whole point is to prove software GL works, a missing OSMesa is a broken
# runner, not an absent feature: set OD_OSMESA_REQUIRED=1 and the skip becomes
# a failure. CI sets it; a person running this by hand does not.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/tools/osmesa_check.c"
required="${OD_OSMESA_REQUIRED:-0}"

skip() {   # skip <reason>
    if [ "$required" = "1" ]; then
        echo "FAIL  OSMesa check: $1 (OD_OSMESA_REQUIRED=1)"; exit 1
    fi
    echo "skip  OSMesa check: $1"; exit 0
}

cc="${CC:-}"
[ -n "$cc" ] || for c in cc gcc clang; do command -v "$c" >/dev/null && { cc="$c"; break; }; done
[ -n "$cc" ] || skip "no C compiler"

# The header is the cheap thing to probe for; the lib comes with it.
have_header=0
for d in /usr/include /usr/local/include /opt/local/include; do
    [ -f "$d/GL/osmesa.h" ] && have_header=1 && break
done
[ "$have_header" = 1 ] || skip "no GL/osmesa.h (install libosmesa6-dev / mesa-libOSMesa)"

bin="$(mktemp -d)/osmesa_check"
# -lOSMesa pulls GL transitively on most Mesa builds; -lGL named too for the
# ones where it does not.
if ! "$cc" "$src" -lOSMesa -lGL -o "$bin" 2>/tmp/osmesa_cc.log; then
    if ! "$cc" "$src" -lOSMesa -o "$bin" 2>>/tmp/osmesa_cc.log; then
        cat /tmp/osmesa_cc.log >&2; skip "would not link against OSMesa"
    fi
fi

# No DISPLAY on purpose: the entire point is that this needs none.
env -u DISPLAY "$bin"
rc=$?
[ "$rc" -eq 0 ] && echo "OSMesa check: ok -- offscreen software GL works here"
exit "$rc"
