# AI backlog

The loop takes the top unblocked item, one per iteration. Struck items stay,
with a pointer to the journal entry that settled them — a list that only grows
is a list nobody believes.

Ordering rule: **cheap instruments before expensive changes.** Half the wasted
days in this project were changes measured on something that could not have
moved.

## ALSO FOR THE USER — a 206-rating model that fails the robustness guard

`build/loop/model.i14-final.bin` rates **206** against the current best of 162,
with much tighter per-seed spreads, and it regresses `1939:NOR hood` from 49 to
31 on two seeds of three while survival falls 91 -> 88. Journal 14 rejects it by
the rule, which was written before the number existed.

The trade is: large gains on the USA, China and Sweden seats for a worse
outcome on the smallest, most-pressured country. Whether that is the right AI
depends on what it is for, which the seat bench cannot settle.

## ~~FOR THE USER TO DECIDE~~ — DECIDED 2026-09-04: pacts have teeth (journal 33), the book is fixed (journal 34)

**A non-aggression pact has no mechanical teeth** (journal 09/10 side finding).
`declareWar` clears it silently, `break_nap` costs nothing, and there is no
trade, defensive or reputation consequence. The only cost of holding one is that
the AI's own target search makes it break the pact a turn before attacking.

So signing a pact costs the AI a turn and buys nothing, and refusing every pact
is CORRECT PLAY. Reward shaping can only make the AI sign against its interest —
journal 09 did exactly that and paid 54 rating points for it.

Two ways out, and both are yours rather than the loop's:
1. **Give pacts teeth.** Breaking one costs credibility — `m_credibility` exists
   and already reaches the diplomacy head via `CREDIBILITY_WEIGHT`. Note the
   game currently punishes LYING about intentions and not BREAKING A TREATY,
   which is an inconsistency in its own terms.
2. **Give pacts a benefit** — trade, stability, a defensive obligation.

Until one is true, the honest configuration is the one that refuses them, and
the loop should stop trying to make diplomacy pay.

## ~~THE TEACHER IS WORTH CLONING ON ONE SEAT~~ — measured, closed (journals 29b-31)

`OD_SEAT_SCRIPTED=1` scores the hand-written rung on its own scale: 115 overall
against the model's 162 — but **172 on `1914:FRA rush` against the model's
160**. It is the only thing measured tonight that beats the model on a survival
seat. `OD_BC_FROM_SCRIPT` exists for exactly this and is off by default.
Sweep it at LOW weight; wholesale cloning costs 47 points.

**Swept, journal 30 — REJECT at 0.3 and 1.0.** Rush FELL (135 -> 94/115), hood
dropped to the teacher's 23. Whole-policy cloning imports the script's blind
spots (no navy, no staging, weak economy) into seats the model already wins.
Remaining option: per-module cloning, WAR head only.

**War-only measured too, journal 31 — REJECT (rush 93).** The teacher's rush
advantage is emergent from the whole script and does not transfer by cloning
any subset. **Cloning is closed.**

## TARGET (user, 2026-09-04): rating 300 minimum, 400 wanted

Best measured is 157 under the corrected seat score (journal 32c; earlier
figures quoted as 162/206 are ~5 high). The arithmetic says where the remaining
points are, and it
is NOT where the last few iterations were looking:

    seat              par   held   score   held for 300   held for 400
    1914:FRA rung     6.7   16.5     246           20.1           26.8
    1914:SWE rung     1.0    2.8     280            3.0            4.0
    1939:USA rung     5.6   12.1     216           16.8           22.4
    modern:CHN rung   2.5    7.9     316            7.5           10.0
    1914:FRA rush     6.7    9.6     143           20.1           26.8
    1939:NOR hood     1.3    0.4      31            3.9            5.2

**Three seats already clear 300. The rating is being held down by the two
SURVIVAL seats** — `1914:FRA rush` (2.1x needed) and `1939:NOR hood` (9.8x).

So from here the target and the robustness guard point the SAME way: every
remaining point comes from not dying. Growth work is finished — CHN and SWE are
already above what 300 asks of them, and pushing them further is capped at 500
anyway.

**Priority order is now fixed by this:**
1. `1939:NOR hood` — a small country with one relentless neighbour. Worth 45
   rating points on its own if it reaches 300, and it is the worst seat in every
   model measured (7, 62, 49, 31).
2. `1914:FRA rush` — a great power in a world where everyone attacks.
3. Everything else is already sufficient.

400 needs all six at 4x par including Norway holding 5.2% under permanent
attack. Report honestly if that proves out of reach rather than chasing it with
seat-set changes, which would void every stored score.

## Standing direction (from the user, 2026-09-04)

**The complaint is that the AI is a diplomatic pushover** — it accepts
ceasefires, pacts, guarantees and calls to arms it should refuse. Items 1–3
exist for that and stay at the top until the diplo head can be shown to
discriminate. Wider architecture work continues underneath it.

**Budget: unlimited machine time.** A multi-hour training run needs no
permission — but it still goes through `tools/odlock.py` (2 concurrent games)
so the machine stays usable, and it still writes to its own model file, never
`data/ai/model.bin`.

---

## Now

### 2. ~~The diplomacy reward never pays for winning~~ — `diplo-gain-term` — **DONE, journal 05**
**Journal 04 found the cause; this is the fix.** `diploReward` has a dedicated
1.2-weight penalty for provinces LOST and no term at all for provinces GAINED —
gains reach it only through a log-compressed `global`. At 20+ provinces,
refusing a ceasefire and *winning* already scores negative while accepting
scores zero, so accepting strictly dominates. 82/82 is the optimal policy for
that reward.

Add the mirror of `dLost`: provinces gained while at war, weight 1.2, same tanh
shape, gated the way `dLost` is gated. **Then retrain the diplo head** — a
reward change cannot move a frozen model, so a bench without a retrain reads
108 and says nothing.

Guard against over-correction: the call-to-arms fix in this same function had to
be walked back from an 18:1 asymmetry, and the failure mode of over-shooting is
an AI that refuses everything, which is worse than one that agrees to everything.

### 1. ~~Rebalance the agreement terms~~ — `diplo-rebalance` — **DONE (2.5), journal 10**
Kept at 2.5: rating 158, survival 86, alliances 83-85% with a strength split.
5.0 was measured and rejected (rating 106). The knob is `OD_DIPLO_PACT_WEIGHT`
and its default is still 1.0 — **shipping 2.5 means changing that default**,
which is a deliberate act the user should make, not a silent one.

### 1b. What is left on diplomacy
**Journal 08 priced it: 4.7 to 1 against signing a pact.** The journal-05 gain
term (`1.2*tanh(dGained/2)`) is correct and nothing beside it was rescaled, so
every agreement term is now dominated: a pact pays at most +0.19 while the
conquest it forecloses pays +0.91. Refusing everything is the reward working as
written, for the third time in this project.

Sweep the pact/co-belligerent weights up (env knob, default the current 0.6) and
judge on: do pacts come off 0%, AND does the rate MOVE with circumstance —
`to stronger` differing from the headline — while the rating holds near 160.
A rate that merely flips from 0% to 100% is not progress.

### 2. Give diplomacy its OWN trunk — `diplo-own-trunk`
**Demoted by journals 07 and 08.** Detaching cost rating; silencing one kind
broke that kind. Both say the head NEEDS to shape the shared representation, so
"the trunk is the constraint" is not supported. Revisit only if the reward
rebalance leaves rates pinned.
**Journal 07 rejected the cheap version and sharpened this.** Detaching the
diplo head from the shared trunk (`OD_DIPLO_TRUNK_GRAD=0`) cost 9 rating and 24
worst-seat for 3 survival — because it does not give diplomacy a better
representation, it removes its ability to shape ANY representation, leaving it a
linear readout of an embedding optimised for four other heads.

But it PROVED the mechanism: alliances went from a 0% constant to a 44-57% rate
conditioned on the asker's strength (to-stronger 52-56%), the first intermediate
policy any kind has held. Freeing the representation frees a kind to judge.

So: a small diplomacy-only trunk — `{FEATURE_COUNT, ~128, 96}` — that the diplo
head shapes freely, feeding the per-kind pairs, with the shared trunk still read
alongside it (concatenated) so nothing already learned is thrown away. Adds
capacity instead of removing it. New blob in the model file; follow the
`replicateOutputBlocks` precedent so old files still load.

### 1b. ~~Detach diplomacy from the trunk~~ — REJECTED, journal 07
**Promoted by journal 06.** The per-kind head works — calls to arms now hold a
different policy from everything else — but ceasefires, pacts and alliances are
still pinned at 0%. All seven pairs read ONE embedding, and the diplo head
backpropagates into it while ceasefires supply ~73% of its samples, so one
question reshapes the representation the other six are answered from.

`OD_DIPLO_TRUNK_GRAD` is built and defaults to 1.0 (old behaviour). Sweep 0.0
and 0.25 against the journal-06 recipe and read whether pacts come off 0%.
If detaching helps, the fuller version is a small diplomacy-only trunk.

### 2. ~~Give the diplo head capacity~~ — `diplo-capacity` — **DONE, journal 06**
**PROMOTED TO TOP by journal 05, and no longer speculative.** With the reward
corrected the head collapsed to refusing ALL FIVE request types — including
non-aggression pacts, which transfer no territory and therefore cannot have been
moved by a territorial reward term. One `{320,2}` linear map cannot hold "refuse
ceasefires while winning, still sign pacts". Sweden pays for it: 210 -> 10.

Take variant (b) from the original item — type-conditioned output,
`{320, 2 * OFFER_KINDS}` indexed by the request — so the head structurally
cannot smear one type's policy over another. NOTE this changes the model file
layout: `model.loop-base.bin` and every league checkpoint carry a `{320,2}`
diplo blob, so the loader needs to handle both or the reset path must rebuild
the head. Check `serialize`/`readBlob` before writing any of it.

Retrain exactly as journal 05 did (reset the head, `--worker N --workers M`,
frozen snapshot before benching) and judge on SWE recovering without giving back
FRA/USA/CHN.
**Now the top item.** Journal 03 settled the cheap route: a hand-written floor
on the ceasefire answer does NOT help, and its apparent wins were single-seed.
The saturation is real (82/82) but it cannot be fixed from above the head --
forcing `refuse` on a policy sitting at ~1.0 measures worse than the idea is
(`mask-changes-need-a-retrain`). So the head has to learn to discriminate.
`m_diplo = NeuralNet({TRUNK_OUT, DIPLO_ACTIONS})` is a single linear map from
the shared 320-wide trunk to accept/reject, serving **every** offer type. The
type arrives only as a one-hot in the trunk's *input*, so type-conditional
behaviour has to survive the trunk, which is optimising for four other heads.

Two variants, one per iteration:
- (a) a hidden layer: `{320, 96, 2}`;
- (b) type-conditioned output: one accept/reject pair per offer type,
  `{320, 2 * OFFER_TYPES}`, indexed by the request — the head then *cannot*
  smear one type's policy over another.

(b) is the more interesting architecture and the cheaper claim to test.
Both need the head retrained (`LR_DIPLO = 0.002`; the head is small and trains
fast, the trunk need not move).

### 3. ~~Accepting has no price~~ — `diplo-cost` — **SETTLED, journal 04**
Check credit assignment before adding capacity: `m_diploValue` is a separate
`{143,160,1}` critic and the diplo sample is recorded on `MOD_COUNT` with its
own `diploFeatures`. Verify the *return window* attributed to an accept is long
enough to contain the consequence. A pact that costs you a war fifteen turns
later cannot be learned from a five-turn window, and a head that never sees the
cost will accept everything — which would explain the complaint without any
capacity problem at all.

### 4. The league contains no rusher — `league-exploiter`
**Design settled by reading, 2026-09-04 — this is now an implementation task.**
`loadLeagueOpponent()` (AISystem.cpp ~9451) does PFSP over `league-{0..5}.bin`
beside the model, weighting each slot by how badly it beats us
(`lossRate^2 + 0.05`). Every slot is a past checkpoint of the SAME lineage, and
the loader hard-requires the `ODLG` header, so no scripted opponent can occupy
one. That is the whole reason the policy drifts off its rush defence: nothing in
the pool ever plays a rush.

The machinery to fix it is already there and needs no new concept:
- `m_scriptedThisCountry` (AISystem.cpp:2286) is already a PER-COUNTRY switch,
  currently `isRandomCountry(cid) && s_scriptedControl`.
- `s_exploitVariant` already selects SCRIPT_BLITZ (3) and the other hand-written
  exploits, and `--vs-exploit` already drives it.

So: add a virtual slot to the PFSP draw meaning "the exploiter". Read the rest
of the path 2026-09-04; the specifics are:

- `loadLeagueOpponent()` weights `present` slots by `lossRate^2 + 0.05`. Draw
  over `present.size() + 1`; if the extra wins, set `m_leagueIsExploiter = true`,
  set `m_leagueLoaded = true` **without loading a file**, and return true.
  `m_leagueLoaded` is what gates `assignLeagueCountries()`, so it must be set or
  no countries are handed over.
- `assignLeagueCountries()` needs no change: it only picks WHICH countries.
- At AISystem.cpp:2286, `m_scriptedThisCountry` currently reads
  `isRandomCountry(cid) && s_scriptedControl`; add
  `|| (m_leagueIsExploiter && m_leagueCids.count(cid))`, and make sure
  `m_leagueThisCountry` is FALSE in that case so nothing reaches for
  `m_leaguePolicy`, which holds no weights on an exploiter map.
- Set `s_exploitVariant = 3` (SCRIPT_BLITZ) for the map.
- `s_leagueGames`/`s_leagueLosses` are `[LEAGUE_CHECKPOINTS]`; the exploiter
  needs its own index, so size them `LEAGUE_CHECKPOINTS + 1` and give it the
  last slot. Without that its win/loss record is either lost or stolen from a
  real checkpoint.

It then carries the same bookkeeping as every real slot, so PFSP plays it more
often exactly while the policy is bad at it — which is the property wanted.

**Note the urgency has DROPPED since journal 06.** The per-kind head already
took `1914:FRA rush` from 59 to 213 and `1939:NOR hood` from 54 to 62, so rush
defence is no longer the standing weakness this item was written for. Worth
doing, no longer the emergency.

Do NOT simply set it to a fixed share: the point of PFSP is that the pressure
follows the weakness.
Self-play erodes rush defence: five hours of training beat its own predecessor
at every merge while losing 7 points of land against a rusher on 5 worlds of 5.
The merge guard now *rejects* such a merge (`_vs_exploit`), which stops the
bleeding but wastes the run.

Fix the cause: make `SCRIPT_BLITZ` (variant 3) a permanent league member so the
policy is trained against it rather than merely screened against it. Check
first how `m_league*` opponents are sampled and whether a scripted opponent can
occupy a league slot at all.

Expensive — needs a real training run to demonstrate — so it goes after the
cheap diplomacy work, but it is the highest-value structural item on the list:
it is the mechanism that has been quietly undoing other improvements.

### 3. Actions a bias cannot reach — `dead-action-retrain`
Journal 12: `artillery` is inert at +60 while `stage` responded to +5, though
both print P = 0.0. The shape table rounds to one decimal and hides the
difference between ~4% and ~1e-30. Reaching the far ones needs a retrain
(`--reset-ai-head war`), not a thumb on the scale — and the war head is this
model's main strength, so weigh that before spending it.

Cheap first: print the policy shape with more precision, so "dead" can be
separated from "rare" without a 20-minute sweep.

## Later

### 5. The stance head holds one opinion — `stance-diversity`
Consolidate 99.5% of country-turns, so `STANCE_BIAS` is pinned to 0.0 and a
whole wired posture mechanism does nothing. A posture that never changes is a
fixed offset, not a posture. Try an entropy floor on the stance head, or
features with a longer time-constant than the ones it currently sees; raise
`STANCE_BIAS` off zero only once the eval shows a real distribution.

### 6. Eight of twelve economy actions are dead — `econ-retrain`
`fort`, `port`, `specialize`, `destroyer`, `carrier`, `fund down`, `focus bldg`,
`focus navy` all sit at exactly 0.0. Known: reallocating what it already spends
has been tried four ways and every one lost land, and a mask cannot make a
collapsed head act. What has *not* been tried is re-initialising the head and
retraining it with an entropy floor, trunk frozen.

### 7. Instrument: does the rating see leader mechanics at all — `bench-blind-spots`
`--vs-script` ADVANTAGE is blind to anything that acts on the winner. Write down
which mechanisms the seat bench can and cannot separate, so items are not queued
against an instrument that cannot answer them.

---

## Settled

- **`diplo-audit`** — done, journal 02. The pushover is one offer kind:
  82/82 ceasefires accepted vs the rung's 31%. Counters shipped.
- **`ceasefire-rate`** — REJECTED, journal 03. A floor on the ceasefire answer
  costs the USA seat consistently and every gain was one seed of three. Reverted.
- **headless bench** — done, journal 01. Ratings run windowless in ~3 min.
- **`diplo-capacity`** — KEPT, journal 06. One accept/reject pair per request
  kind: rating 108 -> 160, survival 68 -> 87, worst seat 7 -> 62, both guard
  seats up. Calls to arms hold a different policy from the rest; ceasefires and
  pacts do not yet, which is item 1.
- **Open question from journal 06:** what actually restored Sweden (10 -> 120)?
  It was NOT the return of pacts — those are still at 0%.
- **`diplo-trunk` detach** — REJECTED, journal 07. Removing capacity is not the
  same as freeing it. Superseded by item 1.
- **NEW, and it undercuts an assumption in journals 02-06:** is 0% ceasefire
  acceptance actually WRONG? A country asking for a ceasefire is evidence it is
  losing, whatever its army totals say. 100% was demonstrably bad; nothing
  measured so far shows 0% is. **Needed: a seat where the AI is LOSING**, to see
  whether accepting is what saves it. The current six seats cannot answer this —
  the model wins or holds on five of them.

---

## THE LONG ROAD — what "consistently beats humans" would take (asked 2026-09-04)

Ordered by leverage per unit of work, cheapest gates first.

**1. Exploration, first because everything else needs it.** Measured H = 0.000
at temperature 1 on every diplomatic head; `artillery` sits at 1e-33. A
saturated policy cannot be improved by search — there is no distribution to
reweight — and cannot discover an action it assigns zero to. Per-head entropy
floors (the global `PPO_ENTROPY` was correctly measured and kept at 0.01 for the
WAR head; that says nothing about a binary head with two orders of magnitude
fewer samples). Cheap, and it gates items 2 and 4.

**2. Outcome-based objective, shaping as auxiliary.** `diploReward` has ~10
hand-weighted terms and THREE of them were found mis-tuned in one session
(journals 04, 08, 09). A hand-shaped reward caps the ceiling at the quality of
its weights. Train on the outcome; keep the shaped terms as an auxiliary head.

**3. League with exploiters.** Design already recorded above. Self-play without
one measurably erodes rush defence while every merge passes the guard.

**4. SEARCH — the defining gap, and the largest job.** `searchDepth` is 0 at
every difficulty. Depth 2 was tried and lost (65.9% -> 62.8% of land at 8.5x
think time) "because the Q head already estimates what the search re-derives" —
a sound verdict on SHALLOW search, not on search. Chess branches ~35; this
branches into the thousands across countries and provinces, so the unlock is
MCTS over an ABSTRACTED action space (plans and stances, not raw actions) rolled
forward with the existing `m_dynamics` model. Everything Stockfish-like lives
here.

**5. Compute.** ~5,000 turns per 30-minute run on one core; AlphaZero-class is
millions of games. Isolated data dirs bought 3-4x tonight. The rest is hardware.

**6. NOT AN AI PROBLEM: make the game worth mastering.** A pact has no teeth,
and the script declares war on two soldiers. Where mechanics do not reward good
play, "superhuman" means exploiting degeneracies. Stockfish is superhuman at
chess BECAUSE chess is tight. Tightening diplomacy probably buys more apparent
intelligence than any training run on this list.

**Feasibility, honestly.** Unbeatable-by-anyone is a multi-year, large-compute
project. "Beats a strong human most of the time" is reachable on the ordering
above. Do not report progress toward it by weakening the scripted rung — that
lowers the ruler rather than raising the AI, and voids every stored score.

### Exploiter share must be CAPPED — settled by journal 20
`OD_LEAGUE_EXPLOIT` now takes the cap as its value (0.25 default; a bare `1`
reads as "on at the default", not "always"). Uncapped, PFSP gave the rusher 7
maps of 8 and the run scored 102 against the 162 it started from — because PFSP
weights by loss rate and a policy that never masters a blitz keeps that rate
high forever, so the weighting keeps feeding it. A league that is 88% one
opponent is not a league.

