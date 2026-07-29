#!/usr/bin/env bash
# Four players on one machine, one command.
#
# WHAT THIS SOLVES
#
# A pseudonym is derived from the account you sign in with, so two copies of the
# game signed in as you are ONE player twice -- the lobby correctly reads the
# second as your own reconnect and hands it your seat. Testing multiplayer alone
# therefore needs several identities, and the real account service will not give
# you those without several real provider logins.
#
# So this runs the LOCAL dev issuer instead. It mints a stable pseudonym per
# token, `dev-alice` is always Alice, and each client is handed its session
# directly -- no OAuth, no browser, no second Google account. Plain http is
# allowed for exactly this case and no other: AccountClient permits an insecure
# issuer only on localhost.
#
# WHAT IT IS NOT
#
# Not a test of the account screen -- these clients arrive already signed in --
# and not a test of the real service. It is a test of everything AFTER sign-in:
# hosting, joining, seats, turns, timers, the host console.
#
#   tools/playtest.sh            four clients: a playing host and three joiners
#   tools/playtest.sh --spectate host does not take a seat; three joiners play
#   tools/playtest.sh --bridge-check
#                                same, plus a mod on all four that exercises
#                                the Net and Audio bridges and puts the answers
#                                on screen. See tools/playtest-mod/mod.c for
#                                what the panel means.
#   tools/playtest.sh --verify   NO windows: runs the four-peer check headlessly
#                                and exits non-zero if anything is wrong. This is
#                                the one to run on a platform you are qualifying.
#   tools/playtest.sh --clean    throw the playtest data away and exit
#
# WHY FOUR AND NOT TWO
#
# Two peers cannot see the failures that matter. Seats that collide, a country
# claimed twice, a turn that begins for whoever was pumped last, a reconnect
# that hands somebody another player's country -- every one of those is fine
# with one joiner. --verify is those cases, automated; the windowed modes are
# for everything a person has to look at to judge.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$root/.playtest"
port="${OD_PLAYTEST_PORT:-52700}"
issuer="http://localhost:$port"

# A playing host plus three joiners is four players. Dave is the one added when
# this grew from three: the names are arbitrary, but they are STABLE, because
# the stand-in issuer derives a pseudonym from the token and `dev-dave` has to
# keep meaning Dave across runs for a reconnect to be a reconnect.
PLAYERS="alice bob carol dave"

# The NEWEST build wins, not a fixed order. Two build directories is the normal
# state here -- CLion writes cmake-build-debug, the command line writes build --
# and preferring either by name means a playtest can silently exercise a binary
# that predates the change being tested. That failure looks exactly like the
# feature not working, which is the worst way for it to look.
game=""
for candidate in "$root/cmake-build-debug/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
                 "$root/build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
                 "$root/build/OpenDoctrines" \
                 "$root/cmake-build-debug/OpenDoctrines"; do
    [ -x "$candidate" ] || continue
    [ -z "$game" ] || [ "$candidate" -nt "$game" ] && game="$candidate"
done

mode="play"
bridge=0
verify=0
for arg in "$@"; do
    case "$arg" in
        --clean) rm -rf "$work"; echo "removed $work"; exit 0 ;;
        --spectate) mode="spectate" ;;
        --bridge-check) bridge=1 ;;
        --verify) verify=1 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

# --verify needs no game binary, no windows and no data directories: it is the
# net stack talking to itself, which is the only part of a four-player game a
# machine can judge on its own. Everything below this point is about windows.
if [ "$verify" = 1 ]; then
    build="${OD_BUILD_DIR:-$root/build}"
    command -v node >/dev/null || { echo "node is needed for the dev issuer" >&2; exit 1; }
    cmake --build "$build" --target NetConnectTest >/dev/null || {
        echo "could not build NetConnectTest" >&2; exit 1; }
    exec "$root/tests/connectivity_test.sh" "$build"
fi

[ -n "$game" ] && [ -x "$game" ] || { echo "build the game first: cmake --build build" >&2; exit 1; }

