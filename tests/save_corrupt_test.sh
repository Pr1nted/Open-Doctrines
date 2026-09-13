#!/usr/bin/env bash
# A damaged save is refused, not fatal.
#
#   tests/save_corrupt_test.sh [build-dir]
#
# WHY THIS EXISTS
#
# Reported on Windows, 1.2.0a: play a few turns, let the autosave write, close
# the application, open it again, load the world -- and the game DISAPPEARS. No
# message, no menu. The cause was one throwing call: state.json went to
# nlohmann::json::parse() with no error handling, and nothing between there and
# main() catches -- not replaySaveTurns, not LOAD_SAVE_FINALIZE (whose
# try/catch covers readMetadata and stops short of the replay), not
# updateLoading, and main.cpp has neither a top-level catch nor a
# set_terminate. A truncated state.json therefore reached std::terminate.
#
# Nothing in the suite could have caught it. SaveRoundTripTest exercises the
# ARCHIVE and links SaveManager alone -- no window, no GL, and no map -- so it
# never reaches the code that reads a save into a world. This does, through the
# dedicated server, which drives the same async loader the game does.
#
# THE TWO CORRUPTIONS ARE DIFFERENT AND BOTH MATTER
#
#   truncated archive   the zip will not open, extractODM returns nothing, and
#                       LOAD_ODM_SAVE has always reported that properly
#   valid zip, damaged  the zip opens, state.json parses -- and THAT is the one
#   state.json          that aborted the process
#
# A test that only wrote a truncated file would have passed against the bug.
#
# WHAT IS ASSERTED
#
# Not the exit code, which may reasonably change: that the process was not
# KILLED. An exit status above 128 is a signal (134 is SIGABRT, which is what
# an uncaught C++ exception becomes), and that is precisely the defect.

set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
fail=0

srv="$build/OpenDoctrinesServer"
[ -x "$srv" ] || srv="$build/Release/OpenDoctrinesServer.exe"
[ -x "$srv" ] || srv="$build/Release/OpenDoctrinesServer"
if [ ! -x "$srv" ]; then
    # The server build is reported by its own step in run_all.sh; saying it
    # twice turns one failure into two.
    echo "  no dedicated server binary under $build -- skipping"
    exit 0
fi

PYBIN="$("$root/tools/find_python.sh" 2>/dev/null || echo python3)"
work="$build/savecorrupt"
rm -rf "$work" && mkdir -p "$work"

ok()   { printf '  %-58s ok\n' "$1"; }
bad()  { printf '  %-58s FAILED\n' "$1"; fail=1; }
note() { printf '      %s\n' "$1"; }

