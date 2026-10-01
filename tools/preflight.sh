#!/usr/bin/env bash
# Everything that has to be true before a tag, run here rather than in CI.
#
#   tools/preflight.sh                 the lot
#   tools/preflight.sh --fast          skip the guests (suite + qualify only)
#   tools/preflight.sh --only guests   one stage
#   tools/preflight.sh --list          what the stages are
#
# WHY THIS EXISTS WHEN THERE IS ALREADY CI
#
# CI runs on the machine that built the thing. That is the condition every
# packaging bug hides in: the libraries are already installed, the paths are
# the build's own, and the artifact is never carried anywhere. Everything that
# has actually gone wrong lately was invisible to it --
#
#   the Windows installer, which nothing had ever run until a real Windows
#   machine ran it and found two bugs in an afternoon;
#   the .deb's dependency names, which only a machine without those libraries
#   can check;
#   the launcher's prefix, which only an install can exercise;
#   the save-durability test, which passed everywhere and had never once run
#   on Windows;
#   and the arm64 Linux build, which stops at configure for want of a sealed
#   archive and which CI would have failed on the tag itself.
#
# So: a gate that runs on THIS machine, against guests that are not the build
# machine, before anything is pushed.
#
# WHAT IT DOES NOT DO. It is not a replacement for CI and must not become one.
# CI is the thing that runs for other people, on hardware nobody here owns,
# every push. This is the thing that runs before you ask it to.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)/.."
ROOT="$(cd "$ROOT" && pwd)"
GUEST="$ROOT/tools/qemu_guest.sh"
LOGDIR="${OD_PREFLIGHT_LOGS:-$ROOT/build/preflight}"

# EVERY RUN IS KEPT, under its own directory, because the question a pipeline
# gets asked most is "did this used to work" and a single overwritten log
# cannot answer it. tools/preflight_dashboard.py reads these.
RUNID="$(date +%Y%m%d-%H%M%S)"
RUNDIR="$LOGDIR/runs/$RUNID"
mkdir -p "$RUNDIR"
ln -sfn "$RUNDIR" "$LOGDIR/latest"

# TSV, written from bash, parsed by python. Bash cannot be trusted to emit
# valid JSON -- one quote or backslash in a stage description and the
# dashboard is reading a syntax error -- so the shell writes the dumbest
# possible format and the reader does the escaping.
meta() { printf '%s\t%s\n' "$1" "$2" >> "$RUNDIR/meta.tsv"; }
meta id        "$RUNID"
meta commit    "$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null)"
meta subject   "$(cd "$ROOT" && git log -1 --format=%s 2>/dev/null | tr '\t' ' ')"
meta branch    "$(cd "$ROOT" && git rev-parse --abbrev-ref HEAD 2>/dev/null)"
meta host      "$(uname -sm)"
meta started   "$(date +%s)"
meta status    running

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
note()  { printf '  %s\n' "$*"; }

STAGES=(suite qualify packages guests multiplayer)

# CROSS-PLAY NEEDS NO ACCOUNT, which was not obvious and nearly cost one.
#
# NetHost::open() wants an issuer, a session token and a serverCredential, so
# the first plan here was a throwaway account whose token would be copied into
# every guest -- credentials sitting in VM disk images, for a test.
#
# None of that is necessary. tests/mock_issuer.mjs is a stand-in account
# service that signs real Ed25519 tickets, and tests/net_connect_test.cpp
# drives a real NetHost and a real NetSession against it. It only ever ran
# both ends in ONE PROCESS, which proves the protocol and says nothing about
# two machines -- so a `connect` mode was added, and now the two ends can be
# on different computers.
#
# The tunnels are what make it work. Both ends insist http:// is only
# acceptable on localhost, which an address like 10.0.2.2 is not; ssh -R puts
# this machine's issuer on the guest's OWN 127.0.0.1, and ssh -L brings the
# guest's listening port back here. Everything is loopback at both ends, so
# nothing is relaxed to allow it and nothing is exposed to the network.
MOCK_PORT="${OD_PREFLIGHT_ISSUER_PORT:-9911}"
GAME_PORT="${OD_PREFLIGHT_GAME_PORT:-7777}"
declare -a RESULTS=()
started=$(date +%s)

