#pragma once
#include <string>

/**
 * ── PARROTZERO: THE AI'S OWN VERSION, INDEPENDENT OF THE GAME'S ──
 *
 * The game ships as 1.1.2a. That number moves for reasons the AI does not
 * care about (a UI fix, a map, a translation), and the AI moves for reasons
 * the game's number cannot express. Keeping one number for both is how a
 * bench figure ends up attached to the wrong build: this project spent a day
 * labelling binaries v11..v25 by hand and half its confusions came from it.
 *
 * So: ParrotZero <ARCH>.<RULES>.<PATCH>, and a model file is a separate
 * identity again.
 *
 *   ARCH   the network shape and the action space. It IS the model file's
 *          format byte (see AISystem::saveModel), so it cannot drift from
 *          the thing that decides whether a weights file still loads.
 *          Bumping it means old model files are refused, on purpose.
 *
 *   RULES  behaviour a bench can see: a resolver rule, a reflex, a default
 *          that changes what the AI does. Bump this whenever a measurement
 *          taken before the change no longer describes the code.
 *
 *   PATCH  traces, counters, comments, knobs that default to off. Anything
 *          that provably cannot move a number. If you are unsure whether a
 *          change is PATCH or RULES, it is RULES.
 *
 * A trained weights file has its own tag (parrotzero-N24), because "which
 * model" and "which rules" are different questions and today they were
 * repeatedly mistaken for each other.
 *
 * ── RULES HISTORY ──
 *   4.x  districts became something the AI does rather than something it
 *        draws. It passes regional law where a district is in trouble (priced
 *        as a share of GROSS income, because an AI treasury is near zero and a
 *        rule gated on cash in hand never fires); it publishes or withholds the
 *        four figures in its country profile on the same terms the player does;
 *        and it keeps an authored division rather than re-cutting it.
 *
 *        Also two things that make earlier measurements wrong rather than old.
 *        The district reflex was asking getProvinceRebellionChance(pid) -- the
 *        one-argument form, which means "against the PLAYER" -- so every AI
 *        country judged its own ground by another government's unrest. And
 *        relational feature 3 no longer means "their treasury": it means "their
 *        treasury IF THEY PUBLISH IT, otherwise a guess from their industry and
 *        ground". THE SHIPPED MODEL WAS NOT TRAINED ON THAT. It costs 2 rating
 *        points against a frozen policy, with survival and worst seat unchanged,
 *        and 11.1% of relational reads are fogged; OD_AI_FOG_TREASURY_OFF
 *        restores perfect information for a control arm.
 *   3.x  the difficulty profile can gate a RULE (useGuarantorBar,
 *        useSiegeReflex) and both are TRUE on every rung: single-rule
 *        ablations said otherwise, but they were taken with campaigns off
 *        and the signs reverse in the configuration that ships. Also:
 *        defensive campaigns off (they were on by accident, -12), and the
 *        bench records its difficulty rung per label.
 *   2.x  campaigns on by default: a multi-turn commitment against one enemy
 *        country, opened by a reflex, which steers recruitment, reinforcement
 *        and the attack chooser until that enemy is beaten. The first change
 *        to lift the bench floor -- N24 249 -> 265, survival 88 -> 100, worst
 *        seat 28 -> 110. See docs/ai/CAMPAIGNS.md.
 *
 *        THE FLOOR HALF OF THAT LINE IS UNVERIFIED, and left standing only
 *        because deleting a number is worse than annotating it. `worst seat`
 *        is a NORMALISED score, held/par, and the seat that carries it is
 *        1939:NOR:hood in 531 of the 688 runs this project has stored, at a
 *        median of 0.37% of the world. On a par that small the column
 *        multiplies: a later campaigns measurement recorded "-20 worst seat"
 *        which in land is 0.50% -> 0.23%, a quarter of one percent, while the
 *        other five seats GAINED between 0.8 and 4.5 percent. The trade that
 *        number described was mostly the magnifier.
 *
 *        This entry predates the per-seat tables in od_bench_results.json, so
 *        "28 -> 110" cannot be converted to land at all. A worst seat of 110
 *        means the lowest seat held above its par, which on a six-seat bench
 *        means every seat did -- either a remarkable run or a different seat
 *        became the floor, and there is no way left to tell. Read the rating
 *        and survival halves; treat the floor half as unsourced.
 *
 *        See [[floor-not-rating]] in the project memory. The general rule it
 *        arrived at: any floor delta in this project's history is suspect
 *        until it has been re-read as a per-seat land share.
 *   1.x  the state at the end of 2026-09-05: trade and ceasefire rules, the
 *        amphibious doctrine, the embarkation and boat-routing fixes, the
 *        bankruptcy-unrest ramp, the siege reflex, the guarantor bar, depth,
 *        supply, standing battles. Knobs off by default: width-margin gate,
 *        withdraw reflex, call-to-arms reflex, pacification reflex, coalition
 *        bar, crash austerity, mask gates.
 */
namespace ai {

inline constexpr const char* NAME        = "ParrotZero";
inline constexpr int         ARCH        = 8;   // == the model file format byte
inline constexpr int         RULES       = 4;
inline constexpr int         PATCH       = 0;

/// "ParrotZero 8.1.0"
inline std::string versionString() {
    return std::string(NAME) + " " + std::to_string(ARCH) + "." +
           std::to_string(RULES) + "." + std::to_string(PATCH);
}

/// The tag a trained weights file carries: "parrotzero-8.1-N24".
inline std::string modelTag(const std::string& lineage) {
    return "parrotzero-" + std::to_string(ARCH) + "." + std::to_string(RULES) +
           "-" + lineage;
}

}  // namespace ai