newest_src="$(find "$root/src" -type f \( -name '*.cpp' -o -name '*.h' \) -newer "$game" -print -quit 2>/dev/null || true)"
if [ -n "$newest_src" ]; then
    echo "WARNING: $(basename "$(dirname "$(dirname "$(dirname "$game")")")")'s binary is older than $newest_src" >&2
    echo "         you are about to playtest a build that predates your changes." >&2
    echo "         build first, then re-run." >&2
    exit 1
fi
echo "using $game"
command -v node >/dev/null || { echo "node is needed for the dev issuer" >&2; exit 1; }

# ---- the bridge probe -------------------------------------------------------
# Built here rather than shipped built: it must be compiled against the CURRENT
# sdk/gearbox_generated.h, or it would test an ABI the host no longer has.
moddir="$root/tools/playtest-mod"
modfile="$moddir/bridge-check.odmod"

build_probe() {
    local cc=""
    for c in "$(command -v clang || true)" \
             /opt/homebrew/Cellar/emscripten/*/libexec/llvm/bin/clang \
             /usr/local/Cellar/emscripten/*/libexec/llvm/bin/clang \
             /opt/homebrew/opt/llvm/bin/clang; do
        [ -x "$c" ] || continue
        if "$c" --print-targets 2>/dev/null | grep -qi wasm32; then cc="$c"; break; fi
    done
    [ -n "$cc" ] || { echo "--bridge-check needs a wasm32-capable clang" >&2; exit 1; }
    "$cc" --target=wasm32 -nostdlib -O2 -I "$root/sdk" \
          -Wl,--no-entry -Wl,--allow-undefined \
          -o "$moddir/mod.wasm" "$moddir/mod.c"
    "$root/tools/pack_odmod.sh" "$moddir" "$modfile" >/dev/null
    echo "built the bridge probe"
}

[ "$bridge" = 1 ] && build_probe

