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

## NEXT (as of 2026-09-05 01:00) — in order
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
