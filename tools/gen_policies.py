#!/usr/bin/env python3
"""Author data/policies.json, and keep every doctrine's advertised numbers true.

WHY THIS EXISTS

policies.json carries, for each doctrine, both the effects the resolver reads
and the "gains"/"costs" lines the doctrine screen prints. Those were written by
hand and had drifted: doctrines advertised an unrest reduction the resolver
applied at a hundredth of the stated value, and most of them advertised nothing
else because they DID nothing else -- a doctrine could move the political
compass and essentially not another number in the game.

So the file is generated. The tradeoff text is derived from the effects rather
than typed beside them, which makes "it says -15% industry cost" and "it applies
-15% industry cost" the same fact instead of two facts that agree today.

  python3 tools/gen_policies.py            # write data/policies.json
  python3 tools/gen_policies.py --check    # fail if the file has drifted

LEVERS are the research tree's own effect names (see Game::getTotalEffect), so a
doctrine and a research node are the same kind of thing to every caller. Sign
convention follows the tree: a COST reduction is stored POSITIVE, because
buildCostMod() subtracts it.
"""
import json, sys, os

# name -> (template, is-a-cost-reduction)
#
# A COST lever is stored positive when it makes the thing CHEAPER -- that is the
# research tree's convention, because buildCostMod() subtracts it. So its text
# has to flip the sign it prints, or a doctrine that halves the price of a
# factory advertises "+50% industry cost".
LEVER_TEXT = {
    "industryCostPct":     ("Industry cost {sign}{v:.0f}%", "cost"),
    "industryUpkeepPct":   ("Industry upkeep {sign}{v:.0f}%", "cost"),
    "conscriptionCostPct": ("Recruitment cost {sign}{v:.0f}%", "cost"),
    "navyCostPct":         ("Ship cost {sign}{v:.0f}%", "cost"),
    "maintenanceCostPct":  ("Upkeep {sign}{v:.0f}%", "cost"),
    "conscriptionPct":     ("Manpower {sign}{v:.0f}%", True),
    "armyAtkPct":          ("Army attack {sign}{v:.0f}%", True),
    "armyDefPct":          ("Army defence {sign}{v:.0f}%", True),
    "navyAtkPct":          ("Ship attack {sign}{v:.0f}%", True),
    "navyDefPct":          ("Ship defence {sign}{v:.0f}%", True),
    "navySpeedPct":        ("Ship speed {sign}{v:.0f}%", True),
    "resourceModPct":      ("Resource income {sign}{v:.0f}%", True),
    "popModPct":           ("Population income {sign}{v:.0f}%", True),
    "popGrowthPct":        ("Population growth {sign}{v:.1f}%/turn", True),
    "migrationRate":       ("Migration {sign}{v:.0f}%", True),
    "indoctrinationPct":   ("Indoctrination {sign}{v:.0f}%", True),
    "passiveIncome":       ("Treasury {sign}{v:.0f}/turn", True),
}


def lever_line(name, v):
    tmpl, kind = LEVER_TEXT[name]
    up = (v > 0) if kind != "cost" else (v < 0)
    return tmpl.format(sign="+" if up else "-", v=abs(v))