# ---- one data directory per player ----------------------------------------
# Bulk assets are symlinked: a map pack is hundreds of megabytes and no client
# writes to it. Only what a client writes is its own.
seed_player() {
    local name="$1"
    local dir="$work/$name"
    mkdir -p "$dir/saves/multiplayer"
    for entry in "$root/data"/*; do
        local base; base="$(basename "$entry")"
        case "$base" in account.json|config.json|servers.json|saves|tools) continue ;; esac
        # In bridge mode every client gets its OWN mods directory. Symlinking
        # the real one would write the probe into data/mods, and would make all
        # three share an enable-state -- which is the opposite of what a mod
        # attestation check needs to be tested against.
        if [ "$bridge" = 1 ] && [ "$base" = mods ]; then continue; fi
        [ -e "$dir/$base" ] || ln -s "$entry" "$dir/$base"
    done

    # Pointed at the dev issuer, and pre-agreed: the consent gate is a thing to
    # test deliberately, not to click three times before every run.
    # Rewritten every run, deliberately: whatever a previous session saved here
    # is not what the next playtest wants.
    #
    # Music is forced to zero. Three clients each starting their own track at
    # their own moment is not three-part harmony, it is three radios in one
    # room -- and the one thing you cannot do about it is turn it down in three
    # places while trying to watch a turn resolve. Effects stay on: those are
    # tied to what you are testing.
    cat > "$dir/config.json" <<EOF
{
  "accountIssuer": "$issuer",
  "accountAgreed": true,
  "fullscreen": false,
  "musicVolume": 0,
  "nowPlayingToast": false
}
EOF
    # The session itself. This is what replaces signing in.
    cat > "$dir/account.json" <<EOF
{"issuer": "$issuer", "token": "dev-$name"}
EOF
    chmod 600 "$dir/account.json"

    # The probe, already enabled and already granted. Clicking through the mod
    # menu three times before every run is not the thing being tested, and a
    # grant is a persisted user decision -- so this simply is that decision,
    # made in advance.
    #
    #   Core 1 | UI 32 | Audio 2048 | Net 4096 = 6177
    if [ "$bridge" = 1 ]; then
        mkdir -p "$dir/mods"
        cp "$modfile" "$dir/mods/"
        cat > "$dir/mods.json" <<'EOJ'
{
  "schema": 1,
  "mods": [
    { "id": "com.opendoctrines.bridge-check", "enabled": true, "grants": 6177 }
  ]
}
EOJ
    fi
}

mkdir -p "$work"
for who in $PLAYERS; do seed_player "$who"; done

# ---- the issuer -------------------------------------------------------------
node "$root/tests/mock_issuer.mjs" --port "$port" > "$work/issuer.log" 2>&1 &
issuer_pid=$!

cleanup() {
    echo
    echo "stopping..."
    for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    kill "$issuer_pid" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for _ in $(seq 1 40); do
    curl -sf "$issuer/" >/dev/null 2>&1 && break
    sleep 0.25
done
curl -sf "$issuer/" >/dev/null 2>&1 || { echo "dev issuer did not start; see $work/issuer.log" >&2; exit 1; }

# ---- the clients ------------------------------------------------------------
pids=()
for who in $PLAYERS; do
    OD_DATA_DIR="$work/$who" "$game" > "$work/$who.log" 2>&1 &
    pids+=("$!")
    sleep 0.4                       # stagger, so four windows do not overlap exactly
done

cat <<EOF

  Four clients are running, each a different player:

    Alice   $work/alice     (developer badge)
    Bob     $work/bob
    Carol   $work/carol
    Dave    $work/dave

  Issuer: $issuer          logs: $work/*.log

  What to do:

EOF
if [ "$mode" = "spectate" ]; then
cat <<'EOF'
    1. ALICE:  Multiplayer -> Host a game -> Rules tab -> tick
               "Host only -- I am not playing", then Start hosting.
    2. ALICE:  copy the invite (address + code) from the lobby.
    3. BOB, CAROL and DAVE: Join a game, paste both, tick "I understand", Join.
    4. Each picks a country, ALICE presses Start game.
    5. Only Bob, Carol and Dave should have countries; Alice watches.
EOF
else
cat <<'EOF'
    1. ALICE:  Multiplayer -> Host a game -> Start hosting, pick a country.
    2. ALICE:  copy the invite (address + code) from the lobby.
    3. BOB, CAROL and DAVE: Join a game, paste both, tick "I understand", Join.
    4. Each picks a country, ALICE presses Start game.
    5. Press Ready on each. The turn resolves when all four are in.

  Worth exercising while you are there:

    - Esc on Alice: the host console -- who is ready, time left, resolve now.
    - Close Carol entirely: Alice's console shows her seat as away, and the
      turn can still be resolved without her.
    - Reopen Carol: she should return to her OWN country, not re-pick.
    - Have Bob and Dave both try to pick the SAME country: the second is
      refused by name rather than silently ignored.
    - Set a turn timer on the Turns tab and watch the per-player clocks.

  tools/playtest.sh --verify checks the seat, claim, turn, disconnect and
  reconnect rules above without any of this clicking. What it cannot judge is
  how any of it LOOKS, which is what these four windows are for.
EOF
fi

if [ "$bridge" = 1 ]; then
cat <<'EOF'

  BRIDGE CHECK is installed and enabled on all four, but NOT yet running:
  mods.json records the user's decision, and activation happens from the mod
  menu -- a mod is never started behind the player's back. So first, on EACH
  client:

    0. Mods -> "Reload modloader". The mod goes Active.

  This matters beyond convenience: attestation only reports mods that are
  actually Active, so a client that skipped this presents a different mod set
  and the host will refuse it. That refusal is itself worth seeing once --
  reload on Alice, Bob and Dave only, and Carol should be turned away by name.

  Once the game starts, a panel down the right edge shows what the Net bridge
  reports. What it should say:

    peers   4 (a playing host and three joiners); 0 would mean no bridge
    self    a different id on each client, and NOT 0 on a playing host
    host    1 on Alice only

    Press "Ping all" on Alice -> Bob, Carol and Dave's "recvd" goes up and
    "from" becomes Alice's id. Alice's own does not: a broadcast goes to
    everyone ELSE. Press it on Bob -> the other three see it, Bob does not.

    Press "Sound" -> a sound plays on that client only. It is not networked;
    it is there to show the audio bridge reaches raylib at all.

  If the panel never appears, the mod did not load: check the log.

EOF
fi

echo "  Ctrl-C here closes all three and the issuer."
echo
wait