# ── A data directory that is NOT the repository's ──
#
# The loader resolves a save as <data>/saves/<name>.odsv, so the fixtures have
# to live in a data directory -- and writing them into the shipped one would
# leave probe saves in a tree somebody else is working in. Symlinks cost
# nothing and keep every real asset reachable.
data="$work/data"
mkdir -p "$data/saves"
for entry in "$root/data"/*; do
    name="$(basename "$entry")"
    [ "$name" = "saves" ] && continue
    ln -s "$entry" "$data/$name" 2>/dev/null || true
done
if [ ! -e "$data/STDmaps/1939.odmap" ]; then
    echo "  no 1939.odmap under $root/data/STDmaps -- skipping"
    exit 0
fi

# ── The fixtures ──
#
# Built here rather than played, so this test needs no display, no turns and no
# determinism. A .odsv is a zip of map.odmap + metadata.json + index.json +
# state.json, and a REAL shipped map goes in: a fixture the loader rejects for
# some unrelated reason would prove nothing about state.json.
"$PYBIN" - "$root" "$data" <<'PY'
import json, sys, zipfile, os
root, data = sys.argv[1], sys.argv[2]
odm = open(os.path.join(root, "data", "STDmaps", "1939.odmap"), "rb").read()

meta = {
    "save_name": "CorruptProbe", "version": "test",
    "created": "2000-01-01 00:00:00", "last_played": "2000-01-01 00:00:00",
    "turn_count": 0, "province_count": 0, "ship_count": 0,
    "player_country_id": 0, "compass_convention": 2,
    "country_compasses": {}, "country_treasuries": {},
}

def write(name, state):
    p = os.path.join(data, "saves", name)
    with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(zipfile.ZipInfo("map.odmap"), odm, zipfile.ZIP_STORED)
        z.writestr("metadata.json", json.dumps(meta))
        z.writestr("index.json", '{"turns":[]}')
        z.writestr("state.json", state)
    return p

write("ProbeGood.odsv", "{}")
# Cut mid-object: what a half-written state.json looks like.
write("ProbeBadState.odsv", '{"pendingUpgrades":[{"provinceId":1,')
# A zip that never finished: the other corruption, which has always reported.
good = open(os.path.join(data, "saves", "ProbeGood.odsv"), "rb").read()
open(os.path.join(data, "saves", "ProbeTrunc.odsv"), "wb").write(good[: len(good) // 2])
print("  fixtures:", ", ".join(sorted(os.listdir(os.path.join(data, "saves")))))
PY
[ -f "$data/saves/ProbeBadState.odsv" ] || { bad "fixtures were built"; exit 1; }

cfg="$work/server.json"
"$srv" --write-config --config "$cfg" >/dev/null 2>&1

load() {  # load <save> <logfile> -> echoes the exit status
    "$srv" --config "$cfg" --data "$data" --load "$1" --check > "$2" 2>&1
    echo $?
}

# 1. THE CONTROL. Without this the rest measures nothing: if a good fixture
#    does not load, "the bad one did not load either" is not a result.
out="$work/good.log"
rc=$(load ProbeGood.odsv "$out")
if [ "$rc" -eq 0 ] && grep -q "world loaded:" "$out"; then
    ok "a sound save still loads"
    note "$(grep -o 'world loaded:.*' "$out" | head -1)"
else
    bad "a sound save still loads"
    note "exit $rc; $(tail -2 "$out")"
fi

# 2. THE REGRESSION. Exit 134 here was the shipped bug.
out="$work/badstate.log"
rc=$(load ProbeBadState.odsv "$out")
if [ "$rc" -ge 128 ]; then
    bad "a damaged state.json does not kill the process"
    note "killed by signal $((rc - 128)) -- $(grep -i 'terminating\|libc++abi' "$out" | head -1)"
elif [ "$rc" -eq 0 ]; then
    bad "a damaged state.json does not kill the process"
    note "loaded a damaged save as though it were sound"
else
    ok "a damaged state.json does not kill the process"
    note "exit $rc"
fi

# 3. AND IT SAYS SO. The report asked for this in as many words: "add error
#    message before crashing the application".
if grep -qiE "damaged|could not load the world" "$work/badstate.log"; then
    ok "and it says the save is damaged"
else
    bad "and it says the save is damaged"
    note "$(tail -3 "$work/badstate.log")"
fi

# 4. The other corruption keeps reporting the way it always did.
out="$work/trunc.log"
rc=$(load ProbeTrunc.odsv "$out")
if [ "$rc" -ge 128 ]; then
    bad "a truncated archive does not kill the process"
    note "killed by signal $((rc - 128))"
elif [ "$rc" -eq 0 ]; then
    bad "a truncated archive does not kill the process"
    note "loaded a truncated archive as though it were sound"
else
    ok "a truncated archive does not kill the process"
fi

# 5. Nothing left behind. The abort used to leak the unpacked map beside the
#    save, ~800 KB per attempt.
if ls "$data/saves"/*.tmp.odmap >/dev/null 2>&1; then
    bad "a failed load leaves no unpacked map behind"
    note "$(ls "$data/saves"/*.tmp.odmap)"
else
    ok "a failed load leaves no unpacked map behind"
fi

exit $fail
