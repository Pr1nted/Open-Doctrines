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

# 6. Without --check it gets all the way to the credential gate.
#    THE POINT OF THIS CASE: every check above stops before hosting, so they
#    would all pass on a server that could never host at all. This asserts the
#    last thing standing between it and a live session is the account, and not
#    something earlier that a non-zero exit would have hidden.
out="$work/nocreds.log"
"$srv" --config "$cfg" --data "$root/data/" --no-tunnel --port 0 < /dev/null \
       > "$out" 2>&1
if grep -qi "sign in and register" "$out"; then
    ok "a real run reaches the account gate and stops there"
else
    if grep -q "session open" "$out"; then
        # Somebody ran this on a machine with credentials. Not a failure --
        # it is a stronger result than the test was asking for.
        ok "a real run opened a session (this machine has credentials)"
    else
        bad "a real run reaches the account gate and stops there"
        note "$(tail -3 "$out")"
    fi
fi

# 7. The console dispatches commands and `stop` ends the process. Driven
#    through stdin exactly as an operator would, which is also the only way to
#    prove the reader thread and the loop are talking to each other.
out="$work/console.log"
printf 'help\nstatus\nstop\n' | "$srv" --config "$cfg" --data "$root/data/" \
    --no-tunnel --port 0 > "$out" 2>&1
if grep -q "step-go" "$out"; then ok "the console prints its command list"
else bad "the console prints its command list"; fi

echo
if [ "$fail" -eq 0 ]; then echo "dedicated server: all ok"; else echo "dedicated server: FAILED"; fi
exit $fail