# A stage reports pass, fail or skip, and a SKIP IS NOT A PASS -- the report
# counts them separately and the exit code ignores them, so a pipeline that
# quietly stopped running half its stages cannot read as green.
record() {
    RESULTS+=("$1|$2|$3")
    # Written AS IT HAPPENS, not at the end: a run that takes twenty minutes
    # and is watched in a browser has to show the stage it is on, and a run
    # that is killed half way still leaves what it had got through.
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "${4:-}" >> "$RUNDIR/stages.tsv"
}

# The stage that is running right now, so the page can show it spinning
# rather than showing nothing until it finishes.
running_now() { printf '%s\t%s\n' "$1" "$2" > "$RUNDIR/current"; }
finish_now()  { rm -f "$RUNDIR/current"; }

run_stage() {   # run_stage <name> <description> <command...>
    local name="$1" desc="$2"; shift 2
    bold "── $name: $desc"
    local log="$RUNDIR/$name.log"
    running_now "$name" "$desc"
    local t0; t0=$(date +%s)
    if "$@" > "$log" 2>&1; then
        local dt=$(( $(date +%s) - t0 ))
        green "   pass (${dt}s)"; record "$name" pass "${dt}s" "$desc"
    else
        local dt=$(( $(date +%s) - t0 ))
        red "   FAIL (${dt}s) -- $log"
        tail -12 "$log" | sed 's/^/     /'
        record "$name" fail "${dt}s" "$desc"
    fi
    finish_now
    # Kept as a stable name too, so "the last packages log" is one path.
    ln -sf "$log" "$LOGDIR/$name.log" 2>/dev/null || true
}

skip_stage() { local n="$1" why="$2"; bold "── $n: skipped"; note "$why"; record "$n" skip "$why" "$why"; }

# ── the stages ───────────────────────────────────────────────────────────────

stage_suite()   { "$ROOT/tests/run_all.sh"; }
stage_qualify() { "$ROOT/tools/qualify.sh"; }

# The Linux packages, built and installed in a guest that does NOT have the
# game's libraries -- which is the only way to find out whether the .deb
# declares them correctly.
stage_packages() {
    local g=pf-debian
    "$GUEST" create "$g" debian12 2>/dev/null || true
    "$GUEST" start "$g" || return 1
    "$GUEST" wait  "$g" 240 || return 1
    # Idempotent, and cheap once it has run: the guest this gate depends on
    # must be rebuildable from nothing, or the first time one is lost the
    # gate is lost with it.
    "$GUEST" provision "$g" packages || return 1
    # The guest is on a forwarded port, not an ssh alias; this hands over the
    # options to reach it.
    eval "$("$GUEST" sshenv "$g")"
    "$ROOT/tools/linux_vm_test.sh"
}

# Every guest the plan calls for, booted and proven to run the game's own
# checks. The multiplayer sessions from tools/preflight_plan.py hang off this
# once the cross-play driver exists.
stage_guests() {
    local ok=0
    for spec in "pf-debian:debian12" "pf-freebsd:freebsd14"; do
        local g="${spec%%:*}" os="${spec##*:}"
        note "guest $g ($os)"
        "$GUEST" create "$g" "$os" 2>/dev/null || true
        "$GUEST" start "$g"   || { ok=1; continue; }
        "$GUEST" wait  "$g" 240 || { ok=1; continue; }
        "$GUEST" ssh   "$g" 'uname -sr' || ok=1
    done
    return $ok
}