## 2026-09-04 evening
- DECIDED: opening-book bloc rule kept (free on the champion, −10 on the shipped model; journal 35c).
- DECIDED: per-kind collapse guard + diplomacy uniform pull (journal 35, 35a). Training-only.
- OPEN: ceasefire kind swings to always-yes once unfrozen (journal 35c). Candidates: pact weight (N5 running), longer training (N4 running), or a war-state-conditional agreement term (accept pays only when the signer is behind).
- OPEN: NAP kind at H 0.67 — undecided, 17% yes; the script now keeps its word, so the correct rate is higher than that.
- DECIDED (user): trade rules — never cede land at a loss; auto-accept gifts and clearly favourable deals (journal 35f).
- OPEN (user's call): ceasefire terms have no net floor and no truce follows a ceasefire; a negotiating model can be farmed declare→terms→re-declare.
- DECIDED: ceasefire rules A/B (accept costless peace while losing; refuse unpaid peace while winning), verified on the champion (journal 35g). Ruler → v6.
- TOOL: `OpenDoctrinesServer --probe-trade <seat> [--seed N] [--data D]` asks one AI country seven crafted trades and prints PROBE_OK/FAIL; the only way to exercise the trade rules (no eval proposes trades).
- MEASURING: search at play (OD_MCTS_SIMS=4/8) on the champion, v6 ruler — quick pair hinted +15 at +43% time.
- CLOSED: search at play — 4/8 sims = greedy prior = −32 (journal 35h). Train-with-search / ship-without stands.
- OPEN: play temperature — the sampled policy beats its own argmax by 32; T=0.18 is untested against neighbours. Bench the champion at two other temperatures on the v7 ruler.
- DECIDED: ceasefire rule B scoped to winners with a claim on the asker (journal 35h).
- OPEN: navy head — ~2,000 embarks per training map, 0 hostile landings (journal 35l). Check what `embarks` counts; if armies sit at sea, that is dead army.
- FINDING: two-map runs collapse (N4 88, N8 107, N10 81) while one-map runs hold (139–165); map 2 was one fixed continents world. N11 (ladder step) and N12 (new seed) running to attribute it.
- CONFIRMED SINK: 0% of ~9,000 embarkations per eval reach a hostile shore; half a port garrison per order leaves the map (journal 35m). Measuring with OD_NAVY_BIAS embark=−20; then a mask rule or a resolver fix.
- TEST CASE: training map 2 of base seed 20260910 (continents seed 1551539546, 37 countries) wrecks any model trained on it (81–107); a near-identical world on seed 20260912 gives 169. Find what happens in that game.
- RESOLVED: "training degrades past ~1,000 turns" was that one world (journal 35n). Length is open again; N13 (3 maps, incl. a shipped Earth map) and N14 (ladder from N12) running.
- NOTICE (2026-09-04 21:2x): economy/combat rule changes planned in docs/design/community-roadmap-2026-09.md (plan only). Reference frozen in build/loop/reference/. When Phase 1 lands: re-baseline (v8), retrain the economy head, and expect MAX_MODULE_ACTIONS to widen to 16 at Phase 2 — plan the TrainStats/buffer widening once.
- DECIDED (for the roadmap's action-space widening): when MAX_MODULE_ACTIONS widens, RESET m_dynamics and let DYN_WARMUP_UPDATES re-earn it — the dynamics net's input one-hot is strided by MAX_MODULE_ACTIONS (AISystem.cpp dynamicsInput), so a width change re-indexes every module's block and deserialize's zero-fill cannot see it. Search at play is closed anyway (journal 35h), so nothing shipped depends on that net. Do the widening once, to 16, with GUARD_HEADS/m_marginal*/Experience::visits following.
- OWNERSHIP: tools/od_bench.py readouts for the production economy (goods, living standards, idle factories) — written by THIS loop when Phase 2 exists, so the bench's scoring semantics stay in one hand.
- SCHEMA (from the roadmap session, for od_bench readouts when the game emits them; keyed by good id so 2-good vs 4-good differs in rows only):
    per country per turn: goods[<id>].produced / consumed / demand / stockpile (float); livingStandards (float, 0..1+, consumer good supply/demand); factoriesIdle, factoriesTotal (int)
    good ids: 4-good = consumer, machinery, fuel, munitions; 2-good = consumer, war_material; `consumer` is the living-standards good in both
    Phase 1 (first): industryCapacityUsed, industryCapacityTotal, industryOvercapProvinces (int, summed over owned provinces) — wire these into od_bench the day Phase 1 lands, before benching it, so a change in the economy head's industry take-rate is visible as such.
- WAITING ON THE TREE (2026-09-04 21:2x): embark-off bench (OD_NAVY_BIAS=0,0,0,-20,0,0,0) and the boat-turn diagnostic eval need a rebuild; blocked until the roadmap session finishes Phase 1 in the shared files. Then: v8 re-baseline (shipped, champion, N12), wire [CAPACITY], and resume.
- PRIORITY after the tree settles: fix the embarkation sink (mask embark unless a landing is actually possible, or a resolver fix in sailing/landing). Evidence: continents/Earth training rounds lose rating 3 of 4 times (journal 35r); 0% landings (35m). Bench with OD_NAVY_BIAS embark=−20 first (queued, needs a build).
- DESIGN (embark sink, ready for when AISystem.cpp is free): a landing needs the boat within shipMaxRangePx (200 px for a boat) of the TARGET PROVINCE CENTRE (processShipDisembarks), but the boat sails to portApproach(pid), a sea point by the port; for any coastal province whose centre is more than 200 px from its approach point the landing test can never pass, so the loaded boat re-issues the same sail order every turn and parks forever (the `boatsParkedOutOfRange` counter measures exactly this). Fix in two places: (1) `bestEmbarkPort` embarks only if some at-war port on the same sea body is LANDABLE (its centre within shipMaxRangePx of its approach point, and navReachable); (2) the automatic landing routine picks the nearest LANDABLE enemy port, not the nearest one. Verify with the counter (parked → 0) and the eval's amphibious line (landings > 0), then bench.
- READY FOR THE NEXT BUILD: STAGNATION_TURNS 400 → 80 in the training loop (journal 35t). Then re-run the ladder step from the best model with two seeds; the sibling spread should collapse if frozen turns were the tail.
- v8 PLAN (revised, journal 36a): no widening/reset at Phase 2. (1) OD_GOODS unset = the v7+P1 gate, done. (2) goods on at OD_AUTOSELL_PCT 100/60/0 on shipped/champion/N11. (3) sweep GOOD_PER_LEVEL, EXTRACT_PER_AMOUNT, CONSUMER_PER_CAPITA, RAW_FLOOR_PRICE (GameStructs.h; hand-tuned, undefended). (4) then retrain from the best under the chosen economy. [LIVING]/[GOODS] readouts are wired in od_bench.
- RESOLVER FIX (after Phase 2; Game_TurnLogic.cpp): embarkation — find or spawn the boat BEFORE removing men; transfer in whole crews (n/100) and leave the remainder in the province; if no boat can exist, cancel the order and keep the men; count `m_navMenDeleted` and print it in the [EVAL] amphibious line. Then AI mask: embark only if a landable at-war port is reachable. Then re-bench (the noembark 151 vs control tells how much the leak costs today).
- DESIGN (Game_AITrain.cpp, after Phase 2): replace the raw stagnation count with a freeze test that cannot fire on a mid-war lull — rotate when (no province changed hands between real countries for N turns) AND (no war between real countries is in progress, or N ≥ 400). Keep OD_STAGNATION_TURNS for the A/B. Decide after N20 (paired 400 control on N18's world).
- WRITTEN, NOT BUILT (journal 36c): processEmbarkations boat-first rewrite + three counters. Build after the goods sweep and the roadmap session's review; then re-gate (v8). AI embark mask deferred: measure landings after the resolver fix first (bestEmbarkPort already requires a hostile port on the same sea body).
- REPORTED (journal 36d): goods economy consumer demand ~76× production over a 120-turn bench (living standards 0.36, 1% of demand met); machinery/fuel/munitions have zero demand before Phase 4. Do not tune rates until the roadmap session fixes the denominator.
- SEQUENCE AGREED: sweep → embark fix build → v8 gate (shipped/champion/N11, goods off) → tell the roadmap session → its army-maintenance fix (maintenanceCostPct applied nowhere today) → re-gate.
- LADDER: six steps from N11 all down (three under the discredited 80 rule). Next tickets: from N11 at 400 on the v8 binary once the embark fix is in (training worlds stop leaking men); also one from the champion as a lineage control.
- ASKED (roadmap session, Game_Loading.cpp): call buildNavGrid() whenever the land/sea raster loads, not only when ships.json exists — generated training worlds have no nav grid, so no embarkation can sail there (journal 36i).
- v8.1 = v8 + port-table coastal gate in processEmbarkations; re-gate queued. After: ladder tickets on v8.1 with two seeds.
- WRITTEN, NOT BUILT: auto-landing targets landable ports only (journal 36k). Build as v8.2 after the v8.1 gate + N22/N23 benches; expect "parked" to fall and landings to rise above 9%.
- NEW REALITY (v8.1, journal 36l): the rung lands 9% of its embarkations; coastal seats collapse (SWE 0, CHN 59, NOR 15 for the champion). Retrain on worlds with a nav grid once the loader builds it unconditionally; the first tickets on v8.1 are the baseline of "never saw a landing".
- BALANCE QUESTION for the user/roadmap: the scripted cohort embarks 100% of the time it can; with landings real, is that the intended amphibious tempo?
- v8.3 = v8.2 + amphibious doctrine (1+3 all cohorts, 2 script only) + loaded-boat routing counters. Re-gate; then tickets from N11 on v8.3.
- LATER TICKET (goods on): can the politics head learn the compass as an economic lever (Phase 3 needs no new action)? Bench with OD_GOODS=1 once the economy is tuned.
- OPEN (needs AISystem.cpp, closed for Phase 4): loaded boats show 1,273 "parked" boat-turns while the router counts 0 arrived / 0 stuck for them — the only other erase site is the reflex's own re-target (AISystem.cpp ~6136), so either the reflex erases and re-pushes every turn (target flipping between near-equidistant ports) or orders never reach the router. Decisive instrument: an aiDebug line per loaded boat per turn (index, crew, enemyPid, enemyD vs LAND_RANGE, has-order, sailingTo). Add when the file reopens; run 40 turns; read.
- SWEEP WHEN GOODS-ON IS BENCHABLE, IN THIS ORDER: first GOOD_OUTPUT_SCALE and EXTRACT_SCALE (GameStructs.h — they set how much a country can make at all; every Phase 4 rate only divides that output, so a Phase 4 sweep against unswept Phase 2 rates measures the pair), hold them; then the Phase 4 rates in BuildCosts.h (ARMY_FUEL_PER_10K, RECRUIT_MUNITIONS_PER_10K, FUEL_SHORTFALL_PRICE, per-shell fuel/munitions, the two reserves). Three seeds, six seats, READ PER SEAT: living standards, army size and fuel are coupled through population, so a rate balanced on a peaceful seat can be ruinous on a besieged one (Sweden, China).
- FIXED (journal 37a): navy action 4's "unload home" fallback emptied every loaded boat a turn after loading. v8.4 re-gate running. After it: read landings %, parked boat-turns, "came home"; expect the rung's invasions to become real under the doctrine.
- Model of record: N24 (169 on v8.3). Tickets N26/N27 from N24 on v8.4.
- NEXT AT SEA (after Phase 6): 158 loaded-boat orders erased as stuck per 80 turns — read the router's stop-at-coast branch for a boat on a straight fallback leg; the nav route may be failing where navReachable said yes.
- STUCK BOATS, diagnosis plan (Game_TurnLogic.cpp, after Phase 6): navRoute fails only for no grid / no start or goal cell / different components / no path; on failure the router falls back to a straight leg, stops at the first coast, and next turn moves 0 → "stuck" → erased → the reflex re-pushes the same order. Add an OD_BOAT_TRACE line at the planning step printing which of navRoute's four exits fired (start cell, goal cell, component, no path). Likely the start cell: a boat that stopped on a coastal pixel may sit outside navCellNear's radius.
- WRITTEN, NOT BUILT: validNavy's "land" is valid only when a hostile port is within a loaded boat's range, matching the executor (mask/executor parity). Goes into the post-Phase-6 build.
- DONE: boats sail and land (resolver boat-first + port-table gate + "land" fallback removed + mask parity); v8.4 gate 121/116/160/147. Ruler now v9 (Phase 6 width). Stuck-order trace in the v9 check.
- ROADMAP COMPLETE (all six phases, roadmap session). Next for this loop: v9 gate → tickets from the v9 model of record; goods-on benchability (Phase 2 scales sweep first).
- STUCK BOATS, still open: the two-cell short hop never fires (v9.1 check identical). Next instrument: at the stuck erase, print the leg length, route size and whether the first step pixel is land — the zero-move leg may be long, or not the first. Game_TurnLogic.cpp, after Phase 5.

## NEXT (as of 2026-09-11) — in order

0. ~~TRAINING DETERMINISM~~ **RESOLVED journal 269: pin OD_AI_THREADS.**
   Training is deterministic given a fixed thread count -- 3 of 3 identical at
   8 maps pinned, 3 of 3 identical at 8 maps default, and the two arms differ
   from each other. learningThreads() is cores-minus-one scaled by the resource
   limiter, so an unpinned count moves between runs and changes float
   accumulation order in the gradient reduction. That is where journal 263's
   "sd 112" came from. **Every training experiment from here sets
   OD_AI_THREADS, and then costs ONE run per arm instead of 10-40.**
   Unresolved and recorded: one divergence in twenty pinned one-map runs
   (journal 267) that this does not explain.

1. ~~Re-settle what journal 263 retracted, now that it is cheap.~~ **ALL THREE
   SUB-ITEMS SETTLED (journals 270, 271, 272).** Parent struck journal 303 --
   it had read as open work since 272. With threads
   pinned, one run per arm suffices, but bench BOTH hold-out sets -- the
   remaining uncertainty is the seed sample, not the training.
   - ~~scripted-opponent "+82"~~ **SETTLED journal 270: REJECTED.** Set C +74,
     set D -72. Opposite signs at equal magnitude. The original +82 was set C
     alone; set C still gives +74, so the number reproduced and the
     generalisation never existed. The fix stays (the variant mix had never
     executed) but claims no play benefit.
   - ~~"89% of parent"~~ **SETTLED journal 271: the figure was against the
     WRONG BASELINE.** It compared to N24 (433) on set C; the loop's declared
     reference data/ai/model.loop-base.bin scores 14 (set C) and 23 (set D),
     and eight maps of training takes it to 159/178. **Training WORKS from the
     declared reference -- 7-11x, both sets agreeing.** "Training degrades the
     model" is parent-specific: strong parent pulled down (N24 433 -> 305),
     weak parent pulled up (loop-base 14 -> 159). **Every journal from 257 to
     270 trained from N24, not from loop-base. Use the declared reference.**
   - ~~the 8/16/24-map curve~~ **SETTLED journal 272: NON-MONOTONE, both sets
     agreeing.** From loop-base: parent 14/23, 8 maps 159/178, 16 maps 100/108,
     24 maps 159/158. Longer is NOT better (24 = 8 for 3x the compute) and
     there is no peak. Recipe: **train 8 maps and measure**; re-measure if the
     length changes. Also: an oscillating curve means any early-stopping or
     bench-gated scheme is sampling a wave. Shape is parent-specific -- it does
     not match journal 259's curve from N24 -- which retires LOOP.md's standing
     "train ~1,000 turns" note as a general claim.

2. ~~Does training help the SHIPPING model?~~ **SETTLED journal 273: NO, both
   sets agree.** N24 433/398 -> trained 379/272 (-55, -126). Training moves a
   model toward a middle band: loop-base 14/23 -> 159/178 (up), N24 433/398 ->
   379/272 (down). Parent information survives (159 vs 379 are far apart) but
   the SIGN is set by where the parent sits. **Training is a bootstrap for weak
   models, not a way to improve the release model.** Improving N24 needs either
   a recipe that holds a strong parent up (the journal 262 anchor, never tuned
   past its first value of 0.5, which was far too strong) or rules -- where this
   project's one shipped win came from.

   ORIGINAL: **Does training help the SHIPPING model?** Everything settled in 270-272
   was measured from loop-base, which scores 14/23 -- training multiplies a
   weak model. The model that actually ships is N24 (data/ai/model.bin,
   md5 4a137043), and journal 259 suggested training pulls a strong parent DOWN
   (433 -> 305) but read one seed set from an unpinned thread count. Re-test it
   properly: 8 maps from N24, threads pinned, both hold-out sets, with N24's own
   baseline measured on the same seats and seeds. This is the question that
   decides whether training is usable for the release model at all.

3. ~~Tune the anchor~~ **SETTLED journal 274: REJECTED as built.** From N24:
   no anchor 379/272, K=0.05 70/112, K=0.15 31/42 -- monotone in K on both
   sets, ~300 points lost at the gentlest setting. The pre-registered reason
   fits: the anchor pulls toward the LEAGUE CHECKPOINT, which training rewrites
   as it goes, so it drags the policy toward a degrading copy of itself.
   **This rejects this anchor, not anchoring.**

4. ~~Anchor to a PINNED target.~~ **SETTLED journal 277: REJECTED.** Pinned
   0.05 gives 82/213, pinned 0.15 gives 49/6, against 379/272 unanchored and
   433/398 for the parent. Pinning beats the league target on 3 of 4 cells, so
   journal 274's diagnosis was partly right -- and every anchored arm is still
   catastrophic. More anchor is worse in both families on both sets, which
   indicts the MECHANISM: a fixed-weight cross-entropy pull in the same batch at
   the same learning rate drowns the learning signal when strong enough to
   constrain forgetting. A trust region on the STEP, or a divergence-scaled
   penalty, are different mechanisms and untested. **This construction is closed.**

   HISTORICAL, journal 276: no converter needed --
   loadOpponentModel() fills the anchor's nets from an ordinary model file, and
   OD_ANCHOR_MODEL now reaches it from training (87,578 pulls against a pinned
   N24). Journal 275's zero was self-inflicted: hand-seeded ODAZ files made
   loadLeagueOpponent reject the slots and load nothing. READY TO BENCH.

   SUPERSEDED NOTE, journal 275: a hand-copied model.bin
   is NOT a valid league file -- models are ODAZ, league checkpoints are ODLG,
   and the loader rejects the former by name. Pinning needs code that serialises
   the parent into ODLG. Also note OD_SAVE_INTERVAL does NOT stop checkpoints:
   writeLeagueCheckpoint() has a second, ungated caller on map rotation
   (AISystem.cpp:1499), so seeded slots get overwritten regardless. Both must be
   handled for a pinned anchor to be testable.

   ORIGINAL: **Anchor to a PINNED target.** Seed the league slots with N24 and set
   OD_SAVE_INTERVAL (committed 1062e85) high enough that training never
   overwrites them, so the pull is toward the parent for the whole run. Open
   question first: is a hand-copied model.bin a valid league file? Journal 262
   saw no firing after hand-seeding, but that was the unsized-scratch bug,
   since fixed -- so verify the anchor fires before spending a bench on it.

   ORIGINAL: **Tune the anchor, the one idea aimed at holding a strong parent up.**
   Journal 273 settled that training degrades N24 (-55, -126 on the two sets).
   The anchor built in journal 262 -- a cross-entropy pull toward the frozen
   net's distribution, in the same batch at the same learning rate -- exists for
   exactly this and was tried at ONE value, K=0.5, which destroyed the model
   (37 vs 433). Everything below 0.5 is untested. Sweep small values from N24,
   threads pinned, both hold-out sets, against the known unanchored result.

5. ~~Short steps from the SHIPPING model.~~ **SETTLED journal 278: 2 maps is
   **RETIRED BY JOURNAL 363 -- read before trusting anything below.** Journal
   278's two-map "parity" (433/404 against 433/398) was three seeds per set on
   the headline statistic. Re-measured at eight seeds on the per-seed
   statistic, same recipe as journal 357: **N24 336 -> 223, -113, CI [-203,
   -23]**. Two maps is destructive, and indistinguishable from eight.
   PARITY, not improvement.** From N24: 1 map 343/296, 2 maps 433/404, 8 maps
   379/272 -- non-monotone, like the loop-base curve. Two maps is the only known
   non-destructive step from a strong parent, and the +6 on set D is a sixth of
   the 35-point spread the SAME frozen model shows between sets, so it is not a
   gain. The seat profiles differ from the parent's in both directions and the
   two sets disagree about which seats move: a reshuffle, not a lift. **Do not
   build a ladder of 2-map steps on the +6** -- that compounds noise, which is
   what journal 261's ladder was already doing.

   ORIGINAL: **Short steps from the SHIPPING model.** Every length measured from N24
   under the fixed method is 8 maps or more, and all of it degrades. Journal 272
   showed the length curve from loop-base is NON-MONOTONE, so "8 degrades" does
   not imply "1 degrades". Journal 258 did look at 1 map from N24 and found
   France destroyed, but that was unpinned and set C only -- exactly the two
   flaws that invalidated five other claims. Measure 1 and 2 maps from N24,
   threads pinned, both hold-out sets. Cheapest untested region of the space.

6. ~~Re-test the research-bar optimum on set D.~~ **SETTLED journal 280: THE
   SETS DISAGREE.** set C peaks at 0.45 (435), set D peaks at 0.65 (440);
   going 0.45 -> 0.65 is -63 on C and +91 on D. The optimum is NOT established
   at three seeds per set, and journal 246's rejection of 0.65 (-19, set C) was
   single-set too. **Does not affect the shipped change** (austerity ordering +
   siege gate, +71/+42 on TWO sets, journal 239) -- 0.45 is the pre-existing
   default, not something shipped by this loop.

7. ~~Resolve the research bar on FRESH seeds.~~ **SETTLED journal 281, and the
   answer has two halves.** Eight fresh seeds: 0.45 wins 5 of 8, mean
   difference +42 for 0.65 with 95% CI [-49,+132] -- not significant. BUT the
   distributions differ in shape: 0.45 mean 336 / sd 95 / min 160; 0.65 mean
   378 / sd 31 / min 330. 0.45 is usually slightly better and occasionally
   collapses (its two worst seeds score 160 and 224; the same seeds at 0.65
   score 415 and 396). **FOR THE USER: whether to change the default is a
   design question** -- usually-stronger-sometimes-collapsing vs consistently
   good -- not a measurement one. Also explains journal 280: the hold-out sets
   disagreed because each held a different share of 0.45's collapse worlds.

   ORIGINAL: **Resolve the research bar on FRESH seeds.** Journal 280 left a shipped
   default unresolved: set C peaks at 0.45, set D at 0.65, and its own
   conclusion was that this needs MORE SEEDS rather than more knob values.
   Both hold-out sets are now spent on this question, so use fresh ones. 0.45
   vs 0.65 only -- the two candidates -- on 8 fresh seeds. Takes precedence
   over auditing further map entries, because those will keep producing 3-seed
   disagreements until the seed count question is settled on one example.

8. ~~Check the SHIPPED change for robustness, not just mean.~~ **SETTLED
   journal 282: net win, NOT uniform.** 8 fresh seeds: shipped wins 6, mean
   +89 (CI [-24,+201]), but sd rises 61 -> 95 and it creates two collapse
   worlds the control handles (1618033: 281 -> 224; 3141592: 274 -> 160).

9. ~~Test the combination: shipped change WITH research bar 0.65.~~ **SETTLED
   journal 283 with NO new runs** -- journal 281 swept the bar with shipped
   defaults active, so its 0.65 arm already WAS the combination, on the same 8
   fresh seeds as 282. Consistency check passed (281's 0.45 column == 282's
   shipped column on all 8 seeds). Result: **shipped+0.65 beats pre-change on
   8/8 seeds, +130, CI [+73,+187] SIGNIFICANT** -- the only configuration
   measured that clears zero. The shipped config alone does NOT (+89, CI spans
   zero). Combination also cuts sd 95 -> 31 and raises the floor 160 -> 330.
   **FOR THE USER: OD_RESEARCH_BAR 0.45 -> 0.65 is a candidate default change.**
   Limits: 8 seeds, one model, three seats; the gain over the CURRENT default
   is not significant by mean, its case is variance and worst case.

10. ~~Validate 0.65 on a second model before any default change.~~ **SETTLED
   journal 284: REJECTED -- the finding INVERTS on a second lineage.** On N35,
   0.45 beats 0.65 on mean (295 vs 258), variance (sd 47 vs 68) AND floor (211
   vs 177) -- the exact reverse of N24, where 0.65's whole case was variance and
   floor. 0.65 wins 2/8 seeds. **The journal 283 default-change candidate is
   WITHDRAWN.** Everything in journals 280-283 is a statement about N24 and does
   not transfer. The existing 0.45 default stands, and on evidence rather than
   inertia. Journal 283's
   case rests entirely on N24. bench-resolution-limit says ~10 points does not
   survive a change of model and to require sign agreement across models. Run
   the same 8 fresh seeds, 3 arms, on a second model (loop-base trained 8 maps,
   or N37) before the default is touched. Journals
   281 and 282 are one story -- the shipped change's two collapse worlds are
   exactly the worlds bar 0.65 repairs (415 and 396 against 224 and 160). If
   the combination keeps the +89 average and removes the failure mode, it is
   strictly better than what ships. Measure it; do not infer it --
   ablations-dont-compose records superadditive help AND harm in this project
   with sign unpredictable from the parts. 8 fresh seeds, 3 arms (control,
   shipped, shipped+0.65) or 2 if reusing journal 282's numbers.

10. ~~Re-test the other bench-based entries~~ **SUPERSEDED BY 10b, which is
   complete (journals 293, 295, 297).** This entry is a mangled merge of two
   items; its live content moved to 10b. Struck journal 303. Original text
   follows. Journal 281
   showed a mean can hide a bimodal outcome: 0.45 beats 0.65 on 5 of 8 seeds
   while having a worst case 170 points worse. The austerity-ordering + siege
   gate is the one AI change this project has shipped, and it was judged on two
   MEANS (+71 set C, +42 set D, journal 239) -- its distribution was never
   looked at. If it is usually-good-occasionally-catastrophic, that is
   decision-relevant for a released default. 8 fresh seeds, report min and sd.

10b. ~~Re-test the other bench-based entries~~ **ALL THREE DONE (journals 293,
   295, 297).** ~~reflex-layer audit~~ **SETTLED journal 297: FOUR OF THE
   ELEVEN ARE NOT SHIPPED.** peace, pacification, withdraw and callToArms each
   open with their own env gate and default OFF, so ablating them is a no-op --
   confirmed exactly, identical decision hash over identical decision counts
   (86,188 FRA / 277,784 CHN) on both seats. The AI runs SEVEN reflexes:
   garrison, fortify, redeploy, austerity, manpower, siege, campaign -- all
   live on both seats. **Trap: OD_ABLATE on a default-off reflex is a silent
   no-op**, so the obvious 264-run sweep would have produced four meaningless
   nulls indistinguishable from measured ones.

28. ~~Two reflex headers contradict their code.~~ **FIXED journal 302.** Both
   now read "OFF by default"; proven inert by an identical decision hash.
   Audited ALL fifteen dispatched reflexes rather than the two that were named:
   naval and industry already said "off by default" correctly, and peace,
   pacification and researchAusterity make no header claim, so there were
   exactly two contradictions.
   **CORRECTS journal 297's headline.** "The AI runs seven reflexes" is true of
   the ELEVEN ABLATABLE ones; the dispatch calls fifteen. Real count: **8 ON**
   (garrison redeploy fortify austerity campaign manpower siege[difficulty]
   amphibious), **7 OFF** (callToArms withdraw peace pacification naval
   industry researchAusterity). The last three are not in OD_ABLATE at all.
   **And the audit's own regex was wrong about siege** -- it reads its default
   from the difficulty table, not an env var; journal 297's ablation had
   already proven it live. A regex that classifies code by nearby text finds
   candidates and cannot decide them.

   SUPERSEDED: AISystem.cpp:7275 says
   "CALL TO ARMS REFLEX (OD_CALL_REFLEX, on by default)" and :7369 says
   "WITHDRAW REFLEX (OD_WITHDRAW_REFLEX, on by default)". Both default OFF, and
   callToArms' own body comment three lines below says "OFF by default" and
   explains why with numbers. Someone skimming section headers to see what the
   AI does gets 4 of 11 wrong. Another session's file -- for the user to place.

29. ~~OD_DECISION_HASH never prints on its own.~~ **FIXED journal 300.** Split
   into its own idempotent atexit hook, registered by either instrument. Truth
   table verified: 0 lines with neither var, 1 with OD_DECISION_HASH alone
   (the fix), 1 with both (not 2), 0 with OD_ACT_HIST alone; share 21.4 in all
   four. **Bonus:** the hash from the freshly built build/ binary equals the
   one journal 297 recorded from build/loop/relcheck/b/ --
   14669681761325311781 -- so two independently compiled binaries decide
   86,188 decisions identically. Inertness proven by hash, not by score.
   PENDING COMMIT: 4 hunks in AISystem.cpp/.h, shared files, stage the hunks.

   SUPERSEDED: It records under its own env
   var but is only reported by dumpActionHistogram, whose atexit hook is
   registered solely under OD_ACT_HIST -- so OD_DECISION_HASH=1 alone yields
   silence, which reads as "the instrument found nothing". My defect, from
   1062e85; it cost journal 297 an aborted batch. Fix: register the atexit hook
   when EITHER var is set, or print the hash from its own hook. Two lines.

   SUPERSEDED:
   ~~minority gates~~ **SETTLED journal 295.** Of the three, only ONE is still a
   live knob (OD_CALM_GATE); the conciliation cap and pacification trim are no
   longer parameterised. Result: NULL on the graded pair -- N24 +21 CI
   [-37,+79], N47 +34 CI [-43,+110] -- so the default (OFF) stands, but its
   one-seed justification (227 -> 209) is RETIRED: on the same 3-seat metric
   over 8 seeds it is +56, opposite sign. The mechanism comment is now the only
   reason for the default. Also: N47's gate arm graded 12/24 vs its control's
   18/24, the first time an ARM of one model tripped the comparability check.
   REMAINING: the reflex-layer audit (11 verdicts; fortify already re-done in
   journals 285/286/291).

24. ~~AISystem.cpp's calm-gate comment overstates its evidence.~~ **FIXED
   journal 301.** The one-seed figure is replaced by journal 295's 48 runs,
   with the reason the old number was wrong (the all-seats metric contains two
   500-or-0 seats) so nobody re-derives it. Default unchanged and still right,
   on the mechanism argument now named as the reason. Proven inert by an
   identical decision hash over 86,188 decisions, not merely a matching score.
   **Also corrected: my own false blocker.** I had filed 24 and 28 as "another
   session's file -- for the user to place" and skipped both twice. Hard rule 1
   lists GIT operations, not edits, and this loop edits src/ as its normal mode.
   Correct a factual claim in shared source when you have the measurement;
   leave style and design alone.

   SUPERSEDED: It cites
   "N24 from 227 to 209 (all seats)" as the reason the gate is off. Journal 295
   measured +56 on that metric over 8 seeds. The DEFAULT is still right on the
   mechanism argument in the same comment; the number beside it should be
   replaced with the 48-run result or removed. Small edit, another session's
   file -- for the user to place.

25. ~~Three 4-of-4 sign patterns in three iterations.~~ **SETTLED journal 296:
   the intervals are FINE; the P-values were forking paths.** Permutation
   (50k draws) agrees with the t-CI on all four N24 arms including the one that
   cleared zero. Seeds have NO persistent character (mean off-diagonal
   correlation -0.09 across five configurations), so cells are not dependent
   either. The real cause: ~6 candidate 4-cell groupings per entry, P(some
   4-of-4) = 0.55, so three in three iterations is the expected yield of
   looking. All three retracted as evidence; no verdict moves.

31. ~~Re-price the industry reflex.~~ **SETTLED journal 304: NOT RESOLVABLE at
   24 runs.** OD_INDUSTRY_REFLEX=1, both models, 8 fresh seeds, graded pair:
   N24 -21 CI [-80,+37] perm p=0.52, N47 +19 CI [-69,+107] perm p=0.71, pooled
   -9 CI [-58,+40]. Arm graded counts matched their controls exactly (14/14,
   18/18). **Journal 208's -29.40 does not replicate** -- that entry ran 3
   seeds on 6 seats with CHN pinned at 500 in every arm, where the resolution
   is ~96 points. Its "series hypothesis CONFIRMED" (+37.84 swing) is now
   UNSUPPORTED rather than disproved. The default stays OFF by default rather
   than by evidence.

33. ~~Which actions can the policy not reach?~~ **SETTLED journal 305: TEN of
   39, and six are POLITICS.** 421,519 offers ignored across two seats. Zero
   actions are never OFFERED -- every one is legal sometimes, so this is the
   policy declining, not a mask or rule. Known already: ECON a3/a5/a6 (port,
   two ship slots) and a11 (a research branch). **New: POLITICS a1, a3, a4, a7,
   a10, a11 -- half that head.** Largest dead action in the AI is POLITICS a10,
   REPRESS, at 126,783 offers and zero takes: the direct answer to "why does
   the AI never repress?" is that it never has, not that it tried and lost.

35. ~~Read the exponents of journal 305's ten dead actions.~~ **SETTLED journal
   306: ALL TEN ARE OUT OF REACH.** Largest is 5.73e-10 against LOOP.md's 1e-6
   threshold; seven are 0.00e+00 (softmax underflow). None is a lever; each
   needs --reset-ai-head and a retrain. **"Why does the AI never repress?" is
   answered**: zero probability across 126,783 legal offers -- never a priced
   decision. The politics head is collapsed ONTO A SUBSET (a8 runs at 5.3e-01),
   not globally. Instrument restored: LOOP.md cited a policy-shape table that
   no longer existed in src/.

36. ~~Is the action collapse lineage-specific?~~ **SETTLED journal 307: NO.**
   Twelve actions are dead (pi < 1e-6) in all three models measured -- N24
   (rated 433), N47 (253) and loop-base (14). Repress and the naval actions are
   dead in every one. **Journal 306's "each needs a retrain" is WITHDRAWN**: a
   retrain reproduces this. Also: better models have MORE actions reachable
   (23/22/21 of 39, monotone with rating), so training OPENS actions -- the
   twelve are a floor none of the three has climbed off.
   **Limit:** this separates "this lineage" from "every lineage"; it does NOT
   separate "structural" from "correctly learned".

37. ~~The discriminating run: a freshly reset head.~~ **SETTLED journal 308:
   LEARNED, NOT STRUCTURAL.** All six dead POLITICS actions come alive on a
   reset head -- repress 0.00e+00 -> **6.51e-01**, "enact the costed doctrine"
   -> 6.76e-01, against a uniform of 8.3e-02. Nothing in the masks or reward
   forbids them; three policies each drove them to zero independently. Journal
   256's "priced refusal" reading SURVIVES on better evidence. **And the flag
   did not exist** -- resetModuleHead had no caller while four places
   documented `--reset-ai-head`; wired this iteration (PENDING COMMIT,
   ServerMain.cpp).

39. ~~What is the reward telling the politics head?~~ **ANSWERED journal 309:
   A CREDIT-ASSIGNMENT DEFECT.** Repress = save money, lose alignment. The
   POLITICS reward charges the alignment loss THREE times (-2.5 rebellion,
   -0.6 level, -0.5 change; up to -3.6, continuous) and pays for the money with
   a BINARY +/-0.2 on the sign of netIncome. The actual benefit -- minority
   spend is 18% of gross -- is rewarded in the ECONOMY head's return (+2.2
   netIncome potential, +0.5 treasury, -1.2 broke). **The head that takes the
   action pays the cost; another head collects the payoff**, 18:1 against, which
   is why the action underflows rather than merely shrinking.
   Also: the source comment beside the level term describes a model "repressing
   at 207 per thousand against random's 88" -- that term was added to stop
   over-repression and overshot to exact zero; journal 256 then read the 0.00%
   as correct play.

60. ~~Read the [NOOP] histogram.~~ **DONE journal 326: 27.1% of everything the
   AI does, does nothing -- and ONE rule is 16.2% of all actions.**
   73,954 refused executions of 273,034; top reason **"reinforce: nothing to
   move" at 44,291 (59.9% of refusals)**. That is WAR a2, executed 64,292 times
   and refusing **68.9%** of them. Invisible to both main instruments: the
   bench cannot see a wasted turn, and ACTHIST counts a refused action as
   PICKED. Second never-read instrument found in four entries, after [GATE].

59. ~~Did the reinforce mask regress, or never cover this?~~ **NEITHER --
   ANSWERED journal 327.** OD_REINFORCE_GATE (the mask's refinement, default
   OFF) recovers only 5.1%: reinforce no-ops 44,291 -> 42,027, total refusals
   73,954 -> 73,739. **The mismatch is in the EXECUTOR.** validWar asks "is
   there ANY frontier with a neighbouring garrison >= 200?"; reinforceProvince
   takes the LARGEST neighbour of a specific destination and **returns false if
   it already carries a move order, without trying the next-largest**. A country
   whose biggest garrisons are all in motion refuses even with adequate
   second-choice sources beside every frontier.

61. ~~Make reinforceProvince fall back to the next-best source.~~ **BUILT AND
   DIAGNOSIS CONFIRMED (journal 328), GATED OFF.** OD_REINF_FALLBACK=1:
   "reinforce: nothing to move" **44,291 -> 29,269 (-33.9%)**, total refusals
   73,954 -> 53,739. Clears the 25% threshold set beforehand.
   **But the France seat fell 21.4 -> 9.1 on that same seed** (-183 in seat
   terms). One seed decides nothing, so the change is DEFAULT OFF and nothing
   is recommended. Default path is the original verbatim, verified by decision
   hash -- the first gating attempt broke tie-breaking (sort by province id vs
   first-encountered) and was caught by the hash.

62. ~~Judge OD_REINF_FALLBACK on the graded pair, 8 fresh seeds.~~ **NOT
   RESOLVABLE -- journal 329. Stays OFF.** Graded pair **-21 CI [-76,+35]**
   perm p=0.55, 2/8; all three seats **+28 CI [-38,+94]**, 4/8. Both span zero
   and point opposite ways. Arms comparable (14/24 vs 13/24), and the control
   reproduced journal 282's column on all 24 observations.
   **Journal 328's -183 was the ARRANGEMENT, not the change**: that run had
   only 1914:FRA in play, and the seat bench builds its world from the seat set,
   so a one-seat run is a different world rather than a subset. The same seed
   in the three-seat run reads 21.4 -> 19.5.
   So: a defect that wastes 16.2% of the AI's actions, a two-line fix that cuts
   it by a third, and a bench that cannot see whether that helps. **The
   strongest single argument yet for item 26** -- at 32 seeds the interval would
   be ~29 wide instead of ~56.

 Both arms on
   one binary -- the flag exists so they can be. Pre-register before running.
   **The bench can only REJECT at this sample size** (journal 296, ~60-point
   floor); if it lands inside the interval the honest report is "the waste is
   real and fixing it is not measurably good or bad", and shipping becomes a
   game-design judgement rather than a measurement. Consider also that
   reinforcing from a second-best province spreads force that might have been
   concentrated -- withdrawing-loses-battles records two movement rules that
   both cost.

 The concrete
   fix journal 327 identified: replace "pick the max garrison, fail if it is
   already moving" with a loop over candidate sources in descending garrison
   order. **It has a floor-free success criterion** -- does "reinforce: nothing
   to move" fall from 44,291 -- so unlike every knob this sequence has tested it
   does NOT need item 26 settled. Pre-register a threshold before running.
   Caveats: it is game logic and changes play, so the decision hash WILL move;
   and more reinforce orders executing is not automatically better -- check the
   seat scores on several seeds before recommending, and remember
   withdrawing-loses-battles and caution-rules-trade-growth.

 validWar carries a
   mask added precisely to stop "reinforce: nothing to move", with the comment
   "measured at 3,181 times in a 400-turn run". Journal 326 measures **44,291**
   on 1914:FRA / N24 / seed 13579 -- 14x. The 3,181 has NO seat, model or seed
   recorded beside it, so the comparison may be apples to oranges. Determine
   whether the mask fires at all on this seat before concluding anything:
   count validWar's reinforce-mask rejections the way journal 325 counted
   nextPortBuy's. **Highest-value open item the loop can act on** -- a sixth of
   the AI's actions are at stake and it needs no bench.

57. ~~Why are the ship actions dead?~~ **ANSWERED journal 323: they are rarely
   POSSIBLE, not rarely affordable.** First capture of the [GATE] econ table,
   which has printed on every OD_ACT_HIST run since journal 305 and which every
   grep I wrote filtered out:
       a3 PORT   offered 6.0%  cash-blocked  8.2%  NOT-POSSIBLE 85.8%
       a5 SHIP   offered 9.1%  cash-blocked 22.9%  NOT-POSSIBLE 68.0%
       a1 industry 6.6%        cash-blocked 52.7%  not-possible 40.6%
   **Industry and fortify are money-bound; ports and ships are
   possibility-bound.** The record treats "the AI will not buy X" as one
   phenomenon and it is at least two.
   **And validEconomy's own comment already explains ports**: "the research
   deadlock that capped every country at port level 1 was only just lifted, so
   no model has ever been trained in a world where port level 2 was reachable".
   Four entries were spent on naval deadness without citing it.

56. ~~Is the port research deadlock actually lifted?~~ **PARTLY -- ANSWERED
   journal 324.** portCap is 1 on **83.5% (FRA) / 88.8% (CHN)** of
   country-turns and 2-or-3 on 11-16%. The research path exists and is reached;
   it just reaches a minority. validEconomy's "no model has ever been trained
   in a world where port level 2 was reachable" is QUALIFIED, not refuted -- it
   is reachable on 11-16% of turns, so models have seen it, rarely.
   The cap-1 rate tracks the port NOT-POSSIBLE rate (85.8% / 92.2%) closely,
   consistent with "cap 1 and every coastal province already has its port" --
   **correlation, not measured causation**; a cap-1 country with a portless
   coastal province can still build.

58. ~~Count the two port conditions TOGETHER.~~ **ANSWERED journal 325: the cap
   is the lever for about HALF.** Of the country-turns where nextPortBuy fails:
   **cap-bound 54.6% (FRA) / 45.5% (CHN)** -- every owned port at cap, so
   raising portCap opens them; **no coast / no port 45.4% / 54.5%** -- nothing
   opens those; **top-4 window 0.0% on both** -- the population window I
   suspected never fires, because when the lookup fails there is no coastal
   portless province anywhere.
   So journal 324's correlation was causal for half of it. Raising the cap is
   worth doing and is not a complete fix.
   **Denominator caution recorded:** PORTFAIL counts COUNTRY-TURNS (nextPortBuy
   is cached per country per turn); [GATE] counts DECISIONS. Do not multiply
   them -- I nearly did.

 Journal 324 left the causal
   step unmeasured: is "port not possible" actually "cap reached AND no
   portless coastal province", or is the coastal condition doing the work? One
   counter splitting nextPortBuy's two branches at the gate. Cheap, and it
   decides whether raising the port cap would open the action at all -- which
   is the only reason the answer matters.

 Journal 323: the comment
   says it "was only just lifted", but ports read NOT-POSSIBLE on 85.8% of econ
   decisions on 1914:FRA. Either the lift has not reached these seats, or
   "not possible" is dominated by something else (a coastal condition, or ports
   already at their reachable max). One run with the per-province reason would
   settle it. **Do not assume the deadlock** -- journal 322 retracted exactly
   this kind of inference one entry earlier.

55. ~~Is the ship term really adversely skewed?~~ **NO -- journal 321's claim
   RETRACTED in journal 322.** Measured P(fleetUseful) = **0.848** against a
   break-even of 0.556, so the term's EV is **+0.263**: it REWARDS buying
   ships. I had read "0.4 against 0.5" off the coefficients without asking how
   often each branch fires. **Ships are therefore still unexplained** -- the
   four mechanisms become three and an open question. Next suspect (NOT
   asserted): the +0.263 must clear the hull's cost through the treasury and
   `broke` terms.
   Also found: **the reward block does not execute under --eval-ai** -- anything
   instrumented inside a reward expression is invisible to the bench and needs
   a training run.

54. ~~Is the credit defect general?~~ **NO -- FALSIFIED journal 321.** The
   decisive pair: POLITICS a1 and a8 both enact a policy, identical credit
   structure (money from ECON, unrest to POLITICS), and a1 is DEAD (0.00e+00)
   while a8 is LIVE (0.53, taken 5,384x). What separates them is what each
   SELECTS: a8 maximises `2*unrestReduction + |opinionShift| + 0.5*minorityGrowth`
   -- term for term what the POLITICS reward pays -- while a1 picks by the
   country's COMPASS, and ideological fit is in no reward expression.
   **Four dead actions, four mechanisms:** split credit (repress, j.309);
   adverse skew (ships: ECON gets +0.4 if the fleet is useful, -0.5 if not --
   cost AND benefit in the buyer's own head); unpriced benefit (ports: no
   reward term mentions them); and an objective the reward does not contain
   (a1). **No single defect, so no single fix.**
   Partly restores journal 256: for a1, declining is correct play given the
   reward as written.

53. ~~Verify the pending stack as a set.~~ **DONE journal 320: all seven checks
   pass.** Both targets build (including the WINDOWED game, which nothing in
   this sequence had compiled); decision hash equals the reference over 86,188
   decisions, covering five journals' worth of AISystem edits and their
   interactions; an unflagged run prints ZERO lines of new instrument output;
   od_bench.py works end to end; both wired flags work and write nothing;
   data/ai/model.bin unchanged. 26 hunks across 5 files, every one attributed.
   **`data/saves/Modern Day.odsv` in the diff is NOT this work** -- it was
   already deleted at session start; do not sweep it in.

71. ~~Bench OD_WAR_BAR_RESEARCH, with the direction pre-registered.~~
   **RUN, journal 338: NOT RESOLVABLE at 24 runs per arm, and the
   pre-registered direction is contradicted in sign.** 381 -> 422 on the three
   rung seats (+42 against a ~60 floor), 301 -> 331 on the four that actually
   ran. No seat clears p=0.05; modern:CHN comes closest at 0.07. The gate stays
   default-off, which is where it was. **I predicted DOWN and it read up:
   record that, do not re-describe it as a win.** Two things the run leaves:
   - **72** below, the reason the arm could not see its own effect.
   - the rush seat's collapse rate was 5/8 in BOTH arms, so its -5 score is
     "rush unresolved" (journal 291), not a guard violation.

72. ~~Pre-register on the statistic the mechanism moves, not on the rating.~~
   **INSTRUMENTED journal 339, so it is now automatic rather than a note.**
   `od_bench.py --compare` prints per-seat raw LAND SHARE with both medians,
   the difference and a reproducible unpaired permutation p (20,000 shuffles,
   fixed seed), and NAMES every seat pinned at CAP in both arms -- the seats
   whose land moves where the rating cannot see it. Per-seed values are now
   persisted; they used to die at exit, which is why 863 archived results carry
   no statistics at all. Validated against journal 338's hand-computed table:
   all four d exact, all four p inside shuffle noise.
   **Still a judgement call, and the part no instrument makes for you:** the
   tool tells you a seat is capped, it cannot tell you which quantity your
   mechanism buys. Decide that before the arm, not after.

75. ~~863 archived results have no per-seed data.~~ **DONE AS FAR AS THE LOGS
   ALLOW, journal 371: 22 rows recovered, 839 have no surviving log.** Parsed
   from od_bench's own output with every seat mean asserted to 1e-6. The parser
   was validated on 25 rows that already had spread: 25/25 exact, and a 0.05
   mean shift caught 25/25. Closed: the rest are unrecoverable by construction.
   ORIGINAL: Journal 339 persists it
   going forward; everything before is seat means only, and `--compare` now
   says so rather than printing an empty table. Not recoverable in general --
   the values were discarded at exit, not stored badly. Recoverable in the ONE
   case where the run log survives, by parsing od_bench's own printed output
   with an assertion that every recovered mean matches the stored mean (done
   for the two it338 rows; they carry a `spread_note`). **Do not backfill
   without that assertion, and do not backfill from memory or from a
   reconstruction.** Low priority: the archive's value is its ratings, and a
   comparison that needs statistics can be re-run.

76. ~~"better on N/M seats", and one pass over od_bench's remaining ratios.~~
   **SWEPT journal 340: four of the six published numbers were wrong, all the
   same way.** The land line summed the whole stored dict while the rating
   iterates the filtered seats; bench_score.txt accepted any run (a 1-seat
   2-seed 30-turn smoke published itself as THE rating, and the line had no
   turns field to give it away); capacity printed one N beside two means; goods
   halved any good that only half the runs reported. The two correct ones --
   combat width, population -- are the two that print numerator and denominator
   together. **Six instances in three iterations** counting journal 338's
   seat-filter expansion and 339's miscounted multiple-comparison note.

78. ~~A head action that does nothing on 99.92% of the hull-turns it is
   offered.~~ **ANSWERED journal 342: the fleet is four hull-ranges from the
   nearest enemy coast, and there is ALWAYS a coast.** A hostile port existed
   on 100.0% of 71,373 scans across two seats (15-40 of them each time), so it
   is not a diplomacy or geography fact. The nearest is a median 2-4x the
   hull's range away -- mean 41-47 deg against a 10 deg range. Also: journal
   341's 71,373 was half the real denominator; of 135,258 own hull-turns, a
   third have no crew (item 81) and a fifth are already ordered.
   **The mask is still the wrong move** (masking-waste-costs). Successors: 81,
   82. Journal 341's by-product: the amphibious landing action found a
   hostile shore in range 58 times in 71,373 hull-turns across two seats.
   Journal 326's worst single no-op was `reinforce: nothing to move` at 16.2%
   of everything executed; this is a different order of magnitude.
   **DO NOT MASK IT** -- memory masking-waste-costs, removing a 99.9% no-op
   action cost 59 points because the freed probability went somewhere worse.
   The question is why a hostile shore is so rarely within one hull's range:
   are the hulls in port, is the range small, or are the targets far? That is
   three counters and one seat, not a bench arm. **Note the denominator**: the
   counter increments per SHIP per invocation, so 0.08% is per hull-turn and
   the per-decision rate is unmeasured.

87. **od_bench prints a rating its own error bar does not describe.**
   **HALF DONE, journal 352: the error bar now hangs off the number it was
   computed from.** `report()` prints a labelled PER-SEED rating line carrying
   the se and the noise threshold; the headline keeps its value and gets a
   pointer. `--compare` prints the per-seed difference against the floor built
   from both arms and says whether it clears. Verified against journal 351's
   hand computation (336/248, -88, floor 90, DOES NOT CLEAR) and against a
   single-seat run where the two ratings agree exactly, as they must.
   **STILL FOR THE USER -- the other half:** whether the HEADLINE should become
   the per-seed figure. It is biased upward wherever a seat is capped (+45 on
   the standard control, +11 on the pacify arm, so not even a constant offset
   between arms being compared), and every one of the 865 stored rows is the
   biased one. Switching re-scales the archive against 350 journal entries.
   ORIGINAL:
   Journal 351: `report()` computes the headline from the MEAN seat shares
   while the `+/- se` line computes per-seed ratings from the spread. Because
   `min(share/par, CAP)` is concave, the printed number is biased UPWARD
   wherever a seat is bistable or capped -- **+45 points on the standard 3-seat
   control** (381 printed against 336 per-seed), and +11 on the pacify arm, so
   the bias is not even constant between arms being compared. Every stored
   rating in the archive is the biased one.
   **Do not silently switch the headline.** Changing it would move every number
   in `build/od_bench_results.json` relative to every number in the journal --
   the disease LOOP.md's seat-set note exists to prevent. Print BOTH, label
   them, and use the per-seed one whenever the figure is compared with an se
   or with another arm. That is a `report()` change of a few lines and it needs
   the user's nod because it changes what the headline means.

96. ~~REPLICATE journal 366 on seed set 2.~~ **REPLICATED journal 367.** On
   the independent seed set the rusher model reads **+165 over the self-play
   model, CI [+79, +250]**, and **+43 against N24, inside the floor**. Pooled over
   16 seeds: +154, CI [+93, +214]; +21 vs N24. China annihilated 0/8 (self-play
   3/8, N24 1/8). Unpredicted: the USA against N24, +14.07 at p 0.003, clearing
   the correction -- +10.24 on seed set 1 -- an observation to test, not a claim.
   ORIGINAL: **REPLICATE journal 366 on seed set 2. TOP LOOP-ACTIONABLE ITEM.** The
   rusher-trained model (scratchpad trained8-rusher.bin, md5 c429f777) read
   parity with N24 and +143 over the no-rusher model on seed set 1. Journal 354
   is why one seed set is not a result. **Cheap: no training, 24 bench runs** --
   both comparison arms already exist on seed set 2 (it354-ctl-s2 = N24,
   it359-trained8-s2 = the no-rusher model). Seeds 11111, 2468135, 777777,
   31415926, 5772156, 1414213, 9090909, 6180339; three rung seats; binary
   84d15b60 (pin363). Pre-register: +clears over the no-rusher model again,
   inside N24's floor again.
   (Ran as it367-rusher8-s2, stored.)

105. **Separate "the winner wins faster" from "the loser is annexed."** Journal
   405 measured wars getting shorter under the doctrine reflex and registered in
   advance that its instrument cannot tell those apart -- both shorten a war. Needs
   conquest counts per war (provinces changing hands, and whether a side ceased to
   exist) alongside the existing OD_WARLIFE pairs. Only worth doing if the mechanism
   matters for a decision; the ship question does not depend on it.

104. **FOR THE USER, and it dates the +52. The reference decision count moved
   +12.2% on 2026-09-20.** 1914:FRA seed 13579 ran 81,340 decisions from journal 382
   through journal 404 and runs **91,296** on build 764f5e5; the new reference triples
   are **2904055102330604147/91296** (reflex off) and **742276098712025459/130092**
   (=3). The commits in between are 764f5e5 (pacification rebate, AI manpower ceiling)
   and 976d457 (an unaudited effects block). Journals 399-403's +52 was measured on
   the old world. Nothing says it is gone -- the two arms would both move -- but
   **before shipping mode 3 it is worth one re-measurement on the current build**:
   48 rung seeds, both arms, ~2.5 h, the same statistic as journal 402.

103. ~~**The seat gains +52 while buying 3-11% of the doctrines. What does the
   WORLD do with them?**~~ **HALF ANSWERED journal 405: turnover, and the screen is
   MIXED.** New instrument OD_WARLIFE (off by default, inert under three gates). Eight
   paired worlds: wars are shorter in 6 of 8 (sign test p 0.145), mean 4.2 turns
   shorter. 1914:FRA 31.4 -> 25.2 turns, shorter on 4 of 4 seeds; modern:CHN 24.7 ->
   22.4, shorter on only 2 of 4, but it **starts 106 more wars and ends 107 more at
   unchanged concurrency** -- the same slot story told as throughput instead of
   length. The registered screen wanted 3 of 4 on both seats and got 4 of 4 and 2 of
   4, so this is recorded as partial support, not as settled. Successor: 105.

   ORIGINAL: Journal 404 settled who buys and left the bigger question
   open: mode 3's gain to the scored seat is almost entirely indirect. Two candidates,
   neither tested -- decisive wars free the one-war slot (memory
   stalled-wars-lock-the-war-slot, and journals 385-392 built the instruments), or an
   armed scripted world consolidates in a way the model exploits better than the rung.
   Cheap first cut: with the reflex on and off, count wars STARTED and ENDED per world
   and the mean war length, on one seat and four seeds. If war length falls, the slot
   story is live and it connects this rule to rule C.

102. ~~**[DOCREFLEX] counts by doctrine, not by country, so the reflex cannot be
   attributed to a seat.**~~ **DONE journal 404: the seat is NOT the buyer.**
   Per-country counter (isoA3, bench seat marked), proved inert under BOTH gates --
   gate off reproduces the reference triple and =3 reproduces journal 403's hash to
   the digit. Readings on six seats x two seeds: the seat is **0-11% of the world's
   purchases** (FRA:rung 1 of 10, USA 2 of 18, CHN 1 of 31, FRA:rush 1 of 18), so
   self-arming cannot explain journal 403's rush sign, which stays unexplained.
   **1939:NOR NEVER buys** -- it is below the median army, so the rule structurally
   cannot fire for that seat, which is why the hood guard read 0.41 vs 0.41 and makes
   that guard weaker than it looks. And the buyers are **middling states** (BEL, NLD,
   CHE, AUS, CAN, IDN), which corrects journal 401's "arm only the big": the median is
   over every living country and is a low bar. Successor: 103.

   ORIGINAL: Journal 403 found 1914:FRA:rush collapsing slightly LESS
   under the rule and could not check the obvious explanation -- that an armed France
   is a defended France, the seat qualifying for its own reflex -- because the counter
   aggregates the world. A per-cid line (or a filter like OD_ACT_HIST_CID) is a few
   lines in AISystem.cpp and would settle it. Cheap, and it is the only thing standing
   between the favourable rush sign and a mechanism. Note the sign is NOT significant
   (p 0.860), so this explains a direction, not a finding.

101. **Small, found closing journal 372's loose end (journal 375). For whoever owns
   research, not the loop.**
   - basic_training says "Unlocks army", but no rule gates on it; the only named-node
     research checks in src are navy1 and arty1. Fix the text or add the rule.
   - Rebel states start with EMPTY research, below the tier-3 grant every map
     country gets at load. A design call: inherit from the parent, grant tier 3,
     or leave as is.
   - countries.json "research" loads into Country::research and only the map editor
     reads it; the game never applies it. Inert on the shipped maps.
   **Loop lesson, already applied:** resolve a seat's cid from the map's
   countries.json by ISO, never by grepping names in a log. A rebel state can carry
   the seat's name.

100. **The AI never chooses a side at a research fork; node order does. And an
   extra research group can take BOTH sides.** Journal 372. Answers journal
   337's "why atk runs 2.5x def" as far as research goes.
   - Every mutex pair ties on cost, and both AI choosers take the cheapest node
     with a strict `<` in declaration order. So def_tactics, total_war,
     arty4a and arty6a are always chosen and their siblings never are. 5 of 5
     (CORRECTED journal 375: 4 of 4 on the CHN seat plus 1 on a rebel state that journal
     372 took for France; journal 373's world-wide count stands)
     in traced play, hashes unchanged. The army line tops out at +50 atk /
     +25 def: 2:1 by construction.
   - Game::isNodeAvailableFor blocks a mutex sibling only once it is
     RESEARCHED. The extra-group chooser (Game_Research.cpp:122-136) can
     therefore start off_tactics while def_tactics is in progress in the main
     slot, and completion never re-checks. ~~Frequency unknown.~~ **CONFIRMED,
     journal 373: 41 / 71 / 52 fork nodes held with their sibling per world.**
   ~~Next, unblocked: the instrument.~~ **BUILT, journal 373: OD_RESEARCH_PROBE, plus a
   research/doctrine split on OD_WAR_BAR_PROBE. Kept for the record:** An env-gated counter at the two
   completion inserts (Game_Research.cpp:148, :167): per completed node, the
   count and how many countries hold both sides of a mutex group. Also
   atk/def split into research vs doctrine at the war-bar probe. It needs a
   rebuild, so check the other editor's commits and re-verify the three-seat
   hash triple first. Only then decide whether the double-hold is a defect worth
   fixing. A fix changes the game for every AI, so it is a bench arm with
   its own model question (memory campaigns-and-siege-need-their-model).
   **RESULT, journal 373, three reference seats, decision hashes unchanged:**
   - Main slot: 0 second-declared fork nodes in 14,089 completions, world-wide.
   - Every second-declared node came from an extra group, nearly always
     alongside its sibling: off_tactics 13/13, 25/25, 12/12. The same holds for
     navy5, navy8, volunteer_force, arty4b, fortress_doctrine and ind_res.
   - The attack lean is research, entirely (research atk/def +43.6/+16.4,
     +31.9/+18.8, +43.7/+23.0). Doctrines are net slightly negative on both.
     Journal 337's question is closed.
   **FIX BUILT AND BENCHED, journal 374: PARKED as a null.** OD_RESEARCH_MUTEX_FIX
   (off) closes the double-hold completely: 71 -> 0 on modern:CHN. Bench: land
   share -0.88 / +0.76 / +4.77 at p = 0.82 / 0.89 / 0.35, per-seed rating +1
   against a floor of 78. The collapse rates moved both ways (CHN annihilated
   2/8 -> 0/8, FRA 0/8 -> 1/8) and neither is resolvable at 8 seeds.
   **DECIDED by the user 2026-09-14 13:57: settle the collapse rates first**, at
   ~128 seeds per arm, before choosing between shipping and staying gated. Journal 376.
   **DECIDED by the user 2026-09-15 ~06:05: SHIP ON BY DEFAULT. DONE journal 382** (source flipped,
   OD_RESEARCH_MUTEX_FIX=0 turns it off, proven live on 3 seats; PENDING COMMIT). New reference
   triple (pin382, fix on): FRA 2684914276584272262/81340, CHN 7982296607771096629/237960,
   USA 10540971485333803371/163204.
   **SETTLED journal 376 (128 fresh seeds per arm): no harm anywhere, and the FRA harm
   signal reversed.** Below-par rate FRA 8/128 -> 1/128 (p 0.036), CHN 10 -> 7 (p 0.62).
   Neither clears the pre-registered 0.025. Land share FRA +3.33 (p 0.003), CHN +2.14
   (p 0.047). Per-seed rating +33 vs floor 21, CLEARS.
   **FOR THE USER, recommendation: ship it on by default.** This rests on the secondary
   statistics and the absence of harm, since the primary rates did not clear. Flipping the
   default changes the reference hash triple, which the loop re-records in the same iteration.
   ORIGINAL: **FOR THE USER:** ship it as a rules fix (the player already obeys this
   rule) or leave it gated. A null does not establish safety; a verdict on
   the collapse rates needs item 86's ~128 seeds per arm.
   ORIGINAL: **Next, unblocked: the fix, as a bench arm.** Gate it. The per-country
   availability check should treat a mutex sibling IN PROGRESS in any of the
   country's research groups as taken, exactly as the player's
   ResearchNode::isAvailable already does, and completion should refuse a
   node whose sibling is held. Bench it at 400 turns against the same-binary
   control on per-seat land share (memory pre-register-on-the-moved-statistic).
   **Pre-registered direction: null or slightly DOWN for the seat**, because every
   AI loses a free +10 atk node while the seat's neighbours lose it too. A
   null does not license shipping without the model question (memory
   campaigns-and-siege-need-their-model).
   **For the user, not the loop:** whether a mutex fork SHOULD belong to the
   model. Today it is a constant of the node table. Giving the model the choice is
   a new action and a retrain (memory mask-changes-need-a-retrain).

99. ~~FOR THE USER -- the rusher recipe is 1 for 2 across training seeds.~~ **DECIDED by
   the user 2026-09-14 13:57: option (c), fix the league-draw defects (item 97) first,
   then re-run the rusher recipe.** Queued after item 100's rate bench (journal 376).
   **Part two DONE, journal 378: NOT ESTABLISHED, the seeds disagree in sign.** Fixed
   league (rusher 2/8 per run): seed 424242 -> 2894ddfc, rating 419, **+83 over N24
   (floor 78, 3/3 seats) -- the first trained model above N24**; seed 777001 -> d371d0ac,
   rating 167, -169 under N24. The league draws were nearly identical, so the training
   worlds, not the league, carry the difference. Confound: the comparison models were
   trained on pin363, these on pin377.
   ~~Next: bench 2894ddfc on seed set 2.~~ **DONE journal 379: HOLDS, +133 over same-binary N24
   (439 se 9 vs 306 se 37, floor 75, 3/3 seats, USA land +17.75 at p < 0.001).** The MODEL
   beats N24 on two seed sets (+83, +133): established. The RECIPE is not (seed 777001: -169).
   **Next, unblocked, in order:**
   (1) ~~Rush guard for 2894ddfc.~~ **DONE journal 380, 32 seeds: NO HARM DETECTED.** FRA:rush
       collapsed 17/32 -> 13/32 (p 0.45), below par 21 -> 14 (p 0.13), land +3.04 (p 0.09);
       NOR:hood 32/32 below par in both, same share. Rules out a large rush regression
       (rate difference >~0.35), not a small one.
   (2) ~~Third training seed of the fixed-league recipe.~~ **DONE journal 381, two seeds: the
       RECIPE FAILS.** 555001 -44, 888001 -27 (neither clears): with 424242 +83 and 777001 -169,
       1 of 4 training seeds beat N24. 2894ddfc is unaffected. Its set-2 and rush-guard
       results were measured after it was selected.
   **DECIDED by the user 2026-09-15 ~06:05:** (a) 2894ddfc -> MORE CHECKS FIRST: a third seed
   set and the 1914:SWE seat before any ship. (b) SEARCH 4 MORE TRAINING SEEDS (~14 h). The user
   notes a new OD version may be released within that window, so every result must name its
   binary, and a release mid-search means re-checking the reference hashes before comparing.
   **Queue, in order:** (1) ~~fork fix ON by default + re-record the hashes~~ DONE journal 382.
   **THE BASELINE MOVED in journal 382**: commit 8727ca5 (migration conserves people) changed every
   decision, so all stored N24 / 2894ddfc rows describe the old game.
   **(2) DONE journal 383, on pin382, seed set 3: rung seats +80 over N24 (floor 69, CLEARS, 3/3),
   so the candidate holds in the migration-fixed game. But 1914:SWE reads WORSE**: land 9.14 -> 2.44
   (p 0.036, one of four seats compared, so not established), below par 1/8 -> 3/8.
   **(2b) DONE journal 384: 2894ddfc IS WORSE ON 1914:SWE.** 32 fresh seeds: land 10.67 -> 2.98
   (p 0.001), annihilated 6/32 -> 15/32 (p 0.032). **The candidate is a trade**: great powers +80..+133,
   and the small neutral lost about half the time. **FOR THE USER, without a recommendation: ship the trade or not.**
   **DECIDED by the user ~14:00: DIAGNOSE SWE FIRST. DONE journal 385: Sweden dies of REBELLION under
   PASSIVITY.** On 4 split seeds, 2894ddfc's Sweden declares ONE war per game (the turn-1 book), peaks at
   25-36 provinces, then 10-24 rebellion wars destroy it, with bankruptcy arriving late. N24's Sweden declares
   8-30 wars and reaches 199-306 provinces. The prediction (conquest after Sweden's own war) is falsified.
   **DECIDED by the user ~14:10: DIAGNOSE THE WAR HEAD. DONE journal 386: NEVER OFFERED, not refused.**
   Sweden alone (new OD_ACT_HIST_CID, inert): 2894ddfc's Sweden is offered "declare war" once in 1,969 war
   decisions (the turn-1 book); N24's is offered it 256 times and takes it 12.5%. When 2894ddfc IS offered
   aggression it takes it more readily (attack 93% vs 60%). Upstream difference: recruit offered 268 vs 564,
   taken 40% vs 74%. Hypothesis, not measured: fewer recruits -> smaller army -> the war bar is never met
   -> declare masked. **DECIDED by the user ~14:25: TRACE THE WAR MASK. DONE journals 387/387b: the war
   bar was NOT the gate; the one-war limit was (96-99.6% of mask calls). 2894ddfc's Sweden REFUSES Norway's
   ceasefire requests every ~25 turns, so its turn-1 war never ends (two seeds) or ends 11-34 turns before
   death (the other two). N24 accepts at the first request and declares its next war 2-4 turns later.
   The loss runs through the DIPLOMACY head's request_ceasefire decision.** Also found: `[CEASEFIRE] War
   ended` prints on REJECTED requests too (Game_TurnLogic.cpp), a misleading log label, filed.
   **DECIDED by the user ~14:35: TEST A CEASEFIRE RULE. DONE journal 388: KEEP (gated, OD_CEASEFIRE_STALL=50).**
   2894ddfc's Sweden 2.98 -> 7.73 land (p 0.031, 32 seeds; annihilated 15 -> 13); its rung +31 (no clear); N24: no
   significant harm (SWE +1.83, rung +56). With the rule on in both, 2894ddfc vs N24: rung +55 (not clearing), SWE
   -4.78 (p 0.107). The gap narrows but does not close. The rule is world-wide.
   **DECIDED by the user ~15:50: RE-CHECK 2894ddfc WITH RULE C. DONE journal 389: NOT RECOMMENDED, clause (a) fails.**
   With rule C on both: rung set 1 +4, set 2 -13, set 3 +55, none clearing (rule off it was +83/+133/+80, all
   clearing). Land still favours 2894ddfc on the great powers (USA +10.85, p 0.001, set 1). FRA:rush now points
   against it (collapses 17 -> 20 of 32, p 0.61). **2894ddfc is no longer a ship candidate once rule C is in the game.**
   **Open question it raises: is rule C an improvement for N24 itself?** Only thin evidence so far (set 3 +56, not
   clearing; SWE +1.83). **DECIDED by the user ~19:05: BENCH RULE C FOR N24. DONE journal 390: NO HARM, GAIN UNPROVEN.**
   N24 rule on vs off, pooled 24 seeds: +38.7 (floor 46.1, no clear; sets +66 / -6.5 / +56.5); land up on all three rung
   seats (FRA +4.22 p .089); SWE +1.83 (p .55); FRA:rush identical, NOR:hood unchanged.
   **DECIDED by the user ~21:30: RESOLVE FIRST. DONE journal 391: SHIP RULE MET.** N24, rule C off vs on, pooled 48
   seeds per arm: +36.1 (floor 28.5, CLEARS); the 24 fresh seeds alone +33.5 (floor 34.1, positive, just misses). No
   harm on SWE / rush / hood (journal 390). **DECIDED by the user ~22:40: MORE SEEDS FIRST. DONE journal 392: UNRESOLVED,
   and the effect shrank.** 48 fresh seeds: +13.3 (floor 30.4, no clear); this run's 24 alone -6.8; all 72 +21.8 (floor
   25.3, no clear). The +36 of journal 391 was regression after selection. Rule C does no measurable harm, and its N24
   gain is small or zero. **DECIDED by the user ~23:55: KEEP RULE C GATED OFF** (already the default; no source change).
   **(3) DONE journal 393: THE SEARCH RETURNED 0 OF 4.** Training seeds 101001 / 202002 / 303003 / 404004 scored -125 /
   -198 / -73 / -12 against N24 on the selection set, so none reached confirmation. **With journal 381 the fixed-league
   recipe is 1 of 8**, and that one hit failed its own later checks. **Backlog 99's training branch is exhausted** at
   ~14 h for 0 candidates. **DECIDED by the user ~08:30 2026-09-16: RE-CHECK THE SHIPPED FORK FIX. DONE journal 394: IT HOLDS.**
   N24 on 48 fresh seeds, current build: +44.1 (floor 30.8, CLEARS), carried by USA land +6.59 (p 0.001); no harm on
   1914:SWE (-0.89, p 0.77). The reference hash triple is UNCHANGED from journal 382, so release 1.2.1a moved no decision
   on the three seats, and two independently built binaries agree to the digit.
   **DECIDED by the user ~12:50: WORK THE RULE BACKLOG. (5) FIRST CANDIDATE DONE, journal 395: item 73
   (OD_WAR_BAR_RESEARCH) is NULL at 48 fresh seeds** -- +20.3 against a floor of 29.6, USA land +3.95 (p 0.046),
   no SWE harm on the registered statistic. **Journal 337's pre-registered DOWN is falsified in direction.**
   Item 32 (researchAusterity) was DEFERRED with a reason: its own comment says it is inert on N24 by design, so it
   needs a model that idles the research slider, and none is pinned.
   **DECIDED by the user ~15:20: RESOLVE ITEM 73. DONE journal 396: NULL AT 96 SEEDS, item 73 CLOSED.** Pooled 96 per arm:
   +10.3 (floor 19.2); the new 48 alone +0.2. Registered SWE annihilation 8/32 -> 14/32 (p 0.188). The gate's effect is
   bounded under ~19 rating points. 1939:USA land is up in both batches (+2.99 pooled, p 0.030, corrected cut 0.017):
   recorded, not a finding.
   **DECIDED by the user ~18:35: THE DOCTRINE HALF. DONE journal 397: EVERY ARMY LEVER IN FORCE IS DEFENCE-ONLY.**
   The three seats enact NO doctrines in 400 turns. The world enacts 12 distinct ones; the only army lever among them is
   demobilisation (-10 def). The maps' starting doctrines add only national_unity (+10 def) and decentralization (+6 def).
   No attack-bearing doctrine is ever enacted or granted, which confirms armyAtkPct is research entirely (journal 373).
   **No rule follows**, and journal 373's -2.20 doctrine ATTACK residual is UNEXPLAINED and filed: nothing counted can
   produce a negative attack lever. Next check if revisited: run OD_POLICY_HIST on journal 373's own seats and build.
   **(8) DONE journal 398: item 34's reachability check.** 8 of 12 politics actions have zero policy picks; a1 (enact
   doctrine) is offered constantly and refused with pi(a) = 0. Head-based rules are ruled out there.
   **DECIDED by the user ~22:45: BUILD A DOCTRINE REFLEX. DONE journal 399: HURTS by its own registered rule.**
   OD_DOCTRINE_REFLEX (in HEAD via 9a0ff43, gated OFF) fires 13-43 times per game and gives the rung seats **+72.4**
   (floor 25.6, CLEARS; FRA +10.41 and USA +10.99 land at p<0.001) -- the largest rule gain this loop has measured --
   while 1914:SWE is **annihilated 16/32 against 6/32, Fisher p 0.017**. The registered harm check makes that a HURTS,
   so it is not a ship candidate as written.
   **DECIDED by the user ~02:25 2026-09-17: NARROW IT. DONE journal 400: the gate keeps the gain and does NOT save SWE.**
   OD_DOCTRINE_REFLEX=2 (army >= median of living countries): rung +52.4 (floor 28.1, CLEARS) against the unnarrowed
   +72.4; 1914:SWE annihilated **16/32 either way** (p 0.017), though under =2 Sweden never buys a doctrine at all.
   **The harm is the neighbours, not the seat**, so no gate on the victim can fix it and the trade is intrinsic.
   **DECIDED by the user ~04:50: TRY A WORLD-SIDE LEVER. DONE journal 401: MODE 3 WINS ON BOTH.** OD_DOCTRINE_REFLEX=3
   (buyer above median AND an enemy above median): rung **+52.1** (floor 26.6, CLEARS; FRA +8.93 p 0.000, USA +4.75
   p 0.045, CHN +3.47 p 0.029) and **1914:SWE back at baseline** (annihilated 9/32 vs 6/32, p 0.556; land 10.37 vs 10.67,
   p 0.921). First rule in this sequence to keep a gain without a measured cost. Firing 10/18/31 vs mode 2's 13/20/43.
   **NOT established: pre-registered as needing replication on fresh seeds** (twice here an effect collapsed on seeds that
   did not raise it: +38.7 -> +13.3, +20.3 -> +0.2). **(12) DONE journal 402: IT REPLICATES.** 48 NEW rung seeds and 32 NEW SWE
   seeds, both arms, on a binary built three commits later: rung **+51.8** (floor 26.2, CLEARS; FRA land +8.09 p 0.000,
   CHN +4.77 p 0.004, USA +4.21 p 0.055; =3 at or above OFF on 33 of 48 seeds, sign test p 0.007) against the first run's
   +52.1, and **1914:SWE unchanged** (annihilated 13/32 vs 11/32, Fisher p 0.797; land 6.92 vs 7.08, p 0.942). Gate off
   reproduces the journal-382 hash triple exactly, so a744106 / b830e95 moved no decision on that seat. Graded counts
   differ (85/144 vs 53/144) but every extra pin is at the 5x CAP and China's 3 annihilations go to 0, so the rating
   UNDERSTATES the difference. Note the new SWE seeds are harder in both arms (OFF 11/32 here vs 6/32 on journal 384's
   set) -- no absolute Sweden figure crosses seed sets.
   **(13) DONE journal 403: THE RULE CLEARS THE RUSH GUARD -- which journals 399-402 had never run.** Four iterations
   produced a ship candidate without touching the two seats LOOP.md names in its one unconditional REJECT rule. Both arms,
   one binary, on the parties build: **1914:FRA:rush collapses 31/64 against 33/64 (Fisher p 0.860)**, land 5.14 -> 6.91
   (p 0.135); **1939:NOR:hood 0.41 vs 0.41** (p 1.000). The reflex fires 11-20 times in those worlds, so both are nulls
   and not blanks. **Claim limited on purpose: no LARGE harm detectable at 64 seeds** (~0.25 resolvable; item 86's 0.12 is
   not reachable) and the two 32-seed halves disagree in direction -- do not quote "it helps the rush seat".
   **(14) AMENDED BY JOURNAL 404 -- read this before the ship decision.** The hood guard is weaker than journal 403 made
   it sound: 1939:NOR is below the median army, so the rule **cannot fire for that seat at all**, and 0.41 vs 0.41 measures
   only the world around it. The rush guard is a real reading (France does qualify, and buys once per world); the hood one
   is structurally inert. Nothing measured says the rule harms a small exposed seat -- 1914:SWE at 32 seeds twice says it
   does not -- but "both guard seats clear" overstates what was run, and the honest count is one guard seat cleared and one
   that the rule cannot reach.
   **(15) AND THE WORLD MOVED UNDER IT, journal 405: the reference decision count is up 12.2% on build 764f5e5.** All of
   (12)-(14) was measured before that. See item 104 -- one 48-seed re-measurement on the current build is the honest
   precondition for shipping.
   **FOR THE USER, the only open question on this rule: ship it on by default?** Two rung runs on disjoint seeds and two
   binaries (+52, floor ~26), 1914:SWE unharmed at 64 seeds across two sets, 1914:FRA:rush clear at 64, 1939:NOR:hood
   inert by construction -- all on builds older than 2026-09-20. The change is
   one default in AISystem.cpp (mode 0 -> 3). The loop does not flip a default on its own.
   **Remaining in the queue:** 86 (OD_CAMPAIGN_HOMEFIRST as a rate, ~128 seeds, ~9 h).
   ORIGINAL (2h): pin388, N24 rule
   off vs OD_CEASEFIRE_STALL=50 on seed sets 1 and 2 (rung) and rush/hood 32 seeds (journal 380); SWE 32 and set 3
   already exist (journals 383/384 vs 388). Pre-register the ship criterion. This decides whether rule C ships on.
   ORIGINAL (2g): pin388, OD_CEASEFIRE_STALL=50 on
   both models; 2894ddfc vs N24 on seed sets 1 and 2 (3 rung seats) and rush/hood at journal 380's 32 seeds. Then the
   ship question.
   ORIGINAL (2f): gated rule in the AI's
   request_ceasefire decision: accept when the war has stalled N turns (no provinces changing hands) and
   it fills the one-war slot. Pre-register N and the stall test. Bench rung + 1914:SWE for 2894ddfc AND N24,
   gate off vs on, same binary. Question: does it remove 2894ddfc's SWE loss without costing its great-power
   gain, and what does it do to N24?
   ORIGINAL (2e): count, for
   cid 39 on seed 1556220086 under both models, which validity condition of war a4 fails (counters behind
   OD_ACT_HIST_CID, hash-verified). Then the ship question again.
   ORIGINAL (2d): OD_ACT_HIST on 1914:SWE,
   N24 vs 2894ddfc, split seed 1556220086 first. Is the war action never offered, or offered and refused?
   ORIGINAL: **FOR THE USER: the ship question again, with the diagnosis.** Optional next diagnostic: OD_ACT_HIST on
   1914:SWE for both models, to see whether the war action is never offered or offered and refused.
   ORIGINAL (2c): on journal 384 seeds
   where N24 holds Sweden and 2894ddfc is annihilated, trace one seat per arm: when Sweden dies, and
   whether it is a war, a rebellion or bankruptcy (OD_ECON_TRACE on the seat cid, taken from
   countries.json by ISO, never from a log grep; journal 375).
   (3) Then the 4-seed training search (user-approved, ~14 h) on pin382. Select on seed
   set 1, confirm on set 2, and gate every candidate on 1914:SWE (32 seeds) and the rush/hood seats.
   ORIGINAL (2): on pin382, bench N24 AND 2894ddfc on a THIRD seed set (8 fresh seeds)
   plus 1914:SWE. The first question is whether 2894ddfc still beats N24 in the migration-fixed game.
   (3) The 4-seed search, on pin382 or on whatever build is current, stated per result.
   ORIGINAL: **FOR THE USER, two decisions:** (a) ship 2894ddfc as data/ai/model.bin; (b) whether
   "train k seeds, select on set 1, confirm on set 2" is worth its cost (~3.5 h per seed, 1 hit
   in 4 so far). The loop has no further unblocked training work on item 99 without (b).
   **FOR THE USER, now: whether to ship 2894ddfc as data/ai/model.bin** (rung +83/+133 on two
   seed sets, rush/hood no harm at 32 seeds). The loop continues with (2) either way.
   ORIGINAL: **Part one DONE, journal 377** (item 97 fixed). **Next, unblocked: part two.** Re-run
   the recipe with OD_LEAGUE_FIX=1: 8 maps x 3000 turns, cap 0.5, training seeds 424242
   and 777001, each benched on the 3 standard seats x 8 seeds against c429f777 /
   1a43100e and N24. ~3 h per training arm. BUILD FROM THE HEAD EXPORT (scratchpad/src377
   method, pin377) until the shared Game.h is repaired; see journal 377's BUILD INCIDENT.
   ORIGINAL: **FOR THE USER -- the rusher recipe is 1 for 2 across training seeds.**
   Journal 368: journal 360's recipe and seed 777001 plus the league rusher
   reads 227, **-18 against the matching self-play model** and **-110 against
   N24, CI [-193, -26]**. Seed 424242 had given parity with N24 (journals
   366-367). So the repair belongs to one training run, not the recipe. What
   survives: model c429f777 is at parity with N24 on two bench seed sets --
   parity, not a gain, nothing to ship -- and China's annihilations fall under
   the rusher on both training seeds (4->2, 4->1), a direction too small to
   resolve. **The binding question is now training-seed variance**: any claim
   that a recipe WORKS needs several training seeds, at ~2.5 h each plus a
   bench. Options: (a) more training seeds of the rusher recipe, to estimate
   how often it produces a parity model; (b) accept that training from N24 is
   not currently a route to the rating target, and return to rules (memory
   rules-beat-training); (c) fix item 97's league-draw defects first, since the
   rusher dose itself varies run to run (8/8, then 7/8).
   **DECISION-FREE PROBE DONE, journal 369 -- direction only.** On
   1914:FRA:rush each rusher model collapses on fewer worlds than its matching
   self-play model: 6 -> 3 on seed 424242, 5 -> 4 on seed 777001 (land p 0.25,
   0.87; rate differences below the ~0.49 eight seeds resolve). The bigger move
   is on the training seed whose rung rating also recovered; the seed that
   failed there barely moved here. 1939:NOR:hood below par 8/8 for every model.
   Reads as a mechanism that worked on one training run and mostly not the
   other -- consistent with options (a) and (c), deciding nothing.

98. ~~A second TRAINING seed for the rusher recipe.~~ **DONE journal 368: the
   repair FAILS on the second training seed.** Rusher 227 vs self-play 245 (-18,
   inside the floor) and vs N24 336 (-110, CI [-193, -26], clears below). See
   item 99.
   ORIGINAL: **A second TRAINING seed for the rusher recipe. TOP LOOP-ACTIONABLE ITEM.**
   Journals 366-367 replicated the rusher league's repair on two bench seed
   sets, but from ONE training seed (424242). Journal 360 is the precedent:
   self-play's damage was checked on training seed 777001 and held. Ask the
   rusher's repair the same question -- journal 360's recipe exactly (seed
   777001) + `OD_LEAGUE_EXPLOIT=0.5`, compared against it360-trained8-t777001
   (self-play, same seed, same worlds) and it349-control. Pinned binary 84d15b60,
   fresh isolated tree. Price ~2.5-3.5 h training + ~20 min bench. Validity: the
   rusher drawn >= 2 times. Pre-register: clears over the seed-777001 self-play
   model, inside N24's floor.
   (Ran as it368-rusher8-t777001, stored.)

97. ~~Two defects in the league draw make OD_LEAGUE_EXPLOIT's cap ineffective.~~
   **FIXED behind OD_LEAGUE_FIX (off by default), journal 377.** Mechanical check,
   seed 424242, 12 maps x 40 turns, cap 0.5, one checkpoint: OFF 12/12 rusher maps and 0
   rusher outcomes; ON 4/12, every draw printed, one outcome per rusher map, max share
   0.500. Eval hash triple unchanged. **With the fix on, the cap bounds the rusher's
   expected share of draws**, so journal 366's recipe becomes a MIXED league.
   ORIGINAL: **Two defects in the league draw make OD_LEAGUE_EXPLOIT's cap
   ineffective.** **DOCUMENTATION HALF DONE, journal 370** -- the comments in
   AISystem.h and AISystem.cpp now state that the cap bounds the rusher's draw
   WEIGHT, not its share of maps, and give the two causes and the measured 8/8
   and 7/8. Comments only, no rebuild. **The behaviour fix below is still the
   user's**, and still decides what OD_LEAGUE_EXPLOIT should mean.
   ORIGINAL HEADER: **Two defects in the league draw make OD_LEAGUE_EXPLOIT's cap
   ineffective. FOR THE USER -- a training-behaviour change.** Journal 366:
   (1) `recordLeagueOutcome` (AISystem.cpp:13028) returns early for slot >=
   LEAGUE_CHECKPOINTS, and the rusher is that slot, so its outcomes are never
   recorded and its PFSP weight never adapts -- the header's "played more while
   the policy is losing to it" is false. (2) `unloadGameData()` deletes m_ai
   before every map, so `m_rng{1337}` restarts before each map's first-turn draw;
   with frozen weights the same slot wins every map. Net: the cap bounds the
   rusher's WEIGHT, not its realised frequency -- 0.5 produced 8 of 8. **Do not
   fix without deciding what the league SHOULD be**: journal 366's parity result
   was measured WITH these defects (a 100%-rusher league), and fixing them
   changes what OD_LEAGUE_EXPLOIT means. A rebuild also absorbs the other
   editor's commits since 84d15b60 and needs the three-seat hash re-check.

95. ~~`binary_mtime` records the binary at STORE time, not the one the runs
   used.~~ **FIXED journal 361.** od_bench now fingerprints the binary (md5 and
   mtime) before the first seat and again at store time, stores both beside the
   unchanged `binary_mtime`, and warns with `binary_changed_mid_run: true` when
   they differ. Tested by atomically swapping a pinned binary under a live run:
   flagged, while an untouched run was not, and the swapped run's own result
   (FRA 10.5) matched the clean one -- measuring that a running process keeps
   its launched image. **Detects, does not prevent**: pin a copy for long runs.
   ORIGINAL: **UNBLOCKED journal 360** -- no chain is running od_bench. Journal 360: the concurrent editor rebuilt the server at 19:24:01,
   26 seconds before journal 359 stored its row, and that row recorded the NEW
   mtime -- though its runs almost certainly all executed on the old binary
   (a running process keeps its launched image). od_bench evaluates
   `os.path.getmtime(binary)` when writing the store, so the provenance field
   can describe a binary no run ever executed. **Fix: capture the md5 and mtime
   at the START of the run, capture them again at store time, and print a
   warning (and store both) if they differ.** Python only, no rebuild. This
   matters more now than it did: the other session committed three times on
   13 Sep, and a binary change mid-arm silently mixes two builds into one row.

94. **FOR THE USER -- training degrades N24, established. The open question
   is now a RECIPE.** Journals 357 and 359: 8 maps from N24 on the settled
   recipe costs ~130 per-seed rating points on two independent seed sets
   (-145 and -122, pooled -133, CI [-197, -69]), the loss landing on the seats
   that can fall (China annihilated more often on both sets; France below par
   more often). Knob-stacking is closed (journal 356) and both anchor
   constructions are closed (journals 274, 277), so **item 26's option (b) has
   no avenue pointing upward as currently built.** What is open is a recipe
   that does not pull a strong parent down. Before designing one, re-read
   journal 278's short-step result (item 5) ON THE CORRECTED INSTRUMENT --
   journal 273's three-seed verdict turned out to be unresolvable, and 278 was
   measured the same way. Price per arm: ~2.8 h training + ~35 min bench.
   Untested and cheapest to close first: a different TRAINING seed on the same
   recipe, since everything above is one training run.
   **DONE, journal 360: it degrades on the second training seed too.** Seed
   777001: 336 -> 245, -92 against a floor of 83, CI [-175, -8] -- a clear by
   nine points. The two training seeds' costs (-145, -92) are statistically
   indistinguishable. **China is the robust casualty**: down on every arm this
   question has produced (p 0.021, 0.010, 0.003) and annihilated on 4/8 worlds
   under both training seeds against 2/8. France is down in direction only
   under seed 777001 (p 0.267); the USA trends up on two of three arms, never
   significantly. The recipe question now has a sharper target: a training
   setup that pressures the seats that can fall (memory
   selfplay-erodes-rush-defence).
   **NOW A PRICED CHOICE, journal 364 -- the hypothesis has an existing test
   that has never been run properly.** The recipe of record never trains
   against a rusher (12 league draws across journals 357/360/363, all past
   selves, zero rusher, zero scripted opposition). `OD_LEAGUE_EXPLOIT` adds a
   league slot that plays SCRIPT_BLITZ and is drawn MORE while the policy loses
   to it; it is off by default. Its three earlier tests (4 Sep) were confounded
   (E), void (H) or dosed once in six maps (H2), from a different lineage, on a
   one-seed instrument. **Options, for the user:**
     (a) journal 357's recipe + `OD_LEAGUE_EXPLOIT=0.25` (capped, H2's dose)
     (b) ~~the same with `OD_LEAGUE_EXPLOIT=1` (E's dose, rusher 7 of 8 maps)~~
         **WRONG, corrected journal 366**: any value outside (0,1), including
         1, is read as the default cap 0.25 -- so (b) was (a), and E's
         uncapped 7-of-8 cannot be reproduced. A higher dose means a cap such
         as 0.5.
     (c) `OD_TRAIN_SCRIPTED_SHARE` -- a WEAK test: ~1.7% of countries rush at
         share 0.33
   Price per arm ~3.6 h training + ~20 min bench; comparable to
   it357-trained8 (same recipe, rusher off) and it349-control.
   **Two decisions ride with it.** DOSE -- H2's cap let the rusher in once in
   six maps, which is barely a test. And SEATS -- a rusher threatens
   1914:FRA:rush and 1939:NOR:hood most, and the three rung seats this sequence
   benches exclude both; they were excluded for being bistable (item 70), so
   judging this change means deciding how to read them.
   **Tree note:** the concurrent editor has 25 uncommitted src/ files incl.
   simulation code, so no rebuild; binary 84d15b60 is still valid for
   comparisons.
   **MECHANISM CHECK DONE, journal 365 -- not resolvable, and N24 is already
   weak against a rusher.** The 8-map no-rusher model against N24 on the two
   seats the hypothesis is about: 1914:FRA:rush -1.30 (p 0.70), collapsed 5/8 ->
   6/8, one world's difference; 1939:NOR:hood +0.08 (p 0.31), below par on 8/8
   in both arms. **N24 itself loses the rush seat on 5 of 8 worlds**, so there is
   little rush defence left to erode -- and real room for a rusher in training
   to HELP. **Measurement consequence for options (a)-(c):** judge on the rung
   seats, where journal 357's damage resolved; showing a gain on the rush seat
   is a rate question needing ~128 seeds per arm. Cross-binary check passed:
   the rush path is decision-identical on 84d15b60, so stored rush/hood rows
   stay valid.
   **RESULT, journal 366: training against the rusher REMOVES the damage --
   one seed set, and against a league that was 100% rusher.** Same recipe and
   seed as the no-rusher arm: **rusher 335 vs no-rusher 192, +143, CI [+52,
   +234]**; vs N24 336, -1, inside the floor. The first training run from N24
   not resolvably below its parent. China annihilated back to 2/8; France below
   par 1/8 instead of 5/8. The 0.5 cap did NOT bind -- the rusher took all 8
   draws -- because of two league-draw defects (item 97). Replication: item 96.
   PREREQUISITE DONE, journal 363: LENGTH IS NOT THE LEVER.** Journal 357's
   recipe with 8 maps changed to 2: **N24 336 -> 223, -113, floor 90, CI
   [-203, -23]** -- clears below. Against the 8-map model (192) it is +32 with a
   floor of 91, indistinguishable. So journal 278's two-map "parity" was
   three-seed noise, and the harm is already there after two maps rather than
   accumulating. "Train less" is off the table; the recipe question is what the
   model trains against, not for how long.

93. ~~REPLICATE journal 357 on a second seed set.~~ **REPLICATED journal 359.**
   -122 against a floor of 93 on journal 354's seeds, CI [-215, -29]; pooled
   -133, CI [-197, -69]. China replicates with significance on the new seeds
   alone (p = 0.003); France in direction only (p = 0.085); the USA flat on
   both, trending up on set 2. First candidate finding in journals 281-359 to
   survive a second independent seed set.
   ORIGINAL: **REPLICATE journal 357 on a second seed set. TOP OF THE QUEUE.**
   Training N24 on the settled recipe read **-145 against a floor of 94, CI
   [-239, -51]** -- the first candidate question in journals 281-358 to clear
   the rating floor. Journal 354 is why it is not yet a result: the pacify arm
   read -88 on the standard seeds and +31 on fresh ones. **Cost: 24 runs, ~35
   min, no training** -- bench `trained8.bin` (scratchpad, md5 c25f2a11) on
   journal 354's seeds (11111, 2468135, 777777, 31415926, 5772156, 1414213,
   9090909, 6180339) and compare with `it354-ctl-s2`, which already exists.
   **Must run on the current binary** (same as it354-ctl-s2): no rebuild first.
   Pre-register: FRA and CHN down, USA flat, rating difference clears again.

92. ~~[TRAIN] prints a model path the run does not write.~~ **FIXED journal
   362.** The line now prints m_aiModelPath: a worker run names
   ai/model.w1.bin, a non-worker run still names ai/model.bin, both verified in
   tiny isolated training runs. The rebuild also absorbed committed src/
   changes since 19:24 (including 2bf5c04, a turn-resolver fix), and the
   decision hash is identical on all three bench seats -- FRA, CHN and USA --
   so every stored comparison stays valid.
   ORIGINAL: **UNBLOCKED journal
   359, confirmed free journal 360** -- no pending comparison needs the current binary. The cost of taking
   it: after the rebuild, any comparison against it349-control, it354-ctl-s2 or
   the trained rows needs its control re-run first. Game_AITrain.cpp
   :220 prints "Model: <dir>ai/model.bin" on every training run, but
   setAIWorker (:191) redirects the path to ai/model.w%d.bin whenever --worker
   is given -- which the recipe of record always passes. The true path is
   printed two lines later for workers, so the log states both. Journal 358
   nearly benched the untouched parent against itself because of this.
   **A print fix, not a behaviour fix**: print m_aiModelPath, which is already
   correct by then. Cheap, and it removes a trap that costs whatever a training
   run costs -- currently about five hours (item 91).

91. **A training arm costs ~2.8 hours, not ~40 minutes. CORRECTED journal
   357: the "~5 hours / 75x" first written here was my extrapolation error.**
   The run finished in 168 min over 13,236 turns, not 24,000 -- maps end early
   on STAGNATION_TURNS (8 maps ended at 500-2900 turns of 3000). Per map it is
   ~21x journal 272's figure, not 75x. The conclusion stands, smaller: training
   estimates in this file are stale by an order of magnitude.
   ORIGINAL: **A training arm costs ~5 hours, not ~40 minutes. Every cost estimate in
   this file for training work is stale by 75x.** Journal 357's run (8 maps,
   3000 turns, OD_AI_THREADS=1, OD_LR_SCALE=0.05 -- journal 272's settled
   recipe) is running at **0.66 turns/second**; journal 272 recorded ~40 min
   for 40 maps, about 50 turns/second. Most likely the tree moved: the training
   log shows `embarks=6765 landings=4583`, and amphibious war only became real
   at v8.1 (memory landings-became-real-at-v81), so self-play now simulates
   something the old timing never did. **Re-price before promising any training
   work**: item 2's "~80 minutes" and journal 272's "~40 min" are not usable.
   One training A/B on the current tree is ~10 hours of machine time plus ~70
   minutes of benching.

90. ~~Is there anything between a knob and a retrained policy?~~ **TRIED AND
   CLOSED, journal 356: knob-stacking does not reach the measurable band.**
   The three highest-reach gates this sequence has COUNTED (not point-estimated)
   -- OD_CAMPAIGN_HOMEFIRST, OD_REINF_FALLBACK, OD_WAR_BAR_RESEARCH -- switched
   on together read **+14 against a floor of 83**, with no seat under p = 0.2.
   **The stack is smaller than one of its parts**: homefirst alone reads +56 on
   the same control. Both are inside the noise, so this is not evidence the
   gates interfere -- it is evidence that summing unreliably-signed effects sums
   noise, which journal 354 demonstrated directly by flipping one of them
   between seed sets.
   **Option (b) now has one avenue left: training.** It is the only intervention
   measured moving a model by hundreds (journal 271: 14 -> 159/178), and journal
   355 put the model-scale difference at +306 against a floor of 68.

89. **THE BALANCE SHEET. For the user, before item 26. CORRECTED journal 368 --
   the rusher repair is NOT established; it failed on a second training seed.**
       self-play training degrades N24    ESTABLISHED: -92 to -145, 2 training
                                          seeds, 2 bench seed sets, 2 lengths
       a rusher league repairs it         NOT ESTABLISHED: parity on training
                                          seed 424242 (replicated on 2 bench
                                          seed sets), -110 below N24 on 777001
       model c429f777 at parity with N24  ESTABLISHED on 2 bench seed sets
   The line below this was written before journal 368 and OVERCLAIMS: it
   described one model's replication as the recipe's.
   SUPERSEDED, journal 367 --
   the rusher repair is REPLICATED on two bench seed sets.**
       self-play training degrades N24     -92 to -145, 2 training seeds, 2 seed sets, 2 lengths
       a 100%-rusher league restores it    +143 / +165 over self-play, parity with N24,
                                           2 bench seed sets, 1 training seed (item 98)
   UPDATED journal 366 --
   the first training arm that does NOT degrade N24.** Journal 357's recipe
   and seed with the league rusher on (100% rusher, see item 97): **335 vs N24
   336, parity; +143 over the identical run without it, CI [+52, +234].** One
   seed set; replication is item 96. Every self-play-only arm still clears
   below N24.
   UPDATED journal 363 --
   and across TWO LENGTHS.** Added: seed 424242, **2 maps**, bench set 1:
   **-113, CI [-203, -23]**. Every training arm from N24 on this recipe now
   clears BELOW the parent: 2 maps and 8 maps, two training seeds, two bench
   seed sets.
   UPDATED journal 360 --
   the finding now holds across TWO TRAINING SEEDS as well.**
       training N24 8 maps degrades it
         seed 424242, bench set 1   -145   CI [-239, -51]
         seed 424242, bench set 2   -122   CI [-215, -29]
         seed 777001, bench set 1    -92   CI [-175,  -8]
       consistent casualty: modern:CHN, significant on all three arms
   UPDATED journal 359 --
   the first candidate finding is REPLICATED on a second seed set.**
       candidate questions that clear the RATING floor               1
       replicated on a second independent seed set                   1
         -- training N24 8 maps: -145 (set 1), -122 (set 2), CI excludes zero
            on both; the loss lands on FRA and CHN, not the USA
   It is a finding that the last option-(b) avenue points DOWN. See item 94.
   UPDATED journal 357 --
   the first candidate finding CLEARS, pending replication (item 93).
       candidate questions that clear the RATING floor               1
         -- training N24 on the settled recipe: -145, floor 94, CI [-239, -51]
         -- and it is a LOSS: FRA below par 5/8 (was 0/8), CHN annihilated
            4/8 (was 2/8), USA untouched
       replicated on a second seed set                               not yet
   If item 93 replicates, option (b)'s last avenue points DOWN on the current
   recipe, and the anchors that tried to hold a strong parent up are closed
   (journals 274, 277).
   UPDATED journal 355 --
   the instrument is SOUND; the knobs are an order of magnitude too small.**
   Journal 355 ran a positive control: loop-base against N24, same seats, seeds,
   turns and binary. **+306 rating points against a floor of 68, and all three
   seats on land at p = 0.001 / 0.000 / 0.007.** So the bench is not broken and
   every "not resolvable" in journals 337-354 means what it says.
       the bench resolves            ~68-90 rating points at 8 seeds
       two models differ by           306
       the largest KNOB measured       29   (pacification, pooled over 16 seeds)
   **A factor of ten**, and nothing between a knob and a retrained policy has
   been tried. Candidate findings that clear: still ZERO -- the control is an
   instrument check, not a result about the AI.
   UPDATED journal 354 -- the one result is retired.
       measurements that clear the RATING floor                       0
       measurements that clear a per-seat land test at 8 seeds        1
       of those, surviving a second SEED SET on the same model        0
   `1939:USA` under pacification read d -7.29, p = 0.010 on the standard eight
   seeds (journal 351). On eight FRESH seeds, same model, same seats, same
   binary: **+3.64, p = 0.442.** Pooled over sixteen: -1.83, p = 0.495. The
   whole-arm rating went -88 -> +31 -> -29 pooled, clearing nothing at any
   point. It was one comparison of three on one seed set.
   **What journal 354 also settled, and it cuts the other way:** root-n holds
   on this bench. The se fell 35.4 -> 24.5 and 28.6 -> 22.7 when the seeds
   doubled (predicted 25.0 and 20.2), so the table in LOOP.md is correctly
   priced and 32 seeds really would buy a floor near 42. **It still would not
   be enough:** the largest effect this project has measured is about -29 on
   the rating, which needs ~80 seeds per arm -- 240 runs, six hours -- to
   resolve. That is the real price of option (a), and it is per question.
   ORIGINAL (journal 353):
   After journals 281-353, on eight seeds and three rung seats:
       measurements that clear the RATING floor                      0
       measurements that clear a per-seat land test                  1
       of those, replicated on a second model                        0
   The one was `1939:USA` land share under the pacification reflex, p = 0.010
   on N24. On N47 it reads -2.23 at p = 0.618 -- same sign, a third the size,
   and N47's 95% CI [-10.05, +5.60] INCLUDES N24's point estimate, so the
   models do not disagree: the second simply cannot resolve it. N47 is not
   disqualified either, grading 18/24 and 19/24 against N24's 14/24 and 19/24.
   **And the whole-arm rating inverts sign between the models**, -88 on N24
   against +19 on N47. This is the concrete answer to "is eight seeds enough":
   no, and the shortfall is not marginal.

88. **The loop should stop deciding on the rating.** **WEAKENED by journal
   354: the per-seat land test is better, but it is not a way out.** Its one
   sub-floor success at eight seeds inverted on a second seed set. It remains
   the right statistic for a concentrated effect (journal 348) and it is still
   what `--compare` prints; it just does not lower the number of seeds needed
   to establish something.
   ORIGINAL: Journal 351 makes this
   concrete rather than stylistic. At eight seeds this sequence is **0 for ~15
   on the rating** -- the pacify reflex, which costs a third of the world and is
   the largest effect ever measured here, misses the floor by two points -- and
   the SAME 24 runs resolved `1939:USA` land share at p = 0.010, clearing the
   three-comparison correction. **The cheapest improvement available is not more
   seeds; it is a different statistic**, and journal 339 already built it into
   `--compare`. Relevant to item 26, which the user is deciding: raising the
   seed count buys less than changing what is read.

86. **OD_CAMPAIGN_HOMEFIRST is a RATE question and needs ~128 seeds per arm.**
   Journal 349 measured the gate's consequence as two collapse-rate changes --
   modern:CHN annihilated 2/8 -> 0/8, 1914:FRA collapsed 0/8 -> 1/8 -- against
   a bench that resolves ~0.49 at eight seeds (journal 292). Resolving a
   difference of 0.12 needs ~128 seeds per arm: 384 runs, about nine hours per
   arm pair. **Journal 348's per-seat sensitivity gain does not help here** --
   that gain is for a MEAN and these are proportions. So this is item 26 again,
   reached by a different road: the loop can measure this gate's mechanism at
   eight seeds and cannot measure its consequence at any seed count it may
   spend. **For the user: nine hours of machine time, or leave it off.**
   Note the shape of the risk before deciding -- it makes a previously stable
   great-power seat bistable, which memory worst-seat-swaps-identity says will
   read as a floor lift if only the mean is quoted.

85. ~~`crew <= 0` is a type test in disguise, and it cost an iteration.~~
   **DONE journal 347.** FOUR sites carry that filter, not one (the ship threat
   feature, the amphibious reflex, nearestLandingRange, and the navy
   executor's landing action) and all four mean "loaded boats only". The note
   lives at the reflex with the measurement; the other three point to it by
   name, not by line number.
   ORIGINAL:
   The landing action skips hulls on `s.crew <= 0`, which journal 346 shows
   means exactly "skip the warships". The behaviour is right and should NOT
   change. What is wrong is that it reads as an emptiness check: journal 342
   took it as one, filed three causes, and journal 343 registered a prior for
   one of them -- none of the three was possible. **A comment, not a code
   change** (same rule as item 84): say that only "boat" carries troops, cite
   ProceduralGenerator.cpp:1162, and give the measured split. Cheap.

83. ~~Bench OD_CAMPAIGN_HOMEFIRST.~~ **RUN, journal 349: PARK, not resolvable
   on any seat, and the pre-registered directions scored 1 of 3 against a
   chance expectation of 1.5.** FRA +6.08 (MDD 11.75), USA +2.15 (8.07), CHN
   +3.07 (9.65); rating 381 -> 424 against its own floor of 73-95. I predicted
   FRA and USA would PAY for the campaign yielding and all three rose instead.
   **The effect that is there is a pair of RATE changes the means hide:**
   China annihilated 2/8 -> 0/8, and France collapsed 0/8 -> 1/8 on a seat that
   was not bistable in the control (sd 5.27 -> 12.60). Successor: 86.
   ORIGINAL: It was filed as blocked behind item 26; journal
   348 shows that binds the RATING and not a per-seat land test, which on the
   same eight seeds resolves 8-12pp of land share per seat. This gate buys home
   defence, a per-seat quantity, so under item 72's rule it is pre-registered
   on per-seat land share with the permutation test journal 339 built, and the
   rating is quoted second. **Pre-register the DIRECTION and the seat before
   running.** Cost: two arms x 3 seats x 8 seeds = 48 runs, ~70 minutes.
   ORIGINAL (filed blocked):
   Journal 344 makes it the best-supported bench candidate the loop has: 2-3%
   of decisions at two sites, a mechanism measured rather than assumed, and a
   direction that is genuinely open (it trades campaign commitment for home
   defence, and memory ceiling-and-floor-are-one-decision says that trade cuts
   both ways here). But the war bar moved 3.84% of its evaluations and returned
   +42 against a ~60 floor -- "unresolvable" -- so an 8-seed arm here would
   probably say nothing. **Pre-register the statistic before the arm** (item
   72): this gate buys home defence, so the quantity is per-seat land share on
   the seats that can fall below par, not the rating.

84. ~~The campaign gate's comment describes a rule the code does not test.~~
   **FIXED journal 345, and TWO more were wrong.** Checking all four gates this
   sequence has measured against their own comments: the campaign gate named a
   condition the code does not test; the war bar presented a 14-19% one-way
   discount as a symmetric fidelity fix; the landing pick was accurate and
   silent on the frequency (16 choices in a run) that decides whether it
   matters; OD_REINF_FALLBACK was correct. **The one correct comment is the one
   this loop wrote under journal 331's convention** -- seat, model, seed, turns,
   difficulty beside every number -- and the three wrong ones predate it. That
   is a live argument for item 65.
   ORIGINAL:
   AISystem.cpp:6189 says the campaign yields "when the home front is losing
   ground"; the test is `provincesLost > 0 || worstDeficit > 0`, and journal
   344 measured the first term at 1-3% alone against the second at 39-41%
   alone. The gate is a garrison comparison that fires long before ground is
   lost. **A comment fix, not a code fix** -- do not "correct" the code to
   match the comment, that is a behaviour change wearing a documentation
   change's clothes. Cheap, and it stops the next person benching this from
   describing what they benched wrongly, the way journal 332 described
   OD_REINF_SIZED from a neighbouring comment.

81. ~~A third of the fleet has no crew.~~ **ANSWERED journal 346: they are the
   WARSHIPS, and all three filed stories were wrong.** 100.0% of crewless
   hull-turns are ships whose type is not "boat", and the counts equal the
   non-boat hull-turns exactly on two seats (16,672 and 21,466). Only "boat" is
   ever generated with crew (ProceduralGenerator.cpp:1162, repeated on load),
   so `crew <= 0` in the landing action is a TYPE test in disguise. **Boats are
   never empty** -- a landed hull is erased, a sunk one is swept -- so there is
   no empty-transport population and never was. Journal 342's "a third of hulls
   have no crew" must not be quoted as if it described transports; the landing
   action's real denominator is boat hull-turns, of which 72-74% reach the port
   scan. See 85.
   ORIGINAL: Journal 342, incidental and the
   largest single filter on the landing path: 33.0% of own hull-turns on
   1914:FRA and 25.3% on modern:CHN are hulls with `crew <= 0`. Distinct from
   memory navy-unreachable-not-underpriced, which is about ships never being
   BOUGHT -- these exist and are empty. Three different stories fit and the
   counter cannot tell them apart: a hull that has unloaded and not reloaded, a
   hull built without men, or a hull whose men died. **Count which**, in one
   seat, before reasoning about it. **Journal 343 moves the prior a long way**:
   a fleet that lands 74-81% of what it carries should be full of boats that
   have already unloaded, which is the first of the three. Still worth the
   counter -- it is cheap and the other two are not ruled out. If it is "unloaded and never reloaded" it
   connects to the embark path and memory embark-deletes-men.

82. ~~Is the fleet's distance a POSITIONING problem or a SCALE one?~~
   **NEITHER -- the question was built on a misreading, struck journal 343.**
   The boats are in TRANSIT: the reflex sails them at hostile ports and lands
   the cargo on arrival, and 74-81% of embarkations arrive. A distance averaged
   over a journey does not describe a parked fleet. **Do not raise
   shipMaxRangeDeg** -- journal 342 costed that lever and it aims at a problem
   that is not there. What IS true, narrowly: the HEAD's landing action is idle
   for reach, being a snapshot test from wherever a boat happens to be.
   ORIGINAL: Journal
   342 measured the gap (4x hull range, always) but cannot say whose fault it
   is: ship positions are identical whether the head or the amphibious reflex
   commands, so the number describes where the navy sits, not who steers it.
   Two readings: the boats never sail toward the enemy, or `shipMaxRangeDeg`
   is small for these map scales. **The counter that separates them:** does the
   nearest-hostile-port distance FALL over a run (boats closing) or stay flat
   (boats parked), and how many hull-turns carry a sail order aimed at a
   hostile coast. One seat, read-only, same probe.
   **Do not just raise the range.** It is a game constant that reaches the
   player and the scripted rung equally, so it cancels in a relative metric
   (memory: anything both cohorts share cancels out) and changes the game for
   everyone -- the user's call, not the loop's. But record the size of the
   lever: **24-29% of hull-turns are within DOUBLE the current range**, so a 2x
   range would take "a shore in reach" from 0.08% to roughly a quarter.

79. ~~Whose landings did "0% -> 74%" measure?~~ **THE REFLEX'S -- ANSWERED
   journal 343, and the 74% REPLICATES.** Two seats: every embarkation is the
   head's (one site), and 97-100% of hostile landings are the amphibious
   reflex's. The head ordered 1 and 57, matching exactly the 1 and 57 times
   journal 342 counted it finding a shore. Landings per embarkation 0.813 and
   0.740 against journal 37c's 0.74 on a different harness, map, model and
   horizon. **The registered guess that it was a harness artefact was wrong.**
   ORIGINAL: Memory embark-deletes-men
   records landings going 0% to 74% after two fixes; journal 341 finds the
   HEAD's landing action almost never has a shore to reach. The reconciliation
   that fits is that those were the amphibious REFLEX's landings, not the
   head's -- consistent with journal 310 (the NAVY module is 92% book) and 323
   (ship actions rarely offered). **Unverified.** Registered rather than
   believed, per memory measurements-replicate-explanations-dont. Cheap to
   settle: count reflex-issued against head-issued disembark orders in one run.

80. ~~OD_CAMPAIGN_HOMEFIRST, the last genuinely untested gate.~~
   **MEASURED journal 344: the LIVEST of item 67's three, by two orders of
   magnitude.** Campaigns are open on 6-9% of recruit/reinforce decisions, the
   condition holds on ~44-52% of those, and the pick then actually changes on
   73-97% -- so the gate moves **2-3% of decisions at each of two sites**,
   against OD_WAR_BAR_RESEARCH's 0.68-3.84% of evaluations and
   OD_LANDING_PICK's sixteen decisions in a run. Item 67 is now fully closed.
   **And the condition is not what its comment says:** it is carried entirely
   by `worstDeficit > 0` (a neighbour outguns one garrison); `provincesLost > 0`
   contributes 1-3% alone. See 83 and 84.
   ORIGINAL: The only one
   of item 67's three still unexamined. Unlike the other two its comment makes
   a PREFERENCE claim ("the untested middle" between full commitment and
   recall), not an arithmetic one, so the journal 337/341 probe method does not
   apply directly -- there is no "would this flip" to count, only "how often
   does the home front qualify as losing" (`st.provincesLost > 0 ||
   st.worstDeficit > 0`). That IS countable and is the right first move: if
   the condition is rare the gate is inert like OD_LANDING_PICK, and if it is
   common the gate is a large change needing item 26's seed standard.

77. **The score file still publishes a 30-turn run as a rating.** Journal 340
   gated bench_score.txt on the seat set and on --quick, and added turns and
   difficulty to the line, but did NOT gate on turns: od_bench's own default is
   120 while every measurement since journal 281 used 400, so writing 400 into
   the writer makes the default self-contradictory. A reader can now refuse a
   short run; nothing stops one being written. **This is a scoring-standard
   decision and belongs with the user** -- the same decision as item 26's seed
   count, and probably the same conversation: what is a rating, over how many
   seats, how many seeds, how many turns.

   ORIGINAL: **"better on N/M seats" was dividing by the full seat set.** ~~Open.~~
   **FIXED journal 339** -- a 4-seat run printed "better on 3/6", counting two
   seats it never ran as seats it lost. Now "N/M seats measured by both".
   Filed because the class matters more than the line: this file has three
   entries now (journal 338's seat-filter expansion, 339's miscounted
   multiple-comparison note, and this) where the defect was a DENOMINATOR that
   was not what got measured. Worth one pass over od_bench's remaining ratios.

   ORIGINAL: **Pre-register on the statistic the mechanism moves, not on the rating.**
   Journal 338's whole effect landed on modern:CHN -- median share 14.3 -> 26.0,
   the only seat near significance -- and modern:CHN is PINNED AT CAP in both
   arms, so 13.4% and 23.2% of the world both score exactly 500 and cancel.
   The largest movement in the experiment was arithmetically invisible to the
   number the hypothesis was written on. Memories floor-not-rating and
   capped-small-par-seats-are-coins both say this and were both in context.
   **Concretely, for the next arm:** if the mechanism buys LAND, pre-register
   per-seat land share and a permutation test on it; quote the rating second.
   This costs nothing and is not blocked behind item 26.

73. ~~OD_WAR_BAR_RESEARCH is still open, on the right statistic.~~ **CLOSED journal 396: NULL at 96 seeds per arm**
   (+10.3, floor 19.2; the 48 fresh seeds alone +0.2; SWE annihilation 8/32 -> 14/32, p 0.188). Journal 337's
   pre-registered "the rating goes DOWN" is falsified in direction: it goes slightly UP, unresolvably.
   ORIGINAL: Journal 338
   measured it on the wrong one and 337 measured what it does. If it is revisited:
   the question is whether +9.7 mean share on modern:CHN (p=0.07, 24 runs)
   survives more seeds, and the answer is worth having because it is the
   largest per-seat movement this sequence has produced. That needs item 26's
   decision, so it is BLOCKED, not abandoned. The bill to weigh against it:
   world army 220M -> 169M units at turn 400.

74. **The two-part OD_BENCH_SEATS pattern expands.** ~~Silent.~~ **FIXED
   journal 338** -- the filter now prints the seats it matched and flags when
   patterns expanded. Kept because the lesson is not the fix: the run DID say
   "4 seats x 8 seeds" in its header and that was read past, so the defect was
   an unemphatic true statement, not a missing one.

   ORIGINAL: **Bench OD_WAR_BAR_RESEARCH, with the direction pre-registered.**
   Journal 337 measured what the gate DOES without measuring what it is worth:
   it lowers the war bar by 14-19% for the 73-83% of countries carrying army
   research, and flips 0.68%/3.84% of evaluations on two seats, 98% of them
   toward war. That is the class of change that has cleared item 26's ~60-point
   floor before (research bar 0.65 read +42, pacify -86/-88), so it is worth
   24 runs where the industry reflex was not.
   **Pre-registered, journal 337: the rating goes DOWN.** Reasons, both from
   this tree: the bar 15 lines above it was raised from 1.05 on purpose because
   "the map was permanently on fire", and memory passivity-is-load-bearing
   records three attempts to make this AI fight more costing 40-90% of the
   world. A NULL is the second most likely outcome and must be reported as
   "not resolvable at 24 runs" (LOOP.md STANDING 1), not as "no effect".
   Two loose ends the arm should NOT be allowed to quietly absorb:
   - ~~WHY atk runs 2.5x def is unknown.~~ **Research half answered, see item
     100 (journal 372).** The research nodes total +60 atk to
     +50 def, nowhere near 2.5x, so the rest is doctrines or a selection
     effect in which countries reach that line. Separating them is a counter,
     not a bench arm, and it is the more interesting question: if the
     asymmetry is a DOCTRINE artefact it is a balance bug in its own right,
     visible to human players too.
   - 3,108 flips is an upper bound on extra wars, not a war count -- passing
     the bar makes a neighbour a candidate, and most flips are redundant with
     a candidate that already passed. Count declarations before quoting.

70. ~~Did any rejected change improve SURVIVAL?~~ **NO -- journal 336, and the
   reason matters more than the answer.** Eight arms recomputed: not one
   survival interval clears zero, largest movement 8 points on a control value
   of 92/100. **On the three RUNG seats survival IS China's annihilation rate**
   -- France and the USA are above par on every seed, so min(seat,100) returns
   the cap and they contribute constants; survival = (100+100+China)/3.
   So LOOP.md's "survival not down" clause has been costing nothing, and this
   sequence never reporting it cost nothing either.
   **The tension, unresolved:** survival only varies on the small-par seats
   (1914:SWE par 1.0, 1939:NOR:hood par 1.3) -- which are exactly the seats
   excluded for being bistable. The seats that make survival meaningful are the
   ones that make the RATING unreliable. That is a seat-set question and
   belongs with items 16 and 26.
   Nice case: the journal 334 pair reads -0 survival because China rises
   75 -> 100 while the USA falls 100 -> 71 (below par for the first time in any
   arm). The two cancel.

66. ~~How much of the AI is switched off?~~ **COUNTED journal 332: 33
   default-off boolean gates.** Of 199 OD_* names / 136 getenv sites: 33
   boolean gates default OFF, 3 with computed defaults, 21 value knobs, 79
   traces/selectors/paths (sampled -- no mechanisms hiding there).
   **This sequence has examined 12**; 21 were never looked at. Of those 21,
   **16 sit beside a comment containing numbers** -- off with an argument, i.e.
   a finished experiment -- and **FIVE have no numbers anywhere near them**:
   OD_CAMPAIGN_HOMEFIRST, OD_LANDING_PICK, OD_REINF_GUARD, OD_REINF_SIZED,
   OD_WAR_BAR_RESEARCH. Built paths, switched off, no recorded reason -- the
   same shape as OD_RECRUIT_PICK, which journal 330 found was a real fix for
   the AI's second-largest waste sitting off.
   Caveat: "has numbers nearby" is a crude proxy for "was measured", and
   journal 331 showed many such numbers lack the configuration to be usable.

67. ~~The five unexplained gates.~~ **READ journal 333 -- only THREE are
   unexplained, and journal 332's proxy missed two.**
   **MEASURED, badly:** OD_REINF_GUARD ("+34/+19 fitted, +24/+8 one hold-out,
   -1/-11 another... mean +12 rating, -11 floor. Another trade, not a fix") and
   OD_REINF_SIZED ("takes the worst seat to ZERO on both models"). Both are
   documented in ONE comment beside REINF_GUARD, so SIZED's evidence sits in a
   different function -- "numbers near the gate" is not "numbers about the
   gate". Journal 332's headline should read: at least 18 of 21 have evidence.
   **GENUINELY UNTESTED (was 3, now 1):** OD_CAMPAIGN_HOMEFIRST ("the untested
   middle") -- see item 80.
   ~~OD_LANDING_PICK~~ **CLOSED journal 341: live, but the decision is almost
   never offered.** Two seats, 71,373 hull-turns considered, a hostile shore in
   range 58 times (0.08%), a CHOICE of shore 16 times. Given a choice, container
   order is not the weakest 12 of 16 -- so the pick really is arbitrary, on
   sixteen observations, which is not a measurement. Not worth a bench arm: a
   change touching 16 decisions cannot clear item 26's ~60-point floor.
   ~~OD_WAR_BAR_RESEARCH~~ **CHARACTERISED journal 337 -- see item 71.** It is
   live (0.68% of war-bar evaluations flip on 1914:FRA, 3.84% on modern:CHN)
   and 98% one-directional toward declaring war, because the attack modifier
   runs ~2.5x the defence one on both maps: it is a 14-19% DISCOUNT on the war
   bar, not the symmetry fix its comment describes.

68. ~~Test OD_REINF_FALLBACK and OD_REINF_SIZED TOGETHER.~~ **REJECTED journal
   334 -- the first unambiguous rejection in this sequence.** Graded pair vs
   control: **-142, CI [-209,-75], 0/8 seeds, perm p=0.007.** Against the
   fallback alone: **-121, CI [-163,-80]**. Seat means 317 / 296 / 175.
   **The comment's claim is falsified**: it says "do not make the quantity
   dynamic without solving source selection first -- they are one rule", which
   reads as "selection is the prerequisite and sizing is then safe". Journal
   328 solved selection and sizing on top costs 121 more. Solving selection did
   not make sizing safe; plausibly it made it worse, since the fallback spreads
   the draw and sizing then empties several provinces instead of one.
   Dies on the USA seat (19.6 -> 8.3). [NOOP] also went the wrong way:
   44,291 -> 68,767. Both flags stay OFF, now with a measured reason.
   **Only change in journals 281-334 to clear the ~60-point floor at all.**

69. ~~A rising graded count is not automatically good.~~ **TRUE, BUT IT DOES NOT
   REACH JOURNAL 319 (journal 335).** Two different uses: ONE model with the cap
   varied (j.319) holds the run fixed, so un-pinned observations are pure
   COVERAGE and maximising them is correct -- ranking verified cap-invariant at
   5/8/10/20. TWO arms at a fixed cap (j.334) is where the count can rise
   because an arm got worse. **Item 52's "raise CAP 5 -> 8" stands unchanged.**
   **And the check found the real error in journal 334:** its 19/24 was
   explained as "the USA stopped pinning at the cap". Counted, 3 of the 5 extra
   observations are CHINA, two of them China no longer being ANNIHILATED -- the
   opposite end of the scale. The asserted mechanism covers 2 of 5.
   Also softens 334 slightly: the rejected pair is -142 on the rating AND stops
   China dying twice. The REJECT stands; "worse in every way" would not have.

 Journal 334: the
   rejected pair graded 19/24 against the control's 14/24 -- because the USA
   stopped pinning at the cap, i.e. stopped being good enough to saturate.
   Journal 319 treated the graded count as the thing to maximise when costing
   the cap change. It is a comparability check, not a quality metric.

 The REINF_GUARD
   comment says: "the flat 50 is load-bearing... sizing the move removes that
   accident and takes the worst seat to ZERO. **Do not make the quantity
   dynamic without solving source selection properly first -- they are one
   rule, and this pair is the evidence.**" Journal 328 solved source selection;
   journal 329 tested it alone and got -21 spanning zero. If selection and
   quantity really are one rule, testing either half alone is why neither
   shows anything. **Concrete, mechanism-stated, and the first candidate in
   this sequence with a written reason to expect an interaction.**
   Cautions: sized ALONE took the worst seat to zero on both models, and
   ablations-dont-compose says the pair's sign is not predictable from the
   parts. Still bounded by item 26.

 OD_CAMPAIGN_HOMEFIRST, OD_LANDING_PICK,
   OD_REINF_GUARD, OD_REINF_SIZED, OD_WAR_BAR_RESEARCH. Each is one read to
   find out what it does and whether anything was ever measured. Cheap, and the
   precedent is that this class contains real fixes. **Not a recommendation to
   switch anything on** -- journal 330's two gated fixes both improved their
   refusal rate and neither is recommended, because the bench cannot resolve
   what they do to play.

63. ~~Measure the recruit no-op fixes.~~ **BOTH WORK ON THE RATE (journal
   330), both stay OFF.** Refusal rate 39.5% -> **28.1%** (OD_RECRUIT_PICK) and
   -> **24.7%** (OD_RECRUIT_MASK). PICK also makes the action be chosen 51%
   more often (played 26,019 -> 39,364), so its RAW count rises -- my
   pre-registered threshold was a raw count and was the wrong statistic.
   **The source's ranking is reversed here**: its comment says tightening the
   mask "does not help (108,650 -> 104,895)" and that the CHOICE is the defect;
   on 1914:FRA/N24/13579 the mask is the better of the two. Its numbers carry no
   seat, model or seed -- the second time an undocumented measurement has cost a
   comparison (the reinforce comment's 3,181 was the first).
   Neither recommended: total refusals move opposite ways (mask -12%, pick
   +28%) and journal 329 showed the bench cannot resolve this class.

64. ~~Record seat, model and seed beside every number in a source comment.~~
   **DONE journal 331.** Four comments fixed: my own from journal 328 (the
   omission committed two entries after filing this item), the reinforce mask's
   "3,181", the recruit mask's "108,650", and execWar's ranking claim -- which
   now carries journal 330's rates that reverse it. Old figures are marked
   UNRECORDED rather than given guessed configurations. Comment-only, decision
   hash unchanged.

65. **FOR THE USER -- put the convention in LOOP.md section 7.**
   **EVIDENCE ADDED journal 345:** of the four measured gates, the ONLY comment
   that described its gate correctly was the one written under this convention;
   all three that predate it were wrong, misleading or silent on the number
   that decides the question. That is the closest thing to a controlled
   comparison this question can get.
   ORIGINAL: Journal 331's
   real finding is that I violated this convention in journal 328 while the
   sequence was already paying for it, and filed the item in 330 without
   noticing my own comment was an instance. A rule broken by the person who
   just wrote it is not forgotten, it is unenforced by the format. Section 7
   already says to update the journal, the backlog and memory; one more line --
   "a number in a source comment carries its seat, model, seed and horizon" --
   is read once per iteration. LOOP.md's protocol text is yours.


   Journals 326 and 330 both hit a documented measurement that could not be
   compared against a fresh one because its configuration was not written down
   -- "3,181 times in a 400-turn run" and "108,650 refusals in two 400-turn
   games". Both were careful, numerate observations rendered unusable by one
   missing line. Cheap convention, and this sequence has now paid for it twice.

41. **FOR THE USER -- the candidate fix, and why the loop cannot test it.** Pay
   the politics head in PROPORTION for the upkeep it frees, rather than by the
   sign of netIncome. Blocked twice over: (a) it is a reward change on a
   CONVERGED head, so by resetModuleHead's own doc it needs --reset-ai-head
   plus a retrain to be measurable -- "the head has to be told, not persuaded";
   (b) journal 304 showed the bench resolves nothing under ~60 points at 8
   seeds, so the retrain could not be judged. Needs item 26 first, same as 34.
   Note the standing caution: memory diplomacy-rules-not-rewards records that
   reward shaping here "only ever found the corners". Journal 308's open
   puzzle: a fresh head puts 0.65 on repress and 0.68 on enacting a costed
   doctrine, and training drives BOTH to exact float underflow. That is far
   past "mildly disfavoured" -- six of twelve politics actions are being
   annihilated by something very strong, and nobody has looked at the reward
   terms that do it. This is a REWARD question, not a rule one, and it is what
   journals 305-308 have been circling. Cheapest start: the politics reward
   decomposition per action, not another bench arm.

40. ~~Two documented instruments did not exist.~~ **AUDITED AND CLOSED journal
   311: 20 of 21 real, the last one wired.** Checked every OD_* var (7/7), flag
   (10/11) and tool (3/3) LOOP.md names. Missing: `--probe-trade`, described in
   LOOP.md, BACKLOG.md AND the community roadmap -- which asserts it "exists".
   `Game::runTradeProbe` was defined with a full doc comment and NO CALLER.
   Wired; first run **PROBE_OK, 5/5 rule cases**. So the trade rules are
   correct and had never been executed -- journal 256's "trade taken 0.00%" is
   not the rules failing, it is that no eval proposes a trade.
   All three gaps now closed: policy-shape table (306), --reset-ai-head (308),
   --probe-trade (311).

43. ~~--probe-trade saves the model on exit.~~ **FIXED journal 312.** Sets
   `AISystem::s_readOnlyModel`, the switch --eval-ai and --bench-agent already
   use. Verified: scratch ai/model.bin byte-identical across a run (journal
   311's run had rewritten it), PROBE_OK still 5/5, no save or checkpoint line.
   **Checked the class:** five sites construct an AISystem -- training and
   normal play save by intent, --eval-ai and --bench-agent are guarded at their
   call sites, and the probe was the only unguarded measurement path. It was
   unguarded because I wired it in 311 without noticing the precedent.

44. ~~The no-save guard sits at the call site, not in the function.~~ **FIXED
   journal 314.** `s_readOnlyModel` is now the first statement of both
   `runBenchAgent` (2554) and `runTradeProbe` (2794), ahead of their AISystem
   constructions (2595, 2810). Proved by REMOVING the probe's call-site guard:
   scratch ai/model.bin byte-identical across a run, PROBE_OK 5/5.
   ServerMain:344 kept as redundant cover on the bench-agent path.
   Closes the 311-314 sequence: wiring a flag without copying an unwritten
   convention, catching it, fixing it where the precedent was, then moving it
   somewhere it cannot be forgotten. Both
   measurement paths (runBenchAgent, runTradeProbe) rely on ServerMain setting
   s_readOnlyModel before calling them, so each is only as safe as every future
   caller's memory -- exactly what failed in journal 311. Setting it at the top
   of the two probe functions makes them safe by construction; training and
   normal play never call either, so nothing else moves. Small, but a behaviour
   change in shared code. It builds a real AISystem and
   the normal teardown persists it, so it writes `<data>/ai/model.bin` and a
   league checkpoint. Journal 311 passed --data to a scratch tree and verified
   data/ai/model.bin unchanged, but against the default tree this would
   overwrite the file hard rule 2 protects. Either suppress the save in the
   probe path or refuse to run without an explicit --data. Small, but it is a
   behaviour change in shared code. Journal 306: LOOP.md's
   policy-shape table. Journal 308: `--reset-ai-head`, cited in AISystem.h,
   LOOP.md and ai_bench.py twice, never wired. Both found by following the
   protocol literally. Worth one pass over LOOP.md and tools/ checking that
   every instrument named there still exists -- cheap, and the failure mode is
   silent. An untrained policy is
   near-uniform. If --reset-ai-head shows repress at ~1/12, the zero in all
   three models is LEARNED (and journal 256's "priced refusal" reading is
   right); if it shows ~0, the masks or reward make it unreachable by
   construction and the reward is where to look. ONE run. Needs a model file
   written, so it needs the user's go-ahead.

38. ~~The picked column counts the BOOK.~~ **FIXED journal 310, and it was
   far bigger than the one action that prompted it.** Added a BY POLICY column
   behind the netDriven/!booked/!scripted gate. **Only 56% of decisions in a
   400-turn run are the policy's** -- ECON 83%, POLITICS 58%, WAR 48%,
   **NAVY 7.9%**. NAVY a0: 19,861 plays, zero policy picks. Discrimination
   passed on both known cases; decision hash unchanged. The naval record in
   this project (memory navy-unreachable-not-underpriced, the naval reflex
   comment, journal 256's map) rests on a column that is 92% book for that
   module -- worth re-reading before any naval conclusion is reused.

42. ~~Re-read the naval conclusions against the BY POLICY column.~~ **SETTLED
   journal 313: the claim is TRUE, its evidence was not.** "The navy module is
   healthy" holds on France and China -- the policy is asked ~3,048 naval
   questions per run and takes 5 of 7 actions. But the comment says it was
   measured on the NORWAY seat, and there the policy makes ZERO naval decisions
   in a run containing 54,879. Not a cohort artefact: ECON on that same run is
   99% policy. **Why the policy is never asked a naval question on that map is
   unrecorded and unexplained.**
   Instrument fixed: pi(a) now prints its sample count, because "asked and gave
   zero" and "never asked" both rendered as 0.00e+00 -- opposite findings,
   identical output. My defect from journal 306.

45. ~~Why is the policy never asked a naval question on 1939:NOR?~~ **ANSWERED
   journal 315: 99.4% of its net-driven naval decisions have only ONE legal
   action.** Not the book (143 turns), not the cohort split (scripted 0), not
   routing. `pickAction` fills nprob only when `validCount >= 2`, deliberately
   and with the reason in the comment -- so **pi(a) and BY POLICY count
   CHOICES, not actions**. France is the same mechanism at 79% no-choice /
   21% choice, and that 21% is the 3,048 decisions where it steers a fleet.
   **1939:NOR:hood cannot say anything about the policy's fleet behaviour**;
   it is fine for everything else (ECON 199,655 passes on the same run).

47. ~~Journal 310's "56%" -- retire the headline.~~ **DONE journal 316, and the
   same defect was found in journal 305's headline.** `s_offHist` counts every
   valid action on every decision -- all countries, single-option decisions
   included -- so "421,519 ignored offers" is really **225,207 declined
   choices** (53.4%). Repress: 126,783 offers -> **70,887 real choices, zero
   taken**. The inflation is PER-ACTION, 9% to 98%, so no correction factor
   exists; each row needed measuring. The zeros are untouched.
   **Lesson: counters written for one question get quoted for another.**
   s_offHist was built to show which actions are legal and got quoted as how
   often the AI declined them.

48. ~~Re-check the remaining s_offHist quotes.~~ **DONE journal 317: the naval
   reflex comment's port figure is unsupported on its own seat.** On 1939:NOR
   the policy was offered a PORT as a choice **0 times** (s_offHist says 6,591)
   in a run where the econ head faced 102,455 choices. Warship: 3,583 choices
   declined, not 6,020 offers. The research-funding half survives (97.9% vs the
   quoted 95.5%) because that action is offered constantly.
   **Both of that comment's claims are now corrected** -- journal 313 found the
   "navy module is healthy" measurement was of the scripted world on the same
   seat. The refusal itself reproduces on France and China (1,134 port and
   4,639 ship choices, all declined), so the DEFAULT is unaffected.

50. ~~Is the model that ships the best one on disk?~~ **UNRESOLVED journal 318,
   and it cannot be resolved on this seat set.** N37 (the backlog's "model of
   record") against N24 (what ships) on 8 fresh seeds: graded pair -37 CI
   [-111,+37] perm p=0.37, three seats -38 CI [-150,+74]. Both span zero.
   **N24 stays** -- an incumbent needs no justification. But the comparison is
   compromised: N24 loses its instrument on CHINA (0/8 graded), N37 on the USA
   (3/8, five seeds pinned at the cap because it plays the USA well). Different
   dead seats = journal 288's not-comparable condition.

52. ~~What would raising the cap actually buy?~~ **COSTED journal 319, zero
   runs.** CAP 8 takes every model from 10-18/24 graded to **20/24**; CAP 10
   reaches the ceiling for N24 and N47 (22/24). Beyond that nothing -- the
   residue is WIPED observations and no cap recovers an annihilated seat.
   **The ranking is unchanged at every cap** (N24 > N37 > N35 > N47), so no
   verdict flips and the cap was never hiding a better model.
   **But it buys no PRECISION**: journal 318's N37-vs-N24 moves from -37 to
   -11 and the interval WIDENS (the un-pinned seats have the most spread).
   So items 16 and 26 are complements, not alternatives.
   **Concrete recommendation for the user: CAP 5 -> 8 in tools/od_bench.py.**
   Note it breaks comparability with every rating recorded before it, exactly
   as journal 29b did.

51. **Item 16 has now caught three models, and it compounds with item 26.**
   N35 (USA 0/8 graded), N37 (USA 3/8), N24 (CHN 0/8). Every model benched on
   these seats loses a third of the instrument somewhere, and **a model is
   penalised for being GOOD at a seat** -- N37 pins the USA by outplaying it.
   Raising the seed count does not fix a pinned seat, so items 16 and 26 are
   not independent: the seat set has to be fixed before more seeds are worth
   buying. Both are the user's.

49. **FOR WHOEVER OWNS THE NAVAL REFLEX -- its comment needs rewriting, not a
   number swap.** Both its measurements were taken on 1939:NOR, which journal
   315 showed cannot speak for the policy's fleet (99.4% of its naval decisions
   have one legal action). Replacement numbers, measured: port 1,134 choices
   declined and ship 4,639 on France+China, pi(a) 0.00e+00 on both; the navy
   module IS driven by the policy there (3,048 naval choices, 5 of 7 actions
   used). The conclusion -- leave the reflex off -- stands on the new numbers.

   305 and 310. The naval reflex comment ("offered a port 1,914 times and a
   warship 6,020 times") and journal 256's map ("trade offered 7.5%",
   "industry offered on 6% of decisions") all use the same counter. Each is a
   comment or a map entry that someone will read as the policy's behaviour.
   Cheap -- the numbers are in the runs already made.

   headline.** The played/policy ratio's denominator includes single-option
   decisions (99.4% of Norway's naval ones) and counts every country while the
   numerator counts the model cohort. It never measured "what fraction of the
   game the AI drives". The defensible questions are: was the policy offered a
   CHOICE, and what did it do. Reword the memory and stop quoting the ratio. Journal 313:
   54,879 naval decisions in the run, zero reaching the policy, while ECON on
   the same run is 99% policy. Either the model cohort holds no ships on that
   map or naval decisions are routed around the policy there. It is the bench's
   coastal seat and the one the naval argument has always been made on, so the
   answer decides whether that seat can say anything about fleets at all.

46. ~~Journal 310's "56% is the policy" needs the caveat.~~ **SUPERSEDED by 47
   and done** -- the caveat landed in journal 313 and the memory was retired in
   315. Struck journal 316 as bookkeeping. The played column
   counts every country; the policy column counts the model cohort only, so a
   low share is partly the scripted rung being numerous. The sharp questions are
   whether the policy was ASKED and what it did -- not the ratio. Journal 313
   nearly took the loose reading. Journal 310
   shows the NAVY module is 92% book. Everything this project believes about
   the AI's fleet was measured on the played column. Cheap: one run, already
   instrumented, no bench. Do this before item 32 ever un-blocks. `s_actHist` records the action
   played including booked/scripted turns; `s_probSum` (pi(a)) counts only
   netDriven unbooked non-scripted decisions. NAVY a0 shows 19,861 picks at
   pi = 2.57e-07. Reading picks as "what the AI does" reads the book. Journal
   306's ten-dead list came from picks; the pi(a) measure gives sixteen on the
   same model. Worth a note beside the ACTHIST output.

34. **The politics head is collapsed and the map says the ECON head is.** **RE-MEASURED journal 398 on the current
   build: 7 of 12 politics actions are never played and 8 of 12 never chosen by the policy** (journal 305 had 6 of 12),
   on 3 seats x 2 seeds with seat-filtered histograms. **a1 "enact doctrine" is offered 201-250 times per run and picked
   0 times, pi(a) = 0.00e+00** -- a learned refusal, not an illegal action. So no head-based rule (bias, weight, mask)
   can move it. The two routes left: a gated doctrine REFLEX that bypasses the head, or a head reset plus retrain.
   ORIGINAL: **The politics head is collapsed and the map says the ECON head is.**
   Journal 256's subsystem map names the econ head as the collapsed one and
   does not mention politics; journal 305 finds six of twelve politics actions
   unreachable against four of twelve econ. Any rule built on a dead politics
   action is silently inert (memory: saturated-heads-return-one-number), so
   before ANY new politics rule, check its action against journal 305's list.
   **Do not force these on** -- three attempts to force a declined action have
   cost 40-90% of the world. The useful question is whether a retrain with
   those actions reachable behaves differently, which is a training question
   and needs item 26 settled first.

32. **The other two disabled reflexes.** Journal 302 found naval, industry and
   researchAusterity default-off and not in OD_ABLATE. Industry is done (31).
   navalReflex is NOT worth running -- forcing ships has cost ~40% of the world
   twice. researchAusterity has never been priced at all. Blocked behind item
   26: at 24 runs it would return the same "not resolvable" as industry.

26. **PAIRING BUYS NOTHING ON THIS BENCH -- change the design.**
   **NARROWED BY JOURNAL 348: it binds questions asked of the RATING, not all
   questions.** The floor is a property of the statistic. For an effect
   concentrated on ONE seat, the per-seat land-share test on the SAME eight
   seeds is 1.58x (1914:FRA) to 1.92x (1939:USA) more sensitive, because the
   rating divides a single-seat effect by three while reducing its noise only
   by root three. On modern:CHN the rating can NEVER resolve a land change at
   any seed count -- both arms sit above the 5x cap -- while the land test
   resolves 9.65pp. A DIFFUSE effect is still better read from the rating, and
   that is still floored. **Also corrected here: the floor is 92 rating points
   at 8 seeds, not the 59 LOOP.md quotes** -- the se of 33 is right, the
   conversion dropped the root-two for an unpaired difference.
   ORIGINAL: **AS OF JOURNAL 347 THIS WAS RECORDED AS THE ONLY THING BLOCKING
   THE LOOP.** The
   unblocked queue is down to item 75, which is low-priority by its own text.
   Blocked on this: 32, 34, 73, and 83 -- the last being
   OD_CAMPAIGN_HOMEFIRST, which journal 344 measured at 2-3% of decisions at
   two sites and is the best-supported arm this sequence has produced.
   **JOURNAL 304 MAKES THIS THE BINDING CONSTRAINT, not a note.** Industry was
   the best candidate available -- a whole disabled subsystem, matching the
   user's standing request to see the AI industrialise, of a compounding kind
   that could plausibly be large. At 24 runs per arm it returned nothing, and
   at a pooled se of 25 the loop could not have detected a +40 gain and would
   have called it a null. **The loop's constraint is not an empty queue, it is
   the seed count.** Two options, both the user's: (a) raise the standard to 32
   seeds -- 96 runs/arm, ~100 min, se ~15, resolves ~29, making journal
   208-sized effects visible; (b) stop testing knobs and spend machine time on
   changes big enough to clear 60 -- new mechanisms, or training, the only
   thing measured to move this model by hundreds. Recommended: (a) for queued
   questions, (b) for new work.

   ORIGINAL: Journal 296: Journal 296:
   variance saved by seed-pairing is -12% to +11%, i.e. zero. Journal 291 saw
   this on the rush seat and scoped it to that seat; it holds on the graded
   rung seats too. Consequences: (a) matched arms are not worth arranging,
   (b) precision comes only from MORE SEEDS -- se ~30 at 8 seeds, ~21 at 16,
   ~15 at 32, (c) **the 8-seed standard only resolves effects above ~60**, and
   since journal 281 exactly one measurement has cleared that (pacify -86).
   Decide the standard seed count deliberately: 8 seeds is 24 runs and sees
   almost nothing; 32 seeds is 96 runs and sees ~29. Most of this sequence's
   "nulls" are really "below the floor".

27. ~~Re-phrase the nulls in journals 285, 290, 293, 295.~~ **DONE journal
   303.** A CORRECTION block now sits two lines under each of those four
   headings, so the reader meets it before the claims instead of 300 lines
   later. Additive: 0 original lines removed, 225 entry headings before and
   after. No verdict moves -- each was a decision to leave a default alone, for
   which "cannot distinguish from zero" suffices. The deferral itself was the
   failure journal 299 names: a correction recorded elsewhere does not reach
   the reader of the stale claim.

   SUPERSEDED: They say "no
   effect" where the evidence supports "not resolvable at 24 runs". No verdict
   changes -- each was a decision to leave a default alone, for which
   "cannot distinguish from zero" suffices -- but the record should not claim
   more than it measured. Journal 296 states the correction; the entries
   themselves are left as written, with this pointer.

   SUPERSEDED:
   j.291 seven-of-eight flips, j.294 compression 4/4 cells, j.295 per-seat
   signs 4/4. Each is P ~ 0.06 as a coin and each has intervals spanning zero.
   Either the bench has structure the per-seed interval does not capture --
   in which case the interval is the wrong error model and everything quoted
   with one is mis-stated -- or this is what looking at many small tables
   produces. Worth settling once: simulate the null properly for the 3-seat
   rating rather than assuming per-seed independence.

   SUPERSEDED:
   ~~pacification raise~~ **SETTLED: the direction is closed, default stays
   OFF.** OD_PACIFY_REFLEX=1 on the graded pair: N24 -86 CI [-124,-48] (1/8),
   N47 +17 CI [-39,+73] (5/8), pooled -54 CI [-86,-23]. Journal 40's -84
   REPLICATES on a second model -- I expected it to be the China coin and it
   was not. **The cross-model gate has now fired for real**: the two models
   disagree with non-overlapping intervals, the first valid disagreement the
   project has produced. REMAINING: the minority gates, and the reflex-layer
   audit. ~48 runs each on the N24/N47 pair, controls already measured.

22. ~~Does the pacification reflex pay when you are losing?~~ **SETTLED
   journal 294: NO -- the pattern is arithmetic, and 96 runs were saved.**
   corr(control, effect) = -0.81 against a MECHANICAL baseline of -0.86 (the
   control appears on both axes), so the observed pattern is WEAKER than
   chance; shuffling gives a median bucket spread of 332 against the observed
   233, P = 0.994. The conditional-rule candidate is dead.
   **Surviving, filed not believed:** within each seat across its own 8 seeds
   the reflex compresses the spread in 4 of 4 cells (0.35x to 0.84x) for about
   54 points of mean -- a direct comparison, no baseline-sorting. A REJECT by
   the verdict rule (rating down); only interesting if the floor mattered, and
   the floor is on modern:CHN, the coin.

23. ~~Compute the mechanical baseline before running the arms.~~ **SETTLED
   journal 299, and the audit found more than the paragraph asked for.**
   LOOP.md now carries a STANDING section with all five measurement facts
   (resolution floor, pairing buys nothing, graded counts, chance baselines,
   count-the-groupings) plus what the AI actually runs. It ALSO carried three
   stale claims: the retired "train ~1,000 turns" rule (journal 272 retired it;
   the retirement was written into BACKLOG line 479 and never into LOOP.md, so
   27 entries of agents read the retracted version first), the 5-point rush
   threshold, and "read the worst seat" where the worst seat is the China coin.
   All three now carry SUPERSEDED pointers. Additive only: 81 lines added, 0
   removed, verified by line-differencing.

30. **Who maintains LOOP.md?** **SECOND OCCURRENCE, journal 350.** The loop has
   now patched the protocol twice after tripping over it, and the section that
   was wrong this time is the one journal 299 added: its resolution table
   printed a one-sample interval as a detectable difference, so the floor read
   ~60 when it is ~90, for twelve entries, while od_bench printed the correct
   figure on screen beside it. Also stale: "three seeds" when the standard has
   been eight since journal 281. A document corrected only when someone trips
   over it will be wrong again by iteration 400.
   ORIGINAL: Journal 299 found the protocol 27 entries behind
   its own journal, because settling an item updates the JOURNAL and the
   BACKLOG and nothing routes a finding back into the document both are run
   from. Cheap fix: when an entry supersedes something LOOP.md states, say so
   in that entry's verdict and carry it across in the same iteration. Worth one
   line in LOOP.md section 7 (Journal it), which currently says to update the
   journal, the backlog and memory -- and not LOOP.md itself.

   SUPERSEDED: Journal 291
   spent 32 runs on a question needing 256; journal 294 avoided 96 by five
   lines of algebra. Any derived quantity -- a difference, a ratio, a
   correlation with a baseline on both axes -- has a value it takes under pure
   chance, and an effect must be read against THAT, not against zero. The
   bench now prints a power line for rate questions; there is no equivalent
   for "I sorted an effect by its own baseline", which is a shape this project
   keeps producing. Worth a paragraph in LOOP.md's measurement section.

   SUPERSEDED: Journal 293:
   sorting 32 graded seat-seeds by control score gives +106 (losing), +6,
   -127 (winning), r = -0.81. That is what an insurance rule should look like
   AND what regression to the mean always looks like when an effect is sorted
   by its own baseline. To test it, declare the split in advance on seeds
   chosen by something other than this data -- e.g. pick 8 new seeds, predict
   each seat's arm from the CONTROL run alone, then measure. If it holds, the
   reflex is a conditional rule (fire only when behind) rather than a rejected
   one, and that is a different and better candidate than the one just killed.

   SUPERSEDED (original text): Journal 279 found most of that map is firing counts (seed-proof)
   and journal 280 broke the one bench-based exception it identified. The
   remaining bench-based entries -- pacification raise (-84, single run,
   journal 40), the minority gates, the reflex-layer audit -- have not been
   checked for seed coverage. Rule knobs need no training, so each is ~27 runs.

11. ~~Is fortifyReflex actually load-bearing?~~ **CANDIDATE, journal 285:
   removing it is neutral on N24** (+26, CI spans zero) and raises the floor
   160 -> 256 while cutting sd 95 -> 64, helping on 5/8 seeds. Its "KEEP"
   verdict was -6.40 against an se 9-12 instrument. **NOT a recommendation:**
   this is the exact signature (mean not significant, case on floor+variance,
   one model) that journal 284 destroyed one iteration earlier.

12. ~~Validate the fortify ablation on N35~~ **SETTLED journal 286: INVERTED,
   candidate WITHDRAWN.** N35: removing fortify costs 25 mean, raises sd 47->84
   and collapses the floor 211->92 -- the reverse of N24. fortifyReflex STAYS,
   now on better evidence than its original -6.40. **Two for two: both
   floor-and-variance candidates this sequence cross-checked have inverted on a
   second lineage.** Treat that signature as a candidate, never a finding.

   SUPERSEDED: before any removal, same 8 seeds.
   If the floor gain holds on a second lineage, removing a shipped always-on
   rule is a simplification worth putting to the user.

13. ~~Why do seeds 1618033 and 3141592 break everything?~~ **SETTLED journal
   287: THEY SHARE NOTHING -- it is one bistable seat.** modern:CHN has par 2.5
   against a 5x cap, so it scores exactly 500 or exactly 0 (22 of 24
   observations) and is a survival bit worth 167 of the instrument's 285-point
   observed range. The two seeds are simply where the BASELINE arm's China
   died, and journals 281/282/285 all compared against that SAME baseline
   column -- one coin read three times, recorded as three findings. Drop China
   and 1618033 goes from 2nd-worst to 2nd-BEST of 8. **It also explains both
   cross-model inversions**: on N35 China is dead in every arm, so a third of
   the rating is a constant zero (sd 47 vs N24's 95). Instrument fixed:
   OD_BENCH_SEATS + modern:CHN:rung in KNOWN_BISTABLE.

14. ~~Re-read the floor/variance entries on FRA+USA.~~ **SETTLED journal 288,
   and it corrects journal 287.** Results: the shipped change is BIGGER than
   published (+89 CI spanning zero, 6/8 -> +122 CI [+65,+179], 8/8) and
   journal 283's headline survives untouched (+130 -> +143, 8/8), so the arc's
   one significant result was never the coin. But the two cross-model
   "inversions" do NOT resolve -- the signs still disagree. The reason is that
   **N35 never measured anything**: counting seat-seeds that can move at all,
   N24 ran 14-16 of 24 and N35 only 9-11, and the DEAD SEATS DIFFER (China on
   N24, the USA pinned at CAP 8/8 on N35). Two models saturating different
   seats are scored by different instruments; a comparison between them is
   meaningless, not negative. Both candidates stay rejected on N24 alone.
   Instrument: od_bench now prints the graded-observation count.

15. ~~Re-check the cross-model gate's two verdicts properly.~~ **SETTLED
   journal 289: N47 IS A VALID SECOND MODEL.** It grades 18/24 against N24's
   14/24, and -- the part that matters -- it loses its observations in the SAME
   seat (China), grading FRA 8/8 and USA 7/8 where N24 grades 8/8 and 6/8. N35
   graded USA 0/8 and kept China: the opposite pattern, which is why journals
   284/286 compared two different instruments. A zero-cost screen of 213 stored
   records first showed 59% have a saturated seat on the MEAN alone (CHN 94,
   USA 43, FRA 20) -- saturation is this instrument's normal state. Also
   recorded: od_bench.py defaults to TURNS=120 and the whole 281-288 arc is
   400; set OD_BENCH_TURNS.

17. ~~Re-run the two withdrawn candidates on N47.~~ **SETTLED journal 290: THE
   GATE PASSES; both candidates dead on two valid models.** Graded pair FRA+USA
   -- bar 0.65: N24 +21, N47 -3, pooled +11.5 CI [-28,+51]. No fortify: N24
   +38, N47 +31, pooled +35.9 CI [-12,+83]. Neither clears zero anywhere. Arm
   graded counts 18/18/19 of 24, so the within-N47 comparison is sound.
   **The artefact was demonstrated directly on these runs**: scored with China
   in, fortify reads +26 on N24 and -11 on N47 (an "inversion"); scored on the
   graded pair it reads +38 and +31 (agreement). Same models, same seeds, same
   48 runs.

18. ~~fortifyReflex on the FULL seat set.~~ **SETTLED journal 291: NOT
   CLEARED, fortifyReflex STAYS.** hood clears (-4, CI [-12,+4], inside the
   5-point tolerance, and it is NOT a coin -- 0.3-0.6 against par 1.3, every
   observation graded). The rush seat cannot answer: 7 of 8 worlds FLIP when
   the reflex is toggled, hold rate 3/8 -> 4/8, and 8 seeds can only resolve a
   rate difference of 0.49 against an observed 0.125. Needs ~128 seeds per arm
   (~4 h) to settle, to delete a rule that measurably does nothing. Bad trade.

19. **FOR THE USER -- LOOP.md's rush guard is not well-formed.** "Give up more
   than 5 points on 1914:FRA:rush" is meaningless on a seat whose two regimes
   are ~197 and ~3 in score space: every reading is 0 or a 190-point violation,
   decided by a coin. Restate it as a collapse RATE with a sample size, or drop
   it from the routine guard and keep it as a release gate. The hood half works
   as written. **Until settled, no candidate can be "cleared past the rush
   guard" -- the honest phrasing is "rush unresolved".**

20. ~~Pairing does not work on a knife-edge seat.~~ **SETTLED journal 292:
   advice corrected, power line added.** report() no longer claims paired arms
   are reliable on a bistable seat; it states the measured opposite and prints
   what the seed count in hand resolves (8 -> 0.49, 32 -> 0.24, 128 -> 0.12).
   Validated by discrimination: fires on journal 291's real data, silent with
   no bistable seat, verdict tracks n. Long spreads now summarise.

21. ~~The bench now shouts.~~ **SETTLED journal 298: 16 warning lines -> 5.**
   One terse line per condition, full text behind OD_BENCH_VERBOSE=1, reasoning
   kept in the source comments. Validated by discrimination: fires on journals
   282 and 291's real arms with every figure that mattered, silent on a clean
   arm in both modes, and every terse number verified identical to its verbose
   counterpart. NOT solved: the bistable block still fires on nearly every
   full-seat run because 2 of the 6 standing seats are permanently
   KNOWN_BISTABLE -- a warning that never varies still stops being read, and
   the real fix is the seat set (item 16, the user's).

   SUPERSEDED: A routine 6-seat run is 32 lines of which 16 are
   [BENCH] warnings -- two of six standing seats are KNOWN_BISTABLE so that
   block always fires, and the graded line fires on 59% of records. Self-
   inflicted across journals 287/288/292, each justified individually. Proposal:
   one terse line per condition by default (e.g. "[BENCH] CHN pinned+bistable;
   14/24 graded; 8 seeds resolve 0.49") with the full explanations behind
   OD_BENCH_VERBOSE=1, keeping the reasoning where a reader can reach it. A
   warning nobody reads is worth less than the six iterations it cost to learn.

   SUPERSEDED: od_bench advises paired
   within-seed arms for bistable seats. Journal 291: both arms drew the SAME 8
   worlds and disagreed about 7 of them (independent coins predict ~4
   agreements; P(<=1) = 0.035). Fixing the seed fixes the map, not the outcome,
   because the intervention re-rolls the trajectory. The advice in report()
   should be narrowed, and any bistable seat should be power-checked BEFORE
   runs are spent -- journal 291 spent 32 runs on a question needing 256.

   SUPERSEDED: Pooled
   over two valid models, removing it is +36 with the interval spanning zero --
   no cost, no gain, on the three RUNG seats. That is the simplification item
   11 contemplated, but it is NOT recommendable yet: `1914:FRA:rush` and
   `1939:NOR:hood` were never measured, and LOOP.md's rush guard exists to
   refuse exactly this trade. Needs all six seats before it goes to the user.
   Note `1914:FRA:rush` is KNOWN_BISTABLE -- read its collapse rate, not a mean.

   SUPERSEDED: THE cross-model check that
   journals 284 and 286 claimed to do. Research bar 0.65 and OD_ABLATE=fortify,
   on FRA+USA, 8 fresh seeds, N47. Both are expected to stay dead -- they fail
   on N24's own graded seats (+21 CI [-31,+73] and +38 CI [-18,+94]) -- so this
   tests THE GATE more than the candidates: if N47 agrees, the gate works and
   has simply never been used on a comparable pair. ~48 runs, ~50 min.

   SUPERSEDED: Journals 284 and
   286 rejected on "it inverts on N35"; journal 288 showed N35 could not
   measure either question. The verdicts happen to stand on the N24 evidence,
   but the GATE has never actually fired on a valid comparison. Before trusting
   it again, pick a second lineage whose graded count is comparable to N24's --
   check the new [BENCH] line on candidates BEFORE benching an arm with them.
   N37 and the loop-base-trained models are untested for this.

16. **Are FRA and USA safe on N24?** Journal 288 counted 6-7 of 8 graded for
   USA on N24 and 8/8 for FRA, which is why N24's numbers behaved. But USA is
   2 seeds from the cap and the models keep getting stronger. When a model
   pins USA the way N35 does, the 3-seat rating becomes one seat. Either raise
   CAP for this seat set, or add a seat with room above it. Decide before the
   next ladder stretch, not after a measurement goes strange.

   SUPERSEDED: Free -- the per-seat
   shares are in the run logs, no new runs. Journal 287 showed the shipped
   change goes from +89 (CI spanning zero, 6/8) to +122 (CI [+65,+179], 8/8)
   once the coin is removed, so at least one published conclusion is
   UNDERSTATED rather than overstated. Do this before item 10.

   SUPERSEDED: Journals 281, 282
   and 285 all find their worst behaviour on these same two worlds: bar 0.45
   collapses there, the shipped change creates the collapse there, and
   fortifyReflex is most harmful there. Three unrelated rules, two worlds.
   Identifying what those worlds share would explain a failure mode rather than
   a number -- more valuable than any single rule verdict. Start with the seat
   traces, not another A/B. It is a shipped, ungated,
   always-on rule, and its "KEEP" verdict rests on -6.40 to remove it (two
   hold-out sets, journal 191) -- BELOW the se 9-12 resolution limit measured
   later in journal 244. Its original +4.7 came from the discredited
   instrument. If ablating it costs nothing on 8 fresh seeds, a shipped rule is
   doing nothing and can go. Use OD_ABLATE=fortify. One model first (N24),
   second lineage only if the first shows something -- the pattern journal 284
   established.

## NEXT (as of 2026-09-05 01:00) — superseded above, kept for the record
1. When Phase 5 completes: build v10 (Phase 5 rules + release-as-bankruptcy-rung); gate shipped / champion / N11 / N24 / N28; relabel N29/N30 to the ruler they benched on.
2. Stuck boats: instrument the stuck erase (leg length, route size, first-step land?) in Game_TurnLogic.cpp; fix; check "arrived / stuck"; v10.1 if it moves landings.
3. Ladder: two seeds per step from the v10 model of record (N24 lineage unless the gate says otherwise), two trainers max, no gate concurrently.
4. Goods-on benchability: sweep GOOD_OUTPUT_SCALE and EXTRACT_SCALE first, then Phase 4 rates, per seat; then "can the politics head learn the compass" ticket with OD_GOODS=1.
5. Migration window (this loop schedules): the strategic release action → MAX_MODULE_ACTIONS 12→16 in one migration, GUARD_HEADS / Experience::visits / DYN_ACTION_ONEHOT following, m_dynamics reset, retrain budgeted.
6. Human baseline: three or four of the user's games through --bench-agent on the v10 ruler, per the "beats humans" goal.
- STUCK BOATS, fix written (journal 38a): the grid-entry leg may be up to eight cells (navCellNear's radius) for a hull in off-grid water; the router now jumps that leg. v10.1 check queued behind the v10 gate; gate v10.1 only if "arrived / stuck" moves. If it does not: next suspect is the coast-stop stepping on legs that clip land corners between adjacent cells.
- STUCK BOATS, v10.2 (journal 38d): entry leg sailed by budget → arrived/stuck 29/71 → 43/48, sailing boat-turns 407 → 867, landings 122 → 131 absolute (share 64% → 54% as more boats load). The remaining 48 stuck are on some later leg; instrument that leg (index in route, length, land at first step) when next in the router. v10.2 gate running.
- DONE (journal 38g): stuck loaded-boat orders 71 → 3 per 80 turns. Three router changes: grid-entry leg sailed by budget (v10.2), planned inter-cell legs sailed by budget with the coast stop only on the final approach (v10.3).
- STANDING DECISION: the ruler HOLDS at v10.3 for the next ladder stretch. No rule or resolver change until the ladder has been judged on one ruler (two tickets at a time, gate each pair). Rule changes queue in this file.
- LADDER STEP SIZE (journal 38j): 12 steps from the two best parents, 1 above parent. Paired test on the held ruler: N34 (standard) vs N35 (OD_LR_SCALE=0.25), same seed 20261002, same parent N24. Adopt the lower rate for all tickets if N35 ≥ N34 clearly; otherwise try shorter steps (turnsPerMap 600).
- DECIDED (journal 38l): ladder tickets run at OD_LR_SCALE=0.25. Paired N34 (1.0) 150 vs N35 (0.25) 186, same seed and parent. Open: can a quarter-rate step CLIMB (N36, N37 from N35)? If steps only hold, try 0.5 and shorter maps.
- MODEL OF RECORD: N37 (224 on v10.3; quarter-rate ladder step 2). Next steps from N37 at 0.25: N38 (seed 20261005) running; N36 (from N35) pending. v11 (longitude wrap) builds after N36; N37 benched on v11 right after the gate; N38 relabelled to the ruler it benches on.
- v12 QUEUED (journal 38q): ghost-claims clear (roadmap session) — build after N37-v11 / N39 / N41; gate six models (+N37, N35). Expect rebellions −58% and every claim-reading AI path to change meaning; hold the ruler at v12 for the next stretch.
