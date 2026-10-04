# The AI improvement loop

One iteration of this document is one experiment. The loop runs it over and
over, forever, until the user says stop. Everything that makes the loop *safe*
and everything that makes it *compound* is written down here, because the agent
running iteration 40 will not remember iteration 3.

Read this file, then `docs/ai/LOOP_JOURNAL.md` (the tail), then
`docs/ai/BACKLOG.md`. Run **one** experiment. Append to the journal. Stop.

---

## 0. Hard rules. These are not negotiable and they are not judgment calls.

1. **NEVER commit. NEVER `git add`. NEVER `git stash`, `git checkout --`,
   `git restore` or `git reset` on a path you did not write in this iteration.**
   Another person edits this tree while the loop runs. A `git add -A` here has
   already swept unrelated work into a commit once.
   When something is worth committing, write the proposed message into the
   journal under `PENDING COMMIT` and leave it there. The user commits.
2. **Never overwrite `data/ai/model.bin`.** It is the only copy of tens of
   millions of updates. The frozen reference is `data/ai/model.loop-base.bin`;
   a training run writes to its own file and is compared against that.
3. **Every game process goes through the gate**: prefix with
   `python3 tools/odlock.py --`. `tools/od_bench.py` already gates itself.
   The machine has **16 GB**, the gate allows **2** concurrent games
   (`OD_MAX_GAMES`), and a headless seat peaks near 0.6 GB — so a bench and a
   training worker fit together with room to spare. Do not raise the gate, and
   do not run two benches at once: the windowed binary still spikes to ~2 GB,
   and four of those is what put this machine into swap before.
4. **Revert what you wrote when the verdict is REJECT.** List the paths you
   touched in the journal entry *before* you build, so the revert is mechanical
   and cannot reach anything else.
5. **Ask before anything outward-facing or hard to undo** — pushing, deleting
   model files, changing the seat set, rewriting `AGENTS.md`.

## STANDING: the diplomacy entropy guard stays ON (user, 2026-09-04)

The adaptive collapse guard (`m_entropyCoef`) covers the diplomacy head as well
as the four modules, and it is UNCONDITIONAL — no flag, no env var. Do not put
it behind one, and do not narrow the controller loop back to `m < MOD_COUNT`.

Why it is not optional. Continuations of the same model, same recipe:

    at run A's seed:  guard off 109 / surv 74 / worst 38
                      guard ON  118 / surv 88 / worst 72

(An uncontrolled arm at another seed read 239 and was WEATHER -- journal 21.
The guard is worth +9 rating, +14 survival and +34 worst seat, not +130.)

Every continuation WITHOUT it destroyed the model; the one with it produced the
highest rating measured. A saturated policy — and this head measured H = 0.000
on every request kind — cannot be improved by a reward change, only moved
between corners, which is exactly how journals 05-10 behaved.

Three parts must all stay:
  1. the controller loop runs `m <= MOD_COUNT`;
  2. the diplo PPO update passes `m_entropyCoef[MOD_COUNT]`;
  3. the marginals are recorded in the ANSWER space (ceiling ln2, not ln14).

## STANDING: train ~1,000 turns, not 4,000 (journal 26)

> **SUPERSEDED by journal 272 — do not follow this as a general rule.** The
> length curve is PARENT-SPECIFIC and non-monotone. From the declared
> reference it reads 14 / 159 / 100 / 159 at 0 / 8 / 16 / 24 maps: longer is
> not better, 24 maps equals 8 for three times the compute, and there is no
> peak to tune to. Recipe of record is now **train 8 maps and measure**, and
> re-measure if the length changes. The curve below is one seed from one
> parent and does not transfer. Kept for the record.

A length curve from a GOOD model, one seed, everything else equal:

    turns    0     1008    1839    3948
    RATING  162     184     147     109

Training peaks near 1,000 turns and declines steadily after. The inherited
`6 3000` recipe overshoots it about fourfold, so a change measured that way is
read through more degradation than signal.

Use ~1-2 maps when COMPARING changes on an already-good model. Longer runs are
for producing a new lineage, not for judging a knob.

## STANDING: the seat score was inflated before journal 29b (2026-09-04)

The `[BENCH]` score used to report the MODEL COHORT's share of the world, which
credits the seat with every province held by a country that did not exist when
the cohorts were built at map start. It now reports the seat's OWN share.

  * Every rating stored before journal 29b is ~1 province per seat too high,
    about 5 points on the mean. They are comparable with each other; every
    A/B verdict in journals 01-28 stands.
  * Corrected absolute figures under the fixed score:
        baseline        101 / survival 65 / worst 4
        standing best   157 / survival 88 / worst 38
  * Do NOT compare a pre-29b number with a post-29b one directly. Re-bench.

## STANDING: what this bench can and cannot resolve (journals 287-298)

Five measurement facts, each settled by an entry that spent runs learning it.
They are here because they change what is worth running, not just how to read
it.

**1. The instrument only sees effects above ~90 points, not ~60.** Journal 296
measured the per-seed se correctly at ~30 on EIGHT seeds; the conversion to a
detectable difference dropped the root-two that an UNPAIRED comparison needs,
so the table printed a one-sample interval. Corrected journal 348, and the
measured value on two 8-seed arms agrees: pooled per-seed rating sd 94.0, se
33, MDD 92.

    seeds   runs/arm   se   detectable at 95%   (was printed as)
       8        24     30          83                  59
      16        48     21          58                  42
      32        96     15          42                  29

    MDD = 1.96 x se x sqrt(2).  od_bench prints each arm's own se and its
    "unpaired diffs under ~N are noise" line, which has always been right --
    trust that line over this table if they disagree.

**And the printed rating is NOT the statistic that se describes (journal 351).**
`report()` computes the headline from the MEAN seat shares while the se comes
from PER-SEED ratings. `min(share/par, CAP)` is concave, so the printed number
is biased upward whenever a seat is bistable or capped -- measured at **+45
points** on the standard 3-seat control (381 printed, 336 per-seed). To compare
an arm with its own floor, use the per-seed figure. Backlog item 87.

**So possibly NOTHING has cleared the floor since journal 281.** The one
measurement recorded as clearing it was pacify at −86/−88, which is below 92,
and its arms were stored before journal 339 began keeping per-seed values, so
its own se cannot be recovered (backlog item 75). Do not quote "exactly one
measurement has cleared" — it may be zero. **Say "not resolvable at 24 runs",
not "no effect"**; those are different claims and this project has repeatedly
written the stronger one.

**1b. The floor belongs to the RATING, not to the runs (journal 348).**

> **And an MDD is a property of the ARMS, not of the seat (journal 351).** The
> figures below came from journals 338/349's arms; the pacify arm of journal
> 351 is far tighter and the same eight seeds resolve 4.34pp on France instead
> of 11.75. A change that stabilises the world is easier to detect than one
> that destabilises it. **Recompute the MDD from the arms you actually ran** --
> `--compare` prints each seat's permutation p, which needs no MDD at all.

The
rating averages three seats, so an effect on ONE seat is divided by three while
its noise falls only by root three. Measured on 64 observations, the per-seat
land-share permutation test `--compare` prints resolves, at the SAME eight
seeds:

    1914:FRA  11.75pp   vs the rating needing 18.51pp on that seat   1.58x
    1939:USA   8.07pp   vs                     15.48pp               1.92x
    modern:CHN 9.65pp   vs NEVER — both arms sit above the 5x cap     inf

**Concentrated effect → test the seat. Diffuse effect → the rating.** On the
evidence to date that is not a refinement, it is the whole difference between
measuring something and not: at eight seeds this sequence is **0 for ~15 on the
rating** (journal 351 -- even the pacify reflex, the largest effect it has
produced, misses the floor by two points) and the same 24 runs resolved one of
three seats at p = 0.010. Any
effect on a capped seat is always a per-seat question, and more seeds buy
nothing against a constant. This does NOT rescue a RATE question (a collapse
or annihilation rate): those are proportions, the binomial floor is ~0.49 at
eight seeds, and journal 349 walked into exactly that.