# CROSS-PLAY: this machine and a guest, each hosting for the other.
#
# tools/preflight_plan.py works out which sessions a full matrix needs -- 30
# ordered host/client pairs across six platforms reduce to 7 if each platform
# hosts once and the Mac joins free. This runs the macOS<->Linux pair of them;
# the rest follow the same shape as more guests can build the client.
stage_multiplayer() {
    command -v node >/dev/null || { echo "node is needed for the stand-in account service"; return 1; }

    local g=pf-debian
    "$GUEST" start "$g" >/dev/null 2>&1 || true
    "$GUEST" wait  "$g" 240 || return 1
    eval "$("$GUEST" sshenv "$g")"

    local bin="$ROOT/build/NetConnectTest"
    [ -x "$bin" ] || { echo "build NetConnectTest first: cmake --build build --target NetConnectTest"; return 1; }
    ssh $OD_SSH_OPTS "$OD_SSH_HOST" 'test -x ~/src/build/NetConnectTest' || {
        echo "no NetConnectTest in $g -- run: tools/preflight.sh --only build"; return 1; }

    local issuer="http://localhost:$MOCK_PORT"
    local rc=0

    # FRESH FILES, NOT FIXED PATHS IN /tmp.
    #
    # These were /tmp/pf-mac-host.log, and the first run read a join code out
    # of one left behind by an earlier experiment -- so it "found" a code
    # instantly, joined a host that did not exist yet, and reported "the
    # connection to that server was lost" twice. The failure looked like a
    # networking problem and was a stale file.
    MP_DIR=$(mktemp -d)
    # Module scope, not local: the RETURN trap below runs after this function's
    # locals are gone, and under `set -u` reading one is a fatal error rather
    # than an empty string -- which is how the first version died in cleanup
    # instead of reporting its result.
    cleanup_mp() {
        pkill -f "NetConnectTest .* host $GAME_PORT" 2>/dev/null || true
        ssh $OD_SSH_OPTS "$OD_SSH_HOST" 'pkill -f NetConnectTest' 2>/dev/null || true
        pkill -f "mock_issuer.mjs --port $MOCK_PORT" 2>/dev/null || true
        pkill -f "ssh .*-R $MOCK_PORT:127.0.0.1:$MOCK_PORT" 2>/dev/null || true
        rm -rf "$MP_DIR"
    }
    trap cleanup_mp RETURN

    local mock_log="$MP_DIR/issuer.log"

    # A host prints its join code while still Opening, and prints "Ctrl-C to
    # stop" only once it is Live. Waiting for the code alone raced the host
    # into existence; this waits for the line that means it is listening.
    wait_for_host() {   # wait_for_host <logfile> -> prints the join code
        local log="$1"
        for _ in $(seq 1 90); do
            sleep 1
            if grep -q "Ctrl-C to stop" "$log" 2>/dev/null; then
                grep -o "join code : .*" "$log" | head -1 | sed "s/join code : //"
                return 0
            fi
            grep -q "^FAIL" "$log" 2>/dev/null && break
        done
        return 1
    }

    node "$ROOT/tests/mock_issuer.mjs" --port "$MOCK_PORT" > "$mock_log" 2>&1 &
    local waited=0
    while ! grep -q "mock-issuer ready" "$mock_log" 2>/dev/null; do
        sleep 0.2; waited=$((waited+1))
        [ "$waited" -gt 100 ] && { echo "the stand-in account service never came up"; cat "$mock_log"; return 1; }
    done
    echo "stand-in account service on $issuer"

    # ── this machine hosts, the guest joins ──
    echo
    echo "== macOS hosts, $g joins =="
    "$bin" "$issuer" host "$GAME_PORT" --all > "$MP_DIR/mac-host.log" 2>&1 &
    local code
    code=$(wait_for_host "$MP_DIR/mac-host.log") \
        || { echo "the host never came up"; tail -5 "$MP_DIR/mac-host.log"; return 1; }

    ssh $OD_SSH_OPTS -R "$MOCK_PORT:127.0.0.1:$MOCK_PORT" -R "$GAME_PORT:127.0.0.1:$GAME_PORT" \
        "$OD_SSH_HOST" "cd ~/src/build && ./NetConnectTest $issuer connect 127.0.0.1:$GAME_PORT $code linux-guest" \
        || rc=1
    pkill -f "NetConnectTest .* host $GAME_PORT" 2>/dev/null || true
    sleep 2

    # ── the guest hosts, this machine joins ──
    echo
    echo "== $g hosts, macOS joins =="
    ssh $OD_SSH_OPTS -R "$MOCK_PORT:127.0.0.1:$MOCK_PORT" -L "$GAME_PORT:127.0.0.1:$GAME_PORT" \
        "$OD_SSH_HOST" "cd ~/src/build && ./NetConnectTest $issuer host $GAME_PORT --all" \
        > "$MP_DIR/guest-host.log" 2>&1 &
    code=$(wait_for_host "$MP_DIR/guest-host.log") \
        || { echo "the guest host never came up"; tail -5 "$MP_DIR/guest-host.log"; return 1; }

    "$bin" "$issuer" connect "127.0.0.1:$GAME_PORT" "$code" mac-client || rc=1
    return $rc
}

