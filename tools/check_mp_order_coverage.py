#!/usr/bin/env python3
"""Every queued player order crosses the multiplayer wire in both directions.

    tools/check_mp_order_coverage.py

WHY THIS EXISTS

A hosted game is server-authoritative: the client sends its orders, the host
resolves the turn from them. mpSerializeOrders() packs this player's pending
orders into JSON and mpApplyOrders() reads them back on the host. If an order
vector is added to the game but left out of EITHER half, the order is processed
in singleplayer (where the turn runs locally) and silently vanishes online --
no refusal, no log, the move simply never happens.

That shipped once. m_pendingShipDisembarks -- a boat ordered ashore -- was in
neither half, so in the tournament a landing order lived only on the client that
gave it and the host resolved the turn without it. The boat sat off the coast
and nobody could say why. Nothing caught it because nothing was looking: both
halves agreed, in that they were both missing it.

WHAT THIS ASSERTS

Every `m_pending*` container declared in Game.h is one of:

  * ON THE ORDER WIRE -- its key (the member name with `m_` dropped) appears in
    BOTH mpSerializeOrders (as `j["key"]...`) and mpApplyOrders (as
    `each("key", ...)`). Present in one but not the other is the bug above, in
    whichever direction: sent-but-never-read drops it at the host,
    read-but-never-sent means it never arrives.

  * or on the OFF_WIRE list below, each with the reason it does not belong in
    the order payload -- it rides the choices wire or the turn delta instead.

A new order vector that is in neither fails the check, which is the point: it
cannot be forgotten quietly, because adding it to Game.h without a decision here
turns this red. The author must wire it or name why it is exempt.

It also checks the reverse: a `pending*` key on the wire must map back to a
member that still exists, so a rename on one side cannot strand the other.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_H = os.path.join(ROOT, "src", "Game.h")
MP_CPP = os.path.join(ROOT, "src", "Game_Multiplayer.cpp")

# Member name -> the reason it is deliberately NOT in the order payload. These
# are the player-state things that still reach the host, but by another path.
# A vector listed here is EXEMPT; a vector not listed and not on the wire fails.
OFF_WIRE = {
    "m_pendingMapEdits":
        "authoritative turn delta (Game_TurnLogic.cpp), generated during "
        "resolution -- not a client order",
    "m_pendingDiplomaticActions":
        "diplomacy choices wire -- mpSerializeChoices() in Game_MpChoices.cpp",
    "m_pendingCeasefireTerms":
        "diplomacy choices wire -- carried with its ceasefire action in "
        "Game_MpChoices.cpp",
}


def strip_cxx(src: str) -> str:
    """Drop // and /* */ comments so a key named only in a comment never counts
    as wiring. Newlines are kept so line numbers stay usable in errors."""
    out, i, n = [], 0, len(src)
    while i < n:
        two = src[i:i + 2]
        if two == "//":
            while i < n and src[i] != "\n":
                i += 1
        elif two == "/*":
            i += 2
            while i < n and src[i:i + 2] != "*/":
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            i += 2
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def function_body(src: str, signature_re: str) -> str:
    """The brace-matched body of the one function whose opening matches."""
    m = re.search(signature_re, src)
    if not m:
        return ""
    brace = src.index("{", m.start())
    depth, i, n = 0, brace, len(src)
    while i < n:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[brace:i + 1]
        i += 1
    return src[brace:]


def pending_members(game_h: str):
    """Every `std::vector<...>`/`std::unordered_map<...> m_pending<Name>;`."""
    rx = re.compile(
        r"(?:std::vector|std::unordered_map)\s*<[^;{}]*>\s+"
        r"(m_pending[A-Za-z0-9_]*)\s*;")
    return [m.group(1) for m in rx.finditer(game_h)]


def main() -> int:
    with open(GAME_H, encoding="utf-8") as f:
        game_h = strip_cxx(f.read())
    with open(MP_CPP, encoding="utf-8") as f:
        mp = strip_cxx(f.read())

    members = pending_members(game_h)
    if not members:
        print("FAIL  found no m_pending* order vectors in Game.h -- the "
              "declaration shape changed and this checker reads nothing")
        return 1

    serialize = function_body(mp, r"Game::mpSerializeOrders\s*\(")
    apply = function_body(mp, r"Game::mpApplyOrders\s*\(")
    if not serialize or not apply:
        print("FAIL  could not find mpSerializeOrders/mpApplyOrders bodies in "
              "Game_Multiplayer.cpp -- the wire moved and this checker is blind")
        return 1

    def sent(key):      # j["key"] somewhere in the serializer
        return f'j["{key}"]' in serialize

    def read(key):      # each("key", ...) somewhere in the applier
        return re.search(r'each\(\s*"' + re.escape(key) + r'"', apply) is not None

    problems = []
    on_wire = 0
    for member in members:
        key = member[2:]        # drop the m_ prefix; that is the JSON key
        if member in OFF_WIRE:
            # Make sure an exempt vector is not ALSO half-wired -- a stale
            # exemption left after someone started wiring it is its own trap.
            if sent(key) or read(key):
                problems.append(
                    f"{member} is on the OFF_WIRE list but its key "
                    f'"{key}" appears in the order payload too -- remove the '
                    f"exemption or the partial wiring, not both")
            continue
        s, r = sent(key), read(key)
        if s and r:
            on_wire += 1
        elif s and not r:
            problems.append(
                f'{member}: serialized as j["{key}"] but mpApplyOrders never '
                f'reads it (each("{key}", ...)) -- the host drops it in silence')
        elif r and not s:
            problems.append(
                f'{member}: mpApplyOrders reads each("{key}", ...) but '
                f"mpSerializeOrders never sends it -- it never leaves the client")
        else:
            problems.append(
                f'{member}: on neither side of the order wire. Either carry it '
                f'(j["{key}"] in mpSerializeOrders AND each("{key}", ...) in '
                f"mpApplyOrders) or add it to OFF_WIRE in this script with the "
                f"path it travels instead")

    # Reverse: a pending* key on the wire must still name a real member, so a
    # rename on one side cannot leave the other pointing at nothing.
    known_keys = {m[2:] for m in members}
    wire_keys = set(re.findall(r'j\["(pending[A-Za-z0-9_]*)"\]', serialize))
    wire_keys |= set(re.findall(r'each\(\s*"(pending[A-Za-z0-9_]*)"', apply))
    for key in sorted(wire_keys):
        if key not in known_keys:
            problems.append(
                f'the wire carries "{key}" but no m_{key} vector exists in '
                f"Game.h -- a rename stranded one side of the pair")

    # A named exemption that no longer matches a member is dead and misleading.
    for member in OFF_WIRE:
        if member not in members:
            problems.append(
                f"OFF_WIRE lists {member}, which is no longer declared in "
                f"Game.h -- drop the stale exemption")

    if problems:
        for p in problems:
            print(f"FAIL  {p}")
        print(f"\n{len(problems)} multiplayer order-coverage problem(s). An "
              f"order vector must cross the wire in both directions or be "
              f"exempted with a reason; see this script's header.")
        return 1

    exempt = len(OFF_WIRE)
    print(f"ok    {on_wire} player order vectors cross the multiplayer wire "
          f"both ways, {exempt} exempted by name")
    return 0


if __name__ == "__main__":
    sys.exit(main())