**2. Seed-pairing buys nothing.** Journal 296 measured variance saved at −12%
to +11% across four knobs: zero. Seeds have NO persistent character (mean
cross-configuration correlation −0.09), because toggling a knob re-rolls which
worlds go well. The se stays honest, but matched arms are not worth arranging
and precision comes only from MORE SEEDS.

**3. Read the graded-observation count before any comparison.** `od_bench.py`
prints `N/M observations GRADED`. An observation pinned at 0 or at the cap is a
constant. Two arms — or two models — that saturate DIFFERENT seats are scored
by different instruments, and a comparison between them is meaningless rather
than negative. Journals 284 and 286 rejected two candidates for "inverting on a
second lineage" when the second lineage graded 10/24 and could not measure
either question. Journal 295 caught two arms of the SAME model differing
18/24 vs 12/24.

**4. A derived quantity has a value under pure chance — read the effect against
THAT, not against zero.** Journal 294: sorting an effect by its own baseline
gives `corr(X, Y−X)` ≈ −0.86 for free at the observed variances; the observed
−0.81 was WEAKER than chance and the "insurance rule" it looked like did not
exist. Five lines of algebra replaced 96 planned runs. When the baseline is
awkward, shuffle one arm against the other a few thousand times. Related: "N of
M seeds helped" has a null of M/2, not 0.

**5. Count the groupings you looked at.** Three entries in a row reported a
4-of-4 sign pattern at "P = 0.06". With ~6 candidate groupings per entry,
P(some 4-of-4) = 0.55 — the expected yield of looking. Do not attach a P to a
pattern you found by looking; report the observation without one, or
pre-register the grouping on the next dataset.

**And two things the AI actually is.** Of the eleven ablatable reflexes only
SEVEN are on by default (garrison, fortify, redeploy, austerity, manpower,
siege, campaign); peace, pacification, withdraw and callToArms each self-gate
off, so `OD_ABLATE` on them is a silent no-op (journal 297). Two section
headers in AISystem.cpp say "on by default" and are wrong. And `od_bench.py`
defaults to **TURNS = 120** — set `OD_BENCH_TURNS=400`, which every measurement
in journals 281-298 used.

## 1. Orient (cheap, always)

```bash
git status --porcelain            # record it; anything already dirty is not yours
tail -80 docs/ai/LOOP_JOURNAL.md
cat data/ai/bench_score.txt       # RATING SEATS SEEDS EPOCH SEAT_SET_SIZE
```

If `data/ai/model.loop-base.bin` is missing, stop and say so — every number in
the journal is relative to it.

## 2. Pick exactly one hypothesis

Top unblocked item in `docs/ai/BACKLOG.md`. One. An iteration that changes two
things measures neither, and this bench costs twenty minutes.

Write the hypothesis into the journal **before implementing it**, in the form
"if X then seat Y moves by Z" — a prediction you can be wrong about. A
hypothesis that cannot fail is not one, and this project has spent whole days
on changes that could not have moved the number they were measured on.

## 3. Implement, minimally

Smallest change that tests the hypothesis. Not the polished version — the
version that answers the question. Polish is a later iteration and only after
the answer is yes.

## 4. Build

```bash
cmake --build build/ --target OpenDoctrinesServer -j8   # what the bench runs
cmake --build build/ --target OpenDoctrines       -j8   # only if you need the UI
```

`build/` is the **Release** tree and the one the bench uses. **Build the server
target** — it is what every measurement runs; the windowed binary is only for
looking at something with your own eyes.
`cmake-build-debug/` is CLion's and is ~3x slower; never bench from it, and
always pass `--binary` explicitly so nothing picks the wrong tree by mtime.

## 5. Measure

The instrument depends on what changed. Get this wrong and the iteration is
worthless in a way that is not visible until much later.

### The rating — `tools/od_bench.py`. The default. ~3 min.

```bash
python3 tools/od_bench.py \
  --model data/ai/model.loop-base.bin \
  --label loop-NN-shortname
```

**It runs headless.** `build/OpenDoctrinesServer` is the same
`Game::runAIEvaluation` with no renderer linked, so a rating opens no windows,
survives the display going to sleep, and peaks at ~0.6 GB per seat instead of
~2 GB. Build it alongside the game:

```bash
cmake --build build/ --target OpenDoctrinesServer -j8
```

If you only build `OpenDoctrines`, the bench measures a **stale** server. It
prints a `!!` warning when the server is older than the game binary — read it.

Six seats, absolute. 100 = held every seat. Compare with
`--compare loop-base loop-NN-shortname`, which also prints *seats won* and,
since journal 339, a per-seat LAND SHARE table with medians and an unpaired
permutation test.

**EIGHT seeds, not three.** The file's default seed list is three and every
measurement since journal 281 has used eight, set through `OD_BENCH_SEEDS`:
13579, 246810, 555555, 987654, 3141592, 271828, 1618033, 8080808. Three seeds
on a bistable seat is three coin flips (journal 292). Set `OD_BENCH_TURNS=400`
in the same breath — the file's default is 120.

**Read seats won, not only the rating.** A change that lifts the rating by
winning two great-power seats while losing `1914:FRA:rush` (and, as it was then
read, `1939:NOR:hood`) is the exact trade this project has already made once
and rolled back: it buys skill at running a large country and pays in survival,
and survival is the case the AI actually loses.

**Rush guard.** REJECT any change that gives up more than 5 points on
`1914:FRA:rush`, whatever it does to the mean.

> **`1939:NOR:hood` WAS RETIRED FROM THIS RULE by the user, 2026-09-30**
> (backlog item 109, option b; journal 439). It ranked nobody and it could not:
> nine 32-seed arms ever recorded for that seat span means 0.388 to 0.422 — a
> spread of 0.034 land share across two models, a rule on and off, a training
> candidate, and the world change that moved `1914:SWE` by 6.75. Journal 411
> says why: Norway falls 17 provinces to ~4 inside **40 turns**, then moves by
> at most one province over the remaining 360, spends 0.01 on army all game and
> is under $8 for 98.8% of turns. The rump is unattacked rather than defended,
> so nothing an AI change does after turn 40 can reach it. A guard that cannot
> fail is not a guard — it is a line that makes every verdict look checked.
> The seat is still MEASURED; it just does not reject anything.

> **The rush half of this is not well-formed (journal 291).** `1914:FRA:rush`
> has two regimes and nothing between: ~197 and ~3 in score space, a step of
> 190 points. A 5-point threshold on a seat that only ever returns one of two
> values 190 apart is not a threshold — every reading is 0 or a catastrophic
> violation, decided by which side of the knife-edge that world fell. It is a
> COLLAPSE RATE question and needs ~128 seeds per arm to resolve a difference
> of 0.12. ~~**The hood half works as written** (graded, narrow, a −4 means
> something).~~ **WRONG, and retired 2026-09-30 — see the note above.** It is
> graded and narrow because it is very nearly a CONSTANT, which is the opposite
> of the property claimed for it here: a −4 on that seat has never been observed
> under any condition. Until the rush half is restated, no candidate can be
> described as "cleared past the rush guard" — the honest phrasing is "rush
> unresolved". Backlog item 19 is the decision.

> **Caution on WORST SEAT (journal 287).** On the three rung seats the worst
> seat is usually `modern:CHN`, whose par is 2.5 against a 5x cap — it scores
> exactly 500 or exactly 0 on 22 of 24 observations. A "floor" that moves is
> normally that coin landing differently, not the AI holding better. Read the
> raw per-seat share and the graded count before believing a floor.

**Read SURVIVAL and WORST SEAT, printed under the rating.** The rating is a mean
of ratios capped at 500, so a seat that runs away can BUY it while a seat that
dies can only spend 100. Measured on two checkpoints of one run: the later rated
177 to the earlier's 131 while its worst seat fell 26 -> 7, because three seats
reached 2.6-3.3x par and drowned an annihilated Sweden. Survival caps each seat
at its own par, so growth above par earns nothing and only holding counts.

