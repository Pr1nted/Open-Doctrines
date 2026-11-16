# Workstream H — Learn from real history

**Status: paused. The two cheap interventions are measured and negative. What
remains is expensive and needs an explicit decision.**

The goal (from `REWORK_PLAN.md`): train the AI on the real *course* of WW1/WW2 so
it plays those situations more plausibly and carries transferable strategic
lessons into ordinary games. The decision there was to **learn the lessons, not
imitate** — real outcomes as the reward signal, real event traces only as a light
exploration seed.

This workstream ran measurement-first, as the LOOP demands. Below is what was
built, what it measured, and why the cheap path is closed. The primary record is
journals **558 / 558b / 559**.

## 1. The ruler (`OD_HIST_SCORE`) — journal 558

The land-share bench cannot see "did this look like WW2", so H needed its own
instrument first. `OD_HIST_SCORE` is a **read-only** env gate in
`Game::runAIEvaluation` (`src/Game_AITrain.cpp`). Inert unless the var is set AND
the current shipped scenario is "1939"; the off path is byte-identical.

It captures each power's **starting** provinces at `t==0` (the scenario's own
geography is the only reference — no external map knowledge is baked in), then
scores 7 coarse, state-representable WW2 checkpoints per turn and prints a
`[HIST] … SCORE k/7` line at map end. The AI is **not** scripted to meet them — it
measures *emergent* fidelity.

| checkpoint | met when |
|---|---|
| `poland_falls` | ≥70% of POL-start is not POL |
| `poland_to_axis` | ≥50% of POL-start → GER or SOV |
| `france_to_ger` | ≥50% of FRA-start → GER |
| `ger_sov_war` | GER↔SOV were ever at war |
| `usa_vs_axis` | USA↔(GER or JPN) were ever at war |
| `uk_survives` | GBR holds ≥70% of its start at end |
| `ger_collapse` | GER rose ≥1.5× start, then fell to ≤1.1× |

Run all-AI:

```
OD_EVAL_MODEL=<model> OD_HIST_SCORE=1 \
  ./build/OpenDoctrinesServer --eval-ai 3 400 <seed> 3 --scenarios
```

(1939 is the 3rd shipped scenario; 1914/1918 play first and are ignored.)

## 2. The baseline — shipped-187, 6 seeds: **2.83/7**

- `poland_falls` and `poland_to_axis` fire **always** — but they are near-
  deterministic from the start state (the GER–POL war is live at turn 0). That is
  the easy half of the ruler.
- **`france_to_ger` 0/6, `ger_sov_war` 0/6, `usa_vs_axis` 0/6.** The three
  historical **sequence** events never happen in emergent play. France is never
  taken by Germany; there is no Barbarossa; the US never enters. Germany either
  dies to 0 (4/6 seeds) or runs away and holds (2/6).

**Those three zeros are the headroom.** Everything H could add has to turn them on
without regressing the normal land bench.

## 3. Reachability — the gaps are structural, not engine-blocked (journal 558b)

A probe (`reach:` line in the same instrument, 3 seeds) asked whether the engine
*forbids* the missing events or the AI simply never chooses them:

- `naps_broken=1` in 2/3 seeds — the AI **can** break a non-aggression pact to go
  to war.
- `usa_wars_end=43/64/3` — the US is at war with dozens of countries; it is **not**
  isolationist.
- `ger_sov_end="none"` — the GER–SOV NAP dissolves but never becomes a war.

So the mechanisms exist. The AI wages plenty of war and even breaks pacts — it
just wages a **generic chaotic land-grab with no historical structure**: no
Axis/Allies alignment, no specific belligerents, no sequence. "The US never fights
the Axis" is not isolationism; Germany and Japan get consumed by the free-for-all
before the US engages *them specifically*. **H is feasible — the wall is the
policy, not the engine.**

Caveat: the late 1939 world dissolves toward chaos (GER annihilated, dozens of
wars), so `usa_wars_end` is likely rebel-inflated in collapse seeds. A historical
arm must impose *structure*, not merely cause more war.

## 4. The cheap intervention — outcome-reward — is negative (journal 559)

The chosen first lever: a historical-checkpoint **terminal reward** at training
time, validated by a bounded A/B on the ruler, not a multi-day retrain.