def build(p):
    """Fill in derived fields: effects block, tradeoff text, folder."""
    cat = p["category"]
    p.setdefault("folder", {"left": "Left", "right": "Right",
                            "authoritarian": "Authoritarian",
                            "libertarian": "Libertarian"}.get(cat, "Miscellaneous"))
    eff = {
        "minority_growth_rate": p.pop("minority_growth", 0.0),
        "immigration_boost":    p.pop("immigration", 0.0),
        "pacification_cost":    p.pop("pacification", 0.0),
        "unrest_reduction":     p.pop("unrest", 0.0),
        "public_opinion_shift": p.pop("opinion", 0.0),
        "target_minority":      "",
    }
    p["effects"] = eff
    levers = p.get("levers", {})

    gains, costs = [], []
    for name, v in levers.items():
        if v == 0:
            continue
        (gains if v > 0 else costs).append(lever_line(name, v))
    if eff["unrest_reduction"] > 0:
        gains.append("Unrest -{:.1f}%".format(eff["unrest_reduction"] * 100))
    elif eff["unrest_reduction"] < 0:
        costs.append("Unrest +{:.1f}%".format(abs(eff["unrest_reduction"]) * 100))
    if eff["pacification_cost"] > 0:
        gains.append("Pacification budget +{:.0f}/turn".format(eff["pacification_cost"]))
    if eff["minority_growth_rate"]:
        v = eff["minority_growth_rate"] * 100
        (gains if v > 0 else costs).append(
            "Minority growth {}{:.0f}%/turn".format("+" if v > 0 else "-", abs(v)))
    if eff["immigration_boost"]:
        gains.append("Immigration +{:.0f}%".format(eff["immigration_boost"] * 100))
    if eff["public_opinion_shift"]:
        gains.append("Pulls provinces toward the government")
    if p["cost_per_turn"] > 0:
        costs.append("-{}/turn income".format(p["cost_per_turn"]))
    sh = p["compass_shift"]
    if sh["economic"]:
        costs.append("Economic {}{}".format("+" if sh["economic"] > 0 else "", sh["economic"]))
    if sh["social"]:
        costs.append("Social {}{}".format("+" if sh["social"] > 0 else "", sh["social"]))
    p["tradeoffs"] = {"gains": gains, "costs": costs}
    p.setdefault("incompatible_with", [])
    p.setdefault("propaganda_duration", 0)
    p.setdefault("requirements", {"min_economic": -100, "max_economic": 100,
                                  "min_social": -100, "max_social": 100})
    return p


def R(mine=-100, maxe=100, mins=-100, maxs=100):
    return {"min_economic": mine, "max_economic": maxe,
            "min_social": mins, "max_social": maxs}