A change that lifts the rating while survival falls has bought runaway growth
and paid in robustness — the exact trade a training run was rolled back for in
August, on the grounds that holding a great power is the case the AI already
wins. Treat it as a REJECT unless the growth is the thing being tested.

> **But survival is NOT an independent check on the three RUNG seats (journal
> 336).** France and the USA sit above par on every seed there, so `min(seat,
> 100)` returns the cap and they contribute constants: survival is
> `(100 + 100 + China) / 3`, which is China's annihilation rate rescaled.
> Eight arms were recomputed and not one survival interval cleared zero.
> `od_bench` now prints its own warning — `survival varies over only N of M
> seats` — read that line before quoting the number. Survival means something
> on the small-par seats (`1914:SWE` par 1.0, `1939:NOR:hood` par 1.3), which
> are the seats excluded for being bistable; that tension is backlog item 70
> and is not resolved. **And only `1914:SWE` of those two carries information**
> — hood is a constant (retired from the reject rule 2026-09-30), so survival
> computed over a set including it is diluted by a seat that never varies.

**Re-baseline** — re-run the bench on `model.loop-base.bin` under the *new*
binary, and compare against that instead — whenever the diff touches anything
outside the learned-AI decision path (game rules, combat, economy resolution,
the scripted rung). A rule change reaches the scripted world too, and comparing
across binaries then measures the rule twice. Re-baseline every 5 iterations
regardless.

### Model vs model — `tools/ai_bench.py --model A --vs-model B`

Only for comparing two *weight files* (after training). **Both directions,
always**, and compare MAPS WON, not `ADVANTAGE`. `--vs-model` has a large
home-field bias: the same pair read 1.14x one way and 1.91x the other, and the
1.14x belonged to the model that lost 14 of 16 map-instances.

### Never A/B on `ADVANTAGE`

It is an unbounded land ratio and it explodes for a strong model — the same
weights read 2.347, 5.943 and 7.680 against the same rung. Read `land held`
(bounded, 0–1) for a config sweep, and both horizons (300 **and** 400 turns):
a model that wins early and collapses late is a different animal.

### Anything both cohorts share cancels out

Engine rules, executors, naval code — the scripted rung gets the improvement
too, so a *relative* metric reads zero. Use absolute per-mechanism counters
(sinkings, industry built, ghost wars, beached hulls) for those.

### Diplomatic rates: quote them against `net asked`, never `asked`

Gates refuse before the head is consulted — the pact cap, the four call-to-arms
gates, the trade floor. The `[EVAL]` per-kind block prints `net asked N/M`: M is
how often the country was asked, N how often the POLICY was. A rate over M is a
statement about the gates and the policy together; only `saidYes / net asked` is
about the head. Measured 2026-09-04: calls to arms read as a 12-50% acceptance
rate over `asked` and were one or two head decisions over `net asked`.

### A dead action: "0.0" hides fifty nats

The policy-shape table prints small probabilities in exponent form since
journal 13, and it must. `stage` and `artillery` both read 0.0 under the old
`%.1f`: one sat near 4% and moved on a +5 logit bias, the other near 1e-33 and
was bit-identical at +60. **Before targeting a dead action, read its exponent.**
Anything below ~1e-6 is out of reach of a bias and needs `--reset-ai-head` and
a retrain.

### A null result on ONE action says nothing about the mechanism

Journal 12 biased `hold` by +20, saw no change, and nearly concluded the bias
path was broken — which would have retracted a good result. `hold` is masked out
whenever a real option exists. **Control with an action already known to be
live** (`attack` responds to -20) before believing a mechanism is inert.

### One training process at a time

A worker scans `model.w*.bin` for peers every two minutes and pulls a THIRD of
the way toward each. A second run is not isolated by choosing a different worker
id — journal 06 lost a whole experiment to a "harmless" side probe. Use
`--worker 1 --workers 2`, whose only peer name is `w0`, and keep `w0` empty.

### Reflexes and mechanics the bench cannot see

`--vs-script` ADVANTAGE is blind to anything acting on the winner. Judge those
on concentration counters, not the rating.

## 6. Verdict

| | |
|---|---|
| **KEEP** | rating up, **survival not down**, and `1914:FRA:rush` not down more than 5 (hood retired 2026-09-30) |
| **REJECT** | otherwise — revert the paths listed in step 4, immediately |
| **PARK** | interesting but unmeasurable with the current instruments; write the missing instrument as a backlog item |

**A difference below the floor is PARK, not KEEP.** This is where correcting
the floor actually bites: at eight seeds per arm the rating resolves ~90 points
(STANDING 1), so a +43 is not a small win, it is no reading at all. Journals
338 and 349 both produced one and both are PARK. Check the arm's own
`unpaired diffs under ~N are noise` line before writing KEEP.

**And "survival not down" is nearly free on the three rung seats** — it is
China's annihilation rate rescaled there, so it will usually abstain rather
than agree (journal 336, and the caveat in step 5). It is a real clause on the
full six-seat set.

A REJECT is a *result*, not a failure. Most of the value in the journal is the
list of things that do not work, because every one of them looked reasonable.

## 7. Journal it

Append to `docs/ai/LOOP_JOURNAL.md` using the template at the top of that file.
Always — KEEP, REJECT and PARK alike. Then update `docs/ai/BACKLOG.md`: strike
the item, and add whatever the run suggested.

If the finding is durable and general (an instrument that lies, a category of
change that never works), also write it to memory under
`~/.claude/projects/-Users-vladyavdoshenko-CLionProjects-OpenDoctrines/memory/`.

## 8. Stop

One experiment per iteration. Do not start the next one. The loop will call
again.

---

## What is already known. Do not re-derive these.

### Added 2026-10-03 from journals 447-458, because none of it had reached this file

The check that found this: `grep -cE 'war slot|attack order|decided by the draw' LOOP.md` returned 0 after twelve
iterations. Memory nothing-routes-findings-back-to-the-protocol is about exactly that, and it nearly cost a thirteenth
iteration -- the next item up was a fourth attempt at a shape memory unrest-levers-are-not-savings records failing
three times.

- **THE WAR SYSTEM IS CLOSED AS A GROWTH CHANNEL.** All three routes measured and dead: more wars (lifting
  AI_MAX_CONCURRENT_WARS to 2 cost 101.9 rung points and CLEARED its floor, journal 447); shorter wars (the
  re-attack cooldown's duration effect was bimodal, two worlds of six, interval spanning zero, claim withdrawn,
  journals 450-451); more productive wars (doubling ATTACK_ORDERS_PER_TURN moved provinces-per-war-turn by -0.016 on
  an interval of +/-1.2, journal 456). Whatever holds 1914:FRA at 27.6% of the world is not in war count, war length
  or war productivity. Start outside them.
- **A LIMIT CAN BIND CONSTANTLY AND STILL BE WORTH NOTHING.** ATTACK_ORDERS_PER_TURN is hit on 21.6% of attacking
  country-turns with a censored distribution (orders-per-turn decline 32/18/12% then SPIKE to 26.3% at exactly the
  cap), so the demand is real -- and raising it changes nothing. This is a THIRD category beside limits nobody reaches
  (the manpower ceiling, item 130; the war bar, item 138, which misses by a factor of 17). Counting whether a limit
  binds is necessary and not sufficient.
- **BOTH SURVIVAL SEATS ARE DECIDED EARLY AND NEITHER IS REACHABLE.** 1939:NOR:hood is settled before turn 40
  (journal 411) and was retired from the reject rule by the user (journal 439). 1914:FRA:rush is decided by the
  opening draw: a collapsing and a holding seed are the same world to the decimal through turn 15 -- provinces 83->92
  in lockstep, treasury 1772.4 vs 1772.3 -- carry identical standing bills, and part at turn 20-25 purely in
  provinces taken (journal 458). No rule acting after turn 100 reaches either seat. **So the reject rule in section 5
  currently names one seat and that seat cannot move**; whether to restate it is backlog item 145, for the user.
