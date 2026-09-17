#!/usr/bin/env bash
# The dedicated server actually starts, on this platform, with this content.
#
#   tests/server_smoke_test.sh [build-dir]
#
# WHY THIS EXISTS
#
# The server is a SECOND BINARY built from the same sources with a different
# raylib underneath it (src/server/ServerRaylib.cpp). Nothing about the client
# building proves the server does, and nothing about the server LINKING proves
# it can find its data, resolve a map or load a world -- the first run found
# exactly those two faults, and both reported themselves as "no map called
# '1914'", which reads like a missing file rather than a missing path.
#
# WHY IT STOPS SHORT OF HOSTING
#
# Opening a session needs a signed-in account and a registered server
# credential, by design: the host verifies every join ticket against the account
# service's key, so there is no anonymous hosting to fall back on. CI has no
# account and should not have one. So the test drives everything up to that
# point -- config, data directory, map resolution, the whole async world load --
# with `--check`, and then separately asserts that a real run gets as far as the
# credential check and fails THERE. A server that fell over earlier would pass
# a test that only looked for a non-zero exit.

set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
fail=0

srv="$build/OpenDoctrinesServer"
[ -x "$srv" ] || srv="$build/Release/OpenDoctrinesServer.exe"
[ -x "$srv" ] || srv="$build/Release/OpenDoctrinesServer"
if [ ! -x "$srv" ]; then
    echo "no dedicated server binary under $build -- build the OpenDoctrinesServer target"
    exit 1
fi

PYBIN="$("$root/tools/find_python.sh" 2>/dev/null || echo python3)"
work="$build/servertest"
rm -rf "$work" && mkdir -p "$work"

ok()   { printf '  %-58s ok\n' "$1"; }
bad()  { printf '  %-58s FAILED\n' "$1"; fail=1; }
note() { printf '      %s\n' "$1"; }

# ── RUNNING A SERVER THAT MIGHT NOT STOP ──
#
# A real run (no --check) either hits the account gate and exits, or opens a
# session and runs forever. This test ran one with stdin at /dev/null and
# waited for it, so on a machine WITH credentials the whole suite parked here
# -- for 30 minutes in the run that found this, and for 6 days 23 hours in two
# older runs still sitting on the same line. The case even had a branch for
# the credentialed outcome; it was just never reached.
#
# So: `stop` on stdin, the way step 7 already drives the console, and a
# bounded wait so an unexpected hang FAILS instead of parking. There is no
# timeout(1) on macOS, hence the loop.
#
# Returns 124 on timeout, matching timeout(1), so a hang is distinguishable
# from a server that exited by itself.
run_bounded() {                       # run_bounded <seconds> <log> <cmd...>
    local secs="$1" log="$2"; shift 2
    printf 'stop\n' | "$@" > "$log" 2>&1 &
    local pid=$! waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$secs" ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        return 124
    fi
    wait "$pid" 2>/dev/null
}

# ── WHAT A REAL RUN'S LOG MEANS ──
#
# Three outcomes, and only ONE of them is reachable on any given machine:
# `gate` needs a server that is not signed in, `session` needs one that is.
# So the reading is a function, and the self-check below drives all three from
# captured text -- otherwise the credentialed branch is code that no run on an
# unsigned machine has ever executed, which is how it came to be unreachable
# in the first place.
verdict_of() {                        # verdict_of <log> -> gate|session|broken
    if grep -qi "sign in and register" "$1"; then echo gate
    elif grep -q "session open" "$1";     then echo session
    else echo broken; fi
}