# ─── The doctrines ────────────────────────────────────────────────────────
#
# Requirements are no longer a gate -- being the wrong sort of government for a
# doctrine costs political capital, implementation turns and unrest instead of
# refusing it (see Game::doctrineStrain). They still say what KIND of government
# a doctrine belongs to, which is what makes the price meaningful.
#
# Opposed pairs, mostly. A doctrine whose only cost is a number going down is a
# decision nobody has to think about; the interesting ones give up something a
# player wanted.
P = [
    # ── Left ──────────────────────────────────────────────────────────────
    dict(id="land_reform", name="Land Reform", category="left",
         description="Redistribute land to peasants. Feeds the country and angers those who owned it.",
         cost_per_turn=5, implementation_turns=4,
         compass_shift={"economic": -15, "social": -10}, requirements=R(maxe=20),
         minority_growth=0.02, unrest=0.01,
         levers={"popGrowthPct": 0.5, "resourceModPct": 8},
         incompatible_with=["privatization", "flat_tax"]),
    dict(id="state_industry", name="State Industry Expansion", category="left",
         description="Nationalise the commanding heights. Factories get cheaper; the men who ran them leave.",
         cost_per_turn=10, implementation_turns=5,
         compass_shift={"economic": -20, "social": -5}, requirements=R(maxe=40),
         levers={"industryCostPct": 18, "passiveIncome": 3, "resourceModPct": -5},
         incompatible_with=["privatization", "deregulation"]),
    dict(id="worker_rights", name="Worker Rights Act", category="left",
         description="Unions, hours, safety. A healthier country and a dearer conscript.",
         cost_per_turn=3, implementation_turns=3,
         compass_shift={"economic": -5, "social": 15}, requirements=R(),
         minority_growth=0.01, unrest=0.02,
         levers={"popGrowthPct": 1.0, "conscriptionCostPct": -15}),
    dict(id="wealth_tax", name="Progressive Wealth Tax", category="left",
         description="Tax the rich heavily. Pays for the state; capital finds other countries.",
         cost_per_turn=0, implementation_turns=2,
         compass_shift={"economic": -10, "social": 0}, requirements=R(maxe=50),
         pacification=5.0, unrest=0.015,
         levers={"passiveIncome": 6, "industryCostPct": -10},
         incompatible_with=["flat_tax"]),
    dict(id="total_war_economy", name="Total War Economy", category="left",
         description="Every factory to the front. Nothing is built that does not shoot.",
         cost_per_turn=14, implementation_turns=4,
         compass_shift={"economic": -20, "social": -20}, requirements=R(),
         unrest=-0.03,
         levers={"conscriptionCostPct": 25, "armyAtkPct": 10, "navyCostPct": 15,
                 "industryCostPct": -20, "popGrowthPct": -1.0},
         incompatible_with=["consumer_economy"]),

    # ── Right ─────────────────────────────────────────────────────────────
    dict(id="deregulation", name="Economic Deregulation", category="right",
         description="Get the state out of the way. Building is cheap; nobody is watching.",
         cost_per_turn=0, implementation_turns=2,
         compass_shift={"economic": 15, "social": 5}, requirements=R(mine=-40),
         unrest=-0.01,
         levers={"industryCostPct": 20, "passiveIncome": 4}),
    dict(id="privatization", name="Privatisation", category="right",
         description="Sell the state's holdings. A windfall, then somebody else owns the mines.",
         cost_per_turn=0, implementation_turns=4,
         compass_shift={"economic": 20, "social": -5}, requirements=R(mine=-30),
         levers={"industryCostPct": 25, "maintenanceCostPct": 10, "resourceModPct": -8}),
    dict(id="flat_tax", name="Flat Tax", category="right",
         description="One rate for everyone. Simple, popular with those who have most.",
         cost_per_turn=0, implementation_turns=2,
         compass_shift={"economic": 10, "social": 0}, requirements=R(mine=-60),
         unrest=-0.005,
         levers={"passiveIncome": 7, "popGrowthPct": -0.5}),
    dict(id="consumer_economy", name="Consumer Economy", category="right",
         description="Build for the people who live here. They are happier, and there are fewer shells.",
         cost_per_turn=6, implementation_turns=4,
         compass_shift={"economic": 15, "social": 15}, requirements=R(),
         unrest=0.04,
         levers={"popModPct": 12, "popGrowthPct": 1.5, "conscriptionPct": -20,
                 "armyAtkPct": -5},
         incompatible_with=["total_war_economy"]),
    dict(id="free_trade", name="Free Trade", category="right",
         description="Open the ports. Wealth arrives by sea, and so does everything else.",
         cost_per_turn=0, implementation_turns=3,
         compass_shift={"economic": 18, "social": 8}, requirements=R(mine=-40),
         levers={"passiveIncome": 8, "resourceModPct": 10, "navySpeedPct": 10,
                 "industryCostPct": -8},
         incompatible_with=["autarky"]),

    # ── Authoritarian ─────────────────────────────────────────────────────
    dict(id="national_unity", name="National Unity Act", category="authoritarian",
         description="One nation, one story. The line holds; nobody says otherwise.",
         cost_per_turn=8, implementation_turns=3,
         compass_shift={"economic": 0, "social": -20}, requirements=R(maxs=40),
         unrest=0.03,
         levers={"armyDefPct": 10, "indoctrinationPct": 10, "migrationRate": -10}),
    dict(id="censorship", name="Press Censorship", category="authoritarian",
         description="Nothing is printed that was not approved. Quiet, and blind.",
         cost_per_turn=4, implementation_turns=2,
         compass_shift={"economic": 0, "social": -15}, requirements=R(maxs=50),
         pacification=2.0, unrest=0.02,
         levers={"indoctrinationPct": 15, "migrationRate": -12},
         incompatible_with=["free_press"]),
    dict(id="conscription", name="Universal Conscription", category="authoritarian",
         description="Every man of age. The army is enormous and the fields are empty.",
         cost_per_turn=6, implementation_turns=3,
         compass_shift={"economic": 5, "social": -15}, requirements=R(),
         unrest=-0.01, minority_growth=-0.01,
         levers={"conscriptionPct": 30, "conscriptionCostPct": 15, "popGrowthPct": -0.5},
         incompatible_with=["professional_army"]),
    dict(id="secret_police", name="Secret Police", category="authoritarian",
         description="They know before you do. Nothing organises twice.",
         cost_per_turn=12, implementation_turns=3,
         compass_shift={"economic": 0, "social": -25}, requirements=R(maxs=30),
         pacification=10.0, unrest=0.05,
         levers={"indoctrinationPct": 20, "migrationRate": -25, "popGrowthPct": -0.5}),
    dict(id="autarky", name="Autarky", category="authoritarian",
         description="Need nobody. Everything is made here, badly, and cannot be blockaded away.",
         cost_per_turn=9, implementation_turns=5,
         compass_shift={"economic": -10, "social": -15}, requirements=R(),
         levers={"resourceModPct": 15, "industryCostPct": -12, "passiveIncome": -4,
                 "maintenanceCostPct": 10},
         incompatible_with=["free_trade"]),
    dict(id="mass_mobilisation", name="Mass Mobilisation", category="authoritarian",
         description="The whole country under arms. It cannot be kept up for long.",
         cost_per_turn=11, implementation_turns=2,
         compass_shift={"economic": -5, "social": -18}, requirements=R(),
         unrest=-0.04,
         levers={"conscriptionPct": 45, "armyDefPct": 8, "popGrowthPct": -1.5,
                 "popModPct": -10},
         incompatible_with=["professional_army", "consumer_economy"]),

    # ── Libertarian ───────────────────────────────────────────────────────
    dict(id="minority_rights", name="Minority Rights Charter", category="libertarian",
         description="Rights written down and meant. The country grows and argues.",
         cost_per_turn=6, implementation_turns=4,
         compass_shift={"economic": 5, "social": 20}, requirements=R(mins=-40),
         minority_growth=0.03, unrest=0.02,
         levers={"migrationRate": 15, "popGrowthPct": 1.0, "indoctrinationPct": -10}),
    dict(id="open_borders", name="Open Borders", category="libertarian",
         description="Anyone may come. They bring their hands and their grievances.",
         cost_per_turn=3, implementation_turns=3,
         compass_shift={"economic": 5, "social": 15}, requirements=R(mins=-30),
         immigration=0.02, unrest=0.01,
         levers={"migrationRate": 30, "popModPct": 8, "popGrowthPct": 1.5,
                 "indoctrinationPct": -12}),
    dict(id="free_press", name="Free Press", category="libertarian",
         description="Print it. The government finds out what it is doing wrong, publicly.",
         cost_per_turn=2, implementation_turns=2,
         compass_shift={"economic": 0, "social": 15}, requirements=R(mins=-50),
         unrest=0.015,
         levers={"indoctrinationPct": -15, "passiveIncome": 3},
         incompatible_with=["censorship"]),
    dict(id="decentralization", name="Decentralisation", category="libertarian",
         description="Let the provinces decide. They build faster and answer slower.",
         cost_per_turn=5, implementation_turns=4,
         compass_shift={"economic": 0, "social": 10}, requirements=R(),
         minority_growth=0.01, unrest=0.02,
         levers={"industryCostPct": 12, "armyDefPct": 6, "conscriptionPct": -12}),
    dict(id="general_amnesty", name="General Amnesty", category="libertarian",
         description="Let them come home. The prisons empty and so does the grievance.",
         cost_per_turn=7, implementation_turns=2,
         compass_shift={"economic": 0, "social": 18}, requirements=R(),
         minority_growth=0.02, unrest=0.06,
         levers={"indoctrinationPct": -8, "conscriptionPct": -8}),
    dict(id="professional_army", name="Professional Army", category="libertarian",
         description="Fewer soldiers, and every one of them meant it. Expensive.",
         cost_per_turn=10, implementation_turns=4,
         compass_shift={"economic": 8, "social": 10}, requirements=R(),
         levers={"armyAtkPct": 18, "armyDefPct": 12, "conscriptionPct": -25,
                 "conscriptionCostPct": -20},
         incompatible_with=["conscription", "mass_mobilisation"]),

    # ── Naval and colonial ────────────────────────────────────────────────
    dict(id="naval_supremacy", name="Naval Supremacy", category="right",
         description="The fleet first, and the army with what is left.",
         cost_per_turn=12, implementation_turns=4,
         compass_shift={"economic": 8, "social": -5}, requirements=R(),
         levers={"navyCostPct": 25, "navyAtkPct": 15, "navyDefPct": 10,
                 "navySpeedPct": 15, "armyAtkPct": -8},
         incompatible_with=["continental_army"]),
    dict(id="continental_army", name="Continental Army", category="authoritarian",
         description="Land wars are won by armies. The fleet can wait.",
         cost_per_turn=10, implementation_turns=4,
         compass_shift={"economic": -5, "social": -10}, requirements=R(),
         levers={"armyAtkPct": 12, "armyDefPct": 12, "conscriptionCostPct": 15,
                 "navyCostPct": -20, "navySpeedPct": -10},
         incompatible_with=["naval_supremacy"]),
    dict(id="colonial_office", name="Colonial Office", category="right",
         description="Govern the empire from one desk. Efficient, and resented.",
         cost_per_turn=8, implementation_turns=4,
         compass_shift={"economic": 12, "social": -12}, requirements=R(),
         minority_growth=-0.01, unrest=-0.02,
         levers={"resourceModPct": 18, "passiveIncome": 6, "migrationRate": -10},
         incompatible_with=["commonwealth_settlement"]),
    dict(id="commonwealth_settlement", name="Commonwealth Settlement", category="libertarian",
         description="Partners, not possessions. Slower, and it holds.",
         cost_per_turn=9, implementation_turns=5,
         compass_shift={"economic": 0, "social": 18}, requirements=R(),
         minority_growth=0.02, unrest=0.05,
         levers={"migrationRate": 18, "popGrowthPct": 1.0, "resourceModPct": -8},
         incompatible_with=["colonial_office"]),

    # ── Science and tradition ─────────────────────────────────────────────
    dict(id="technocracy", name="Technocracy", category="miscellaneous",
         description="Rule by those who can do the arithmetic. Nobody voted for them.",
         cost_per_turn=10, implementation_turns=4,
         compass_shift={"economic": 0, "social": -8}, requirements=R(),
         levers={"industryCostPct": 15, "resourceModPct": 10, "indoctrinationPct": 8,
                 "popGrowthPct": -0.5},
         incompatible_with=["traditionalism"]),
    dict(id="traditionalism", name="Traditionalism", category="miscellaneous",
         description="The old ways, kept. The country is calm and does not change.",
         cost_per_turn=4, implementation_turns=3,
         compass_shift={"economic": 5, "social": -12}, requirements=R(),
         unrest=0.05,
         levers={"popGrowthPct": 1.0, "indoctrinationPct": 12, "industryCostPct": -10,
                 "migrationRate": -15},
         incompatible_with=["technocracy"]),

    # ── Campaigns: temporary, and they buy attention rather than policy ────
    dict(id="patriotic_education", name="Patriotic Education", category="miscellaneous",
         description="A campaign. The next generation is taught who they are.",
         cost_per_turn=4, implementation_turns=1, propaganda_duration=12,
         compass_shift={"economic": 0, "social": 0}, requirements=R(),
         unrest=0.01, opinion=-0.5,
         levers={"indoctrinationPct": 12}),
    dict(id="opposition_smear", name="Opposition Smear Campaign", category="miscellaneous",
         description="A campaign. Whoever was gaining is now explaining.",
         cost_per_turn=6, implementation_turns=1, propaganda_duration=8,
         compass_shift={"economic": 0, "social": 0}, requirements=R(),
         unrest=0.005, opinion=-0.3,
         levers={"indoctrinationPct": 8}),

    # ── Industry and infrastructure ───────────────────────────────────────
    #
    # These exist because industry upkeep had no counterplay until the
    # Industrial Efficiency research branch, and one branch is a single line
    # through the problem. A doctrine is the other kind of answer: cheaper to
    # reach, paid for every turn, and it costs something a player wanted.
    dict(id="rationalisation", name="Industrial Rationalisation", category="right",
         description="Merge the works, close the worst, standardise the rest. Efficient, and somebody's town dies.",
         cost_per_turn=6, implementation_turns=4,
         compass_shift={"economic": 12, "social": -5}, requirements=R(mine=-40),
         unrest=-0.02,
         levers={"industryUpkeepPct": 20, "popGrowthPct": -0.5},
         incompatible_with=["full_employment"]),
    dict(id="full_employment", name="Full Employment Guarantee", category="left",
         description="Everyone works, whether or not the work is needed. Nobody starves and nothing is cheap.",
         cost_per_turn=12, implementation_turns=4,
         compass_shift={"economic": -18, "social": 5}, requirements=R(maxe=30),
         unrest=0.04,
         levers={"industryUpkeepPct": -15, "popGrowthPct": 1.2, "conscriptionPct": 10},
         incompatible_with=["rationalisation"]),
    dict(id="infrastructure_programme", name="National Infrastructure Programme", category="miscellaneous",
         description="Roads, rail, wire. Everything afterwards is cheaper; the bill arrives first.",
         cost_per_turn=14, implementation_turns=6,
         compass_shift={"economic": -8, "social": 0}, requirements=R(),
         levers={"industryCostPct": 20, "resourceModPct": 10, "navySpeedPct": 5}),
    dict(id="agrarian_priority", name="Agrarian Priority", category="miscellaneous",
         description="The countryside first. The nation eats well and builds slowly.",
         cost_per_turn=3, implementation_turns=3,
         compass_shift={"economic": -5, "social": -5}, requirements=R(),
         unrest=0.03,
         levers={"popGrowthPct": 1.5, "resourceModPct": 12, "industryCostPct": -20},
         incompatible_with=["infrastructure_programme"]),

    # ── The fleet ─────────────────────────────────────────────────────────
    dict(id="naval_supremacy_doctrine", name="Naval Supremacy Doctrine", category="miscellaneous",
         description="The fleet is the country's shield and its budget. The army waits.",
         cost_per_turn=10, implementation_turns=5,
         compass_shift={"economic": 5, "social": -5}, requirements=R(),
         levers={"navyAtkPct": 15, "navyDefPct": 10, "navyCostPct": 12, "armyAtkPct": -8},
         incompatible_with=["continental_army_doctrine"]),
    dict(id="continental_army_doctrine", name="Continental Army Doctrine", category="miscellaneous",
         description="Wars are won by men on ground. The admirals are told to economise.",
         cost_per_turn=8, implementation_turns=4,
         compass_shift={"economic": -5, "social": -5}, requirements=R(),
         levers={"armyAtkPct": 12, "armyDefPct": 10, "conscriptionCostPct": 10,
                 "navyAtkPct": -10, "navySpeedPct": -8},
         incompatible_with=["naval_supremacy_doctrine"]),
    dict(id="merchant_marine", name="Merchant Marine Act", category="right",
         description="Subsidise the hulls that carry cargo. They carry troops too, when asked.",
         cost_per_turn=7, implementation_turns=3,
         compass_shift={"economic": 8, "social": 5}, requirements=R(mine=-30),
         levers={"navyCostPct": 15, "navySpeedPct": 10, "resourceModPct": 8,
                 "navyDefPct": -5}),

    # ── War and mobilisation ──────────────────────────────────────────────
    dict(id="war_economy_total", name="Total Mobilisation Economy", category="left",
         description="Everything the country has, aimed at the front. There is no going back cheaply.",
         cost_per_turn=18, implementation_turns=5,
         compass_shift={"economic": -20, "social": -15}, requirements=R(),
         unrest=-0.06,
         levers={"conscriptionPct": 25, "industryCostPct": 15, "armyAtkPct": 10,
                 "popGrowthPct": -1.5, "migrationRate": -20},
         incompatible_with=["consumer_goods_priority", "demobilisation"]),
    dict(id="consumer_goods_priority", name="Consumer Goods Priority", category="right",
         description="Butter, not guns. The country is content and slow to anger.",
         cost_per_turn=5, implementation_turns=4,
         compass_shift={"economic": 15, "social": 10}, requirements=R(mine=-20),
         unrest=0.06, immigration=0.15,
         levers={"popGrowthPct": 1.2, "popModPct": 15, "conscriptionPct": -20,
                 "armyAtkPct": -5},
         incompatible_with=["war_economy_total"]),
    dict(id="demobilisation", name="Demobilisation", category="miscellaneous",
         description="Send them home. The barracks empty and the fields fill.",
         cost_per_turn=0, implementation_turns=2,
         compass_shift={"economic": 0, "social": 8}, requirements=R(),
         unrest=0.04,
         levers={"maintenanceCostPct": 25, "popGrowthPct": 1.0, "conscriptionPct": -25,
                 "armyDefPct": -10},
         incompatible_with=["war_economy_total", "conscription"]),
    dict(id="veterans_settlement", name="Veterans' Settlement", category="miscellaneous",
         description="Land for those who served. Loyal country, and the ledger notices.",
         cost_per_turn=8, implementation_turns=4,
         compass_shift={"economic": -5, "social": 0}, requirements=R(),
         unrest=0.05, opinion=-0.3,
         levers={"popGrowthPct": 0.8, "conscriptionPct": 8, "indoctrinationPct": 6}),

    # ── The state and its people ──────────────────────────────────────────
    dict(id="universal_healthcare", name="Universal Healthcare", category="left",
         description="Care at the point of need. A bigger, healthier, more expensive nation.",
         cost_per_turn=16, implementation_turns=5,
         compass_shift={"economic": -15, "social": 10}, requirements=R(maxe=40),
         unrest=0.06, minority_growth=0.015, immigration=0.1,
         levers={"popGrowthPct": 2.0, "conscriptionPct": 8, "popModPct": -8}),
    dict(id="austerity_programme", name="Austerity Programme", category="right",
         description="Cut until the books balance. They balance.",
         cost_per_turn=0, implementation_turns=2,
         compass_shift={"economic": 18, "social": -5}, requirements=R(mine=-20),
         unrest=-0.08,
         levers={"maintenanceCostPct": 20, "industryUpkeepPct": 15, "passiveIncome": 8,
                 "popGrowthPct": -1.0},
         incompatible_with=["universal_healthcare", "full_employment"]),
    dict(id="civil_service_reform", name="Civil Service Reform", category="miscellaneous",
         description="Examinations, not connections. Slow to bed in, and then it simply works.",
         cost_per_turn=6, implementation_turns=6,
         compass_shift={"economic": 0, "social": 5}, requirements=R(),
         unrest=0.03, pacification=4.0,
         levers={"industryUpkeepPct": 10, "passiveIncome": 5, "indoctrinationPct": -5}),
    dict(id="martial_law", name="Martial Law", category="authoritarian",
         description="The army governs. Order is immediate and it is remembered.",
         cost_per_turn=12, implementation_turns=1,
         compass_shift={"economic": 0, "social": -25}, requirements=R(maxs=20),
         unrest=-0.15, opinion=-0.4, minority_growth=-0.02,
         levers={"armyDefPct": 10, "popGrowthPct": -1.0, "migrationRate": -25},
         incompatible_with=["general_amnesty", "free_press"]),
]