- **A SINGLE-SEED READING HAS DISSOLVED FIVE TIMES OUT OF FIVE THIS SESSION** (journals 438, 444, 446, 450, 456),
  including one that met a pre-registered verdict rule at p 0.20 with a confirmed mechanism and then read 12/31
  against 13/31 at p 1.0000 on untouched seeds. Pre-registration and a confirmed mechanism make an effect worth
  CONFIRMING; they do not make it real. Keep a block of seeds nobody has run.
- **od_bench DOES NOT SURFACE THE SERVER'S STDERR.** Setting OD_WARLIFE, OD_ACT_HIST or any probe variable on a bench
  arm produces nothing in the log -- the arm logs are od_bench's own ~25 lines. Per-seed counters need their own runs.
  Journal 456 set a probe believing it had fixed exactly this and did not grep the log to check.
- **provinces-per-war-turn IS A WORSE STATISTIC THAN THE RATING**, against the intuition that a count over many wars
  beats a count over seeds: it ranges 0.088 to 3.524 across six worlds, a 40x spread, paired se 0.634. Between-world
  variance swamps the within-world count.
- **THE AI COMMITS 100% OF ITS INCOME FROM TURN ~25.** income == expenses to the decimal for the remaining 375 turns,
  in collapsing and holding seeds alike, with pacification at 30-32% of income and zero before turn 25 (journal 458).
  That is the mechanism behind ai-treasuries-run-at-zero. Do NOT respond by gating unrest spending: memory
  unrest-levers-are-not-savings records three such attempts, all losses.
- **A COMMENT IN src/ai MAY DOCUMENT A VALUE THE CODE NEVER RAN.** Commit 801fd20 ("Complete the revert") restored
  pre-branch VALUES and kept branch COMMENTS, stranding two: AI_CAMPAIGN_SHARE documenting 0.20 above a 0.35, and
  AI_MAX_CONCURRENT_WARS documenting "2 since ParrotZero 8.6.0 ... not one seat lost" above a 1. Both were
  re-measured on this build and BOTH FAILED, so the revert was right twice. `tools/check_ai_comments.py` now fails the
  suite on a numerate comment citing a version newer than AIVersion.h; prose claims are still unguarded (the AI does
  not call setProvinceOutput despite a comment saying it does -- item 143).