# Every save a run of this test caused the server to create, removed. A real
# dedicated server should keep its world; a test should not leave 71 of them
# in data/saves/, which is what 23 MB of "Dedicated (N).odsv" in this tree
# was. Parsed from the log rather than guessed, so it can only ever delete
# files this script's own run announced.
drop_saves_named_in() {               # drop_saves_named_in <log>
    [ -f "$1" ] || return 0
    local line path
    while IFS= read -r line; do
        path="${line#Auto-created save: }"
        [ "$path" = "$line" ] && continue
        case "$path" in "$root"/data/saves/*.odsv) ;; *) continue;; esac
        rm -f "$path" "$path.odkey" "$path.odhost"
    done < "$1"
}

# 1. It runs at all, and says what it is.
if "$srv" --help 2>&1 | grep -q "dedicated server"; then ok "--help runs and names itself"
else bad "--help runs and names itself"; fi

# 2. A default config is written and is valid JSON-with-comments that the
#    server itself reads back. Round-tripping matters more than the contents:
#    a file the server writes and then refuses is the worst possible first run.
cfg="$work/server.json"
"$srv" --write-config --config "$cfg" >/dev/null 2>&1
if [ -s "$cfg" ]; then ok "--write-config writes a file"; else bad "--write-config writes a file"; fi
for key in '"map"' '"port"' '"max-players"' '"tunnel"' '"auto.start-at-players"'; do
    grep -q "$key" "$cfg" || bad "default config contains $key"
done
grep -q '"map"' "$cfg" && ok "default config has the settings it documents"

# The map id must round-trip as TEXT. It is all digits, and a writer that
# guessed the type from the value wrote it as the number 1914 -- which happens
# to work, and would turn a map called 007 into 7.
if grep -q '"map": *"' "$cfg"; then ok "the map id is quoted, not written as a number"
else bad "the map id is quoted, not written as a number"; fi

# 3. A BROKEN config is refused, not silently ignored. Falling back to defaults
#    here would hide a typo in a setting somebody thought they had changed.
broken="$work/broken.json"
printf '{ "max-players": "not a number" }\n' > "$broken"
if "$srv" --config "$broken" --check --data "$root/data/" >/dev/null 2>&1; then
    bad "a bad setting is refused rather than ignored"
else
    ok "a bad setting is refused rather than ignored"
fi

# 4. The whole content chain: data directory, map resolution, world load.
#    Every shipped map, because a map that fails to load on a server is a map
#    nobody can host, and the failure would only surface when somebody tried.
for map in 1914 1939 map; do
    out="$work/check-$map.log"
    if "$srv" --config "$cfg" --data "$root/data/" --map "$map" --check > "$out" 2>&1; then
        if grep -q "world loaded:" "$out"; then
            ok "--check loads $map"
            note "$(grep -o 'world loaded:.*' "$out" | head -1)"
        else
            bad "--check loads $map"
            note "exited 0 but never reported a world"
        fi
    else
        bad "--check loads $map"
        note "$(tail -2 "$out")"
    fi
done

# 5. A map that does not exist fails clearly rather than hanging or crashing.
if "$srv" --config "$cfg" --data "$root/data/" --map "no-such-map" --check \
        > "$work/badmap.log" 2>&1; then
    bad "an unknown map is refused"
else
    grep -qi "no map called" "$work/badmap.log" \
        && ok "an unknown map is refused, by name" \
        || bad "an unknown map is refused, by name"
fi

# 5b. A required mod the server does not have is refused AT STARTUP.
#     A server cannot require a mod it lacks -- it could never satisfy its own
#     check -- so the failure belongs here and not in a rejection message sent
#     to every player who tries to join.
modcfg="$work/mods.json"
"$srv" --write-config --config "$modcfg" >/dev/null 2>&1
"$PYBIN" - "$modcfg" <<'EOF' 2>/dev/null || sed -i.bak 's/"mods": *"[^"]*"/"mods": "no-such-mod"/' "$modcfg"
import re, sys, pathlib
p = pathlib.Path(sys.argv[1]); t = p.read_text()
p.write_text(re.sub(r'"mods": *"[^"]*"', '"mods": "no-such-mod"', t))
EOF
if "$srv" --config "$modcfg" --data "$root/data/" --check > "$work/mods.log" 2>&1; then
    bad "a required mod that is not installed refuses at startup"
else
    grep -qi "requires 'no-such-mod'" "$work/mods.log" \
        && ok "a required mod that is not installed refuses at startup" \
        || bad "a required mod that is not installed refuses at startup"
fi

# 5c. THIS SCRIPT'S OWN READING OF STEP 6, on all three outcomes.
#
#     Step 6 can only ever take one of its three branches on a given machine,
#     because which one it takes depends on whether the server is signed in.
#     That is exactly how its credentialed branch came to be unreachable and
#     stay unreachable. These three lines are captured server output, so both
#     branches are exercised wherever this runs.
sc="$work/selfcheck"
mkdir -p "$sc"
printf '%s\n' "[15:56:12 ERROR] could not open the session: Sign in and register this server before hosting." > "$sc/gate.log"
printf '%s\n' "[03:22:36 INFO] session open. Join code: B3VT-JQ8C" > "$sc/session.log"
printf '%s\n' "  Loaded 1247 provinces, 55 countries, 53 playable" > "$sc/broken.log"
[ "$(verdict_of "$sc/gate.log")" = gate ] \
    && ok "the account gate is read as the gate" \
    || bad "the account gate is read as the gate"
[ "$(verdict_of "$sc/session.log")" = session ] \
    && ok "an opened session is read as an opened session" \
    || bad "an opened session is read as an opened session"
# THE ONE THAT MATTERS: a run that got neither must not be read as either.
# Falling over before the gate is the fault this whole step exists to catch,
# and it looks like silence, which is the easiest thing to mistake for a pass.
[ "$(verdict_of "$sc/broken.log")" = broken ] \
    && ok "a run that reached neither is read as broken" \
    || bad "a run that reached neither is read as broken"

# 5d. AND THE BOUNDED WAIT ITSELF, which on a healthy machine never fires.
#
#     The whole fault this replaced was an unreachable branch. `sleep` does
#     not read stdin and does not exit on its own, so this is a server that
#     ignores `stop` -- the case that parked three suite runs, one of them for
#     6 days 23 hours.
run_bounded 2 "$sc/hang.log" sleep 30
if [ "$?" -eq 124 ]; then ok "a run that will not stop is cut off, not waited on"
else bad "a run that will not stop is cut off, not waited on"; fi

# 6. Without --check it gets all the way to the credential gate.
#    THE POINT OF THIS CASE: every check above stops before hosting, so they
#    would all pass on a server that could never host at all. This asserts the
#    last thing standing between it and a live session is the account, and not
#    something earlier that a non-zero exit would have hidden.
out="$work/nocreds.log"
run_bounded 90 "$out" "$srv" --config "$cfg" --data "$root/data/" \
            --no-tunnel --port 0
rc=$?
case "$(verdict_of "$out")" in
  gate)
    ok "a real run reaches the account gate and stops there";;
  session)
    # A machine with credentials. Not a failure -- it is a stronger result
    # than the case asks for: everything before hosting worked.
    ok "a real run opened a session (this machine has credentials)"
    # ...but it must still have STOPPED, or this is the hang again wearing
    # a pass. `stop` went in on stdin; a server that ignored it is a bug in
    # the console, not an inconvenience for the suite.
    if [ "$rc" -eq 124 ]; then
        bad "and stops when told to"
        note "still running after 90s; killed"
    else
        ok "and stops when told to"
    fi;;
  *)
    bad "a real run reaches the account gate and stops there"
    [ "$rc" -eq 124 ] && note "timed out after 90s without reaching either"
    note "$(tail -3 "$out")";;
esac
drop_saves_named_in "$out"

# 7. The console dispatches commands and `stop` ends the process. Driven
#    through stdin exactly as an operator would, which is also the only way to
#    prove the reader thread and the loop are talking to each other.
out="$work/console.log"
printf 'help\nstatus\nstop\n' | "$srv" --config "$cfg" --data "$root/data/" \
    --no-tunnel --port 0 > "$out" 2>&1
if grep -q "step-go" "$out"; then ok "the console prints its command list"
else bad "the console prints its command list"; fi
drop_saves_named_in "$out"

echo
if [ "$fail" -eq 0 ]; then echo "dedicated server: all ok"; else echo "dedicated server: FAILED"; fi
exit $fail