# Pairs of doctrines a country cannot hold at once.
#
# Stated ONCE here and emitted on both sides of the pair below. The doctrines
# above still carry their own incompatible_with lists -- those are the pairs
# that were authored with the doctrine -- but every one of them named the other
# side only once, which made the pair order-dependent for anything that read a
# single direction. Game::policiesConflict now reads both, and this table keeps
# the file honest for anything that does not.
#
# Two kinds of entry:
#   - DUPLICATES. Two doctrines with near-identical levers under different
#     names. Holding both stacks a bonus that was balanced to be taken once,
#     which is the whole of the exploit; these are the pairs that matter.
#   - CONTRADICTIONS. Both are coherent, but no government runs both.
CONFLICTS = [
    # One service gets the budget. naval_supremacy/naval_supremacy_doctrine and
    # continental_army/continental_army_doctrine are duplicate pairs, so all
    # four are mutually exclusive: any two of them stack.
    ("naval_supremacy", "naval_supremacy_doctrine"),
    ("continental_army", "continental_army_doctrine"),
    ("naval_supremacy", "continental_army_doctrine"),
    ("naval_supremacy_doctrine", "continental_army"),

    # Guns or butter. total_war_economy/war_economy_total are duplicates, as are
    # consumer_economy/consumer_goods_priority; only some of the four cross
    # pairs had been declared, so the other two let a country run a war economy
    # and a consumer economy together.
    ("total_war_economy", "war_economy_total"),
    ("consumer_economy", "consumer_goods_priority"),
    ("total_war_economy", "consumer_goods_priority"),
    ("war_economy_total", "consumer_economy"),
    ("total_war_economy", "demobilisation"),
    ("mass_mobilisation", "demobilisation"),
    ("mass_mobilisation", "consumer_goods_priority"),

    # A police state cannot also be an open one. Secret Police and Martial Law
    # had no conflict with anything that grants rights, so the country could
    # hold both the surveillance bonus and the migration bonus it suppresses.
    ("secret_police", "free_press"),
    ("secret_police", "general_amnesty"),
    ("secret_police", "minority_rights"),
    ("martial_law", "minority_rights"),
    ("martial_law", "decentralization"),
    ("national_unity", "minority_rights"),
    ("national_unity", "open_borders"),
    ("national_unity", "decentralization"),
    ("opposition_smear", "free_press"),

    # What the state is for.
    ("deregulation", "worker_rights"),
    ("privatization", "full_employment"),
    ("austerity_programme", "infrastructure_programme"),
    ("austerity_programme", "state_industry"),
    ("autarky", "merchant_marine"),
    ("worker_rights", "total_war_economy"),
    ("worker_rights", "war_economy_total"),

    # What the country is for.
    ("traditionalism", "open_borders"),
    ("traditionalism", "civil_service_reform"),
    ("agrarian_priority", "rationalisation"),
    ("agrarian_priority", "state_industry"),
    ("colonial_office", "minority_rights"),
]