Machinery (all env-gated, off-path bit-identical):
- `m_histStandingBonus` on `AISystem` (`src/ai/AISystem.h`), folded into
  `m_finalStanding` in `noteMapEnd` (`src/ai/AISystem.cpp`).
- The trainer (`src/Game_AITrain.cpp`) captures the 1939 start geography,
  OR-accumulates the historical-war flags across the turn loop, and at map end
  awards a per-country bonus: **GER** +1 for taking ≥50% of France, +1 for fighting
  the USSR; **USA** +1 for entering against the Axis; **GBR** +1 for surviving.
  Gated on `OD_HIST_REWARD`.
- `OD_HIST_ONLY` forces every training map to 1939 so the reward has density.

**A/B result** (fresh 187 base, 1939-only, same seed/train, reward off vs on;
learned policy benched on the ruler with the reward off at bench time):

| arm | ruler score (3 bench seeds) | Germany peak→end |
|---|---|---|
| OFF (control) | 2, 3, 3 → **mean 2.67** | 35→0, 48→1, 73→0 (rise-and-fall) |
| ON (reward) | 2, 2, 2 → **mean 2.00** | 48→48, 45→45, 43→43 (rise-and-**hold**) |

The reward moved the ruler **down**, and `france_to_ger` / `ger_sov_war` /
`usa_vs_axis` stayed **0/6 in both arms** — it produced none of the events it
rewards. Its only effect was to stop Germany collapsing (rise-and-*hold*), i.e. a
**survival / variance clamp** — the same shape as the LLM surrogate's effect — and
that even cost the `ger_collapse` checkpoint point.

Model protection verified: real `data/ai/model.bin` md5 = `4a137043…` before and
after (training wrote only to isolated `--data` trees).

## 5. Why both cheap levers only clamp variance

This is the recurring wall. The trunk embedding is tanh-saturated
(`[[trunk-embedding-is-saturated]]`, journals 543–545): measured, the
tanh-saturated trunk destroys ~93% of the action signal before the forward model
sees it, so the policy can barely be steered by any bounded, outcome-level nudge.
A terminal reward can lower the *variance* of where a run ends up (don't
overextend into annihilation), but it cannot reach specific mid-game decisions
("declare war on the USSR", "concentrate on France") in a bounded run. The LLM
surrogate (workstream D) hit the identical ceiling: density made it measurable, it
moved play, but broadening and raising its weight were both null on mean land — a
variance clamp, not a mean-mover.

**Conclusion: outcome-reward alone, bounded, from the 187 model, cannot teach
historical structure. H is blocked at the cheap tier by the same net saturation
that closed D and E.**

## 6. What remains — expensive, explicit-decision bets

1. **Stage the real event sequence as a training seed.** Author the WW2 timeline
   via the (currently unused) scripted-events engine — e.g. `wait N turns` then
   `set country.GER.at_war_with.SOV true` — to seed exploration through
   historically relevant states, with the learning pressure still coming from
   outcomes. **Staging must never be scored** (staging `ger_sov_war` directly makes
   the ruler trivially Y); it is measured by benching the *trained* policy with
   staging **off**, and it only counts if it generalises on held-out worlds, not
   the 1939 ruler it was seeded from. Cost: content authoring + a held-out
   generalisation harness.
2. **From-scratch de-saturated trunk retrain.** The one unexplored root cause —
   rebuild the trunk so there is action signal to move, then retry H (and D, and
   E) on top. Cost: days of compute, speculative, and it re-opens every result
   measured against the current trunk.
3. **Bank it and stop.** The ruler, the 187 baseline, the feasibility verdict and
   two measured nulls are a genuine deliverable. Conclude H is blocked by trunk
   saturation at every reachable tier and spend the update elsewhere.

None of (1)/(2) should start without the user picking the direction.

## Instruments left in the tree (all inert unless the var is set)

| gate | where | what |
|---|---|---|
| `OD_HIST_SCORE` | `runAIEvaluation`, `Game_AITrain.cpp` | the 7-checkpoint ruler + `reach:` probe (read-only) |
| `OD_HIST_REWARD` | `runAITraining`, `Game_AITrain.cpp` + `m_histStandingBonus` | the terminal historical bonus at train time |
| `OD_HIST_ONLY` | `runAITraining`, `Game_AITrain.cpp` | force every training map to 1939 for reward density |

Reusable scripts: `scratchpad/hist_baseline.sh`, `hist_reach.sh`, `hist_ab.sh`.
