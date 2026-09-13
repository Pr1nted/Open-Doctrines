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
winning two great-power seats while losing `1914:FRA:rush` and `1939:NOR:hood`
is the exact trade this project has already made once and rolled back: it buys
skill at running a large country and pays in survival, and survival is the case
the AI actually loses.

**Rush guard.** REJECT any change that gives up more than 5 points on
`1914:FRA:rush` or `1939:NOR:hood`, whatever it does to the mean.

> **The rush half of this is not well-formed (journal 291).** `1914:FRA:rush`
> has two regimes and nothing between: ~197 and ~3 in score space, a step of
> 190 points. A 5-point threshold on a seat that only ever returns one of two
> values 190 apart is not a threshold — every reading is 0 or a catastrophic
> violation, decided by which side of the knife-edge that world fell. It is a
> COLLAPSE RATE question and needs ~128 seeds per arm to resolve a difference
> of 0.12. **The hood half works as written** (graded, narrow, a −4 means
> something). Until this is restated, no candidate can be described as
> "cleared past the rush guard" — the honest phrasing is "rush unresolved".
> Backlog item 19 is the decision.

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
> and is not resolved.

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
| **KEEP** | rating up, **survival not down**, and no rush/hood seat down more than 5 |
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

- **Training degrades the shipping model -- established on two seed sets
  (journals 357, 359).** N24 trained 8 maps on the settled recipe (journal 272)
  reads 336 -> 192 and 306 -> 184 per-seed rating, both clearing their floors,
  pooled −133 with CI [−197, −69]. The loss lands on the seats that can fall.
  Two traps, both nearly taken: a `--worker` run writes `ai/model.w1.bin` and
  leaves `ai/model.bin` as the untouched PARENT (Game_AITrain.cpp:191), so
  bench the worker file; and a training arm takes ~2.8 hours because maps end
  early on STAGNATION_TURNS -- do not price it as turns x 3000. One training
  run so far; a different training seed is untested.
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