- **THE AI ALREADY AIMS ITS PACIFICATION BUDGET, AND THAT REFLEX IS WORTH +121 PER-SEED POINTS (journals 461-462).**
  `updateAIDistricts` (Game_Policies.cpp:4875, called from Game_TurnLogic.cpp:1052 for every AI country every 5
  turns, 8+ provinces) cuts a hot district and a calm one and risk-weights the shares, clamped 25/75.
  `OD_AI_DISTRICTS_OFF=1` costs **121 per-seed rating against a floor of 64** and takes 1914:FRA:rung from 34.12% of
  the world to 13.14% at p 0.003. **Do not propose giving the AI district aiming -- it has it. Do not re-open this
  from journal 461 alone**, which measured "binding province-turns" (provinces left above the suppression dial), found
  637x MORE of them with aiming ON, and reads as harm. It is not: a binding province in the CALM district carries
  about a tenth of a point of residual unrest, while the quantity the budget reflex reads is
  `worst = max over provinces of getProvinceRebellionChance`. **A COUNT weights a calm province's rounding error the
  same as a hotspot's real danger.** Third consecutive iteration where a statistic was true of an ADJACENT thing, so:
  before trusting a derived count, say in one sentence which decision reads it. The mechanism of the +121 is still
  unknown, and "aiming frees income" is DEAD on eight paired seeds (journal 464): `pAlloc`, the allocation the head
  chooses, is lower with aiming on in 4 of 8 -- exactly the null -- paired mean +0.0014, CI [-0.0336, +0.0364]. Same
  budget, same share of gross, 2.6x the land, so **the whole gain is in WHERE the money lands**, and every
  "the saving buys something" story is ruled out at once. Journal 463 claimed this mechanism from two seeds with a
  mechanism read off the source, and those two turned out to be the most favourable of the eight -- **a mechanism
  grounded in the code does not protect a two-seed effect.** Aiming also has a HIGHER rebellion rate per
  province-turn (463) while paying nothing extra for it. **FOUR unrest stories have now been tested against the +121
  and all four are dead** (462 exposure-causes-harm, 463 rebellion-count, 464 the budget -- an exact null, 465
  rebellion-location -- 7 of 8 in the right direction but a paired mean of -4.11 pp with the interval including zero).
  The measured facts are: budget identical, rebellion count within 2%, location shifted 4 points. **A 4-point shift
  cannot be a 2.6x difference in land, so the channel is NOT in the unrest system -- do not write a fifth unrest
  story.** The live lead is that `f[24]` (AISystem.cpp:2745) is a policy-net INPUT built from
  `getProvinceRebellionChance` over the first six provinces in hash order, which `pacificationFactor` scales -- so
  aiming changes what the net SEES, not what happens to it. **MEASURED AND DEAD TOO (journal 466): `f[24]` is higher
  with aiming on in 3 of 8 seeds -- worse than the null -- paired mean -0.00177 against a predicted 0.02, intervals
  straddling zero both world-wide and restricted to the countries districts can touch.** So FIVE channels are now
  closed. The one quantity that moves on the scale of the effect is the ARMY: journal 462 measured 57.58M against
  22.52M at turn 400, a 2.56x difference beside 2.6x in land, and `Game_TurnLogic.cpp:3610` multiplies the
  recruitment cap by `1 - getProvinceRebellionChance` -- the only unexamined consumer of unrest. Item 153, and the
  first thing it must do is COUNT whether the cap ever clips, because memory ai-recruitment-is-money-bound says the
  treasury binds and not the ceiling.
  Side finding, **and it was WRONG -- corrected journal 482, read this before quoting it.** I reported that `f[24]`
  "uses 1.3% of its [0,1] span" and is close to a dead input. Measured per SAMPLE rather than per run, over 263,780
  samples: mean 0.0053, **sd 0.0461, min 0.000, max 0.9933.** It is a SPARSE SPIKE feature, not a constant. The
  "1.3%" was the spread of per-run MEANS and I read it as the feature's range -- **averaging within a run and then
  reasoning about variation between runs says nothing about whether a feature moves.**
  What the same cross DID find (journal 482): **`f[10] = nlog(population, 5.0)` is pinned in [0.849, 0.9996]** --
  tenfold real variation compressed into the top 15% of the range, at the 61st percentile of weight, so the net
  cannot tell a 90M France from a 600M China (item 170). And `f[80..84]` are **never assigned at all** (item 168),
  which is what a genuinely dead input looks like: the five smallest weight columns of 143.
  `OD_FEATSTAT` prints per-feature mean/sd/cv/min/max; cross it against the model's own weight columns
  (`ModelPack unpack`, trunk at offset 10, row-major `[out][in]`, in = 143).
  **AND `nlog` IS THE WRONG FORM FOR POPULATION AND ARMY (journal 483, algebra).**
  `nlog(v, scale) = tanh(log1p(v)/scale)`. `log1p` of a population spans 13.8-21.1 over the whole realistic range --
  a factor of **1.52** -- so dividing by any `scale` leaves it 1.52-fold: **the constant is not the bug, the form
  is.** Measured: `f[10]`'s band above 1e6 people is **0.0075** wide and the 90M-vs-600M signal is 3.2% of the
  feature's own sd; `f[11]`'s band above 1e5 men is **0.006**, its apparent full range existing only because some
  countries have zero army. `f[0]` (treasury) works by luck -- small numbers, so its log varies 12.8-fold.
  A floor-subtracting form (`tanh((log1p(v) - 13) / 4)`) gives a 0.764 band. **Needs a retrain, so it rides a
  training run** (item 171).
  Methodological note from the same entry: I tested "is it saturated" as "is the measured MINIMUM above 0.9", and a
  single country with zero army set that minimum to 0 and hid the flattest feature of the three. **Test the working
  range, not the extremes.**
  **AND THE RECRUITMENT-CAP LEAD WAS DEAD CODE (journal 467).** `recruitCap` returns `pool/5` for any non-player
  country before the unrest line runs, `OD_AI_RECRUIT_CAP` is off by default, and `--eval-ai` sets
  `m_playerCountryId = 0` -- so unrest never touches an AI's recruitment ceiling in any benched world, and the army
  difference is downstream of land rather than upstream.
  **THE ASSUMPTION UNDER ALL SIX HUNTS WAS WRONG (journal 467, the bisection).** Forcing `pacificationFactor` to 1.0
  -- its only functional caller is Game_Policies.cpp:1390 -- while leaving the districts drawn costs **nothing**:
  463 (se 10) against 473 (se 16), +10 on a floor of 37. **The suppression aiming is NOT what the feature is worth**,
  whatever the districts code's comments and item 120's brief say. Whether districts-existing carries the +121 through
  some other channel is item 155 and needs the no-districts arm re-run on the same binary -- journal 462's 343 is on
  an older build and is NOT quotable against 467's numbers. Already excluded as causes if it does survive: RNG
  divergence (nothing on that path touches simRand), district laws/policies/cost (all zero for an AI district), and
  other readers of m_districts (mods, LLM, scripts -- none active in a bench).
  **RESOLVED journal 468, and the exclusion that was wrong is the interesting part.** The no-districts arm reproduces
  exactly on the current binary (343 se 31, -121 on a floor of 64, FRA p 0.003), so all three arms sit on one build:
  districts drawn + aiming live 463, districts drawn + aiming NEUTRALISED 473, no districts 343. **Districts-existing
  is worth ~121 and the aiming contributes none of it.** The channel is a SECOND reflex nobody in journals 461-467
  had looked at: `updateAIDistrictLaws` (Game_Policies.cpp:4213, called for every AI country from
  Game_TurnLogic.cpp:1057) returns early when a country has no districts, so removing districts removes the whole
  regional-law mechanic -- and a law carries `incomePct` and `growthPct` as well as `unrestPct`. **That is why six
  consecutive unrest statistics came back flat: the channel is most likely income and growth, not unrest.** Confirm
  with `OD_AI_DLAW_OFF=1` (item 156); `OD_DISTRICT_DEBUG` prints which law each country passes.
  **And the lesson that cost seven iterations: the districts code SAYS the aiming does nothing.**
  `updateAIDistrictLaws`' header reads "the budget split was the whole of its use for the mechanic, and since its
  pacification budget is zero the split moved nothing" -- forty lines below functions I read repeatedly. When a
  mechanic's effect cannot be found, **read the whole file, especially the function after the one that looks
  relevant**, before measuring a sixth consequence of it.
  **BUT THE LAWS ARE NOT THE CHANNEL EITHER (journal 469): -24 on a floor of 57.** Four arms on one binary --
  districts+aiming+laws 463, aiming off 473, laws off 439, no districts 343 -- so the single ablations sum to -14
  against the structural -121: **non-additive by ~107 points**, which is memory ablations-dont-compose measured again
  in its own right. Each mechanic is load-bearing only in the other's absence. **Do not price this feature by
  ablating one mechanic at a time.** The open arm is both gates off with districts on (item 157): near 343 means they
  are jointly necessary, near 450 means the gate or arm C is defective and the right response is an audit, not a
  ninth channel.
  **ANSWERED journal 470, and it retires this whole line: BOTH mechanics off still scores 433, with A/B/D, not with
  C's 343. And the per-seed spread shows the effect was never a mean shift -- it is a COLLAPSE RATE.** 1914:FRA land
  in the no-districts arm reads 0.1 / 0.2 / 1.4 / 4.5 / 21.6 / 24.3 / 25.5 / 27.5: four annihilations and four normal
  seeds, against **0 collapses in 24 runs** pooled over the three district-bearing arms (Fisher p = 0.00195; against
  one arm alone p = 0.0769, which is why eight seeds could not settle it).
  **That is why six consecutive mean-based unrest statistics came back flat** -- on the non-collapsing half there is
  nothing to see, and averaging four annihilations with four healthy seeds manufactures a large mean difference with
  no mechanism anywhere. **In this area read the SPREAD before the mean** (`od_bench` stores it; it was there the
  whole time), and for a collapse use a RATE with pooled arms, not a rating. The live question is now a defect audit
  -- why skipping `updateAIDistricts` annihilates France on half the seeds when both its mechanics are inert --
  starting with `OD_SEAT_TRACE` on a collapsing seed versus a surviving one (item 158).
  **TRACED, journal 471: the collapse is an ARMY THAT STOPS BEING REBUILT, and it is not revolt.** Collapsing pair,
  seed 20260801: provinces identical for FIFTY turns, part at turn 57, then the army goes 6.79M -> 2.52M by turn 100
  and never recovers (its 400-turn peak is 6.82M, at turn 10), with provinces following it down; the district-bearing
  arm holds ~6.8M and reaches 50.58M by turn 395. **Rebel wars are 0 in the collapsing arm until turn 334 and the
  SURVIVING arm has three times as many**, at one foreign war in both throughout -- so stop looking at unrest here
  (six statistics already closed, journals 462-470).
  **And income falls faster than ground**: at turn 70, 13% fewer provinces but 56% less income (401.4 vs 913.4),
  which `computeCountryIncome` iterating `provincesOf` cannot produce on its own. Item 159 asks who takes the ground
  and why income falls disproportionately.
  One methodological note from the control pair: it parts at turn SIX and neither arm collapses, so **divergence
  timing does not predict collapse** -- do not read an early fork as a failure.
  **AND THE INCOME DISPROPORTION IS EXPLAINED (journal 472), with a term worth knowing in its own right.**
  `provinceIndustryIncome` scales by `rate = 2.0 * max(0.35, min(1, capacity/level))`, so a province whose industry
  is overbuilt relative to its capacity earns **0.7 per level instead of 2.0** -- and `provinceIndustryCapacity` is
  computed from POPULATION. So a population fall silently cuts industry income per level by up to 65% with no level
  lost and no event in any log. At turn 70 of the collapsing seed: ground -13%, industry levels -26%, income per
  level -43%, pop income -79%. **Any "income collapsed" question in this game should check the fit term before
  looking for a multiplier.**
  The crux is still open and is E versus C: arm E has pacificationFactor at 1.0 and no district laws, so **its unrest
  is identical to C's**, yet E never collapses. So an unrest-driven emigration story cannot be the whole explanation.
  **AND POPULATION IS NOT IT EITHER (journal 473, 24 runs): France's population GROWS while France is annihilated**
  -- the four collapsing runs read 1.062, 1.386, 1.552, 2.008 on turn-100-over-turn-50 population against survivors'
  0.900 to 1.464. **Two corrections to the journal-472 story:** `cs.pop` is not the headcount (it fell 79% while the
  headcount rose 6%), and **`cs.gross` sums a STORED per-province field** (`cs.gross += ind->second.income`, written
  from the MAP at Game_Loading.cpp:2682 and from the formula only for newly BUILT industry), so the capacity/fit
  mechanism was attached to a function `gross` never calls -- arm A's gross/level is 3.693 against that formula's
  hard cap of 2.0. **The income disproportion is just province heterogeneity: the industrial core is worth several
  times the periphery.**
  **AND THE ARMY IS A SYMPTOM TOO (journal 474): ground falls first in 4 of 4 collapsing runs**, by 29 turns on one
  seed, so journal 471's "army is the leading indicator" is WITHDRAWN -- it rested on five-turn sampling of a single
  seed. Also measured there: **mean `income - expenses` is 0.00% of income in every arm-C run**, collapsed and
  surviving alike, because allocations are clamped to affordable income -- **there is no recruiting surplus in any
  run, so do not propose a rule that spends one.**
  And the obvious successor is dead on arrival: **early ground loss does not separate collapse from survival** --
  three of four SURVIVING runs start losing ground at turns 10, 20 and 47.
  **AND THEN THE WINDOW ITSELF TURNED OUT TO BE WRONG (journal 475), which is the most useful thing in the whole
  sequence.** Early recovery does not separate either (collapsed 0.50/3.00/3.42/5.58 against survivors
  0.83/4.42/5.40/47.00, fully interleaved) -- and **at turn 60 the two groups are indistinguishable: 119.2 provinces
  against 119.0.** The collapsing runs PEAK at 93/156/158/171 provinces at turns 58/151/111/282, the same size as the
  survivors, and only then are dismantled, crossing half their peak at turns 214/284/296/340.
  **Journals 462-474 all measured turns 50-120, so every channel they closed is closed for THAT window and untested
  for 110-340.** Before measuring anything about this collapse again, check that the window contains the decline:
  take each run's own peak turn as the start, because the peaks are up to 224 turns apart.
  **MEASURED IN THAT WINDOW (journal 476): it is a slow grind by ONE opponent, and the four collapses are NOT one
  phenomenon.** Mean simultaneous enemies over each run's own decline window: collapsing 0.99 / 1.07 / 2.24 / 7.49
  against survivors' 2.05 / 3.24 / 3.06 / 5.46 over the same turns -- **three of four collapsing runs face FEWER
  enemies than the survivors, who are attacked more and come through.** Loss rates are 0.33-0.59 provinces per turn
  over 133-185 turns. And seed 31337 is a rebellion cascade (6.73 rebel wars) rather than a grind, so **a rule aimed
  at "the collapse" will fit three cases or one, never four** -- separate them before proposing anything.
  A caution on the statistic, learned by getting it wrong: enemy count is **confounded with success**, because
  AI_MAX_CONCURRENT_WARS caps what a country STARTS, so a large winning power attracts more declarations and survives
  them. Journal 476 pre-registered it as a quantity that "cannot be downstream" and that reasoning was simply false.
  **AND WHO IT FIGHTS IS ABSURD (journal 477, `[WARWHO]` under OD_WARLIFE).** No common opponent across seeds --
  CHE+GER, GER+ARG, CHE+CHN+ITA -- and France, holding 86-156 provinces, spends **94 turns at war with SWITZERLAND**
  (8 provinces, losing 6 net, then re-opening the same war five turns later), 110 turns against CHE on another seed,
  and **113 turns against ARGENTINA while losing 47 provinces.** Its one war slot is held throughout: memory
  stalled-wars-lock-the-war-slot, which was filed as a GROWTH problem and here is costing absolute ground.
  **War length cannot be the discriminator** -- the surviving control holds a 134-turn war and GAINS in it (+13, +3,
  +39) -- so do not propose a rule gated on war length. Two limits of the instrument: the aggressor is NOT
  identifiable (`s_warOpen` is keyed on `{min cid, max cid}`, so print order is by id, not by who declared), and its
  ground figure is a NET change per side, never a transfer between the two.
  **AND THE GROUND GOES TO THE OPPONENT, WHO IS SWITZERLAND (journal 479, `OD_LOSTTO`).** 0.0% of the seat's lost
  provinces go to a non-belligerent in any of four runs -- there is no hidden transfer path -- and in the decline
  window ONE country takes 91%, 75% and 45% of all losses. By turn overlap and cid consistency across seeds,
  **cid 38 is Switzerland, taking 164 and 72 provinces off a France holding 86-156**, and cid 20 is Argentina,
  taking 183 across two consecutive 113- and 112-turn wars. The surviving control's largest receiver is a REBEL
  faction instead: the France that lives bleeds to revolts it absorbs, not to a conqueror.
  **So the question is now the combat resolver, not war selection**, and memory width-makes-numbers-irrelevant
  predicts the answer: `resolveAssault` caps both sides at `combatWidth(pid)`, so above the frontage France's size
  buys nothing, and od_bench already reports 23.8% of 1.96M assaults hitting that cap.
  **DERIVED, journal 480, and it ends this line for the loop.** `depthFactor = min(1.5, 1 + 0.20*log2(troops/width))`
  caps at **5.66x the combat width = 113,137 men**; above the frontage both sides engage exactly `width`, so the
  engaged terms are EQUAL whatever the armies; and a **level-5 fort multiplies the defender by 1.50, exactly
  cancelling the attacker's maximum depth advantage.** So **a defender with 113k men behind a level-5 fort cannot be
  beaten by an army of any size**, and France's 6.79M is sixty times past the last man that matters. That is a
  BALANCE fact, not an AI defect: the loop cannot fix the arithmetic the AI plays inside, and the knobs
  (DEPTH_MAX, DEPTH_PER_DOUBLING, COMBAT_WIDTH_PER_AREA, the fort multiplier) change the game for the human too.
  Item 167, for the user.
  Memory width-makes-numbers-irrelevant was right and understated: **the bar is only 113k men and the ceiling only
  1.5x.**
  Two seeds are worth naming: **606061 has now supplied two mechanisms that did not survive** (463's budget story,
  465's location effect, where it reads -16.1 against the other seven's -2.4). Treat a result that rests on it as
  unreplicated.
  Two further facts settled there, both of which a comment or a grep got wrong: `pac` is CONTINUOUS (AISystem.cpp:8598
  adds `min(0.25, worst/50 + 0.01)`, not 0.125 steps), and the pacification BILL is `income x share`, independent of
  the province count and of how much suppression lands -- so "97.3% wasted" is suppression POINTS, never money.
  Searching `src/ai/` for AI behaviour finds nothing and proves nothing; the per-country reflexes are `Game::` methods.

