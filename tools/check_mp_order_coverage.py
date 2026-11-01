#!/usr/bin/env python3
"""Every queued player order crosses the multiplayer wire, in order, both ways.

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

1. COVERAGE. Every `m_pending*` container declared in Game.h is either
   ON THE ORDER WIRE -- its key (the member name with `m_` dropped) appears in
   BOTH mpSerializeOrders (as `j["key"]...`) and mpApplyOrders (as
   `each("key", ...)`) -- or on the OFF_WIRE list below. Present in one half but
   not the other is the disembark bug in whichever direction.

2. OFF_WIRE IS HONEST. Each exemption names the file its member travels through
   instead (the choices wire, the turn delta), and the member really is
   referenced there. An exemption cannot quietly become a lie after the code it
   pointed at moves or is deleted.

3. DETERMINISTIC ORDER ON THE WIRE. Neither serializer (orders OR choices) may
   iterate a std::unordered_map / std::unordered_set member and emit straight
   into the payload from that same loop: hash order differs between machines and
   builds, so the same board would serialize to different bytes on the host and
   a client. Collect into a vector, sort, then emit -- a loop over an unordered
   member whose body does NOT touch `j[...]` (the collect step) is fine and not
   flagged.

4. APPLY FOLLOWS EMIT. mpApplyOrders reads the order keys in the same sequence
   mpSerializeOrders writes them, so the two stay legible in parallel and a
   reordering that could change a semantics-sensitive apply is caught.

A new order vector in neither bucket fails (1): adding it to Game.h without a
decision here turns the build red, so it cannot be forgotten quietly.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_H = os.path.join(ROOT, "src", "Game.h")
MP_CPP = os.path.join(ROOT, "src", "Game_Multiplayer.cpp")
CHOICES_CPP = os.path.join(ROOT, "src", "Game_MpChoices.cpp")

# member name -> (relative file it travels through instead, why). A vector here
# is EXEMPT from the order wire; a vector neither here nor on the wire fails. The
# file is verified: the member must actually be referenced there (assertion 2).
OFF_WIRE = {
    "m_pendingMapEdits": (
        "src/Game_TurnLogic.cpp",
        "authoritative turn delta, built during resolution -- not a client order"),
    "m_pendingDiplomaticActions": (
        "src/Game_MpChoices.cpp",
        "diplomacy choices wire -- mpSerializeChoices()"),
    "m_pendingCeasefireTerms": (
        "src/Game_MpChoices.cpp",
        "diplomacy choices wire -- carried with its ceasefire action"),
}


def strip_cxx(src: str) -> str:
    """Drop // and /* */ comments so a name in a comment never counts as code.
    Newlines are kept so line numbers stay usable in errors."""
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


def read_stripped(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return strip_cxx(f.read())


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


def loop_body(body: str, start: int) -> str:
    """The statement a `for (...)` controls, starting at index `start`: a braced
    block (matched), or a single statement up to the first ';'."""
    i, n = start, len(body)
    while i < n and body[i] in " \t\r\n":
        i += 1
    if i < n and body[i] == "{":
        depth = 0
        while i < n:
            if body[i] == "{":
                depth += 1
            elif body[i] == "}":
                depth -= 1
                if depth == 0:
                    return body[start:i + 1]
            i += 1
        return body[start:]
    semi = body.find(";", i)
    return body[start:(semi + 1 if semi != -1 else n)]


def pending_members(game_h: str):
    """Every `std::vector<...>`/`std::unordered_map<...> m_pending<Name>;`."""
    rx = re.compile(
        r"(?:std::vector|std::unordered_map)\s*<[^;{}]*>\s+"
        r"(m_pending[A-Za-z0-9_]*)\s*;")
    return [m.group(1) for m in rx.finditer(game_h)]


def unordered_members(game_h: str):
    """Every member declared std::unordered_map / std::unordered_set -- the
    containers whose iteration order is not stable across machines."""
    rx = re.compile(
        r"std::(?:unordered_map|unordered_set)\s*<[^;{}]*>\s+(m_[A-Za-z0-9_]+)\s*;")
    return set(m.group(1) for m in rx.finditer(game_h))


def ordered_keys(body: str, pattern: str):
    """Keys matched by `pattern` in source order, de-duplicated to first use."""
    seen, out = set(), []
    for m in re.finditer(pattern, body):
        k = m.group(1)
        if k not in seen:
            seen.add(k)
            out.append(k)
    return out


def unordered_iteration(fname: str, body: str, unordered, problems):
    """Flag a `for (... : m_someUnorderedMember)` that emits to `j[...]` from its
    own body. Collecting into a local (no `j[` in the body) is the safe
    collect-then-sort step and is left alone."""
    for m in re.finditer(r"for\s*\(\s*[^;{}]*?:\s*(m_[A-Za-z0-9_]+)\s*\)", body):
        rng = m.group(1)
        if rng in unordered and 'j["' in loop_body(body, m.end()):
            problems.append(
                f"{fname} iterates {rng} (an unordered container) and emits to "
                f"the payload from that loop -- its order is hash order, which "
                f"differs between machines and builds. Collect into a "
                f"std::vector, sort, then emit.")


def main() -> int:
    game_h = read_stripped(GAME_H)
    mp = read_stripped(MP_CPP)
    choices = read_stripped(CHOICES_CPP)

    members = pending_members(game_h)
    unordered = unordered_members(game_h)
    if not members:
        print("FAIL  found no m_pending* order vectors in Game.h -- the "
              "declaration shape changed and this checker reads nothing")
        return 1

    serialize = function_body(mp, r"Game::mpSerializeOrders\s*\(")
    apply = function_body(mp, r"Game::mpApplyOrders\s*\(")
    serialize_choices = function_body(choices, r"Game::mpSerializeChoices\s*\(")
    if not serialize or not apply:
        print("FAIL  could not find mpSerializeOrders/mpApplyOrders bodies in "
              "Game_Multiplayer.cpp -- the wire moved and this checker is blind")
        return 1
    if not serialize_choices:
        print("FAIL  could not find mpSerializeChoices body in Game_MpChoices.cpp "
              "-- the choices wire moved and this checker is blind")
        return 1

    def sent(key):      # j["key"] somewhere in the serializer
        return f'j["{key}"]' in serialize

    def read(key):      # each("key", ...) somewhere in the applier
        return re.search(r'each\(\s*"' + re.escape(key) + r'"', apply) is not None

    problems = []
    on_wire = 0

    # (1) coverage, and (2) that OFF_WIRE exemptions are honest.
    for member in members:
        key = member[2:]        # drop the m_ prefix; that is the JSON key
        if member in OFF_WIRE:
            rel, _why = OFF_WIRE[member]
            if sent(key) or read(key):
                problems.append(
                    f'{member} is on the OFF_WIRE list but its key "{key}" '
                    f"appears in the order payload too -- remove the exemption "
                    f"or the partial wiring, not both")
            else:
                path = os.path.join(ROOT, rel)
                found = os.path.exists(path) and \
                    re.search(r"\b" + re.escape(member) + r"\b", read_stripped(path))
                if not found:
                    problems.append(
                        f"{member} is exempted as travelling through {rel}, but "
                        f"that file does not reference it -- the exemption's path "
                        f"is wrong or the code moved")
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

    # Reverse: a pending* key on the wire must still name a real member.
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

    # (3) no nondeterministic iteration straight into either serialized payload.
    unordered_iteration("mpSerializeOrders", serialize, unordered, problems)
    unordered_iteration("mpSerializeChoices", serialize_choices, unordered, problems)

    # (4) mpApplyOrders reads the order keys in the same sequence they are sent.
    emit_seq = ordered_keys(serialize, r'j\["(pending[A-Za-z0-9_]*)"\]')
    apply_seq = ordered_keys(apply, r'each\(\s*"(pending[A-Za-z0-9_]*)"')
    emit_filtered = [k for k in emit_seq if k in set(apply_seq)]
    if emit_filtered != apply_seq:
        problems.append(
            "mpApplyOrders processes the order keys in a different sequence than "
            "mpSerializeOrders writes them:\n"
            f"        emit:  {emit_filtered}\n"
            f"        apply: {apply_seq}\n"
            "      keep the two in the same order so a reordering that changes "
            "apply-time semantics is visible here")

    if problems:
        for p in problems:
            print(f"FAIL  {p}")
        print(f"\n{len(problems)} multiplayer order-coverage problem(s). See this "
              f"script's header for the four properties it holds the wire to.")
        return 1

    print(f"ok    {on_wire} player order vectors cross the multiplayer wire both "
          f"ways and in order, {len(OFF_WIRE)} exempted through a verified path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
