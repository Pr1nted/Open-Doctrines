#!/bin/bash
# Has the TempleOS build fallen behind the C++ one?
#
# Two implementations of the same game cannot be kept identical by a tool, and
# nothing here pretends otherwise. What CAN be done is make the two kinds of
# drift fail the build instead of being discovered by a player:
#
#   NUMBERS drift silently. Retune combat width in Game.h and the HolyC copy
#   keeps the old figure -- both builds run, neither complains, and the
#   TempleOS game is quietly playing a different game. The constants are
#   generated from the C++, so a stale copy is a failure.
#
#   FEATURES drift visibly, but only if somebody looks. The C++ owns the list
#   of map views; adding one there fails this until the TempleOS client has it
#   too, or until it is recorded as skipped with a reason.
#
#     tests/templeos_sync_test.sh
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
PY="$("$root/tools/find_python.sh" 2>/dev/null || echo python3)"
exec "$PY" "$root/tools/templeos_sync.py" --check