- **Training degrades the shipping model -- established on two seed sets
  (journals 357, 359).** N24 trained 8 maps on the settled recipe (journal 272)
  reads 336 -> 192 and 306 -> 184 per-seed rating, both clearing their floors,
  pooled −133 with CI [−197, −69]. The loss lands on the seats that can fall.
  Two traps, both nearly taken: a `--worker` run writes `ai/model.w1.bin` and
  leaves `ai/model.bin` as the untouched PARENT (Game_AITrain.cpp:191), so
  bench the worker file; and a training arm takes ~2.8 hours because maps end
  early on STAGNATION_TURNS -- do not price it as turns x 3000. It also holds
  on a second TRAINING seed (journal 360: seed 777001, -92, CI [-175, -8]);
  the two seeds' costs are indistinguishable. The consistent casualty is
  modern:CHN, down on every arm. **Length is not the lever**: 2 maps costs
  -113 (journal 363, CI [-203, -23]), indistinguishable from 8 maps, which
  retires journal 278's three-seed "two maps is free". **The first exception
  is a rusher league** (journals 366-368) -- **and it is NOT established.** On training
  seed 424242 the recipe plus `OD_LEAGUE_EXPLOIT` produced a model at parity
  with N24 on two bench seed sets, +143 / +165 over the matching self-play run.
  On training seed 777001 the same recipe gave -18 against its self-play run
  and -110 against N24, CI [-193, -26]. **Bench-seed replication establishes one
  MODEL; a claim about a training RECIPE needs several training seeds.** That league was 100% rusher -- the cap
  does not bind (backlog item 97) -- so the claim is about that league only. A binary for a long run must be PINNED
  (a copy beside a `data` symlink -- the server resolves `<exe dir>/../data/`),
  because this tree has a concurrent editor who rebuilds.
### Added 2026-10-04 from journals 476-496. Eight of these ten were missing when audited; two were already in.

- **`OD_*` GATES ARE PRESENCE TESTS: `OD_X=0` SWITCHES X ON.** `OD_ENV` returns the raw `getenv` pointer, and many
  gates are written `if (!OD_ENV("OD_SOMETHING_OFF"))`, so setting the variable to `0` to mean "leave it alone"
  disables the feature exactly as `1` would. Gates read through `atoi(OD_ENV(...))` do NOT behave this way, and both
  styles sit in the same files. **A control arm UNSETS the variable (`env -u NAME`); it never sets it to 0** --
  journal 463 measured off-against-off for a full set of runs and only noticed because both arms returned
  bit-identical numbers. **Treat two arms with identical output as a harness bug until proven otherwise.**
  Also: `env` takes its options BEFORE assignments, so `env VAR=1 -u OTHER cmd` runs `-u` as the command (exit 127,
  journal 467); and zsh does not word-split, so a `case`-built `$FLAGS` is one argument (journals 467, and
  zsh-does-not-word-split).