# ── the runner ───────────────────────────────────────────────────────────────

only=""; fast=0
while [ $# -gt 0 ]; do
    case "$1" in
        --only) only="$2"; shift 2 ;;
        --fast) fast=1; shift ;;
        --list) printf '%s\n' "${STAGES[@]}"; exit 0 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) red "unknown argument: $1"; exit 2 ;;
    esac
done

wants() { [ -z "$only" ] || [ "$only" = "$1" ]; }

bold "preflight: $(cd "$ROOT" && git rev-parse --short HEAD) on $(uname -sm)"
note "logs in $RUNDIR"
note "watch it:  tools/preflight_dashboard.py --serve"
echo

wants suite   && run_stage suite   "the whole test suite"        stage_suite
wants qualify && run_stage qualify "build, play a game, load it" stage_qualify

if [ "$fast" = 1 ]; then
    wants packages    && skip_stage packages    "--fast"
    wants guests      && skip_stage guests      "--fast"
    wants multiplayer && skip_stage multiplayer "--fast"
elif ! command -v qemu-system-aarch64 >/dev/null; then
    # Named rather than silently passed: a gate that skips what it cannot do
    # and still says "pass" is the failure this whole file exists to avoid.
    wants packages    && skip_stage packages    "qemu not installed (brew install qemu)"
    wants guests      && skip_stage guests      "qemu not installed (brew install qemu)"
    wants multiplayer && skip_stage multiplayer "qemu not installed (brew install qemu)"
else
    wants guests   && run_stage guests   "boot every guest the plan needs" stage_guests
    wants packages && run_stage packages "deb/rpm/AppImage in a clean guest" stage_packages
    wants multiplayer && run_stage multiplayer "each end hosts for the other" stage_multiplayer
fi

# ── the report ───────────────────────────────────────────────────────────────
echo
bold "── preflight, $(( $(date +%s) - started ))s"
fails=0; skips=0
for r in "${RESULTS[@]}"; do
    IFS='|' read -r n s d <<< "$r"
    case "$s" in
        pass) printf '   \033[32m%-10s pass\033[0m  %s\n' "$n" "$d" ;;
        fail) printf '   \033[31m%-10s FAIL\033[0m  %s\n' "$n" "$d"; fails=$((fails+1)) ;;
        skip) printf '   %-10s skip  %s\n' "$n" "$d"; skips=$((skips+1)) ;;
    esac
done
echo
meta finished "$(date +%s)"
if [ "$fails" -gt 0 ]; then
    meta status failed
    red "$fails stage(s) failed. Do not tag."
    exit 1
fi
meta status "$([ "$skips" -gt 0 ] && echo partial || echo passed)"
if [ "$skips" -gt 0 ]; then
    printf '\033[33m%s\033[0m\n' "all run stages passed, but $skips were skipped -- that is not a green run"
    exit 0
fi
green "all stages passed"
