#!/usr/bin/env python3
"""Would a new doctrine or regional law change what every AI country does?

WHY THIS EXISTS

Adding a doctrine looks like content and can be a balance change. Two AI
reflexes are an argmax over the WHOLE doctrine table -- AISystem::
enactablePolicy picks the doctrine nearest the country's compass it can pay
for, and the calming reflex (execPolitics case 8) picks the largest unrest
reduction -- and a third, Game::updateAIDistrictLaws, is an argmax over the
whole regional-law table. An entry that beats the incumbent anywhere silently
changes what every model country in the game does, on every map, which is not
a thing to discover from a bench run three days later.

So this replays those three argmaxes over a grid, with the tables BEFORE a
change and AFTER it, and prints the cells whose winner moved.

    python3 tools/ai_policy_pick.py                    # vs git HEAD
    python3 tools/ai_policy_pick.py <ref>              # vs another commit

WHAT IT DOES NOT MODEL. A country's active doctrines, and therefore conflicts
and committed spending; the LLM advisor's nudge; losingGround. Those narrow
the candidate set per country -- they cannot introduce a winner the full-table
argmax did not already rank first, so a grid that is inert here is inert for
the reflex's FIRST choice in a fresh country, which is the case that decides
what the world does from turn one.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# AISystem.h / Game.h, quoted rather than re-derived.
DOCTRINE_BUDGET_SHARE = 0.25


def at_ref(ref, path):
    out = subprocess.run(["git", "-C", ROOT, "show", "%s:%s" % (ref, path)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        return None
    return json.loads(out.stdout)


def compass_pick(policies, econ, soc, total):
    """AISystem::enactablePolicy, with nothing yet committed."""
    budget = max(0.0, total * DOCTRINE_BUDGET_SHARE)
    best, best_score = None, -1e9
    for p in policies:
        r = p["requirements"]
        if not (r["min_economic"] <= econ <= r["max_economic"]):
            continue
        if not (r["min_social"] <= soc <= r["max_social"]):
            continue
        if p["cost_per_turn"] > budget:
            continue
        sh = p["compass_shift"]
        d = abs(econ / 25.0 - sh["economic"]) + abs(soc / 25.0 - sh["social"])
        score = -d
        if total > 1.0:
            score -= 3.0 * (p["cost_per_turn"] / total)
        if score > best_score:
            best_score, best = score, p["id"]
    return best


def calm_pick(policies, econ, soc, total):
    """AISystem::execPolitics case 8, with OD_CALM_GATE off (the default)."""
    best, best_score = None, 0.0
    for p in policies:
        r = p["requirements"]
        if not (r["min_economic"] <= econ <= r["max_economic"]):
            continue
        if not (r["min_social"] <= soc <= r["max_social"]):
            continue
        e = p["effects"]
        score = (2.0 * e["unrest_reduction"] + abs(e["public_opinion_shift"])
                 + 0.5 * e["minority_growth_rate"])
        if score <= 0.0:
            continue
        if total > 1.0:
            score -= 2.0 * (p["cost_per_turn"] / total)
        if score > best_score:
            best_score, best = score, p["id"]
    return best


def law_pick(laws, provinces, income, ceiling):
    """Game::updateAIDistrictLaws, for one district that is in trouble."""
    best, best_value = None, 0.0
    for l in laws:
        if l["unrest_pct"] >= 0.0:
            continue
        bill = l["cost_per_turn"] * provinces
        if bill > ceiling:
            continue
        forgone = max(0.0, -l["income_pct"]) * 0.01 * income
        gained = max(0.0, l["income_pct"]) * 0.01 * income
        value = (-l["unrest_pct"]) / max(0.5, bill + forgone - gained)
        if value > best_value:
            best_value, best = value, l["id"]
    return best


def compare(name, cells):
    moved = [(k, a, b) for k, a, b in cells if a != b]
    print("%-22s %5d cells, %d moved" % (name, len(cells), len(moved)))
    for k, a, b in moved[:40]:
        print("   %-28s %s -> %s" % (k, a, b))
    if len(moved) > 40:
        print("   ... and %d more" % (len(moved) - 40))
    return moved


def main(argv):
    ref = argv[0] if argv else "HEAD"
    old_pol = at_ref(ref, "data/policies.json")
    old_law = at_ref(ref, "data/district_laws.json")
    new_pol = json.load(open(os.path.join(ROOT, "data", "policies.json")))
    new_law = json.load(open(os.path.join(ROOT, "data", "district_laws.json")))
    if old_pol is None or old_law is None:
        print("cannot read the tables at %s" % ref)
        return 2
    op, np_ = old_pol["policies"], new_pol["policies"]
    ol, nl = old_law["laws"], new_law["laws"]
    print("%s: %d doctrines, %d laws -> working tree: %d doctrines, %d laws"
          % (ref, len(op), len(ol), len(np_), len(nl)))

    compass, calm = [], []
    for econ in range(-100, 101, 20):
        for soc in range(-100, 101, 20):
            for total in (20, 60, 150, 400, 1200):
                k = "e%+d s%+d inc%d" % (econ, soc, total)
                compass.append((k, compass_pick(op, econ, soc, total),
                                compass_pick(np_, econ, soc, total)))
                calm.append((k, calm_pick(op, econ, soc, total),
                             calm_pick(np_, econ, soc, total)))

    laws = []
    for prov in (1, 2, 3, 5, 9, 16, 30):
        for income in (0, 5, 20, 60, 150, 400, 1000):
            for ceiling in (0.5, 2, 6, 20, 80):
                k = "prov%d inc%d ceil%g" % (prov, income, ceiling)
                laws.append((k, law_pick(ol, prov, income, ceiling),
                             law_pick(nl, prov, income, ceiling)))

    moved = compare("compass reflex", compass)
    moved += compare("calming reflex", calm)
    moved += compare("regional law", laws)
    print("total moved: %d" % len(moved))
    return 1 if moved else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