- **A SINGLE-SEED FEATURE READING HAS NOW FAILED TO REPLICATE TWICE OUT OF TWICE.** journal 501 measured
  `OD_FEAT_RESCALE` at 23.8 -> 31.3 on one seed and journal 502 nulled it over eight; journal 506 measured
  `OD_FEAT_EFFARMY` at 23.8 -> 36.6 on the same seed and journal 507 nulled it (France -3.95, better on 3 of 8).
  **The temptation came both times from the number being large** -- +7.5 and +12.8 on a seat whose spread is
  23.8 to 50.1. Treat a one-seed feature result as a reason to bench, never as a direction.
  And the reason they null is worth keeping: **a frozen net's weights encode the OLD feature distribution**, so
  making a heavily-weighted input TRUTHFUL is a distribution shift that generically does not help without a
  retrain -- which is where items 171, 180, 196 and the effective-army gate all ended, coherently rather than as
  four separate disappointments.

- **CHOOSE A SQUASH SCALE AT THE VALUES THE FEATURE MUST DISTINGUISH, NOT AT THE RANGE'S ENDS.** journal 504 built a
  war-age feature as `tanh(age/50)`, which reads 0.954 at 94 turns and 1.000 at 304 -- **a band of 0.046 across
  exactly the 94-to-304-turn grind range the feature existed to expose** (journals 477, 489). `/150` spans
  0.556-0.966 there. This is the same defect journal 483 diagnosed in `nlog`, rebuilt by hand one iteration after the
  known-list entry warning about it was written. **A band that looks wide overall can be flat where it matters:
  tabulate the feature at the real values before picking the constant.**

- **DO NOT PRICE A TRAINING RUN FROM A SHORT PROBE. Turn cost rises with the world.** journal 499 measured
  0.1496 s/turn over turns 1-200 of one map and priced the 8-map recipe at 33 min per arm; journal 500's real run
  reached **turn 600 of map 1 in 23 minutes -- 2.30 s/turn, 15.4x slower** -- which projects to **~8.5 hours per
  arm.** Backlog item 91's 168 min over 13,236 turns (1 thread) is the only completed-run figure anyone has.
  **Extrapolating from the first 200 turns of a 13,000-turn run is an invalid extrapolation, not a caveat.**
  What IS verified and reusable: `--data <isolated dir>` protects `data/ai/model.bin` (md5 identical across two
  training attempts) -- **the protection is `--data`, NOT the `--worker` suffix**, which writes `ai/model.bin` inside
  whatever tree `--data` names. And a retrain must be scoped to ONE variable: of items 168/171/180 only 180 is
  gated, so bundling them would measure none of them.

- **A GREP OF ONE DIRECTORY CAN REPORT A SHIPPED FEATURE ABSENT.** `grep -rn m_districts src/ai/` returned nothing
  and I concluded "the AI never draws a district" -- then built two journal verdicts and a user-facing brief on it.
  `updateAIDistricts` is in `Game_Policies.cpp` under `Game::`, called from `Game_TurnLogic.cpp:1052`. **The AI's
  behaviour is not confined to `src/ai/`: the per-country reflexes the turn loop runs are `Game::` methods.** Before
  reporting that the AI cannot do something, grep all of `src/` for the STATE the capability would write.

- **THE ACTION SPACE: 35 REAL ACTIONS, 40% OF THEM DEAD (journals 493-494).** ECON 11, POLITICS 11, WAR 7, NAVY 6 --
  the 12-slots-per-module space is format padding with no executors above those counts (`execNavy` stops at
  `case 6:`, `execWar` at `case 7:`), so **do not count unused slot numbers as dead features.** Of the 35:
  **1 is mask-unreachable (2.9%)**, **13 are offered with pi = 0 (37.1%)**, 21 are live. A retrain targets the 13.
  The single unreachable one is the ceasefire on 1914:FRA (WAR 6) -- the only gate defect in the whole space.

- **THE WAR HEAD HAS TWO HARD ZEROS: IT NEVER PASSES AND NEVER OFFERS PEACE.** Its pass action is offered on all
  3,200 decisions of a 400-turn run and taken **0** times at pi = 0.00e+00; the ceasefire likewise (journals
  492-493). ECON and POLITICS pass at pi 0.12-0.53, NAVY at ~1e-07. **This is a collapsed output, not an untrained
  one** -- on 1939:USA the ceasefire has been legal for ~15% of war decisions throughout training and still gets
  nothing.

- **A MASK OPENED ON A COLLAPSED HEAD CHANGES NOTHING: 224 offers and 472 offers, both taken 0 times at
  pi = 0.00e+00** (journals 491-492). So **a gate fix is not testable on a frozen model** when the head has no mass
  on the action -- and the right response is to cancel the bench rather than measure RNG. Distinct from
  masking-waste-costs, which is about mass MOVING; here there is none to move.

- **`OD_RESEARCH_BAR` IS SWEPT (journals 485-488), and it is a TRADE.** `0.30` is catastrophic: -125 rating and the
  USA below 10% of the world on five of eight seeds against zero of eight. `0.60` gains 1914:FRA about **+16 pp of
  world share across 16 seeds in two sets** (14 of 16, sign p 0.002) -- and costs 1939:USA **-14.8 pp on a second
  model** while costing it nothing on the shipped one. Do not substitute `0.50`: same allocation ceiling, a third of
  the gain, because the bar also governs how often `fund up` is OFFERED. Item 176, for the user.

- **UNEQUAL GRADED COUNTS VOID A RATING COMPARISON -- check them in BOTH arms first.** Standing point 3 says two
  models that saturate different seats are incomparable; journals 486-488 show two ARMS of one model doing it
  (6/16 vs 3/16, and 8/16 vs 11/16), because a gain that pushes a seat past the 5x cap removes it from the graded
  set. **The per-seat LAND figures survive this; the rating does not.** Quote land.

- **PICK A SECOND MODEL BY ITS GRADED COUNT, NOT BY CONVENIENCE.** Journal 487 spent 32 runs on
  `model.loop-base.bin` and learned nothing: it saturates at the FLOOR (1939:USA land 0.00 on 8 of 8 in both arms)
  where the shipped model saturates at the cap. **Run ONE control arm, read `N/M observations GRADED`, and only then
  run the treatment.** `build/loop/model.i14-final.bin` passes (8/16, pinning at the cap, no zeros).

- **A RULE THAT ENDS WARS ENDS WINNABLE ONES -- four instances now.** journal 496 measured `OD_PEACE_REFLEX=1` on
  the shipped model over 8 fresh seeds: 1914:FRA land **37.54 -> 31.46**, better on **1 of 8** (so 7 of 8 worse,
  sign p 0.035), no USA benefit, and **mean war length ROSE 25.0 -> 27.7 turns**. It joins
  caution-rules-trade-growth, withdrawing-loses-battles and passivity-is-load-bearing. **The AI's refusal to sue for
  peace is not obviously a defect**, even though it accepts ~85 ceasefires a run and offers zero (journal 495) and
  sits in 94-, 110- and 304-turn grinds (journals 477, 489). Measure before calling that a bug again.

- **Passivity is load-bearing.** The war head declines most attacks; a reflex
  taking only the "free" 2.5x-margin assaults collapsed France 6.5 → 0.5. The
  AI is not losing because it is passive, it is passive because it is losing.
  Anything that makes it fight more must make it stronger first.
- **The economy head is collapsed** — 8 of 12 actions at exactly 0.0. It is
  largely declining a menu it cannot pay for, which is correct: industry is
  withheld for want of money on 90.4% of the turns it wants it. Reallocating
  its spend has been tried four ways and every one lost land.
- **A mask cannot make a collapsed head act.** Masking an action off a frozen
  policy measures worse than it is; changes of that shape need a retrain.
- **A lump-sum cost gate on an AI action is a prohibition**, because AI
  treasuries run at zero. Price things as upkeep or capacity.
- **Self-play erodes rush defence.** Nothing in the league plays a rush, so the
  policy drops its defence against one while beating its own predecessor at
  every merge. Never judge a training run on head-to-head alone.
- **A rule written in `Game_Render` binds only the local player** — not the AI
  and not the network. Rules go in the resolvers.