def symmetrise(policies):
    """Emit every conflict on both doctrines, and refuse to write a broken one.

    A pair named on one side only is the bug this closes, so the generator is
    also the place that cannot reintroduce it.
    """
    by_id = {p["id"]: p for p in policies}
    pairs = []
    for p in policies:
        for other in p["incompatible_with"]:
            pairs.append((p["id"], other))
    pairs.extend(CONFLICTS)

    for a, b in pairs:
        for side in (a, b):
            if side not in by_id:
                raise SystemExit("conflict names a doctrine that does not exist: " + side)
        if a == b:
            raise SystemExit("doctrine conflicts with itself: " + a)

    for a, b in pairs:
        for x, y in ((a, b), (b, a)):
            lst = by_id[x]["incompatible_with"]
            if y not in lst:
                lst.append(y)
    return policies


def main(argv):
    out = {"policy_version": 4, "policies": symmetrise([build(dict(p)) for p in P])}
    text = json.dumps(out, indent=2) + "\n"
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "data", "policies.json")
    if "--check" in argv:
        cur = open(path).read() if os.path.exists(path) else ""
        if cur != text:
            print("data/policies.json has drifted from tools/gen_policies.py.")
            print("Run: python3 tools/gen_policies.py")
            return 1
        print("policies.json matches its generator ({} doctrines)".format(len(P)))
        return 0
    open(path, "w").write(text)
    print("wrote {} ({} doctrines)".format(path, len(P)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