## Standing rules added 2026-09-04 (evening)

- **Vary the base seed before believing a length effect.** The trainer's
  map sequence is a function of the base seed; "training degrades past
  ~1,000 turns" was one fixed second-map world (journal 35n). A length
  claim needs two seeds.
- **Diplomacy ends are rules, the middle is the head.** Where an answer has
  a clearly right value (gift → yes; cede at a loss → no; costless peace
  while losing → yes; unpaid peace while winning with a claim → no), it
  lives in `decideDiplomacy` ahead of the head, counted, and verified with
  `--probe-trade` and the eval's "by rule" lines. Reward shaping on this
  head only ever found the corners (journal 35c–35g).
- **Kill chains by PID, never by a marker word.** `pkill -f MARKER` matches every chain whose script mentions that marker (a waiter for it included); 2026-09-05 it killed the tickets chain that waited on the gate being cancelled. Record each chain's PID at launch (`echo $! > build/loop/<name>.pid`) and kill that.
- **Match real processes, not chain text.** `pgrep -f model.N11` matches
  the chain wrapper whose script mentions the bench; a bench "running"
  that way may not exist. Match the expanded binary path
  (`OpenDoctrinesServer --train-ai`, `^python3 tools/od_bench.py`).
- **Shared tree protocol.** A file claim is live only once the other
  session has ACKNOWLEDGED it; neither side edits a claimed file before
  then (a near-miss on 2026-09-05 would have failed a build loudly). When another session edits src/, no chain of
  this loop rebuilds `build/`; the other session compiles in its own build
  directory until this loop's benches drain; the loop's own lines are
  snapshotted in `build/loop/reference/v7-source.diff`. Re-baseline once
  per batch of rule changes, never per phase.
- **An A/B is quotable only if both arms ran in one session on one tree.**
  Twice on 2026-09-04/05 a stale baseline (taken before the other
  session's work landed) nearly bought a conclusion.
- **Reference freeze before a rule batch.** Models, binary, bench JSON,
  source diff and sha256 under `build/loop/reference/`; no git tag without
  the user.
- **Memory, not the lock.** `odlock` caps concurrent games, not RAM: on 16 GB, two trainers plus a gate is the ceiling, and one trainer when another session runs evals. Check `vm_stat` free pages and swap before adding a run (2026-09-05: 66 MB free, 5 GB swapped, a gate crawling).
- **Hold the ruler during a ladder stretch.** Five ruler moves in one night (v8.4–v10.3) made every ticket incomparable with its parent. When the goal is climbing, freeze the rules and resolvers, gate the parent once, and run tickets against that number; queue rule changes for the next batch.
- **The ladder.** Train from the last best, ONE map (pangaea), fresh
  base seed, `--vs-script`, ~750–1,600 turns; bench; keep if better. Run
  two or three seeds per step and keep the best ONLY if it beats the
  parent on the same ruler — three of the first eight one-map steps went
  up and five went down, by a lot (journal 35y); a step is a lottery
  ticket on the world drawn, and the gate is what makes the ladder climb. Multi-map steps go through continents/Earth worlds,
  which lost rating 3 of 4 times while the embarkation sink stands
  (journal 35r); revisit after it is fixed. Recipe of record:
  `--train-ai 1 3000 0 <seed> --worker 1 --workers 2 --vs-script --data <isolated dir>`
  with `OD_DIPLO_PACT_WEIGHT=2.5`, `OD_LR_SCALE=0.25` (a full-rate step from a
  strong parent lost 30–130 points in 12 of 13 tries; the quarter rate held
  the parent and beat the full rate by 36 on a paired world, journal 38l),
  STAGNATION_TURNS at its default 400 (80 was tested and cost 92 points on a
  paired world, journal 36e), bench with `OD_WAR_BIAS=0,0,0,0,0,0,0,5`.


**Corrections (2026-09-05 08:00).** `OD_MAX_GAMES` is not a game count: it
is `tools/odlock.py`'s slot count (default 2). Setting it to 5 in the
recipe let five gated processes run at once, which is the opposite of what
the lock is for; dropped from every command. Training sessions end by
STAGNATION_TURNS (400 turns without progress; `OD_STAGNATION_TURNS`), and
with one map that ends the session -- gated play (fewer assaults) freezes a
world sooner, so N55 trained 855 turns. Tickets now use three maps
(`--train-ai 3 3000 ...`) so a frozen world rotates instead of ending.
- **Instrument before widening a mask, always.** Backlog 201 named two bare
  constants in embark's mask as levers; a census showed both refuse ZERO calls
  in 400 turns, so benching either would have produced a bit-identical arm and
  a null indistinguishable from a real null (journal 509). The binding test was
  a different conjunct two frames upstream. Code-reading found the constants;
  only counting found which one binds.
- **Print the parts next to the whole, so the arithmetic can fail out loud.**
  Journal 509's first dump said 1515 refusals + 240 passes against a 1635
  denominator. 1515+240 > 1635 was the only reason the bug surfaced: the
  function was called by the mask AND the executor, and two populations were
  being shown as shares of one. A dump that had printed only percentages, or
  only the binding row, would have read as a clean result.
- **A mask widening is judgeable on a frozen model; a feature change is not.**
  Everything that moved a frozen net was a mask on an action the policy already
  took (research bar, +16pp). Everything that changed an INPUT nulled, because
  the weights encode the old distribution (journals 501-507, five arms). So when
  the loop looks out of frozen-model work, look for actions with a high
  take-when-offered rate and a low offer rate -- not for better features.
- **Pin `--binary` on every bench, and verify the TREATMENT arm moved.**
  `od_bench.py`'s `find_binary()` returns `build/OpenDoctrinesServer`
  (Release); AGENTS.md tells you to build `cmake-build-debug/`. Journal 510
  built a gate the documented way, benched it, and got two byte-identical arms
  -- which read as "the gate does nothing" and was actually "the gate was never
  in the binary". **A correctly inert gate cannot fail loudly**: the control arm
  is right, the treatment arm is the control, and nothing in the output names
  the binary. The only defence is pinning the path and requiring the treatment
  arm to move a mechanism counter before any outcome is believed.
- **Honour the falsifier you wrote, especially when the headline is large.**
  Journal 510 moved 1914:FRA +27.2pp on 8 of 8 fresh seeds and removed the
  seat's collapse -- and its own pre-registered reject condition (a collapse the
  control does not have) fired on the other seat. The default stayed off. A rule
  that only binds when the result is disappointing is not a rule.
- **A code comment's causal claim is evidence about the build it was written
  for, not this one.** Item 203 was filed straight out of `bestEmbarkPort`'s
  comment ("it was shipping out the defence of its own harbours ... survival
  fell 62% -> 48%") and proposed adding a garrison floor. A census on the
  failing seat found a garrison floor ALREADY refusing 15.1% of calls and the
  national share another 24.1% -- both brakes landed after that comment was
  written. Re-measure the mechanism before building on a comment's diagnosis.
- **A world-wide gate changes the opponents too, so a seat's loss may be the
  neighbours' gain.** Journal 510's USA collapse came with largest_pct
  45.7 -> 33.4 and herfindahl 0.250 -> 0.159: the gate made the whole world
  less consolidated, and a seat score is a share. Before "fixing" the seat,
  run the gate for the seat ALONE and for everyone EXCEPT the seat.
- **Ask who ELSE got the change before blaming the seat.** A world-wide gate
  reaches the opponents. Journal 512 split one into "seat only" and "everyone
  except the seat": both were mildly POSITIVE (+2.6, +3.3), while both together
  were negative with a collapse (−4.4) — a −10.3pp non-additive gap. Neither
  "the seat misplays it" nor "the neighbours take its share" was true.
- **Check how many games a rejection rests on.** Journal 510 rejected a
  +27.2pp, 8-of-8 gain over ONE collapse in ONE game on another seat, whose
  mean was a null (p 0.73) and which no other arm reproduced. A falsifier is
  still binding when it fires on thin evidence — but then replicating that
  evidence becomes the top item, not an afterthought.
