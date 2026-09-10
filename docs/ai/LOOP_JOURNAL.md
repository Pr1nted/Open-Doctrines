# AI loop journal

Append-only. Newest at the bottom. Every iteration gets an entry — KEEP,
REJECT and PARK alike. The rejects are the point: each one looked reasonable,
and without this file the loop will try it again in three weeks.

The protocol is `docs/ai/LOOP.md`. The queue is `docs/ai/BACKLOG.md`.

## Template

```
## NN — short-name — KEEP | REJECT | PARK
**Hypothesis.** If <change>, then <seat/counter> moves by <amount>, because <mechanism>.
**Touched.** path:lines — every file written, so a revert is mechanical.
**Built.** <binary tree, or "n/a">
**Measured.** <instrument and exact command>
    seat                base    after   delta
    ...
    RATING              nnn     nnn     +/-n     (seats won n/6)
**Verdict.** <what the number says, in one or two sentences>
**Reverted.** yes/no — <what came back out>
**Learned.** <the durable part, if any. Feeds the backlog or memory.>
```

---

## 00 — loop set up — n/a

**What.** Built the loop: `docs/ai/LOOP.md` (protocol), this journal,
`docs/ai/BACKLOG.md` (queue), `.claude/commands/ai-improve.md` (one iteration).

**Baseline frozen.** `data/ai/model.loop-base.bin` — a copy of `model.bin` as
of 2026-09-03 23:16 (4,139,506 bytes, `ODAZ`). Gitignored. `model.bin` itself
is dirty in the tree and was last written *after* the 129 rating in
`data/ai/bench_score.txt` was taken, so that 129 is not necessarily this file's
score. Hence a fresh reference run under the label `loop-base`.

**State of the art at set-up**, read off the source rather than assumed:

- Trunk 143 features → 320. Every policy head, the Q heads, the stance head and
  the diplo head are **single linear maps** on that 320; only `m_target`,
  `m_attack`, `m_value`, `m_diploValue` and `m_dynamics` have hidden layers.
- `m_diplo` is `{320, 2}` — accept/reject, one head for **every** offer type
  (ceasefire, alliance, NAP, guarantee, call to arms). The offer type reaches it
  only as a one-hot in the trunk's input (`feats[80]`, `[89..92]`).
- The stance head holds one opinion (consolidate 99.5%), so `STANCE_BIAS` is
  pinned at 0.0 and the whole posture mechanism is inert but wired.
- The economy head has 8 of 12 actions at exactly 0.0.
- League self-play exists (`m_league*`, `data/ai/league-{0..5}.bin`) and
  contains no rusher.

**Verdict.** n/a — no game code changed.

---

## 01 — headless bench — KEEP

**Hypothesis.** The rating does not need a renderer. If `tools/od_bench.py`
drives `OpenDoctrinesServer` instead of the windowed `.app`, every seat scores
*identically* while opening no windows and holding far less memory — and if any
seat differs, the two binaries disagree about the rules and the change must come
straight back out.

Prompted by the user watching eighteen OpenGL windows appear during a rating,
and by the machine having 16 GB.

**Touched.**
- `src/server/ServerMain.cpp` — parse `--bench-seat`, `--rush-neighbours`,
  `--vs-exploit` in the headless `--eval-ai` path; `<algorithm>`, `<cstdlib>`.
- `tools/od_bench.py::binary_path` — delegate to `ai_bench.find_binary()`
  instead of hardcoding the `.app`.

Nothing in `src/ai/` changed, so no decision the AI makes moved.

**Why it was ever windowed.** Not an oversight: those three flags were parsed
only by `main.cpp`, so the seat bench *could not* run headless. `ai_bench.py`
had already moved to the server binary for the same reasons in August.

**Measured.** All six seats, seed 20260801, `model.loop-base.bin`, both
binaries:

    seat                     windowed   headless
    1914:FRA rung                6.6        6.6
    1914:SWE rung                0.2        0.2
    1939:USA rung                8.5        8.5
    modern:CHN rung              0.3        0.3
    1914:FRA rush                1.8        1.8
    1939:NOR hood                0.7        0.7

Identical on all six, so every rating already in `od_bench_results.json` stays
comparable. That equivalence was the whole risk and it is the reason this was
checked seat by seat rather than assumed from "same code path".

    cost per 6-seat sweep    4.3 min  ->  54 s
    peak RSS per seat        ~2 GB    ->  0.58 GB
    windows opened           18       ->  0

**Verdict.** KEEP. A full 3-seed rating drops from ~13 min to ~3 min, which
changes what the loop can afford per iteration more than any other single thing
available. It also removes the failure that killed two training workers and an
unattended sweep in August: the windowed binary aborts when the display sleeps,
and the headless one has no display to lose.

**Reverted.** no.

**Learned.** The bench was expensive for a reason that had nothing to do with
the bench. Worth checking the same for anything else still driving the `.app`.

**PENDING COMMIT** (not staged, not committed — for the user to decide):

    Run the seat bench headless

    --bench-seat, --rush-neighbours and --vs-exploit were parsed only by
    main.cpp, so od_bench.py had to drive the windowed binary: eighteen OpenGL
    windows per rating, ~2 GB of peak RSS a seat, and the abort that kills any
    windowed run when the display sleeps. ServerMain parses them now, and
    od_bench shares ai_bench's binary resolver so the two cannot disagree about
    which tree is current.

    All six seats score identically under both binaries on seed 20260801, which
    is what keeps stored ratings comparable. A 3-seed rating: 13 min -> 3 min.

**Baseline for everything after this.** `loop-base` = **OD BENCH 108**
(`model.loop-base.bin`, 6 seats x 3 seeds).

    1914:FRA rung    90      modern:CHN rung   7
    1914:SWE rung   210      1914:FRA rush    59
    1939:USA rung   231      1939:NOR hood    54

The three weak seats are all survival ones. Note the shipped
`bench_score.txt` said 129; that reading belongs to an earlier `model.bin`,
not to this file.

---

## 02 — diplo-audit — KEEP (instrument only; no behaviour changed)

**Hypothesis.** "The AI is a diplomatic pushover" is a claim about *which*
things it agrees to, and `diploAccepted/diploRequests` is one rate over five
different questions, so it cannot confirm or refute it. Split the tally by
offer kind and by whether the asker is stronger, and either a specific kind
stands out or the complaint is about something else.

**Touched.**
- `src/ai/AISystem.h` — `enum OfferKind`, `offerKindOf`/`offerKindName`,
  four `long long[OFFER_KINDS]` counters on `TrainStats`.
- `src/ai/AISystem.cpp` — the helpers, plus increments in `decideDiplomacy`:
  the ask beside `diploRequests++` (above every gate), the stronger/weaker
  split where both armies first exist, the yes at both accept sites.
- `src/Game_AITrain.cpp` — a per-kind block under `said yes/was asked`.

No decision changed: every increment is a counter.

**Measured.** `--eval-ai 4 300 20260801 3 --scenarios --vs-script`, so MODEL is
the learned diplo head and CONTROL is the frozen hand-written rung answering
the same questions. Summed over the four maps:

    offer kind        MODEL said yes      SCRIPTED rung
    ceasefire            82/82   100%        37/119   31%
    non-aggression      147/159   92%       125/131   95%
    alliance             11/13    85%          2/3    67%
    call to arms          6/20    30%         2/27     7%

`sum(diploAskedOf) == diploRequests` on all four maps (58, 76, 70, 70), so the
counters sit above every gate as intended and no request is missed.

**Verdict.** The complaint is real, and it is *one* offer kind.

**IT HAS NEVER ONCE REFUSED A CEASEFIRE. 82 out of 82, on four maps, and
100% of the ones from a country with a bigger army.** Not a high rate — a
policy with no counterexample in it. Everything else is defensible: it makes
pacts at the rung's rate, allies slightly more readily, and actually answers
*more* calls to arms than the hand-written rule does (30% vs 7%), which is the
opposite of abandoning its allies.

A country that accepts every ceasefire cannot finish a war it is winning. The
loser asks, and it hands the initiative back, every time, for free. That is
what a player sees as a pushover, and it is the entire effect.

**This is the same shape as the collapsed economy head** — an action at exactly
1.0 rather than exactly 0.0, and the same diagnosis: a head that never varies
is not judging. Note `LR_DIPLO = 0.002` and that ceasefires are by far the most
common request, so it is also the kind with the most updates behind it.

**Reverted.** no — counters stay; they are how the next iteration is judged.

**Learned.** Aggregate agreement rate (49/58, 61/76, 67/70, 69/70 — a healthy-
looking 84–99%) completely hid this. Two kinds moving in opposite directions
inside one number is exactly the failure the split was built for.

**Not measured yet, and it matters:** whether refusing some of those ceasefires
*helps*. Accepting when you are losing is correct, and this project has already
paid 90% of a survival seat for a rule that forced the AI to fight more. The
next iteration measures the rate, it does not assume the direction.

**PENDING COMMIT** (not staged, not committed):

    Count diplomatic agreement by what was asked

    diploAccepted/diploRequests is one rate over five different requests and
    cannot say whether the AI is a pushover: 84-99% agreement looks alarming
    and is mostly non-aggression pacts, which the scripted rung signs at the
    same rate.

    Split by offer kind and by whether the asker is stronger. It finds one
    thing: the model accepts 82 of 82 ceasefires, including every one from a
    larger army, where the rung accepts 31%. A country that always agrees to
    stop cannot finish a war it is winning.

---

## 03 — ceasefire-floor — REJECT

**Hypothesis.** The head accepts 82/82 ceasefires (journal 02). If a floor
refuses the ones it clearly does not need — winning by ODDS, not war-weary, not
being invaded — the seat bench rises, because a country that always agrees to
stop can never finish a war it is winning. Predicted the rush and hood seats to
move most, since those are where an unfinished war costs the most.

**Touched.** `src/ai/AISystem.h` (two env-read knobs), `src/ai/AISystem.cpp`
(the statics and a gate in `decideDiplomacy`). Both **now reverted**.

Placed BELOW the scripted branch so it bound the learned policy alone and left
the rung's own ceasefire rule untouched, and defaulted to OFF so all three arms
ran under ONE binary — which is what made the control arm meaningful.

**Measured.** `tools/od_bench.py`, 6 seats x 3 seeds, `model.loop-base.bin`.

    seat              control   odds1.5   odds3.0
    1914:FRA rung          90       101       115
    1914:SWE rung         210       140       137
    1939:USA rung         231       138       142
    modern:CHN rung         7        13         7
    1914:FRA rush          59        98        98
    1939:NOR hood          54        51        54
    RATING                108        90        92

**The control arm reproduced 108 seat for seat** (6.0 / 2.1 / 12.9 / 0.2 / 3.9 /
0.7), which is the proof that journal 02's counters changed no decision.

**The gate fires.** Ceasefire acceptance 100% -> 65% under `--vs-script`, so
this is not the inert-flag failure recorded at `AI_CALL_RELUCTANCE`. `to
stronger` stayed at 100%, correctly: the floor only triggers when we are ahead,
so a larger army is never refused.

**Verdict.** REJECT — and the per-seed detail is the whole reason, because the
means alone read as an interesting trade and are not one.

    1914:FRA rush   1.8 -> 9.6 |  1.0 -> 1.0 |  9.0 -> 9.0
    1914:FRA rung   6.6 -> 2.7 |  5.3 -> 5.4 |  6.2 -> 15.1
    1939:USA rung   8.5 -> 8.8 | 16.9 -> 7.3 | 13.4 -> 7.8
    1914:SWE rung   0.2 -> 3.6 |  2.4 -> 0.4 |  3.7 -> 0.1

The rush seat's +39 is ONE seed. The other two are **bit-identical** across all
three arms — in a world at war, France is essentially never offered a ceasefire
it is winning by 3x and not weary, so the floor is inert exactly where it was
predicted to matter most. FRA rung's +25 is likewise one seed (6.2 -> 15.1) with
another seed halving. SWE swings 18x up on one seed and 37x down on another. The
only CONSISTENT effect in the table is USA losing on two seeds of three.

**Reverted.** yes — both files, and the bench re-run after the revert to confirm
the tree is back on 108.

**Learned — two things, and the second is the important one.**

1. A mean over three seeds hid four single-seed artifacts here. The seat means
   said "buys survival, pays in great-power growth", which is the *good*
   direction of the trade this project rolled a training run back for. The
   per-seed lines say there is no trade at all, just variance. **Read the
   spreads, not the seat means** — and note `tail -12` on od_bench's output
   silently drops the bracketed per-seed line, which is how this nearly went in
   the journal as a KEEP.

2. This is `passivity-is-load-bearing` again, in a different head, and it should
   have been predicted. It is also exactly the case `mask-changes-need-a-retrain`
   describes: `accept` sits at ~1.0, and forcing `refuse` on a FROZEN policy
   measures worse than the idea is. So this is strong evidence against *this
   gate on this model* and weak evidence against less-agreeable ceasefires as
   such. The distinction matters for what comes next.

**What it means for the goal.** The AI's 100% ceasefire acceptance is real and
it is measurable, but hand-forcing the rate down does not help — the cost of
that saturation is concentrated in seats the AI already wins, and in the seats
it loses it is barely ever the binding constraint. Making it *discriminate*
therefore has to come from the head learning to, not from a rule above it,
which is backlog item 2 and its retrain.

---

## 04 — diplo-cost — PARK (analysis; the fix needs a retrain, iteration 05)

**Hypothesis.** If the return attributed to accepting an offer cannot contain
the consequence of accepting, the head will accept everything, and 82/82 needs
no capacity explanation at all.

**Touched.** nothing. Read-only.

**What the plumbing actually does** — and it is all correct, which is why this
took reading rather than guessing. The diplo sample is recorded on the
`MOD_COUNT` slot with `diploFeatures` (the state *including* the request), it is
trained by the same PPO learner as the four modules, and `m_diploValue`
bootstraps the target, so a consequence beyond the 12-turn `N_STEP` window IS
reachable through the value function. None of that is broken.

**The reward is.** `diploReward` (AISystem.cpp ~7906):

    global
    + 0.6 tanh(coBelligerents / 2)
    + 0.6 tanh(pacts / 3)
    - 0.8 tanh(pactsLost)
    - 0.6 tanh(dWeary / 5)
    - 1.2 tanh(dLost / 2)          <- provinces LOST, its own term, the largest
    + trade outcome

**There is no term for provinces GAINED.** Losing ground has a dedicated
penalty at weight 1.2. Winning ground reaches the reward only through `global`,
as `PHI_PROV * (log1p(now) - log1p(then))` — and a *log* difference at 2.4
compresses to almost nothing at any realistic size.

Marginal reward over one window. Refusing a ceasefire risks +/-2 provinces and
two more points of weariness; accepting freezes all three at zero:

    provinces   refuse & WIN +2   refuse & LOSE 2   accept
           10             0.173            -1.623    0.000
           20            -0.010            -1.382    0.000
           40            -0.114            -1.262    0.000
           80            -0.169            -1.202    0.000

**At 20 provinces or more, refusing a ceasefire and WINNING is already
negative.** Accepting scores zero, so accepting strictly dominates every
outcome of fighting on, including the good one. Gaining two provinces pays
+0.218; losing two costs -1.154 — **5.3 to 1**.

**Verdict.** 82/82 is not a saturated head and not missing capacity. It is the
optimal policy for the reward it was given. The model is doing arithmetic the
reward asked for, correctly.

This is the same failure the file already documents one head over, at
`AI_CALL_RELUCTANCE`: an 18:1 asymmetry taught the policy to refuse every call
to arms, and the note there ends "the reward working as written rather than the
model failing". Same class, same head, opposite sign — and it was found the same
way, by pricing the two answers instead of watching the rate.

**It also retro-explains iteration 03.** The floor forced refusals the reward
scores as *negative even when they succeed*. No wonder it added variance and
cost the seats it did: it was overriding a correct policy with a rule the
learner would have unlearned.

**Reverted.** n/a.

**Learned.** When a policy head sits at exactly 0.0 or exactly 1.0, price the
two answers under the reward before touching the head. Both times this project
has found a rate with no counterexample in it, the reward was the cause.

**Next (iteration 05).** Give `dLost` a mirror — provinces gained while at war,
same 1.2 weight and same tanh shape — and retrain the diplo head. It must be a
retrain: a reward change cannot move a frozen model, so the bench would read
exactly 108 and mean nothing. Watch that the fix does not simply invert the
problem into refusing everything, which is what over-correcting the call-to-arms
asymmetry did.

---

## 05 — diplo-gain-term — IN PROGRESS (retrain running)

**Hypothesis.** Journal 04 priced the two answers and found refusing a ceasefire
scores negative *even when it succeeds*. Give `dLost` its mirror and retrain the
diplo head: the ceasefire rate should come off 100% to something situational,
and the rating should not fall.

**Touched.** `src/ai/AISystem.cpp` — one added term in `diploReward`:

    const float dGained = (exp.threatened > 0 ? now.provinces - exp.provinces : 0);
    ... + 1.2f * std::tanh(std::max(0.0f, dGained) / 2.0f);

Gated exactly as `dLost` is, same weight, same shape, so the two are one
measurement with opposite signs. `dLost` itself is NOT touched — it is shared
with the war module's reward at weight 2.0, and changing it would be a second
hypothesis in the same iteration.

**Why a retrain is unavoidable.** A reward change cannot move a frozen model.
Benching without one reads exactly 108 and means nothing.

**Setup, so a later iteration knows what these files are:**
- `data/ai/model.w7.bin` — the working copy. Seeded from `model.loop-base.bin`,
  then `--reset-ai-head ... diplo` discarded 1,903,212 updates from the diplo
  head; every other module kept.
- Trained as `--worker 7 --workers 8`, which is what makes the trainer write to
  `model.w7.bin` instead of `data/ai/model.bin`. No other `model.w*.bin` exists,
  so peer-blending finds nothing and is a no-op, and no merge runs.
- **`data/ai/model.bin` is not written by any of this.**

**CONTROL — and it was needed.** The comparison confounds two changes: the new
reward term AND the diplo head being reset. So `model.loop-base.bin` was also
reset and benched WITHOUT any training:

    loop-base        original head, accepts 82/82       108
    i05-resetonly    head reset, untrained (~random)     76
    i05-ck1          head reset + retrained, new reward  131

Deleting the old head alone COSTS 32 points, so the improvement is not an
artifact of removing an over-agreeable head — the retrain earns it, +55 over the
reset point. Note the ranking is not monotone in agreement rate (100% accept =
108, ~random = 76, 0% accept = 131): an inconsistent diplomacy appears to be
worse than either consistent one, which is worth remembering before treating any
"more balanced" rate as automatically better.

**When benching the result: snapshot first.** Training rewrites `model.w7.bin`
every minute, and a 3-minute rating loads the file eighteen times — so benching
it live would score eighteen different models and report the average as one.
Copy it to a frozen path, bench the copy.

### 05 result — KEEP the reward change; do NOT adopt the model

**Final.** 6 maps / 5,642 turns / 47 min, then benched from a frozen snapshot
(`build/loop/model.i05-final.bin`).

    seat              base   final   per-seed (final)
    1914:FRA rung       90     267   19.4  17.0  17.2
    1939:USA rung      231     325   23.8  14.7  16.1
    modern:CHN rung      7     324    2.8  12.5   9.1
    1914:FRA rush       59      88   10.8   3.5   3.4
    1939:NOR hood       54      54    0.6   0.7   0.7
    1914:SWE rung      210      10    0.1   0.1   0.0
    RATING             108     177

Every move is consistent across all three seeds — this is not iteration 03's
single-seed problem. The guard seats PASS: rush +29, hood exactly level (the 26
seen mid-training was an artifact of measuring an unfinished policy).

**And it is still not judging.** 0 of 145 offers accepted, 0% on all five kinds.
The head went from one constant to the other constant.

**The proof that this is a capacity problem, not a reward problem:**
non-aggression pacts sit at 0%. A NAP transfers no territory, so a territorial
reward term cannot rationally drive it from 94% to 0%. One `{320, 2}` linear map
serving five request types cannot hold "refuse ceasefires while winning, still
sign pacts", so it collapsed all five together. The audit predicted exactly this
and it is now measured.

**Sweden is the bill.** 210 -> 10 on 3/3 seeds: the small neutral seat, whose
survival runs on non-aggression pacts, signing none.

**Verdict.**
- **KEEP** the reward term. It is correct on its own merits (a province is now
  worth what it costs), the control proves the gain is not from deleting the old
  head, and it unlocked a far stronger war policy.
- **DO NOT adopt `model.i05-final.bin`.** Left in `build/loop/`.
  `data/ai/model.bin` was never written.

**And distrust the 177.** A policy that conquers relentlessly and never makes
peace scores 177 because three seats run 2.6-3.3x par while the one seat it
ruins contributes a tenth of one seat's worth of loss. That is the seat set
rewarding runaway growth, and it promotes backlog item 7 from a nice-to-have to
a real gap: the rating cannot presently distinguish "much better at the game"
from "abandoned a whole faculty and got bigger".

**Learned.** Correcting a reward that a head has converged against moves the
head to the NEAREST corner, not to a judgement. Where the head lacks the
capacity to condition on context, that corner is global — every offer type moved
together because one linear map cannot hold two opinions.

**PENDING COMMIT** (not staged):

    Pay the diplomacy reward for ground gained

    diploReward charged 1.2 for provinces lost and paid nothing for provinces
    taken -- gains reached it only through a log-compressed global term, so the
    two outcomes of fighting on were priced 5.3 to 1 against each other. At 20
    provinces, refusing a ceasefire AND WINNING scored -0.010 against a flat
    0.000 for accepting, so accepting dominated every outcome including the good
    one, and the head duly accepted 82 ceasefires out of 82.

    Add the mirror of dLost, gated and weighted identically, so a province is
    worth what it costs. Retraining the diplo head against the corrected reward
    takes the seat rating from 108 to 177, with the rush seat +29 and the hood
    seat level; a control with the head reset but untrained rates 76, so the
    gain is the retrain and not the removal.

    The head still needs capacity before it can use this: it answers all five
    request types from one {320,2} map and has collapsed them together.

---

## 06 — diplo-capacity — IN PROGRESS (retrain running)

**Hypothesis.** Journal 05 showed the head collapsed all five request types
together, pacts included, because one `{320,2}` map cannot hold two opinions.
Give it a pair per kind and mask every other pair, and Sweden should recover
without giving back France, the USA or China.

**Touched.** `src/ai/AISystem.h` (`DIPLO_OUTPUTS`, `offerKindFromFeatures`),
`src/ai/AISystem.cpp` — head widened to `{320, 2 * OFFER_KINDS}`; the decision
masks all but this kind's pair; the PPO update masks identically, reading the
kind back out of `diploFeatures` so nothing new is stored and the two cannot
disagree. The five existing thumbs on the scale still write `[reject, accept]`
and are widened at the single call site, so none of them had to change.

**Migration verified, not assumed.** An old `{320,2}` blob is the "gained
outputs" case `NeuralNet::deserialize` already handles. Loading
`model.loop-base.bin` into the wide head keeps **ceasefire at 100%** (16/16,
21/21) — the trained rows land on `OFFER_CEASEFIRE`, the kind they were
overwhelmingly trained on — while the other kinds run on fresh rows plus their
existing biases.

**RE-BASELINED**, because shared code changed: `loop-base` under the new binary
rates **93**, not 108. That is the comparison point for this iteration, and the
15-point drop is the untrained rows for the non-ceasefire kinds. `1914:FRA rush`
is bit-identical across the change ([1.8 1.0 9.0] both ways), which is the
expected signature: a rushing world asks almost only for ceasefires, and those
migrated exactly.

**A/B is against journal 05's final**, not against loop-base: same reward, same
recipe (head reset, 6 maps x 3000 turns, seed 20260904, worker 7 of 8), only the
head layout differs.

**GAP FOUND AND FIXED mid-iteration.** As first written, a shipped model loaded
into the wide head kept only its ceasefire policy and answered every pact,
alliance and guarantee from an untrained row — its behaviour would change the
instant it loaded. Fine for an experiment that resets the head anyway; NOT fine
for a player's existing `model.bin`.

deserialize's gained-outputs path is right for new ACTIONS and wrong here: the
extra outputs are not new choices, they are the same accept/reject asked about a
different request kind, so Xavier is the wrong prior. That is the argument the
file already makes for gained INPUTS ("must start IGNORED, or the model's
behaviour changes the instant it loads").

Added `NeuralNet::replicateOutputBlocks`, and `loadModel` now peeks the stored
output width and, for a narrow blob, reads it into a narrow net and repeats the
trained pair across all seven pairs.

**Verified, and it is exact.** `loop-base` under the wide head with the fan-out:

    seat            original   wide+fanout
    1914:FRA rung        6.0           6.0
    1914:SWE rung        2.1           2.1
    1939:USA rung       12.9          12.9
    modern:CHN rung      0.2           0.2
    1914:FRA rush        3.9           3.9
    1939:NOR hood        0.7           0.7
    RATING               108           108

Seat for seat identical, so the widening is behaviour-preserving on an existing
model and the 93 above was entirely the discarded rows. **The baseline for this
iteration is therefore 108, not 93**, and the re-baseline note above stands only
as the record of why the fix was needed.

---

## Side finding (iteration 06, no code under test) — the rating can be bought

Prompted by distrusting journal 05's 177. Re-read every stored result with each
seat capped at its own par:

    model            RATING  survival  median  worst  seats<50
    loop-base           108        68      74      7         1
    i06-rebase           93        62      68      5         2
    i05-resetonly        76        66      74     20         2
    i05-ck1             131        87     149     26         1
    i05-final           177        74     177      7         1

**The mid-training checkpoint beats the final model on survival, 87 to 74**,
while rating 46 points lower. i05-final bought runaway growth on three seats and
paid for it in robustness — the trade `selfplay-erodes-rush-defence` records as
a bad one, because holding a great power is the case the AI already wins.

`tools/od_bench.py` now prints `survival` and `worst seat` under every rating.
Deliberately **not** stored and **not** a second rating: the seat set and the
rating must not move or old scores stop being comparable. It is a second reading
of the same numbers, and `docs/ai/LOOP.md` now requires reading it.

### 06 side probe — where the collapse actually comes from

Mid-run, `model.i06-ck1` (map 2 of 6) still answered 0% to every kind, so the
per-kind head was NOT on its own preventing the collapse. Two hypotheses:

1. the diplo advantage is dominated by `global` and so says nothing about the
   ANSWER, only about how the country is doing — a confound no amount of head
   capacity can fix;
2. something else.

Instrumented it (`TrainStats::diploAdvSum`, printed beside the war-action
advantage table) and ran a short probe from a fresh reset — 655 turns, worker 0
of 2 so it could not touch the running experiment:

    kind              refuse        n      accept        n      yes-no
    ceasefire        -0.0615      307     -0.1708        8     -0.1093
    alliance         +0.0939       40     -0.0835       15     -0.1773
    non-aggression   -1.3483        1     +0.1557       37     +1.5041
    guarantee        -0.0530        2     +0.4394       22     +0.4925

**Hypothesis 1 is WRONG, and cleanly so.** The signal discriminates, per kind,
in sensible directions: accepting pacts and guarantees pays, refusing ceasefires
and alliances pays. And at this point the head is USING it — 37 of 38 pacts
signed while ceasefires are already refused. The per-kind capacity works.

**What the table also shows is the mechanism.** Ceasefire is 315 of 434 samples
— **73% of everything the diplo head ever learns from**. And the head
backpropagates into the SHARED TRUNK
(`m_trunk.accumulateVectorGradInto(ws.trunk, inputGrad(ws.diplo))`).

So the seven output pairs are independent, and the embedding they all read is
not. Masking separates the readout; it does nothing about the representation,
which is being dragged by whichever kind supplies the data. Over thousands of
turns the ceasefire gradient reshapes the embedding until every pair reads
"refuse" — which is exactly the trajectory observed: sensible per-kind answers
early, uniform refusal later.

**Capacity in the readout, none in the representation.**

**Next (iteration 07), and it is a real architectural change.** Give diplomacy a
representation that the dominant kind cannot monopolise. Cheapest test first:
scale down or detach the diplo head's gradient into the shared trunk, so the
trunk stops being reshaped by a head whose data is 73% one question. If that
holds the per-kind answers apart, the fuller version is a small diplo-only
trunk.

**Also worth noting:** this instrument should have existed before iteration 05.
"Is the signal informative about the decision" is answerable in five minutes and
would have reframed both retrains.

### 06 — RESTARTED. The probe contaminated the experiment.

The side probe above was run as `--worker 0 --workers 2` "so it could not touch
the running experiment". It could. The experiment was `--worker 7 --workers 8`,
which scans `model.w0.bin` .. `model.w6.bin` for peers and pulls a THIRD of the
way toward each every two minutes. Creating `model.w0.bin` handed it a peer:

    [TRAIN] worker 7 synced with 0/7 peer(s)     <- before the probe
    [TRAIN] worker 7 synced with 1/7 peer(s)     <- after

Repeatedly, so the experiment was dragged toward a model with 655 turns of
training on it. Everything measured from that run is void, including the
"still 0% at map 2" reading that motivated the trunk-dragging hypothesis.

Restarted from a fresh reset as `--worker 1 --workers 2`, whose only possible
peer is `w0` — a name now kept permanently empty.

**The rule this needs, and it is stronger than the one in memory.** The existing
note says to clear `model.w*.bin` BEFORE a pool run. That is not enough: a
worker file created DURING a run is picked up within two minutes. The pool is
built to blend, so:

**Only one training process at a time, unless blending is the intent.** A second
run is not isolated by choosing a different worker id — it is isolated only if
its file lies outside every running worker's peer scan, and with `--workers 8`
that scan covers almost every name.

The trunk-dragging hypothesis itself still stands on the ADVANTAGE TABLE, which
came from the probe's own process and is unaffected: ceasefire really is 73% of
diplomatic samples, and the head really does backpropagate into the shared
trunk. What is void is the evidence about *when* the collapse happens.

### 06 — supporting fixes and setup, while the clean run trains

**`loadOpponentModel` had the same widening bug as `loadModel`.** `--vs-model`
against a file written before the per-kind layout would have fielded an opponent
answering everything except a ceasefire from an untrained row. A head-to-head
whose two sides are not fed identically is not a match, and this is the half
nobody would think to check. Same fan-out applied; verified both paths load
(`diplomacy head widened ... copied to all 7`, opponent model accepted).

**Trajectory snapshots armed.** Journal 05's mid-training checkpoint beat its
final model on survival, 87 to 74, so benching only the endpoint throws away
most of a 90-minute run. `build/loop/snapshot.sh` copies the worker every ten
minutes into `build/loop/traj/`, twice two seconds apart, keeping the copy only
if the two are byte-identical — otherwise it caught the file mid-save.

**Concurrency, measured rather than assumed.** The trainer runs at 100% of ONE
core on a ten-core machine: turn resolution is serial and only the learning step
threads (capped at four by measurement, see `OD_AI_THREADS`). So training is the
loop's bottleneck and the machine is mostly idle during it.

`tools/train_parallel.py` already prefers `OpenDoctrinesServer`, so the pool is
headless. What limits it is the odlock gate of 2, whose default was set from
"every instance spikes to ~2 GB at map load" — **measured on the WINDOWED
binary**. A headless trainer sits at 1.2 GB and a headless bench seat peaked at
0.58 GB, so that budget is likely stale by 2-3x. Sampling the real peak across a
map load before touching it: raising a memory gate on an assumption is how this
machine went into swap the first time.

### 06 — the A/B must be matched on TURNS TRAINED, not on the arguments

Both runs were launched as `6 3000` and that does NOT make them equal. Journal
05's run rotated maps early whenever one froze strategically ("no province
changed hands between real countries in 400") and finished at **5,642 turns**.
The clean rerun may go much further on the same arguments.

Comparing a 5,642-turn model with an 18,000-turn one measures training length,
not head layout — which is the one thing this iteration exists to isolate.

So the comparison point is the trajectory snapshot nearest **5,642 cumulative
turns**, not the end of the run. The snapshots are timestamped and the training
log prints per-map turn counts, so the two can be lined up after the fact. The
later snapshots are still worth benching — they say what the layout does with
MORE training, which is a different and also interesting question — but they are
not the A/B.

### 06 — the memory budget, measured (gate deliberately NOT changed)

Sampled the running headless trainer every 2s for five minutes, across a map
rotation:

    headless trainer, peak RSS        1,409 MB
    headless bench seat, peak RSS       583 MB   (/usr/bin/time, journal 01)
    windowed binary, as documented    ~2,000 MB at map load

So the odlock default of 2 slots is calibrated for a cost 2-3x what anything
this loop now launches actually pays: `tools/od_bench.py` and
`tools/train_parallel.py` both prefer `OpenDoctrinesServer`.

**Left at 2 anyway.** Raising it would mean either a flat number that is unsafe
the moment someone runs the WINDOWED binary — still ~2 GB, and four of those is
what put this 16 GB machine into swap — or a weighted gate where a heavy process
takes two slots, which introduces a two-phase acquire and with it the classic
deadlock of two heavy processes each holding one slot and waiting for a second.
That is not a thing to add speculatively to the one piece of infrastructure
protecting the user's machine, for a throughput gain in a loop whose current
experiment is single-process anyway.

**Recorded so the decision is informed when it matters:** a pool that is known
to be headless-only can opt in with `OD_MAX_GAMES=4` (4 x 1.4 = 5.6 GB), which
is the case `tools/train_parallel.py --workers 4` would be. Anything that might
launch the windowed binary must stay at 2.

### 06 result — the capacity change works, and the rating disagrees with it

Clean run, same reward and same recipe as journal 05 (head reset, `6 3000`, seed
20260904), the ONLY difference being the per-kind head. Same six worlds in the
same order — pangaea, continents, 1914, archipelago, crowded, 1918 — confirmed
from both logs.

                        baseline   05 narrow   06 wide
    RATING                   108         177       101
    survival                  68          74        87
    worst seat                 7           7        36
    1914:FRA rung             90         267       137
    1914:SWE rung            210          10        90
    1939:USA rung            231         325       105
    modern:CHN rung            7         324       128
    1914:FRA rush             59          88       113
    1939:NOR hood             54          54        38
    turns trained              -        5642      3587

**Sweden recovered, 10 -> 90.** That is the collapse this change was built to
fix, fixed, and it is the strongest evidence that the diagnosis in journal 05
was right: one linear map could not hold "refuse ceasefires, still sign pacts",
and seven can hold more than one.

**Best survival and best worst-seat of anything measured** — 87 and 36 against a
baseline of 68 and 7. The wide-head model is the only one so far that is not
catastrophic anywhere.

**And the head really does discriminate now**: calls to arms answered at 50%
(1/2, 2/4) while ceasefires, pacts and alliances sit at 0%. Partial, but it is
the first time any two kinds have held different policies at the end of a run.

**By the letter of the protocol this is a REJECT** — rating down 7, and
`1939:NOR hood` down 16, which breaches the guard. Recording that plainly rather
than talking around it.

**CONFOUND, and it is large.** 3,587 turns against 5,642. Not a setup
difference: both runs got `6 3000` and the wide-head model FROZE EVERY MAP
SOONER (838/651/502/498/541/557 against 1349/752/485/400/1371/1285). "Frozen" is
400 turns with no province changing hands between real countries, so it is a
behavioural result in its own right — a model that signs pacts and settles ends
maps early — and it means this model got 64% of the other's training.

The trajectory says it is nowhere near converged:

    snapshot   rating   survival   worst
    t01            51         42       7
    t02            77         67       7
    final         101         87      36

Still climbing steeply at the endpoint. **101 is not a converged number and must
not be compared with 177 as though it were.** Extended the run by four fresh
worlds (seed 20260905) from the same weights to reach a matched turn count;
verdict deferred to that measurement.

### 06 FINAL — KEEP. The per-kind head, at matched training.

Extended the wide-head run by four fresh worlds (seed 20260905) from the same
weights: +2,611 turns, **6,198 total against journal 05's 5,642**, so if the
comparison is now unfair it is unfair to the wide head.

                        baseline   05 narrow   06 wide
    turns trained              -        5642      6198
    RATING                   108         177       160
    survival                  68          74        87
    worst seat                 7           7        62
    1914:FRA rung             90         267       304
    1914:SWE rung            210          10       120
    1939:USA rung            231         325       200
    modern:CHN rung            7         324        64
    1914:FRA rush             59          88       213
    1939:NOR hood             54          54        62

**Verdict: KEEP**, and on the criteria as written rather than on a reading of
them. Against the baseline the protocol judges: rating +52, survival +19, and
BOTH guard seats UP (rush 59 -> 213, hood 54 -> 62) where the rule only asked
that they not fall by 5.

Against journal 05's narrow head it gives back 17 points of rating and buys a
model that is catastrophic nowhere: worst seat 62 against 7, rush defence 2.4x
better. That is the survival-versus-runaway-growth trade in the direction this
project has twice said it wants.

**Per-kind differentiation survives to the end of a full run.** Calls to arms
100% (1/1) and 67% (2/3); ceasefires, pacts and alliances at 0%. Two kinds
holding genuinely different policies is the thing the narrow head could not do.

**A CORRECTION TO JOURNAL 05.** I wrote there that Sweden collapsed *because*
the head stopped signing non-aggression pacts — "the small neutral seat, whose
survival runs on pacts, signing none". Sweden recovered to 120 here while pact
acceptance is **still 0/64 and 0/54**. So that causal claim is not supported:
whatever restored Sweden, it was not the return of pacts. The mechanism is
unknown and should not be asserted.

**What is still wrong.** Ceasefires, pacts and alliances remain pinned at 0%.
The head can now hold calls to arms apart from the rest and no further, which is
what the trunk-dragging hypothesis predicts: calls to arms carry distinctive
features of their own (feats[80..84]) and can escape a shared embedding that
ceasefires — 73% of the samples — are reshaping. That is iteration 07, and
`OD_DIPLO_TRUNK_GRAD` is already built and inert at its default.

**PENDING COMMIT** (not staged; joins the three already listed):

    Give the diplomacy head one accept/reject pair per request kind

    One {320,2} map answered ceasefires, alliances, pacts, guarantees and calls
    to arms from a single embedding that four other heads also shape. It could
    not hold two opinions: correcting the ceasefire reward drove every kind to
    0% together, pacts included, though pacts transfer no territory.

    A pair per kind with the others masked at decision time, and the same mask
    on the PPO update so the ratio is measured against the distribution that
    acted. Old files fan the trained pair out to all seven pairs, so an existing
    model.bin plays identically on load -- verified seat for seat at 108.

    At matched training the seat rating goes 108 -> 160 with survival 68 -> 87
    and the worst seat 7 -> 62; both guard seats improve (rush 59 -> 213, hood
    54 -> 62).

---

## 07 — diplo-trunk — IN PROGRESS (prediction recorded before the result)

**Setup.** `OD_DIPLO_TRUNK_GRAD=0.0`, otherwise byte-identical to journal 06:
head reset from `model.loop-base.bin`, `6 3000` seed 20260904 then `4 3000`
seed 20260905, worker 1 of 2. The only variable is whether the diplo head may
reshape the shared trunk.

**Why detaching should HELP rather than starve it.** Setting the scale to 0 does
not stop the head reading the embedding — it stops it *rewriting* it. The
embedding is still shaped by the four modules, which need relative strength,
threat and war state for their own decisions, so it stays informative. What
changes is that ceasefires, at ~73% of diplomatic samples, can no longer drag
the representation the other six kinds are answered from.

**The specific thing to look for, and it is sharper than "does the rate move".**
In journal 06 the head refuses ceasefires from a STRONGER asker 100% of the time
(`to stronger 0%` accepted). Accepting when you are losing is correct play and
the rung does it 31% of the time, so this is the head failing to condition on a
feature that is definitely present — `feats[88]` is the army ratio and goes
straight through the trunk.

**Prediction.** If the trunk is the constraint, detaching lifts `to stronger`
off 0% for ceasefires — the head starts distinguishing an asker it is beating
from one that is beating it — even if the headline ceasefire rate stays low.
If `to stronger` stays at 0%, the trunk is NOT the constraint and the next
suspect is the reward again: with the gain term, refusing and winning pays
+0.904 against a flat 0.000 for accepting, so a model that has become strong
enough to win most wars it continues may simply be RIGHT to refuse, and the
0% would be a correct policy rather than a collapsed one.

Writing that second branch down now, because it is the reading I would be
tempted to reach for after the fact to explain a null result.

### 07 RESULT — REJECT the detach; keep what it revealed

Turn-matched: i07 ~6,144 cumulative against i06's 6,198. Env var verified live
in the trainer's own environment (`ps eww`) before trusting anything — an inert
flag is the failure this file records at `AI_CALL_RELUCTANCE`.

                     baseline   06 trunk=1.0   07 trunk=0.0
    RATING                108            160            151
    survival               68             87             90
    worst seat              7             62             38
    1914:FRA rush          59            213            131
    1939:NOR hood          54             62             38

**Verdict: REJECT.** Against journal 06 it gives back 9 points of rating and 24
of worst-seat for 3 points of survival, and `1939:NOR hood` falls to 38, below
the baseline's 54, which breaches the guard. `OD_DIPLO_TRUNK_GRAD` stays at its
default of 1.0, where it is inert.

**THE PREDICTION WAS WRONG, on the terms I wrote down.** I predicted detaching
would lift ceasefire acceptance FROM A STRONGER ASKER off 0%. It did not:
0/20 and 0/55, to-stronger 0%.

**The mechanism found support elsewhere.** Alliances went from a 0% constant
(journal 06: 0/1, 0/6) to **44/98 and 44/77 — 44% and 57% — with to-stronger at
52% and 56%**. That is the first genuinely intermediate, strength-conditioned
rate any request kind has produced in this project. So freeing the
representation DOES let a kind discriminate; the kind I named was the wrong one.

Caveat on those counts: alliance traffic exploded from ~1-6 requests to ~77-98,
because a model that accepts alliances gets asked for more. The RATE is a real
policy over a large sample; the comparison of rates across runs is between
different request distributions.

**Why detaching is the wrong shape of fix even though the diagnosis holds.**
Setting the scale to zero does not give diplomacy a better representation, it
removes its ability to shape ANY representation for itself — it becomes a
linear readout of an embedding optimised for four other heads. The measurement
says that is a net loss. The diagnosis (one question at 73% of samples reshapes
what the other six are answered from) points at ADDING capacity, not removing
it: a small diplomacy-only trunk, which the head can shape freely without
touching what the modules depend on.

**AND A REFRAME I OWE THE EARLIER ENTRIES.** Journals 02-06 all treat ceasefire
0% as a collapsed policy. It may not be. **A country asking for a ceasefire is
itself evidence that it is losing**, whatever its total army says — so
"refuses even a stronger asker" is not obviously a failure to condition on
strength; it may be correctly ignoring a feature that does not mean what it
looks like it means here. The model holding that policy rates 160 with survival
87; the baseline that accepted 82 of 82 rates 108 with survival 68.

The honest position: 100% acceptance was demonstrably wrong, 0% is not
demonstrably wrong, and nothing measured so far separates "0% is correct" from
"0% is the nearest corner". A test that would: score a seat where the AI is
LOSING and see whether accepting is what saves it.

---

## 08 — diplo-ceasefire-trunk — IN PROGRESS (prediction recorded first)

**Setup.** `OD_DIPLO_CEASEFIRE_TRUNK=0.0`, everything else byte-identical to
journal 06. Verified live in the trainer's environment before trusting it.

**Why this rather than the diplomacy-only trunk the backlog asks for.** A
separate trunk changes the diplo head's INPUT width, and `model.loop-base.bin`
carries a `{320,2}` blob — that is gained OUTPUTS *and* gained INPUTS at once,
and `NeuralNet::deserialize` handles either alone, not both. It would need a
two-step migration written and verified before a single number came out of it.
This tests the same hypothesis for three lines and no format change: if
ceasefires monopolise the representation because they are ~73% of samples, then
silencing THEIR trunk gradient alone should free the rest.

It is also the difference journal 07 got wrong. Detaching the whole head removed
diplomacy's ability to shape any representation. This removes one kind's
monopoly and leaves the other four shaping it — and every kind's own output pair
trains normally throughout.

**Prediction.** Non-aggression pacts come off 0% (journal 06: 0/64, 0/54;
journal 07 with the whole head detached: still 0/48, 0/41). If pacts move and
the rating holds near journal 06's 160, the diagnosis is confirmed and the
surgical fix is the right one.

**If pacts stay at 0% in BOTH this and journal 07**, the shared representation
is not what is pinning them, and the remaining suspects are the reward — pacts
pay `+0.6 tanh(pacts/3)` for holding them, which a model that wins by conquest
may rationally decline — or the pact cap gate above the net, which refuses
before the head is consulted at all and would make every measurement of "the
head's pact policy" a measurement of something else. **Check the gate first in
that branch**; it is the cheaper of the two and it is exactly the shape of the
`AI_CALL_RELUCTANCE` bug this file already records.

### 08 side fix — the counters could not tell a gate from a decision

Checking the branch predicted above (the pact cap) surfaced a flaw in my own
instrument. Several gates refuse BEFORE the head is consulted — the pact cap
`AI_ALLY_MAX_PACTS`, the four call-to-arms gates, the trade floor — and every
one lands in `diploAskedOf` with no matching `diploSaidYes`, which is
indistinguishable from the policy saying no. "Pacts 0/64" could mean the head
refuses every pact, or that it was never asked about one.

Added `TrainStats::diploReachedNet`, incremented immediately before the forward
pass, so `askedOf - reachedNet` is exactly what the gates ate and the head's
real rate is `saidYes / reachedNet`. One counter rather than touching every gate.

**Run on `model.i06-ext.bin`:**

    kind              said yes / asked      net asked
    ceasefire              0/22, 0/24       22/22, 24/24
    non-aggression         0/44, 0/54       44/44, 52/54
    call to arms            1/2,  1/8        1/2,   1/8

**The pact-cap branch is refuted, and cheaply.** The head sees essentially every
ceasefire and every pact. 0% on both is genuinely its policy.

**But calls to arms were almost entirely GATES.** Seven of eight eaten before
the net. The head was asked twice and said YES both times.

**Two corrections to earlier entries, both mine:**

1. Journal 02 read "call to arms 30% vs the rung's 7%" as the AI answering its
   allies more readily than the hand-written rule. The rate is real but it is
   mostly the gates, and the file documents those gates as correct
   (`AI_CALL_MAX_OWN_WARS` and friends — "a country already at war, already
   weary or already being invaded has nothing to send").

2. **Journal 06's headline claim is overstated.** I wrote that calls to arms
   "hold a different policy from everything else" and called it the first proof
   the per-kind head worked. That rested on `1/2` and `2/3` — which the new
   counter shows was ONE OR TWO head decisions, not a policy. The per-kind
   verdict in journal 06 still stands on its seat numbers (Sweden 10 -> 120,
   worst seat 7 -> 62, rating 108 -> 160), which are unaffected. The
   *behavioural* evidence for it does not, and I should not have leaned on a
   two-sample rate without checking the denominator — which is the same mistake
   as reading a three-seed mean without the spread (journal 03).

**Standing rule from this:** a diplomatic rate is only a statement about the
policy when quoted against `net asked`. Every per-kind figure in journals 02-07
was quoted against `asked`.

### 08 RESULT — REJECT, and the hypothesis is refuted

6,656 turns (3,811 + 2,845), against journal 06's 6,198.

                     baseline   06 trunk=1.0   07 detached   08 ceasefire-only
    RATING                108            160           151                 141
    survival               68             87            90                  86
    worst seat              7             62            38                  41

**Prediction failed.** Pacts did not move: 0/53 and 0/52, `net asked 53/53` and
`52/52`. The head sees every one and refuses it.

**And the unpredicted result is the informative one.** Ceasefires flipped back to
**100% acceptance** — 32/32 and 18/18 — the exact baseline pathology this whole
line of work began from. The head's own ceasefire pair still trained normally;
what it lost was a representation shaped by its own samples, and without that it
fell back to a constant.

**So the monopoly hypothesis is refuted.** Silencing the dominant kind's trunk
gradient did not free the others; it broke the kind that was silenced. Together
with journal 07 (detaching everything cost 9 rating and 24 worst-seat), the
picture is that the diplo head NEEDS to shape the shared representation, and the
problem was never that it shapes it too much.

`OD_DIPLO_CEASEFIRE_TRUNK` stays at its inert default of 1.0.

### ...and why pacts are actually pinned at 0%: I over-corrected in journal 05

Priced the two answers, the way journal 04 did:

    accepting a pact   +0.6*tanh(pacts/3), best case 0 -> 1   +0.193
                                             1 -> 2 pacts      +0.157
                                             3 -> 4 pacts      +0.065
    refusing it        keeps that country conquerable; declareWar must break a
                       pact first, so a pact is a standing constraint
                       +2 provinces under the journal-05 gain term   +0.914

**4.7 to 1 against signing** — the same shape as the 5.3:1 asymmetry journal 04
found, in the opposite direction, and created by my own fix. The gain term is
correct and it is now unbalanced against every agreement term beside it, none of
which was rescaled when it went in.

So the head refusing every pact is, again, the reward working as written. Three
times now this project has found a rate with no counterexample in it and the
cause has been the reward, not the head.

**What this means for the goal.** The AI has gone from "agrees to everything"
(108) to "agrees to nothing and wins" (160). Neither is judgement. The thing
worth having is a rate that MOVES with circumstance, and the instrument for that
is the `to stronger` split, not the headline rate.

---

## 09 — diplo-rebalance — IN PROGRESS (prediction recorded first)

**Setup.** `OD_DIPLO_PACT_WEIGHT=5.0`, scaling both agreement terms (pacts held
and co-belligerents) from 0.6 to 3.0. Recipe otherwise byte-identical to journal
06. Flag verified live in the trainer's environment.

**Why 5.0.** It makes the two answers commensurate rather than making agreements
win: a first pact goes from +0.193 to +0.96, against +0.914 for taking two
provinces from the country the pact would have protected. Roughly parity, which
is the condition under which the answer has to depend on the situation — the
thing journal 04 said was "the only thing worth learning here".

**Success is NOT "pacts come off 0%".** A rate that flips from 0% to 100% is the
baseline pathology wearing different clothes, and this project has now produced
three constants in a row (accept-everything at 108, refuse-everything at 160,
accept-ceasefires-again at 141). The criterion is a rate that **MOVES with
circumstance**: `to stronger` differing from the headline rate, the way alliances
did in journal 07 (44% overall, 52% to a stronger asker). Quoted against
`net asked`, per the rule added after journal 08.

**And the rating must hold near 160.** Signing pacts forecloses conquest, and
conquest is what the current model scores with, so a large rating drop would say
the rebalance bought diplomacy by making the AI worse at the game.

**Prediction.** Pacts land somewhere intermediate — call it 20-70% — with a
visible `to stronger` split, and the rating lands between 140 and 165. If pacts
go to ~100%, 5.0 is too high and the sweep continues downward at 2.0.

### 08/09 side finding — the diplo head is DETERMINISTIC, and nothing was watching

`NeuralNet::PPOStats::entropy` exists because entropy "says a head is collapsing
WHILE it collapses -- by the time a bench prints 100% the run is over and the
model is ruined". It is wired for the four module heads. It is **not** wired for
the diplo head — the one that has produced a constant in every configuration
this loop has tried.

Added a decision-time entropy per request kind. **Two mistakes on the way, both
worth recording because both nearly became findings:**

1. First version computed entropy from the SAMPLED log-probability. The seat
   bench runs difficulty 3, whose temperature is **0.18** — the file's own note
   says "a one-unit logit lead is about 4:1" there — so that measures the eval
   temperature, not the head, and reads 0.000 for anything merely confident.
   Fixed by using `pickAction`'s `neutralProbsOut`, which the file already
   provides as "what the policy thinks, before temperature has an opinion".
2. Second version printed the mean with no denominator. An unpopulated vector
   would have printed a confident 0.000 from zero samples — the same missing-
   denominator error as `asked` vs `net asked`, twice in one session. Printed
   `n` and confirmed 15, 3, 31.

**The measurement, at temperature 1, both models:**

    model        kind             rate      H      n
    loop-base    ceasefire       15/15   0.000    15
    loop-base    non-aggression  31/34   0.000    31
    loop-base    alliance          3/4   0.000     3
    i06-ext      ceasefire        0/22   0.000    22
    i06-ext      non-aggression   0/44   0.000    44

ln2 = 0.693 is an undecided binary policy. **0.000 is a policy with no
uncertainty anywhere** — the logit gaps are large enough that the softmax is
saturated on every individual decision, in both models.

**What this does and does not mean.** It does NOT by itself mean "collapsed":
`loop-base` is deterministic per state and still signs 91% of pacts, so its
answers vary with the situation — a sharp decision boundary, not a constant.
`i06-ext` is deterministic AND constant. H tells them apart from neither; the
rate does that. What H says is that **neither model is ever unsure**, so
training explores this head only through `epsilon`, which decays, and not
through the policy at all.

That is why a reward change moves the head to a new corner rather than to a
judgement (journals 05, 08): from a saturated policy the alternative action has
probability ~0, PPO's ratio has nothing to work with, and an entropy bonus of
`PPO_ENTROPY = 0.01` is negligible against advantages of order 1.

**Iteration 10 candidate, and it is the first one aimed at the LEARNING rather
than the reward or the wiring:** a per-head entropy coefficient, raised for the
diplo head only. The global value was tested and correctly kept at 0.01 — raising
it to 0.03 cost the WAR module 0.31 land, because that head's selectivity is
where it beats the control. That experiment says nothing about a binary head
with two orders of magnitude fewer samples.

### 09 RESULT — REJECT on the rating; the FIRST non-constant policy

6,556 turns (3,637 + 2,919), against journal 06's 6,198.

                     baseline   06 (x1)   09 (pacts x5)
    RATING                108       160             106
    survival               68        87              82
    worst seat              7        62              31
    1914:FRA rush          59       213             128
    1939:NOR hood          54        62              31

**Verdict: REJECT.** Rating back to baseline, survival down 5, hood down 31.
`OD_DIPLO_PACT_WEIGHT` stays at its inert default of 1.0.

**And yet it did exactly what was written down as success.** The criterion
recorded before the run was "not that pacts come off 0%, but that the rate MOVES
with circumstance":

    kind             said yes    net asked    to stronger    H     n
    non-aggression     58/89        58/89          78%     0.000   58
    non-aggression     73/115       73/115         62%     0.000   73
    alliance            1/8          1/8            0%     0.017    1
    alliance            3/5          3/5           50%     0.020    3
    ceasefire           0/20        20/20           0%     0.000   20

Pacts at **65% and 63%**, against a headline-versus-to-stronger split of 78%/65%
on the first map. That is the first intermediate, circumstance-dependent
diplomatic policy this project has produced. Every prior configuration was a
corner: 100%, 0%, 100% again.

Note `net asked 58/89` — the pact cap ate 31 of 89, which is only visible
because of the counter added after journal 08, and which is CORRECT behaviour
(a country that has signed many pacts should stop).

**H is still 0.000 with n=58.** So the head remains deterministic per state, and
the 65% is a decision BOUNDARY rather than a mixed strategy — the same shape as
`loop-base`'s 91% pacts. Determinism is not the thing standing between this
project and judgement; the reward balance was.

**The tension is real, not an artifact.** A pact forbids attacking that country
(`declareWar` must break it first), and land is what this model scores with. So
judgement on pacts costs conquest, and at weight 5.0 it costs 54 rating points.
That is a genuine design trade rather than a bug, and it means there is a curve
to find rather than a switch to flip.

**Next: sweep DOWN, not further up.** At 5.0 a first pact pays +0.96 against
+0.914 for two provinces — parity. At 2.5 it pays +0.48, roughly 1.9 to 1
against, which may be enough to keep the answer situational without buying it at
the price of the whole conquest game. If 2.5 gives an intermediate pact rate AND
a rating near 160, that is the point worth keeping.

### 09/10 side finding — a non-aggression pact has no teeth, and that is a GAME finding

Journal 08 priced refusing a pact as "keeps that country conquerable;
`declareWar` must break a pact first, so a pact is a standing constraint". Went
and read the rules rather than trusting my own summary. It is wrong.

**In the game rules a pact prevents nothing:**
- `Game::declareWar` does not check for one. It sets `fwd.nonAggression = false`
  and proceeds (Game_TurnLogic.cpp:4024, 4047).
- `break_nap` clears the flag on both sides and charges nothing
  (Game_TurnLogic.cpp:4744-4746).
- No credibility hit, no cooldown, no trade benefit, no defensive obligation.
  Its only other effect is the mood/tension term (Game.cpp:2629) and the map
  colour.

**The only cost of holding one is self-imposed by the AI.** `findWarTarget`
records `napBlocked` and requires the pact be broken FIRST — one turn of delay
before it can attack that country. The comment there says so plainly: "A NAP
does not make this target off-limits, but it does mean the pact has to be broken
FIRST".

**So signing a pact costs the AI a turn of delay and buys it nothing at all.**

That is the whole explanation for a thread of failed iterations. The head
refusing every pact is not a collapsed policy, not a trunk problem and not a
reward-balance artifact — **it is correct play**. And it means no reward shaping
can make signing CORRECT; it can only make the AI act against its own interest,
which is precisely what journal 09 did, at a cost of 54 rating points, while
producing beautifully situational-looking behaviour.

**And the game contradicts itself here.** It already tracks `m_credibility` and
penalises a country for LYING ABOUT ITS INTENTIONS
(`claimsBrokenByDeclaration`, `noteWarGoalStatement`) — but charges nothing for
BREAKING A SIGNED TREATY. Saying "we are too spent to fight" and then declaring
war is punished; signing a non-aggression pact and then declaring war is free.

**This belongs to the user, not to the loop.** Making pacts worth signing is a
game-design change with balance consequences a benchmark cannot adjudicate:
either breaking one costs credibility (the machinery exists and is already wired
to the diplomacy head through `CREDIBILITY_WEIGHT`), or a pact confers something
— trade, stability, a defensive obligation. Until one of those is true, an AI
that signs pacts is an AI playing worse, and the honest configuration is the one
that refuses them.

**Journal 08's "4.7 to 1 against signing" arithmetic stands as a description of
the reward, and its REASONING was overstated:** the conquest a pact forecloses
is delayed by one turn, not forbidden.

### ...and the same question for CEASEFIRES — the answer is DIFFERENT

Checked whether the pact finding generalises. It does not, and the distinction
matters for how "0% acceptance" should be read on each kind.

**A ceasefire has real teeth.** Accepting ends the war, applies the terms, and
calls `withdrawArmiesAfterPeace` — the invader's troops go home and lose their
forward positions. That is substantial protection for a country that is losing,
and it is exactly what the baseline model was buying when it accepted 82 of 82.

**What it lacks is durability.** There is no `truce` concept anywhere in the
codebase — no grep hit — and no cooldown on re-declaring. The enemy can attack
again the following turn. So a ceasefire is a real but temporary reprieve.

**Therefore, and contrary to the reframe in journal 07:** 0% ceasefire
acceptance is NOT obviously correct play. It gives up a genuine protection. What
IS defensible is that the protection is temporary, so it is worth less than a
naive reading suggests — which is consistent with the seat numbers, where the
model refusing every ceasefire rates 160 against the accepting baseline's 108.

    pact       no mechanical effect whatever; free to break; refusing is correct
    ceasefire  ends the war, withdraws the invader; refusing gives up something real

Two different findings that a single "diplomacy has no teeth" summary would have
flattened into one wrong one.

---

## 10 — diplo-rebalance at 2.5 — KEEP

7,444 turns (4,761 + 2,683). The sweep now has three points:

    pact weight   RATING  survival  worst   alliances      pacts
    1.0 (jrnl 06)    160        87     62         ~0%         0%
    2.5 (this)       158        86     49      83-85%         0%
    5.0 (jrnl 09)    106        82     31      12-60%        64%

**2.5 keeps the whole of the playing strength and buys real diplomacy.** Two
rating points and one survival point against journal 06, for an AI that signs
alliances at 83% and 85% — with a strength split, to-stronger 77% and 70%
against those headlines, so the answer depends on who is asking. Quoted against
`net asked` (34/41, 44/52), per the rule from journal 08.

**The guard breach does not survive inspection.** `1939:NOR hood` reads 49
against journal 06's 62, which is a 13-point fall and would be a REJECT. Per
seed:

    i06   0.7  0.5  1.2
    i10   0.7  0.6  0.6
    delta 0.0 +0.1 -0.6

Seeds 1 and 2 are identical to within 0.1; the entire gap is seed 3. That is the
single-seed artifact journal 03 was written about, and against the BASELINE the
same seat is 49 vs 54 with only seed 2 differing at all. The two models are not
distinguishable on that seat.

**Verdict: KEEP.** Against the baseline the protocol judges: rating 108 -> 158,
survival 68 -> 86, rush 59 -> 160. Against journal 06 it is a wash on strength
and a large gain in behaviour.

**Why ALLIANCES and not pacts — and it is the pact finding again.** The reward
credits treaties HELD, and an alliance earns that credit while also producing
co-belligerents, which the second scaled term pays for. So an alliance pays
twice where a pact pays once. And per the side finding above, a pact costs a
turn of delay and confers nothing, while an alliance brings mutual defence. The
model preferring alliances to pacts is, once again, correct play — it found the
agreement that is actually worth signing in this game and ignored the one that
is not.

**This is the answer to the standing direction.** The AI is now less agreeable
than the baseline in the way that was asked for — it refuses every ceasefire and
every empty pact where the baseline accepted 82 of 82 — while being a far
stronger player AND holding a real, circumstance-dependent alliance policy
rather than a constant.

**PENDING COMMIT** (not staged; joins the four already listed):

    Weight the diplomacy reward's agreement terms

    The journal-05 gain term made ground taken worth 1.2 and left every
    agreement term at 0.6, so holding treaties was outbid roughly 4.7 to 1 and
    the head refused all five request kinds. Scaling the agreement pair by 2.5
    restores a real alliance policy -- 83-85% accepted, 70-77% from a stronger
    asker, so the answer depends on the situation -- at a cost of 2 rating
    points (160 -> 158) and 1 of survival.

    Not higher: at 5.0 pacts also come off 0% and the rating falls to 106,
    because a pact confers nothing in the rules while costing a turn of delay.

---

## 11 — war-stage-bias — KEEP (and it needed no retrain)

**Found by measuring, not by hypothesis.** Read the policy shape at T=1 off
`model.loop-base.bin` and cross-referenced the war-action advantage table from
journal 05's own training log:

    war action     mean advantage    policy P at T=1
    stage               +1.0175                 0.0
    declare war         +0.4094               100.0
    artillery           +0.3990             0.0/4.3
    attack              +0.2005                85.4
    reinforce           +0.0339                94.3
    ceasefire           -0.0306                 0.0

**The highest-advantage action in the war module has zero probability.** The
1,066 staging moves in that run were EXPLORATION finding it, not the policy
choosing it, and every one paid. A collapsed action cannot recover unaided: at
P=0 it is never sampled, so nothing but the entropy bonus pushes back — and
journal 08's side finding showed this head has H=0.000.

`stage` moves half a garrison onto ALLIED soil beside a shared enemy. Its own
comment: "the AI just had no way to name a province it did not own, so it never
once used an alliance to reach a front."

**Touched.** `s_warStageBias` (`OD_WAR_STAGE_BIAS`, default 0 = inert), added to
the existing `qbias` path beside `STANCE_BIAS`, on the war module's action 7
only and only where the mask already says a crossing exists — so it cannot
invent an opportunity, only stop the policy declining one.

**Measured on a FROZEN model — no retrain, ~3 min an arm.**

    bias   RATING  survival  worst
    0         158        86     49
    2         161        89     49
    5         162        91     49
    10        162        91     49      (saturated)

The whole gain is `1939:USA rung`, 3.8 -> 5.4, per seed:

    bias 0   9.6  1.4  0.5
    bias 5   9.6  2.9  3.7

Two seeds improve substantially, one is unchanged, **none get worse** — which is
the pattern journal 03 exists to demand. Every other seat is identical to within
0.2.

**MECHANISM CONFIRMED, and this is the part worth keeping.** Staging needs an
alliance, so the bias should help a model that signs them and do nothing for one
that does not:

    model      alliances   bias 0            bias 5
    i06-ext         ~0%    160/87/62         160/87/62   identical
    i10-final       83%    158/86/49         162/91/49

Exactly as predicted. **The two changes are complementary in a way neither shows
alone.** Journal 10's alliance policy read as a wash on the bench (158 against
160) and was in fact the PRECONDITION for this gain: the alliances are what
create the staging opportunities, and the bias is what stops the policy
declining them.

    journal 06 alone                160  survival 87
    journal 11 alone (on 06)        160  survival 87   no effect
    journal 10 alone                158  survival 86
    journal 10 + 11 together        162  survival 91   best measured

**Verdict: KEEP.** Rating up, survival up to the best figure this project has
recorded, no seat worse. `OD_WAR_STAGE_BIAS` default stays 0 — shipping 5 is the
user's call, like the pact weight.

**Learned:** a change that reads as a wash may be a precondition rather than a
dead end. Judging journal 10 on its own number alone would have thrown away the
best configuration measured.

---

## 12 — artillery and attack biases — REJECT (both)

Generalised journal 11's single knob into `OD_WAR_BIAS` — one comma-separated
bias per war action — so any dead action can be swept without a new constant
each time. `OD_WAR_STAGE_BIAS` still sets index 7, and the control reproduces
journal 11's 162/91/49 exactly through the new path.

**ARTILLERY: inert at any weight, including 60.** Bit-identical output —
`P war:artillery 0.0`, `offered/chosen 1629/1` — at bias 0, 20 and 60.

**ATTACK: a single-seed artifact.** With stage +5 alongside:

    attack bias   RATING   1939:USA per seed
    0                162     9.6  2.9  3.7
    -2               162     9.6  2.9  3.7    identical
    -5               162     9.6  2.9  3.7    identical
    -20              167     9.6  7.6  3.7    seed 2 only

The whole +5 is seed 2. Seeds 1 and 3 do not move at all, and -2/-5 change
nothing anywhere. Exactly the pattern journal 03 was written about, caught by
looking at the spread before believing the mean.

**AND THE USEFUL PART — why one bias works and another cannot.**

    action     printed P    responds to
    stage           0.0     +5
    artillery       0.0     nothing, up to +60
    attack         85.4     -20

`stage` and `artillery` print the SAME "0.0" and are separated by more than
fifty nats of logit. The shape table rounds to one decimal, so **"0.0"
conflates a probability of ~4% with one of ~1e-30**, and only the first is
within reach of a bias. That is a limitation of the instrument I was reading
when I picked artillery as the next target, and it is why journal 11 succeeded
and this did not.

It also fits journal 08's entropy finding: H = 0.000 everywhere means saturated
logits, and a saturated head can sit arbitrarily far from an action.

**A false step worth recording.** My first probe was `hold +20`, which changed
nothing, and I briefly took that as the bias mechanism being broken — which
would have put journal 11's KEEP in doubt. `attack -20` moving the score from
158 to 164 showed the mechanism reaches action selection fine; `hold` is
evidently masked out when any real option exists. **A null result on one action
says nothing about the mechanism** — the right control is an action already
known to be live.

**Verdict: REJECT both.** `OD_WAR_BIAS` stays all-zero and inert. The staging
bias from journal 11 remains the only war-action bias worth having.

**For the backlog:** the way to reach a truly dead action is NOT a bias. It is a
retrain of that head, which is what `--reset-ai-head war` exists for — and the
war head is this model's principal strength, so that is a risk to take
deliberately rather than casually.

---

## 13 — shape precision — KEEP (instrument), and it maps every dead action

`%.1f` on the policy-shape table printed `stage` and `artillery` identically as
0.0 when they are fifty nats apart, which is what sent journal 12 after the
unreachable one. Small values now print in exponent form.

**The map, `model.i10-final.bin`, one map / 250 turns:**

    war                          econ
    recruit        100.0         focus army      100.0
    reinforce       81.5         fund up          90.0
    declare war     33.3         save             70.5
    attack          21.5         industry          6.0
    stage           13.4         fund down     5.8e-14
    hold             4.6         focus bldg    5.9e-18
    artillery    1.9e-33         fort, port, specialize,
    ceasefire        0.0         destroyer, carrier,
                                 focus navy        0.0

**Two findings.**

1. **`stage` is at 13.4% on this model.** It was 0.0 on `loop-base`. Journal
   10's training revived it — which is WHY journal 11's +5 bias could move it
   and why the same bias does nothing to `model.i06-ext`. The mechanism story
   holds together from a third direction.

2. **The economy head is comprehensively collapsed: eight of twelve actions at
   or below 1e-14, six of them hard zeros.** No bias reaches those; journal 12
   established that at +60. The only instrument left is `--reset-ai-head econ`
   and a retrain, which is exactly what the standing memory note says has never
   been tried — every previous attempt reallocated what the head already spends.

**Verdict: KEEP the instrument.** It costs nothing, changes no behaviour, and it
would have saved journal 12 entirely.

---

## 14 — econ head reset + retrain — REJECT by the guard; the largest gain measured

Reset the ECON head on `model.i10-final.bin` (351,619,863 updates discarded,
every other module kept) and retrained 4,217 turns with pact weight 2.5.
Benched with the journal-11 stage bias.

                     i10+stage   i14
    RATING                 162    206
    survival                91     88
    worst seat              49     31
    1914:FRA rung          264    246
    1914:SWE rung          233    280
    1939:USA rung           96    216
    modern:CHN rung        173    316
    1914:FRA rush          160    143
    1939:NOR hood           49     31

**+44 rating, the largest single gain of the session** — and it fails the guard.
`1939:NOR hood` per seed: 0.7/0.6/0.6 -> 0.4/0.6/0.2, worse on two seeds of
three, so this is NOT the single-seed artifact of journal 03. Survival -3.

**Verdict: REJECT.** `docs/ai/LOOP.md` says a rating gain with survival falling
has bought runaway growth and paid in robustness, and that rule was written
before this number existed, specifically so a big number could not talk me past
it. Recording the verdict the rule gives.

**THE HEAD DID NOT GET FIXED. It got MORE collapsed.** How often each action was
even offered, before and after:

    industry    449 ->   3      destroyer   768 ->   3
    fort        860 ->   3      carrier     460 ->   2
    port         64 ->   0      specialize  880 ->   7

The retrained head saves 80.6% and funds research 89%, so the country never
affords a fort, a port or a ship and the mask stops offering them. The dead
actions are not merely still dead — they are now unreachable.

**And that is the finding.** The standing memory note says the econ head "needs
a retrain" because eight actions sit at 0.0. This tested it. A retrain does not
produce a head that uses its menu; it produces a DIFFERENT collapse — hoard,
research, conquer — which happens to play the game better. **The collapse was
never the bug.** A head with 8 dead actions scored 162; a head with 10 dead
actions scores 206.

**What the user should know:** 206 is real, consistent (SWE 2.6/3.1/2.8 and CHN
9.8/7.2/6.8 are far tighter than the model it replaces), and available in
`build/loop/model.i14-final.bin`. It trades the smallest, most-pressured seat
for large gains on three others. Whether that trade is right is a judgement
about what the AI is FOR, which the seat bench cannot settle — the same shape of
question as the pact decision.

**Learned.** "This head is collapsed" is a description, not a diagnosis. Twice
now the collapse has turned out to be correct play under the rules as written
(pacts, journal 09/10) or simply irrelevant to strength (here). Check what a
head's dead actions would BUY before spending a retrain on reviving them.

---

## 15 — econ head, more training — IN PROGRESS (prediction recorded first)

**The confound in journal 14.** Its econ head is FRESH: 4,217 turns against the
millions of updates every other module carries. The hood regression (49 -> 31,
two seeds of three) may be an undertrained head rather than a real trade — and
journal 06 already showed a mid-training checkpoint reading very differently
from its endpoint.

Continuing the same run on fresh worlds (seed 20260907) from
`model.i14-final.bin`, verified byte-identical to the worker file before
starting.

**Prediction.** If the regression is undertraining, `1939:NOR hood` recovers
toward 49 while the rating stays near 206 — and that combination would be a
clean KEEP that beats everything measured, on both axes at once.

If hood stays near 31 while the rating holds, the trade is REAL: this economy
buys great-power growth with small-country survival, and the guard's rejection
in journal 14 stands on its merits rather than on a technicality.

**Third possibility, and the one to watch for:** the rating falls back toward
162 as the head trains further. That would mean journal 14's 206 was itself the
mid-training artifact — the mirror image of journal 06, where the endpoint was
worse than the middle. Any of the three is informative; only the first is a
KEEP.

### 15 side finding — the worst seat is decided by the SCRIPTED OPENING, not the policy

Chasing the 300 target, which the arithmetic says lives entirely in the two
survival seats. `1939:NOR hood` is worth 45 rating points on its own and has
been the worst seat in every model measured (7, 62, 49, 31).

**What happens to Norway:**

    [WAR] NOR declares war on MCK        <- Norway STARTS a war
    [WAR] SWE declares war on NOR        <- then the rusher blitzes it
    [WAR] NOR vs R128 (rebellion war)    <- and it has a rebellion

A small country about to be attacked by its largest neighbour opens an
offensive war of its own.

**It is not the policy's decision.** Biasing `declare war` (index 4, a live
action at 33.3%) to -3, -10 and -30 gave **bit-identical** results on all three
seeds. Per journal 12's lesson I checked against a known-live control rather
than concluding the mechanism was broken — and the answer is that the decision
is made in the OPENING BOOK: `AI_OPENING_TURNS = 20` of a 120-turn seat, played
from the script by every country including the seat.

**Measured with a diagnostic override** (`OD_OPENING_TURNS`, default unchanged):

    opening book   hood per seed      score
    20 turns       0.4  0.6  0.2         31
    0 turns        0.8  0.5  0.5         46

Two seeds of three improve; the mean goes 0.40 -> 0.60. **The book is costing
the worst seat about 15 points.**

**This is not a licence to turn the book off.** The note at `AI_OPENING_TURNS`
is right that training and play must see the same opening, and disabling it at
eval alone creates exactly the divergence it exists to prevent. What the
measurement says is narrower and more useful: **the book's opening is bad for a
small country with a hostile larger neighbour**, and that is a fixable book
rule, not an AI-training problem. No further policy work on this seat can reach
it.

### ...and the standing human comparison

`od_bench.py` was built so a person can play the same seats. One human score is
stored — `1914:FRA rush`, seed 20260801, from a prior session:

    human (prior Claude session)      9.2
    loop-base (the shipped model)     1.8
    i06-ext / i10+stage / i14      10.9 / 11.1 / 10.8

So the SHIPPED model loses heavily to that human and this session's models beat
it by roughly 20%. One seat and one seed, and a Claude session is not a proxy
for a skilled player — but it is the only human number on the board, and the
direction of travel is unambiguous.

## 15 RESULT — REJECT. Journal 14's 206 was a mid-training artifact.

4,217 further turns on the fresh econ head, fresh worlds, everything else equal:

                  i11-b5   i14    i15
    RATING           162    206    101
    survival          91     88     66
    worst seat        49     31     28

**The third branch fired** — the one recorded before the run precisely so it
could not be explained away afterwards: "the rating falls back toward 162 as the
head trains further; that would mean journal 14's 206 was itself the
mid-training artifact." It did, and further: 101, below the 108 baseline.

**So 206 is retired.** It was an unconverged checkpoint of a freshly reset head,
in the same family of error as journal 05's endpoint being worse than its own
middle — measured in the opposite direction here. A reset head passes through
states that bench well and do not survive more training.

**The econ reset is a dead end**, three ways now: it does not revive the dead
actions (journal 14), the collapse was never what held the rating back, and its
best-looking checkpoint does not hold up. `--reset-ai-head econ` is answered.

**Standing best returns to `model.i10-final.bin` + stage bias: 162 / 91 / 49.**

---

## 16 — script declare-from-strength — REJECT the fix; report the bug

**A real bug, found chasing the Norway seat.** `scriptedChoice`, MOD_WAR:

    if (st.army > std::max(1LL, st.enemyAdjArmy) * 2) return pick({4,1,0});

`enemyAdjArmy` is hostile troops standing ON OUR BORDERS, so it is ZERO at
peace. `max(1,0)*2` is 2, and "declare from strength" reads **"do I have more
than two soldiers"**. Every peaceful country with an army declares war on
somebody. It is why `1939:NOR hood` opens with Norway attacking a third party in
the book turns before Sweden blitzes it.

**Fixed behind `OD_SCRIPT_DECLARE_FIX`** — compare against the army of the
country `findWarTarget` would actually attack — because this code is the
BENCHMARK'S RULER and changing it silently voids every stored rating.

    seat              shipped   fixed
    1914:FRA rung         264     222
    1914:SWE rung         233     130
    1939:USA rung          96     155
    modern:CHN rung       173     220
    1914:FRA rush         160     160
    1939:NOR hood          49      38
    RATING                162     155
    survival               91      90

**Verdict: REJECT.** Rating -7, worst seat -11. And the hypothesis that started
it is WRONG: hood did not improve, it fell, and only on one seed (0.6 -> 0.2,
the other two identical) so even that is noise.

**Why the bug is load-bearing.** Removing peacetime declarations makes the rung
tamer, and the rung is what every seat is scored against. Two seats got easier
(USA, CHN), two got harder (FRA, SWE). It is not a uniform improvement in either
direction — it is a DIFFERENT benchmark.

**But it is still a bug, and it is the user's call.** A peaceful country
declaring war on a random neighbour is behaviour a PLAYER notices as broken,
whatever the benchmark prefers. Fixing it means renaming the benchmark and
re-measuring everything, which is a cost worth paying for game quality and not
for a rating.

### ...and the loop stopped serialising

I had been running one training at a time since journal 06, on the grounds that
workers blend with any peer file they find. That was an untested assumption
about the FIX rather than the problem: `--data <dir>` moves the whole model
namespace, so separate data directories are fully isolated.

    build/loop/data{A,B,C}/   symlinks to the real data/, with their own ai/
    both runs report:         "synced with 0/1 peer(s)"
    total RSS:                well under the 1.4 GB/instance measured

Three or four experiments can now run at once. The serialisation cost most of
the session's wall-clock.

---

## 17-19 — items 1-4 from the long road, built while four runs trained

**1. Exploration — the collapse guard did not cover diplomacy.** There is an
adaptive entropy controller (`m_entropyCoef[m]`, raised when a head's marginal
entropy falls below `ENTROPY_FLOOR_FRAC` of its ceiling). It watched the four
MODULE heads. The diplomacy head — measured at H = 0.000 on every request kind,
in every model — was never in it. Arrays widened to `MOD_COUNT + 1`, marginals
recorded in the ANSWER space so the ceiling is ln2 rather than ln14, and the
base coefficient is now per-head (`entropyFor`) so the war head's hard-won 0.01
does not move.

**3. League exploiters — the rusher is now a league MEMBER.** A virtual PFSP
slot carrying the same win/loss bookkeeping as a real checkpoint, so the pool
plays it more often exactly while the policy is losing to it. No file to load,
so a young run trains against a rusher from turn one. This replaces screening
(the merge guard rejects a bad run after paying for it) with prevention.

**4. SEARCH — rebuilt as a policy improvement operator.** `searchScores` is a
beam search whose leaf evaluation is `max Q(emb)`, i.e. it re-derives its own
ranking; that is why depth 2 cost 8.5x for nothing, and the verdict was right
about THAT search. The replacement is latent MCTS: PUCT over `m_dynamics`
rollouts, the VISIT DISTRIBUTION as output, Dirichlet root noise in training
only, and — the point — the policy trained toward the visits by masked soft
cross-entropy (`accumulateCrossEntropyTargetInto`, added to NeuralNet).

Root noise matters more here than in chess: journal 13 mapped ten actions at or
below 1e-14, and PUCT's exploration term is proportional to the prior, so it
cannot rescue them. Noise at the root is the only mechanism in this codebase
that can hand a 1e-33 action a visit.

Inference-only on a frozen model: rating 162 -> 149, survival 91 -> 92, worst
seat 49 -> 56. More robust, less expansionist — the expected profile for search
over a cautious value function that the policy was never trained against. The
test that matters is a run trained WITH it.

**2. Outcome objective — IT ALREADY EXISTS AND IS SWITCHED OFF.**

    static constexpr float VALUE_MC_WEIGHT = 0.25f;   // documented
    float AISystem::s_valueMcWeight = [] {
        if (const char* e = std::getenv("OD_VALUE_MC")) { ... }
        return 0.0f;                                   // ACTUAL default
    }();

`noteVictory` and `noteMapEnd` already settle open windows with a terminal (+4
for a win, +/-2 for final share) and `m_outcomeBuf` already holds states
awaiting their map's result. The blend that would train the value head toward
that outcome is **off unless `OD_VALUE_MC` is set** — so the value function is
trained entirely on its own bootstrap of a hand-shaped reward, which is exactly
the ceiling the long-road note describes.

The cheapest item on the list, and it needed no new code at all. Found by
reading the default rather than the constant beside it.

---

## 18/19 — MORE TRAINING RELIABLY DESTROYS THE MODEL

Four independent continuations from `model.i10-final.bin` (162):

    run   what it was                      RATING  survival  worst
    --    starting point (i10 + stage)        162        91     49
    A     more training, seed 20260910        109        74     38
    B     more training, seed 20260911         29        27      0
    D     trained WITH latent MCTS             73        70     21
    i15   more training on i14 (journal 15)   101        66     28

Three seeds, four runs, every one worse. Run B lost a seat entirely (worst seat
0). This is not the gentle drift `selfplay-erodes-rush-defence` describes — it
is collapse, and it is the reason every long run tonight went backwards.

**The structural cause, and it explains all of it.** Training is SELF-PLAY
against past selves. The bench measures play against a FROZEN SCRIPTED RUNG.
Those are different objectives. Optimising to beat yourself does not improve
play against an opponent that never adapts, and can destroy it: the policy
settles into a self-play equilibrium the rung is not party to and punishes.

The August note saw the symptom — a lineage beating its own predecessor at every
merge while losing seven points to a rusher — and treated it with a screen
(`_vs_exploit`, `BLITZ_REGRESSION_MARGIN`), which throws away the run after
paying for it. The measurement here says the screen is not enough: a
five-hour-equivalent continuation lands at 29.

**So the loop's real obstacle is not the reward, the heads, or the search. It is
that the training objective and the evaluation objective are different games.**

That makes journal 20 (run E: `OD_VALUE_MC=0.25` + `OD_LEAGUE_EXPLOIT=1`) the
critical experiment rather than an incremental one — it is the only run so far
with a SCRIPTED opponent inside training. The exploiter has been drawn twice in
that run, confirmed in its log.

**And it retires a plan.** "Train the current best for longer" is not a route to
300; it is a route to 29. Any further training must be measured against the rung
DURING the run, not after it.

---

## 20 — the entropy guard on the diplomacy head — the strongest lead of the session

Run C is `model.i10-final.bin` trained 4,249 turns with ONE change from runs A
and B: the adaptive entropy collapse guard now covers the diplomacy head
(journal 17). Same starting model, same recipe, comparable turn counts.

    run   guard on diplo?   RATING   survival   worst
    A            no            109         74      38
    B            no             29         27       0
    C           YES            239         89      36

**It is the only continuation tonight that did not collapse**, and it produced
the highest rating measured. Seat by seat against the standing best:

    seat              i11-b5   i14   runC
    1914:FRA rung        265   246    260
    1914:SWE rung        233   283    317
    1939:USA rung         96   215    283
    modern:CHN rung      171   317    388
    1914:FRA rush        160   144    150
    1939:NOR hood         49    31     36
    RATING               162   206    239
    survival              91    88     89

**Verdict by the guard: REJECT** — survival 91 -> 89, rush 160 -> 150, hood
49 -> 36. The same trade as journal 14: growth seats up hugely, survival seats
down. Recording the verdict the rule gives, as before.

**But the anti-collapse result is separate and matters more than the rating.**
Journal 18/19 established that continued training reliably destroys the model
(109, 29, 73, 101 across four runs). C is the first continuation that improved,
and the difference is an exploration fix. That is exactly what the long-road
note predicted: a saturated policy cannot be improved, only moved between
corners, and every reward change this session behaved that way.

**CONFOUND: A, B and C used different seeds** (20260910/911/912). 109 and 29
against 239 is a large gap, but seed is not controlled. Launched the direct
test — the guard at **run A's exact seed 20260910** — which is the one arm that
isolates the guard from the seed. If it lands near 239, the guard is the cause;
near 109, C was weather.

Recording that BEFORE the result, because 239 is exactly the kind of number I
would otherwise be tempted to keep on faith.

## 20 RESULT — value-MC + league exploiter — REJECT (102)

4,917 turns, 8 maps, `OD_VALUE_MC=0.25` and `OD_LEAGUE_EXPLOIT=1`. Both
confirmed live: the value-blend line printed at startup and the RUSHER was drawn
as the league opponent on **7 of 8 maps**.

    seat              i11-b5   runC   runE
    1914:FRA rung        265    260     79
    1914:SWE rung        233    317    160
    1939:USA rung         96    283    121
    modern:CHN rung      171    388    128
    1914:FRA rush        160    150     81
    1939:NOR hood         49     36     38
    RATING               162    239    102
    survival              91     89     83

**REJECT.** Worse than the starting point on every seat but the hood.

**And it does NOT refute the objective-mismatch diagnosis — it complicates it.**
The prediction in journal 18/19 was that a scripted opponent inside training
would prevent the collapse. The exploiter was drawn on 7 maps of 8 and the run
still landed at 102, so training against a rusher is not sufficient on its own.

Two candidate readings, and I cannot separate them from one run:
  * **Too much of it.** PFSP drew the rusher 7 times in 8 because its loss rate
    stays high — the policy never gets good at it, so the weighting keeps
    feeding it. A league that is 88% rusher is not a league, it is a different
    single opponent, and the policy specialises against a blitz at the cost of
    everything the rung does.
  * **Confounded with value-MC.** Both changes went in together, against my own
    one-hypothesis-per-iteration rule, because they were asked for together.
    Either could be responsible.

The fix for the first is a CAP on the exploiter's share of the draw (it should
be one opponent among several, not the default); the fix for the second is to
run them separately. Both go on the backlog.

**What survives from this run:** the exploiter mechanism works and is
observable, and `OD_VALUE_MC` is confirmed to reach the trainer. Neither had
been true before tonight.

## 21 — the guard, seed-controlled — the 239 was WEATHER

Run F: the entropy guard at **run A's exact seed 20260910**, the one arm that
isolates the guard from the seed.

    at seed 20260910          RATING  survival  worst
    starting point (i10+stage)   162        91     49
    A   no guard                 109        74     38
    F   GUARD                    118        88     72
    C   guard, seed ...912       239        89     36

**Journal 20's 239 was the seed.** The prediction recorded before this run was
"near 239 means the guard is the cause; near 109 means C was weather". It landed
at 118. Calling it weather and retiring the 239 as a headline.

This is the third single-arm result this session that did not survive its
control — journal 03 (single-seed), journal 14/15 (mid-training artifact), and
now this. The rule that keeps catching them is the same one: **record the
falsifying observation before the run, then honour it.**

**What the guard IS worth, measured properly:** +9 rating, **+14 survival, +34
on the worst seat**. That is the largest robustness gain anything produced
tonight, and it is on precisely the axis the 300 target needs — the rating is
held down by the two survival seats, not by growth.

**And it does not stop the decline.** 162 -> 118 even with the guard. Continued
training still costs rating; the guard converts a COLLAPSE (109, and run B's 29)
into a managed loss with much better robustness. The objective mismatch of
journal 18/19 stands as the unsolved problem.

**Standing best is unchanged: `model.i10-final.bin` + stage bias, 162/91/49.**
Nothing trained tonight has beaten it at a controlled seed.

## 22/23 — value-MC and exploiter, isolated (H VOID — the exploiter never ran)

Run H was `OD_LEAGUE_EXPLOIT=0.25` alone. It scored 85, and the score means
nothing: **the rusher was drawn 0 times in 6 maps.**

    if (present.empty()) return false;    // <- bailed here
    ...
    const int EXPLOIT_SLOT = ...;         // <- exploiter added here, too late

A fresh model directory has no `league-*.bin`, so `present` is empty on the
early maps and the function returned before the exploiter could join. By the
time a checkpoint appeared, two maps were left and the 25% cap lost both draws.

**The comment above that code claimed the opposite** — "always available: unlike
a checkpoint it needs no file, so a young run that has never checkpointed still
trains against a rusher from turn one". The intent was right and the code did
not implement it. Moved the push above the early return; run H2 draws the rusher
on map 1 of 1.

**How it was caught, and the general rule:** the draw count was logged and
checked. Without that, 85 would have been filed as "the capped exploiter hurts"
from a run in which the exploiter never appeared — a confident wrong conclusion
from a real measurement of the wrong thing. **Log the count of the thing you are
testing, not just its effect.** Journal 08 learned the same lesson for `net
asked`; this is the same error in a different place.

H is discarded rather than reported. H2 relaunched with the fix.

## 22 RESULT — value-MC alone: 136, the best continuation measured

`OD_VALUE_MC=0.25` alone, 4,102 turns from `model.i10-final.bin` (162).

    run   change                          RATING  survival  worst
    --    starting point                     162        91     49
    A     nothing (control)                  109        74     38
    B     nothing (control)                   29        27      0
    G     value-MC ALONE                     136        78     38
    F     entropy guard alone                118        88     72
    E     value-MC + UNCAPPED exploiter      102        83     38

**Two separable mitigations, and they protect different things:**

    value-MC     best RATING of any continuation      136 (vs controls 109, 29)
    entropy guard best ROBUSTNESS                     worst seat 72 (vs 38)

Neither reaches 162 — continued training still costs rating — but both convert
the collapse into a managed loss, and **they have never been run together.**
That combination is the obvious next experiment and it is cheap.

**And journal 20's confound is now resolved.** E (value-MC + uncapped exploiter)
scored 102 while G (value-MC alone) scored 136, so the uncapped exploiter was
the harmful half, not the value blend. That is what the cap exists for.

**Watch on H2:** the rusher has been drawn on 1 of the first 2 maps against a
25% cap. Small sample, but if it keeps running hot the cap is not binding and
the run repeats E's error at a different ratio. Check the final ratio before
reading its score.

### Search at PLAY time is too slow to bench — a practical limit on item 4

The `i19-srch` arm (search-trained model, 24 sims per decision at play) was
killed after **95 minutes** without finishing a rating that normally takes 3.
That is roughly a 30x think-time cost, and it settles a practical question the
design note left open:

  * **Training** with search is affordable — run D trained 4,960 turns in 29 min
    with `OD_MCTS_SIMS=24`, because the search runs on one country's decision
    while the rest of the turn is unchanged.
  * **Playing** with search at the same budget is not, at least not through the
    bench harness, which plays 18 seat-runs of 120 turns with every country
    thinking.

This matters for how the AlphaZero loop should be used here: train WITH search,
then ship the distilled policy WITHOUT it. That is the cheap half of the idea
and it is the half that survives — the policy keeps what the search taught it
via the visit-count targets, and costs nothing extra at play time.

A shipped search would need a far smaller budget (4-8 sims) or to run only for
the handful of decisions that matter. Neither is measured yet.

## 23/24 — capped exploiter: 166 headline, REJECT on composition

H2, `OD_LEAGUE_EXPLOIT=0.25` alone, 4,659 turns. The rusher was drawn **1 of 6
maps (17%)** — the cap binds, unlike journal 20's 7 of 8.

**Same-seed comparison (all three at 20260914), which is the useful part:**

    E   value-MC + UNCAPPED exploiter    102
    G   value-MC alone                   136
    H2  CAPPED exploiter alone           166

The cap is the difference between the exploiter being the harmful half of
journal 20 and being the best single change at this seed.

**But 166 is a REJECT, and the composition says why:**

    seat              start    H2    delta
    1914:FRA rung       265   283      +18
    1914:SWE rung       233   120     -113
    1939:USA rung        96   207     +111
    modern:CHN rung     171   213      +43
    1914:FRA rush       160   134      -26   <- guard
    1939:NOR hood        49    41       -8   <- guard
    RATING              162   166       +4
    survival             91    90       -1

+4 on the mean is a wash of two large opposite moves (Sweden -113, USA +111),
and BOTH survival seats fall. That is the trade the 300 target cannot afford:
the arithmetic says every remaining point lives in `rush` and `hood`, and this
run gives up 26 and 8 of them to buy growth that is already sufficient.

**The survival column did its job.** On the rating alone this reads as the first
continuation to beat 162 and would have been kept.

**Caveat on the test itself:** one drawn map out of six is thin evidence about
the exploiter. What it does establish is that the CAP works and that uncapped
was the problem.

---

## 25 — the human ladder: --bench-agent headless, and the bug that made it lie

**Ported `--bench-agent` to `OpenDoctrinesServer`.** It was in main.cpp only, so
the one FAIR human-vs-AI instrument this project has — the agent gets the same
action menu the policy gets and the same executor runs the choice, so a
difference in result is a difference in JUDGEMENT — required a window and an
OpenGL context. Same argument as `--bench-seat`, moved for the same reason.
`srvResolveDataDir` made public; `WindowShouldClose` is already stubbed in the
server, so the loop needed no other change. Runs at 0 windows, ~520 MB.

**AND IT WAS SEATING THE WRONG COUNTRY.**

    m_forcedStartIso = iso;                    // startBenchSeat: "NOR"
    ...
    startNewGameWithName(...)                  // clears it on line one

`startNewGameWithName` clears `m_forcedStartIso` deliberately — it is "the one
door every new world comes through" and a stale forced country would leak out of
a tutorial. The seat was assigned BEFORE that door and wiped by it, so
`--bench-agent 1939:NOR:hood` seated **France**, played France for 120 turns,
and would have reported the score under Norway's name.

**Why nobody noticed:** the only seat ever played by hand is `1914:FRA:rush`,
and FRA is 1914's default. The bug and the intent agreed by luck, so the one
human score on record is valid — and every future one on any other seat would
have been silently wrong.

Fixed by re-setting the seat after the door. Verified: `Player selected:
Norway`, land 17/1194 = 1.42%, against the seat's par of 1.3%.

**This is the third instrument bug this session** (journal 08's `asked` vs `net
asked`, journal 23's exploiter draw count, and now this). All three had the same
shape: a measurement that looked right and was measuring something else. The
defence that keeps working is to check the thing you are testing ACTUALLY
HAPPENED — the country's name in the log, the draw count, the denominator —
rather than only its effect.

## 25b — the mitigations do NOT stack

Run I: value-MC + entropy guard, at run A's seed 20260910, so all three arms are
directly comparable.

    at seed 20260910          RATING  survival  worst
    A   nothing                  109        74     38
    F   entropy guard ALONE      118        88     72
    I   guard + value-MC         117        80     47

**REJECT.** Combining them gains nothing on rating (117 vs 118) and gives back
most of the guard's robustness — survival 88 -> 80, worst seat 72 -> 47.

The assumption going in was that two mitigations protecting different things
(value-MC the rating, the guard the robustness) would compose. They do not, at
least at this seed: the guard alone dominates the combination on every axis.

**Working hypothesis for why, untested:** both act on the same failure. The
guard raises exploration when a head collapses; value-MC changes what the value
head is trained toward, which changes the advantages the policy sees, which
changes when the guard fires. They are not independent knobs on independent
problems — they are two interventions on one feedback loop, and running both
detunes the controller.

**Standing best is unchanged at 162.** Of everything measured tonight, the
entropy guard alone is the only intervention that improves the axis the 300
target actually needs (worst seat 38 -> 72), and it still does not stop the
overall decline from 162.

---

## 26 — a training LENGTH curve — IN PROGRESS

Every continuation tonight ran ~4,000 turns and every one degraded (109, 29, 73,
101, 117, 118, 136, 166). Nobody has asked whether a SHORTER run improves before
it hurts — the question the collapse makes obvious in hindsight and which no
single-endpoint experiment can answer.

Four runs from `model.i10-final.bin` (162), same seed 20260910, differing ONLY
in length: 1, 2, 3 and 5 maps. The seed is run A's, which at 6 maps / 3,948
turns scored 109, so the curve has both ends already.

**What each outcome would mean:**
  * **monotone decline from 162** — training from this model is simply harmful
    and the only use for the trainer is producing NEW lineages, not improving
    this one.
  * **a peak at short length** — there is an optimal amount and the loop has
    been overshooting it all night; every rejected result should be re-read as
    "trained too long" rather than "the change was wrong".
  * **noise with no shape** — the variance dominates the signal at this scale
    and the honest conclusion is that single runs cannot rank changes at all,
    which would invalidate a lot of tonight's A/Bs.

The third is the one that would hurt, and it is the one to look at hardest:
runs at the same seed and same recipe already span 109 to 166 depending only on
what else was switched on, which is a wide band for a 4-point curve to sit in.

## 26 RESULT — training PEAKS at ~1,000 turns, and the loop overshot it 4x all night

All from `model.i10-final.bin` (162), seed 20260910, differing only in length:

    turns          RATING  survival  worst
    0  (start)        162        91     49
    1,008            184        88     26
    1,839            147        89     54
    3,948 (run A)    109        74     38

**A peak, and an early one.** 1,008 turns beats the starting model by 22 points;
by 4,000 turns the same recipe has lost 53. The shape is monotone after the
peak.

**Every experiment tonight ran ~4,000 turns.** Journals 18-25 — value-MC, the
entropy guard, the exploiter capped and uncapped, the search run, the econ
retrain — were all measured roughly 4x past the peak. Each was therefore
measuring "the change PLUS heavy overtraining", with the overtraining the larger
term. That does not make the verdicts wrong, but it does mean they were taken in
a regime where the trainer was destroying more than any change was contributing,
and the differences between them were being read through that.

**The peak still fails the robustness guard**: worst seat 49 -> 26 at 1,008
turns, so 184 is not adoptable as it stands. Survival is level (91 -> 88).

**What this changes about the method, and it is the important part.** The loop's
default recipe (`6 3000`) was inherited from journal 05 and never questioned —
it was chosen when the question was "does this reward change do anything", where
more turns seemed safer. It is the wrong default for comparing changes on a
model that is already good. **Re-run the promising arms at ~1,000 turns before
concluding anything about them**: value-MC (136 at 4,102), the entropy guard
(118 at 3,892) and the capped exploiter (166 at 4,659) were all measured deep in
the declining region.

## 27 — the same arms at the RIGHT length, and the ranking REVERSES

All at ~1,000 turns (1 map), seed 20260910, from `model.i10-final.bin` (162):

    arm                    RATING  survival  worst
    control (no change)       184        88     26
    capped exploiter          204        89     36
    value-MC + exploiter      163        87     46
    value-MC                  102        77     33
    search (24 sims)          100        83     28

**Compare with the SAME arms at ~4,000 turns (journals 18-25):**

    arm                  @4000   @1000
    control                109     184
    value-MC               136     102
    capped exploiter       166     204
    search                  73     100

**value-MC ranked BEST at 4,000 and WORST at 1,000.** The ordering of changes
reverses with training length. So journals 18-25 were not merely noisy — they
measured a different regime, and any conclusion drawn there about which change
is better does not transfer.

**Rexp (capped exploiter, 204) is still a REJECT**, seat by seat against the
standing best:

    1939:USA rung   +170      1914:FRA rung    -7
    1914:SWE rung    +67      1914:FRA rush   -25   <- guard
    modern:CHN rung  +60      1939:NOR hood   -13   <- guard

### THE STRUCTURAL FINDING OF THE SESSION

Every high-rating result tonight has the identical shape:

    model            RATING   rush   hood   what it did
    i14 (econ)          206    144     31   growth up, survival down
    C   (guard)         239    150     36   growth up, survival down
    H2  (exploiter)     166    134     41   growth up, survival down
    Rexp (@1000)        204    135     36   growth up, survival down
    best (standing)     162    160     49   --

**Not one intervention has raised the rating and the survival seats together.**
Rating and survival are in TENSION for this AI, and the 300 target lives
entirely on the survival side: three seats already clear 300, and the mean is
held down by `1914:FRA rush` and `1939:NOR hood`.

So the way to 300 is not another knob that buys growth. It is whatever breaks
the trade — and the two things measured tonight that bear on it are both OUTSIDE
the network: the scripted opening costs the hood seat ~15 points (journal 15),
and the reward pays 2.4 x log(provinces) for growth with no comparable term for
surviving intact.

## 28 — loss aversion on ground — REJECT, hypothesis refuted

Weighting the NEGATIVE half of `PHI_PROV * (log1p(now) - log1p(then))`, at
~1,000 turns, seed 20260910:

    arm         RATING  survival  worst   rush   hood
    control        184        88     26      -      -
    LA = 1.5       143        89     38     97     38
    LA = 2         133        83     38     60     38
    LA = 3         115        85     28    104     31
    standing best  162        91     49    160     49

**REJECT at every setting.** It costs rating (184 -> 143/133/115) AND fails to
buy what it was for: `1914:FRA rush` lands at 60-104 against the standing best's
160, `1939:NOR hood` at 31-38 against 49.

**The hypothesis is refuted.** The growth-versus-survival tension of journal 27
is not caused by the land term being symmetric — making it asymmetric does not
break the trade, it just makes the model worse at both halves.

**Where that leaves the trade.** Four changes raise the rating by selling the
survival seats (journal 27); one change that explicitly prices losing ground
higher improves neither. So the tension is not a simple matter of reward
weights, and the two things measured tonight that DO bear on the survival seats
are both outside the network:

  * the scripted opening costs `1939:NOR hood` about 15 points (journal 15) —
    Norway declares war on a third party in the book turns before it is blitzed;
  * `1914:FRA rush` is a world where every country attacks without pause, and
    the AI's rating there tracks how long it survives a fight it did not choose.

Neither is reachable by reward shaping, and the first is a game rule.

## 29 — training length, the short end: a DIP before the peak

    turns     RATING  survival  worst   rush   hood
    0            162        91     49    160     49
    300           81        72     33     58     31
    500          105        82     33    100     31
    700          117        88     36    118     38
    1,008        184        88     26    135     36
    1,839        147        89     54      -      -
    3,948        109        74     38      -      -

Not a peak from the start — a **dip to 81 at 300 turns**, recovery to 184 at
~1,000, then decline. The first few hundred turns actively damage the model
before anything is learned. Candidates: the opening book (first 20 turns of
every map are behaviourally cloned from the script, which the model may have
diverged from), or simple fresh-map shock on a policy tuned to the seats.

**And the survival seats are below the standing best at EVERY length.** rush
peaks at 135 against 160; hood at 38 against 49. There is no training length
from `model.i10-final.bin` that improves them. **Training length is closed as a
lever for the 300 target.**

## 29b — "how good is the teacher?" — measurement broken, 0 on every seat

`OD_SEAT_SCRIPTED=1` (the seat plays the hand-written rung) scored **0.0 on all
six seats**, USA included. That is not a result; the rung cannot be annihilated
everywhere. The `[BENCH] seat` score must read the seat's held share from the
MODEL cohort, and marking the seat scripted moved it into the control cohort
where that read finds nothing. Diagnosing.

## 29b RESULT — the hand-written teacher, scored on its own scale

Fixed the `[BENCH]` score to read the SEAT's own share rather than the model
cohort's (the cohort is empty when the seat plays the script). Then
`OD_SEAT_SCRIPTED=1`:

    seat              rung    best model
    1914:FRA rung      207         265
    1914:SWE rung       90         233
    1939:USA rung      121          96
    modern:CHN rung     76         171
    1914:FRA rush      172         160   <- the rung is BETTER here
    1939:NOR hood       23          49
    RATING             115         162
    survival            82          91

**A mixed teacher.** The script beats the model on `1914:FRA rush` — a world
where everyone attacks — and loses on Norway-under-attack and overall. Cloning
it wholesale would cost 47 points; cloning it at LOW weight might transfer the
rush skill without the rest, which is what `OD_BC_FROM_SCRIPT` was built for
and what its own note predicts ("the script is not better at everything").

Launched BC at 0.3 and 1.0, ~1,000 turns, seed 20260910. Control at this length
is 184 / rush 135 / hood 36. **The thing to read is rush**: if BC lifts it toward
the teacher's 172 while the rest holds, cloning is a lever on the survival side
— the first one measured tonight that is not a growth-for-survival trade.

## 30 — behavioural cloning from the script — REJECT, hypothesis refuted

`OD_BC_FROM_SCRIPT` at 0.3 and 1.0, ~1,000 turns, seed 20260910:

    arm             RATING  survival  worst   rush   hood
    control            184        88     26    135     36
    the teacher        115        82     26    172     23
    BC = 0.3            79        72     26     94     23
    BC = 1.0            88        79     21    115     23
    standing best      162        91     49    160     49

**REJECT at both weights**, and the refined hypothesis is refuted as well. The
bet was that low-weight cloning would transfer the teacher's rush skill (172)
without its weaknesses. The opposite happened: rush FELL from the control's 135
to 94 and 115, while hood dropped to exactly the teacher's 23. **It inherited
the weakness and none of the strength.**

**Why, most likely:** `OD_BC_FROM_SCRIPT` clones EVERY module. The script never
builds a navy, never stages, and is worse at the economy — and the rush seat
needs all of those. Pulling the whole policy toward the script imports the
teacher's blind spots into the seats where the model was already better. The
one thing the teacher does better (fighting a world at war) is a WAR-head
behaviour, and it is drowned by four modules being dragged toward a worse
player.

If there is anything left in cloning it is PER-MODULE cloning — war head only.
That is a small change and a direct test; whole-policy cloning is closed.

## 31 — war-only cloning — REJECT; cloning is closed in every form

`OD_BC_FROM_SCRIPT=1.0 OD_BC_MODULES=war`, verified live in the log
(`restricted to module mask 0x4`), ~1,000 turns, seed 20260910:

    arm             RATING  survival  worst   rush   hood
    control            184        88     26    135     36
    BC all modules      88        79     21    115     23
    BC war only        110        77     21     93     23
    standing best      162        91     49    160     49

Rush fell to 93 — below the control AND below whole-policy cloning. Restricting
the pull to the one head where the teacher is better made the seat WORSE. So
the teacher's rush advantage is not a war-head behaviour that transfers; it is
an emergent property of the whole script playing together, and it cannot be
cloned in pieces.

**Cloning is closed**, whole and per-module.

### THE MAP IS COMPLETE — every network-side lever is measured

    lever                          verdict          journal
    reward: gain term              KEPT             05
    reward: pact weight 2.5        KEPT             10
    reward: loss aversion          REJECT           28
    head: per-kind diplomacy       KEPT             06
    head: capacity via trunk       REJECT           07, 08
    exploration guard              KEPT (robust.)   17, 21
    value-MC                       REJECT @1000     22, 27
    league exploiter (capped)      REJECT (trade)   23, 27
    search (train / play)          REJECT / too slow 19, 27
    econ head retrain              artifact         14, 15
    training length                peak ~1000       26, 29
    staging bias                   KEPT             11
    cloning, whole / war-only      REJECT           30, 31

**Standing best: 162 / survival 91**, from a baseline of 108 / 68.

**Where the remaining points are, and why the loop cannot reach them:** the 300
target lives in `1914:FRA rush` and `1939:NOR hood`. Four different changes
raised the rating by selling those two seats; one change priced losing them
higher and helped neither; the only thing that beats the model on either seat
is the hand-written script, and its advantage does not transfer by cloning.
The two measured levers that DO bear on those seats are game rules — the
scripted opening (costs hood ~15) and pacts having no teeth — and both are the
user's decision, not the trainer's.

Further training experiments from here have low expected value. The loop
continues, but the next real gain needs one of those two decisions.

## 32 — THE STANDING BEST DOES NOT REPRODUCE UNDER THE CURRENT BINARY

Re-benched `model.i10-final.bin` + stage bias, the model every number in this
journal since journal 11 is relative to, under the binary as it stands now:

    seat              i11-b5 (journal 11)   now
    1914:FRA rung          17.7             17.6
    1914:SWE rung           2.3              2.2
    1939:USA rung           5.4              5.2
    modern:CHN rung         4.3              4.2
    1914:FRA rush          10.7             10.6
    1939:NOR hood           0.6              0.5
    RATING                  162              157
    survival                 91               88
    worst seat               49               38

**Every seat moved by ~0.1, in the same direction.** The bench is deterministic
(same seed, same map, same model), so this is not noise. One of the changes
made since journal 11 — all of which were meant to be inert at their defaults —
is not. A uniform small shift across every seat is the signature of an extra
RNG draw in the decision path, not of a rule change.

**Consequence, stated plainly:** until the cause is found and removed, the
standing best under the binary the user would build is **157 / 88 / 38**, not
162 / 91 / 49. The journal's later comparisons (27-31) were made against the
new binary consistently, so they stand relative to each other — but the
headline is 157 until proven otherwise.

Launched a split: `loop-base` read 108 seat-for-seat after the fan-out change
(journal 06). If it still does, the drift is confined to the 14-output decision
path; if not, it is in a later change and affects every model. Bisecting from
there.

This is the reason the protocol re-baselines. It was skipped for changes that
"only added counters", and one of them did not.

## 32b — first split: it hits EVERY model, and it points down on every seat

`loop-base` under the current binary: **101**, against 108 in journal 06.

    seat              journal 06 per seed     now
    1914:FRA rung     6.6  5.3  6.2           6.4  5.2  6.0
    1914:SWE rung     0.2  2.4  3.7           0.0  2.2  3.5
    1939:USA rung     8.5 16.9 13.4           8.3 16.6 13.1
    modern:CHN rung   0.3  0.0  0.2           0.2  0.0  0.1
    1914:FRA rush     1.8  1.0  9.0           1.6  0.8  8.9
    1939:NOR hood     0.7  0.8  0.6           0.5  0.6  0.5

**All 18 seat-seed values moved DOWN by 0.1-0.3. None moved up.** An RNG desync
scatters results in both directions; a one-directional shift of roughly
constant size is a SCORING change, not a game change.

**Hypothesis, recorded before the diagnostic runs:** the `[BENCH]` score fix in
journal 29b. It now reports the seat's OWN share (`seatProv / tot`) where it
used to report the MODEL COHORT's share (`trainedProv / tot`). The two differ
whenever a country exists at map end that was not in `randomCids` at map start
— a state formed mid-game, a rebellion that became a country. The OLD score
credited those provinces to the seat. If this is right:

  * the "drift" is the bench becoming MORE accurate, not a bug;
  * every rating stored before journal 29b was inflated by a few points;
  * they remain comparable with EACH OTHER, and the absolute scale is ~5 lower.

A one-seat diagnostic printing both shares is running. If `model-cohort share`
exceeds `seat's own` by the observed gap, this is confirmed.

## 32c — CONFIRMED: the "drift" was the bench becoming accurate

One seat, both shares printed (`1914:FRA`, seed 20260801):

    model-cohort share   6.60   (journal 06 read 6.6 -- matches to the digit)
    seat's own share     6.44   (43 vs 42 provinces)
    the difference       1 province, belonging to a country outside both
                         cohorts at map start

The old `[BENCH]` score credited to the seat every province held by a country
that did not exist when `randomCids` was built — a state formed mid-game, a
rebellion that became one. The fix in journal 29b reports the seat's own share.
**The new number is the correct one.**

**What this means for every number in this journal:**

  * Ratings stored BEFORE journal 29b are inflated by ~1 province per seat,
    roughly 5 points on the mean. They remain comparable with EACH OTHER —
    every A/B in journals 01-28 compared inflated to inflated — and every
    verdict stands.
  * The corrected absolute figures, both measured under the fixed score:
        baseline (loop-base)        108 / 68 / 7   ->   101 / 65 /  4
        standing best (i10+stage)   162 / 91 / 49  ->   157 / 88 / 38
    The improvement the session produced is unchanged in size: +56 rating,
    +23 survival, +34 worst seat.
  * `od_bench.py`'s SCORE regex reads `^\[BENCH\] seat`, so the diagnostic
    `[BENCH] note:` line is ignored by the tool and harmless to leave in.

**Not a bug hunt that found a bug in the AI — a bug hunt that found the ruler
was slightly wrong, and it has been for as long as the seat bench has existed.**
Worth stating because the first reading ("a change I made isn't inert") was the
wrong one and I spent a bisect on it. The tell was in the data the whole time:
eighteen values moving the same direction by the same amount is a measurement,
not a game.

## 32d — closed. The bench parses with the diagnostic line; 157 / 88 / 38 reproduces.

Nothing is running. Every network-side lever is measured and recorded. The
corrected headline stands: **baseline 101 / 65 / 4 -> standing best 157 / 88 /
38**, from `model.i10-final.bin` + `OD_WAR_BIAS` index 7 = 5 +
`OD_DIPLO_PACT_WEIGHT=2.5`. Seven PENDING COMMIT items, nothing staged,
`data/ai/model.bin` untouched since 2026-09-03.

The next gain needs one of two game-rule decisions the loop cannot make:
give pacts teeth, or fix the scripted opening for a small threatened country.

---

## 33 — THE USER DECIDED: give pacts teeth, fix the opening book

Both are GAME-RULE changes. Both change the ruler the seat bench scores
against, so every rating before this entry is on the old scale and the
benchmark is re-baselined below. Recording that before the numbers exist.

**A. The opening book / "declare from strength".** `s_scriptDeclareFix` is now
ON by default (`OD_SCRIPT_DECLARE_FIX=0` restores the bug). The test compares
the country's army against the army of the target `findWarTarget` would
actually attack, instead of against `max(1, enemyAdjArmy) * 2` — which is 2
for any country at peace. Measured earlier at 155 against 162 on the OLD scale
(journal 16); the user's call is that a peaceful country attacking a random
neighbour is broken behaviour regardless of what the bench preferred.

**B. Pacts get teeth.** Breaking a non-aggression pact — by `break_nap`, or by
`declareWar` while one stands — now costs credibility with the betrayed
party, through the same `m_credibility` machinery that already punishes lying
about intentions. The inconsistency journal 09/10 found (the game punished a
false statement and not a broken treaty) is closed. The diplomacy head already
reads credibility through `CREDIBILITY_WEIGHT`, so a country that breaks pacts
becomes harder to make pacts with — which is what gives signing one a value
the reward can learn.

**B, implemented.** `CRED_HIT_PACT = CRED_HIT_CAUGHT = 0.35` — a broken treaty
costs exactly what a caught lie costs (the existing scale: 0.35 for a
statement already disprovable, 0.25 for conduct that disproved it later). Wired
at both places a pact ends: `declareWar` over a standing pact, and `break_nap`.
Both charge the breaker's credibility with the betrayed party, which the
diplomacy head already reads through `CREDIBILITY_WEIGHT = 0.5`.

Build and both re-baselines are chained; the numbers below are the FIRST on the
new ruler and are not comparable with anything above this line.

**Verification plan, recorded before the numbers.** `[EVAL] credibility N
caught out` counts every `loseCredibility` call. Under the OLD rules (pacts
free) it read **14-18 over 4 maps x 300 turns** — every one a caught lie. Under
the new rules every broken pact adds one, so the count MUST rise; if it does
not, the hook is not firing and the re-baseline measures nothing. Same rule as
journals 08, 23, 25 and 32: check the thing being tested happened before
reading its effect. The declare-fix is checked the same way — Norway's opening
on the hood seat must no longer show `NOR declares war` before Sweden attacks.

## 33 VERIFICATION — the declare-fix did NOT fire on the seat it was for

Hood seat, new binary, `s_scriptDeclareFix` defaulting ON:

    105: [WAR] NOR declares war on MCK
    113: [WAR] SWE declares war on NOR

Norway still opens with a declaration. Exactly the failure the verification
plan was written to catch — the effect was about to be read off a re-baseline
in which the rule never ran.

**Hypothesis, recorded before the diagnostic:** the hood seat passes
`--vs-exploit 3`, which sets `s_exploitVariant = SCRIPT_BLITZ` globally. If the
seat's OPENING BOOK inherits that variant, Norway's first twenty turns are
played by the blitz script — whose war line is `pick({3,4,1,2,5,0})`, declare
whenever attack is not legal — and the "declare from strength" branch I patched
is never reached on this seat at all. The fix would be real for the rung and
irrelevant for the book.

**Hypothesis refuted, cause found.** Two checks:

    OD_SCRIPT_DECLARE_FIX=0 -> 1 declaration      =1 -> 1 declaration   (identical)
    run of 19 turns -> 1 declaration   (the book is the first 20)

The flag does nothing on this seat, and the declaration is INSIDE the book. Not
blitz either — `exploitHere` requires `isRandomCountry`, and the seat is not
one. The seat's book plays `trainingVariant`, which defaults to
**`SCRIPT_AGGRESSOR`**. The branch I patched belongs to the default rung; the
aggressor variant declares from its own fixed pick-list and never reaches it.

So journal 16's "fix" was correct for the rung and irrelevant to the thing it
was aimed at. Two patches to the wrong function, one measured as REJECT and one
just made the default, and neither touched Norway's opening. The verification
plan caught it before the re-baseline was read as the effect of a rule that
never ran.

**Pact teeth CONFIRMED firing.** `[EVAL] credibility` over the same 4 maps x
300 turns: **30 caught out**, against 14-18 under the old rules. Every extra
hit is a broken pact now being charged. (The frozen model's pact acceptance is
still 0% — expected; teeth change what a policy can LEARN from signing, not what
a frozen one does.)

**And the aggressor IS the default branch.** `SCRIPT_AGGRESSOR = 0`; there is
no separate case, so the seat's book runs exactly the "declare from strength"
code that was patched. The test is executing and PASSING: Norway's 55,000 army
is more than twice Macedonia's, so `strongEnough` stays true and the script
declares — correctly, by its own terms.

**So the real missing rule is not about the target's strength at all.** It is
that a small country should not open a war of choice against a weak neighbour
while a STRONGER, non-allied neighbour stands next to it. The test asks "am I
twice as strong as who I would attack" and never "is anyone twice as strong as
me". That is the book fix the user actually asked for.

## 34 — the book fix, properly: "is anyone stronger standing next to me?"

`CountryStat` has `enemyAdjArmy` (hostile troops only) and `allyAdjArmy`, and
nothing for a neighbour that is not hostile YET — which is precisely the
neighbour that matters. So the rule walks the country's own frontier for the
largest adjacent non-allied army, regardless of war state, and if it exceeds
ours, "declare from strength" is off. `s_scriptLoomFix`, ON by default (the
user's decision), `OD_SCRIPT_LOOM_FIX=0` to compare.

This is the third patch aimed at Norway's opening. The first two were to the
right function and the wrong test. Recording the verification target before
the build: a 40-turn hood run must show NO `NOR declares war` before
`SWE declares war on NOR`.

**First number on the new ruler (teeth + rung declare-fix, book NOT yet
fixed):** `loop-base` = **118 / 67 / 4**, against 101 / 65 / 4 on the corrected
old ruler. The world got easier for the baseline by 17 points, almost all of it
growth seats — a rung that no longer declares on two soldiers is a calmer rung.
The standing best on this ruler is still benching; the chain re-baselines both
again once the book fix is in.

**v2-best is VOID.** The v3 chain was told to wait for the running bench
(`until ! pgrep -f od_bench`) before building. It did not wait: `v3-base` was
found running 34 seconds old while `v2-best` was 1:46 in. So the loom-fix
binary was built and installed while v2-best was mid-run, and od_bench launches
a fresh process per seat-seed — its later seats ran the new build. A mixed
measurement. Killed before it could be stored under a label that would have
looked like a clean number.

`v2-base` (118 / 67 / 4) finished BEFORE the build and stands.

**Process lesson, and it is the same one as journal 06's peer-blending:** a
guard that gates on "no bench running" is not a guard if the guarding process's
own command line matches the pattern — or if it exits for any reason I did not
verify. Do not chain a BUILD behind a wait; run builds only when nothing is
measuring, and confirm it with a process listing, not a loop condition.

## 34 VERIFIED — Norway no longer opens a war of choice

Direct 40-turn hood run against the binary built 18:31 (confirmed to contain
`OD_SCRIPT_LOOM_FIX` by `strings`):

    119: [WAR] SWE declares war on NOR

and nothing before it. Under every earlier binary the same run showed
`NOR declares war on MCK` at ~line 105, ahead of the Swedish attack. **The
looming-neighbour rule is the fix; the two "declare from strength" patches
were not.** Third attempt, first one aimed at the right condition — the test
that mattered was never "am I stronger than my target" but "is anyone stronger
standing next to me".

Both of the user's decisions are now implemented AND verified to fire:
  * pact teeth      -> credibility hits 14-18 -> 30 over the same 4 maps
  * opening book    -> Norway's opening declaration is gone

The v3 chain's log file is 0 bytes (its echoes were lost; the benches it
launched are real). Reading `v3-base` and `v3-best` from
`build/od_bench_results.json` instead.

## 35 — the first retrain under the new rules — IN PROGRESS (prediction first)

The v3 benches measure the FROZEN model on the new ruler, which says how the
world changed, not whether the rules teach anything. Pact teeth only matter
once the diplomacy head is retrained under them: the point of the user's
decision was to make signing a pact worth something the reward can LEARN.

Setup: `model.i10-final.bin`, ~1,000 turns (journal 26's corrected length),
seed 20260910, pact weight 2.5, everything else default — so the only
difference from journal 27's control (184 on the OLD ruler) is the rules of the
game. No build in the chain (journal 33's lesson).

**Prediction.** Under the old rules the head refused 0/64 pacts and was RIGHT
to. Under the new rules a broken pact costs the breaker 0.35 credibility with
the victim, which the head reads through `CREDIBILITY_WEIGHT` — so a pact is
now a real constraint on the OTHER side, and holding one is worth something.
The pact rate should come off 0% and, per the standing rule, be read against
`net asked`. If it stays at 0/60 with the net consulted on every one, the
teeth are not reaching the reward and the next step is to check that
`credibility()` is in the diplo FEATURES the head sees, not only in its bias.

Read alongside `v3-best` (the same model, un-retrained, on the same ruler):
that pair is the clean A/B.

## 34b — THE LOOM RULE ON THE WHOLE RUNG BREAKS THE WORLD

    ruler                              rating  surv  worst   SWE  hood  rush
    v2  teeth + rung declare-fix         118    67      4    247    44    56
    v3  + book loom-fix (on the RUNG)     61    50      0      0    18    76
    v3  standing best                     93    67      0      0    26   118

**Sweden is annihilated on all three seeds under v3.** The loom rule went into
`scriptedChoice`, which every rung country runs — so it did not fix Norway's
opening, it changed how the whole world fights. A mid-sized country with any
bigger neighbour now never declares; mid-powers stop fighting each other; the
single strongest power in each region is unopposed and eats the small
neutrals. Sweden dies to Russia. The seat that was 247 is 0.

**The overreach:** the user asked for the OPENING BOOK fixed — the seat's own
first twenty turns — and I applied the rule to the rung as well. v2's rung
(declare-fix only) gave the best baseline ever measured, 118. The loom rule
belongs in the book and nowhere else.

**Fix:** scope `s_scriptLoomFix` to book turns only. `scriptedChoice` cannot
tell a book turn from a rung turn, so the call site passes it. Norway's opening
must still show no declaration (that is the whole point), and the rung must
return to v2 behaviour (Sweden 247, baseline ~118).

Retrain N1 (journal 35) is 60% through training under the BROKEN v3 world;
its bench and eval launch after training and will run on the scoped binary.
Its pact-rate reading is still informative (teeth are unchanged); its rating is
not, and its label `v3-retrain` is now a misnomer — noted.

**Guard false-positive, again.** `pgrep -f "od_bench.py --model"` refused the
build because a WRAPPER SHELL's argv (the N1 chain, still training) contains
that string. Same failure as journal 33's wait-loop, opposite sign: there it
let a build through during a bench, here it blocked one when none was running.
A process guard has to match the real process — `^python3 tools/od_bench.py`
or the `OpenDoctrinesServer --eval-ai` seat runs — never a substring a `bash -c`
wrapper also carries. Fixed in the guard from here on.

## 34c — book-only loom: Norway fixed, and the baseline falls anyway

Build clean. **Norway verified**: 40-turn hood run shows only
`SWE declares war on NOR` (line 110) — no Norwegian declaration. The book fix
does what the user asked, scoped to the seat's own opening.

    ruler                                  base rating  surv  worst
    v2   teeth + rung declare-fix                118      67      4
    v3   + loom on the WHOLE rung                 61      50      0   (Sweden dead)
    v4   + loom on the seat's BOOK only           76      62      7

**76 is far below v2's 118**, and the only thing that changed between them is
the seat's first twenty turns. Either those twenty turns matter far more than
their share of the game — the opening sets which wars exist for all 120 — or
the "bug" of opening with a war of choice was PROFITABLE on the growth seats
and the fix removes that profit. Seat-by-seat breakdown below decides which.

**N1, the first retrain under the new rules: 198 / 88 / 28** — but its bench
may have straddled the v4 rebuild (journal 33's race). Checking timestamps
before believing it; the number is quarantined until then.

**N1 cleared.** Binary rebuilt 18:42:31; N1's training finished 18:43:02, so
all eighteen of its seat-runs started after the rebuild and ran on v4. The
198 / 88 / 28 is a clean single-binary number, and it sits on the SAME ruler
as v4-base's 76. First model ever trained with pacts that cost something,
first model over 190 on any ruler, and survival 88 beats every earlier model's.
Whether it beats the previous best on this ruler is v4-best's job (running).

**Where the 42 points went** (shipped model, v2 ruler → v4 ruler; only the
seat's own book turns differ):

    seat        v2-base  v4-base
    FRA rung        206       65    -141
    SWE rung        247      127    -120
    USA rung        151      158
    CHN rung          4        7
    FRA rush         56       76     +20
    NOR hood         44       23     -21   <- the seat the fix was FOR

Two findings. First, the book's opening war of choice was PROFITABLE on France
and Sweden: both have a stronger neighbour (Germany, Russia) that the loom rule
now treats as a reason not to declare, but the book was declaring on a WEAK
neighbour while the strong one stayed at peace, and winning. The rule confuses
"a stronger army is adjacent" with "a stronger army will fight me". Second,
Norway got worse, not better: it no longer declares on Sweden's ally, survives
the opening, and still scores 23 — Sweden attacks either way, and the
early land Norway used to grab is gone.

What actually killed Norway was not that Sweden was stronger and adjacent; it
was that the TARGET was Sweden's ally. The correct "declare from strength"
compares our army against the target's whole bloc — target + allies +
guarantors — not against the biggest thing next door. That keeps France's
opening against a lone weak neighbour and still stops Norway from attacking a
Swedish protectorate. Rule redesign next; the loom rule stays only until then.

**v4-best vs N1, same ruler:**

    model                       rating  surv  worst
    v4-best  (prev. champion)     196     80     21
    N1       (retrain, teeth)     198     88     28

A tie on rating; N1 wins the floor by 8 and 7. Note the ruler itself moved:
the champion read 157 on v2 and 196 on v4, the shipped model 118 → 76. The
book-only loom makes the seat's opening easier for a trained model and worse
for the shipped one. Cross-ruler numbers are not comparable (standing rule).

**The retrain did not learn to sign pacts.** N1 eval, non-aggression, four
maps: 0/48, 0/54, 0/56, 0/55 signed — with the net actually asked 37–51 times
per map — and **H 0.000 on every map (n=37..51)**. The NAP kind of the head is
deterministic: it refuses with probability ~1. Meanwhile the script's word is
now worth something (credibility 0.994 mean, 40 caught over four maps), so a
script NAP is exactly the free security the head is turning down. It cannot
discover that with zero entropy: never signs, never sees the value, never
moves. The "diplomatic pushover" has been fixed into a wall. The entropy guard
was supposed to prevent this — investigating whether it measures the head as a
whole (so a lively ceasefire kind hides a dead NAP kind) rather than per kind.

## 35 — two structural fixes: bloc-strength opening, per-kind collapse guard

**Bloc rule** (replaces the loom walk, `src/ai/AISystem.cpp` declare branch).
The loom rule asked "is anyone bigger next door?"; the right question is "who
is in the war I would start?". The book-turn declare test now sums the
target's allies and guarantors (both relation directions — guarantees are
stored one way in places) and refuses if that bloc out-armies us: twice the
target, once the bloc. Norway vs Macedonia+Sweden: refused. France vs a lone
weak neighbour with Germany at peace next door: allowed, as it was in the 206
opening. Still book-only, still `OD_SCRIPT_LOOM_FIX=0` to disable.

**Per-kind guard** (`GUARD_HEADS = MOD_COUNT + OFFER_KINDS`). The collapse
guard's diplomacy slot measured H over the MEAN answer distribution across all
seven kinds. A head that always refuses NAPs and always accepts alliances has
a healthy-looking mixture entropy, so the guard slept while the NAP kind died
(N1: H 0.000, 0/213 signed). Each kind now has its own marginal, floor,
deficit and PPO entropy coefficient; the `[GUARD]` log prints a line per kind
that was asked. The head was per kind since iteration 21; the guard was not.

Per-seat, v4 ruler, previous champion vs N1 (rating 196 vs 198):

    seat        v4-best   N1
    FRA rung        201   225
    SWE rung        353   360
    USA rung        427   255   (N1 seeds 11.6 / 3.1 / 28.2 — one seed carries it)
    CHN rung         57   208   <- the rebellion seat, tripled
    FRA rush        118   110
    NOR hood         21    31

N1 is the more even model: CHN 57 → 208 and the floor up 7, paid for on USA.

Next: rebuild → verify Norway → v5-base / v5-best / N1-on-v5 (new ruler,
everything re-measured) → N2 = N1's recipe under the per-kind guard, to see
whether a NAP kind with entropy discovers that a script pact is now worth
signing. The orphaned headless-agent run (waiting at its FIFO since 15:36,
game state lost at compaction) was killed; the harness works and gets re-run
fresh.

## 35a — bloc rule verified; the guard could see but not act

**Norway, 40 turns, bloc rule:** only `SWE declares war on NOR` (line 109).
The seat no longer opens a war on Sweden's protectorate. v5 ruler running.

**The per-kind guard's first report** (N2, turn 200, per kind of the diplo
head): guarantee, call-to-arms and trade at marginal 0.000/0.693, floor
0.104, coefficient already pinned at 0.150 (the maximum), "under" for 168–200
batches. So the thermometer works — and the entropy bonus does nothing: a
head that answers with p ≈ 1e-4 has an entropy gradient of about p·log p,
which is zero for practical purposes. The module heads have a second
mechanism for exactly this case — the uniform pull (`m_headDeficit` →
`UNIFORM_PULL_K`, a cross-entropy toward uniform whose gradient is (p − ½),
unsaturated). Checking whether the diplomacy PPO path ever got it.

**It is the whole head, not one kind.** N2's guard block at turn 300: all six
kinds that were asked — ceasefire, alliance, non-aggression, guarantee, call to
arms, trade — at marginal 0.000/0.693, all under the floor for 168–200
batches, all with the coefficient at its 0.150 ceiling. The diplomacy head
refuses everything with p ≈ 1. That is the "less agreeable" work of iterations
21–25 taken to its limit by training: the pushover became a wall, and under
pacts-with-teeth a wall is now the wrong answer to a script that keeps its
word.

The module heads escape this state through the uniform pull; the diplomacy
PPO path has the guard's thermometer (coefficient) but not its actuator (the
pull) — it was never wired in. Wiring it now, per kind, mirror of the module
block: cross-entropy toward both legal answers, weight
`UNIFORM_PULL_K · deficit / 2`, only while that kind is under its floor.
Training-only code; the v5 ruler is unaffected, so N3 (this recipe under the
pull) is comparable to N1-v5 and N2-v5 without another re-baseline.

## 35b — v5 ruler: the "stupid" opening was scoring points

    v5 ruler (teeth + rung declare-fix + BOOK-ONLY bloc rule + per-kind guard)
    model                          rating  surv  worst   FRA  SWE  USA  CHN  rush  NOR
    v5-base   (shipped)              108     75     10    103  250  150   84    57    8
    v5-best   (i10-final, champion)  148     87     21    251  130  145  188   158   23
    N1-v5     (retrain, teeth)       148     81     23    240  210   68  248    94   23
    N2-v5     (N1 + per-kind guard)  110     84     15    157  140  139  120    91   15

**Norway across rulers, shipped model:** v2 (book war of choice kept) 44 →
v4 (loom) 23 → v5 (bloc) 8, seeds [0.0 0.0 0.4]. The seat the fix was FOR
gets worse with every version of the fix. Sweden attacks Norway either way;
the land Norway grabbed from Macedonia in the "suicidal" opening was land it
still held at turn 120. By the score, the opening war was not the loss —
the Swedish attack was, and no book rule prevents it. Journal 15's "costs the
worst seat ~15 points" was a guess that the measurement now contradicts.
France: 206 (v2) → 65 (loom) → 103 (bloc); Sweden 247 → 127 → 250. The bloc
rule gives Sweden back and half of France.

So the book fix as decided costs the shipped model ~10 points and Norway
most of what it had, while making the opening LOOK sane. Running the
champion and the shipped model on the same binary with `OD_SCRIPT_LOOM_FIX=0`
(labels `*-nobook`) to price the rule on the trained model too. **The
user asked for this fix; whether to keep it at this price is their call.**
The rule stays on, flag-disableable, until they say.

**N2 (per-kind guard, thermometer only): 110.** Pacts unchanged — NAP 0/36,
0/60, 0/64, 0/50, H 0.000 on every map, exactly as predicted without the
actuator. The 38-point gap to N1 is one run each with a nondeterministic
trainer; whether the guard's pinned 0.150 coefficient hurt through the shared
trunk or this is run-to-run noise is what N3 answers. N3 (with the pull) is
training on the rebuilt binary.

## 35c — the pull works; the head swings to the other wall

**N3 (per-kind guard + uniform pull), v5 ruler: 139 / 85 / 23.**
Seeds: FRA rung [1.7 1.4 14.9], USA [0.6 1.4 16.1] — two seeds annihilated,
one seed wins big, on both growth seats. Champion 148, N1 148, N2 110.

**The head moved.** N3 eval, net column, two maps:

    kind             said yes    H (T=1)
    ceasefire        23/23 100%  0.000   <- from "never" to "always", deterministic
    alliance         24/52  46%  0.123
    non-aggression    9/54  17%  0.671   <- undecided (ln2 = 0.693)

The guard did its job: the last block has every kind above its floor with the
coefficient back at base. But the ceasefire kind did not stop at "sometimes";
freed from saturation, the reward carried it to the opposite corner. That IS
the original pushover (iteration 1's complaint). The agreement terms (pact
weight 2.5) pay for signing regardless of whether the signer is winning; once
the kind had any mass on "yes", that gradient never let go. The wall had
been hiding a reward that, on its own, wants a pushover.

Two retrains launched in parallel, both from the champion, both with the
pull: N4 = two maps (~1,700 turns), reward unchanged — does acceptance become
conditional given time? N5 = one map, pact weight 1.0 — is the swing the
reward's doing? N3's eval re-run unfiltered for the advantage table.

**Book fix priced (same binary, `OD_SCRIPT_LOOM_FIX=0`):**

    model                  book ON       book OFF
    champion (i10-final)   148/87/21     150/87/23
    shipped (loop-base)    108/75/10     118/67/4   (= v2-base exactly: sanity)

The bloc rule costs the trained model nothing and the shipped model ten
rating (for +8 survival, worst 4 → 10). Kept, as the user asked; the price
is the shipped model's and small.

## 35d — N5: the ceasefire swing is not the pact weight

    model                        rating  surv  worst   ceasefire yes   NAP yes   alliance yes
    N3  (pull, pw 2.5, ~875 t)     139     85     23     23/23 100%     17%        46%
    N5  (pull, pw 1.0, ~760 t)     112     73     17     11/11 100%     60%        22%
                                                         25/25 100% (map 2)

N5's per-decision H is 0.000 on every kind, yet the NAP kind says yes 60% of
the time: it became a CONDITIONAL policy — deterministic per state, varied
across states — which is the shape a good answer has. The ceasefire kind did
not: 100% yes in every state at both pact weights. So the weight is not the
lever. Priced under the reward: accepting a ceasefire earns the agreement
bonus now; refusing earns nothing now, and the land a winning war would still
take arrives turns later through a value head that evidently cannot see it.
"Yes" wins in every state. The wall had hidden a reward that, for ceasefires,
wants a pushover — which is the exact behaviour the user opened with.

N5 also lost Sweden entirely [0.0 0.2 0.3]: a country that signs every
ceasefire cannot finish the war it needs to finish. N3's full eval: ceasefire
"to stronger" 100% on both columns — it accepts from the weaker side too.

Fix in design: the ceasefire agreement term pays only when the signer is not
winning; when it is, the term goes to zero (or negative — giving up a war it
was winning). N4 (two maps, pw 2.5) still training, turn 1100 — its eval says
whether time alone would have made the ceasefire kind conditional.

**The head can see it; the reward does not pay for it.** The ceasefire
decision's features include the offerer's army relative to ours (feature 88,
tanh of the log ratio) plus the country block (threatened provinces, losses).
The window reward has no ceasefire-specific term at all: the only
per-decision credit is the trade outcome. Accepting a ceasefire zeroes the
gated dLost/dGained terms and relieves weariness — a certain small positive —
against a continued war whose gains the value head must forecast. Certain
beats forecast; "yes" wins in every state, at any pact weight.

Fix: a per-decision ceasefire credit written into the Experience at answer
time — accept while winning −0.8, refuse while winning +0.4, accept while
losing +0.6, refuse while losing −0.4, nothing when it is even. "Winning" =
our army > 1.25× the offerer's and no province lost this turn; "losing" =
the mirror, or hostile troops on our border outnumbering ours there. Weighted
by `OD_CEASEFIRE_CREDIT` (default 1, 0 disables) for the A/B. Training-only;
the v5 ruler stands.

**N4 (two maps, 2,026 turns, pw 2.5, pull): 88 / 65 / 4.** Ceasefire 17/17
100% (H 0.011), NAP 28/31 90%. CHN annihilated on all three seeds, USA on
two. Time did not make the ceasefire kind conditional; it made everything
worse, on schedule with the ~1,000-turn peak (memory: training degrades the
model). Both retrains that were meant to separate "not enough training" from
"the reward" point the same way: the reward. The ceasefire credit is in the
code (`Experience::ceasefireCredit`, `OD_CEASEFIRE_CREDIT`), N6 (pw 2.5) and
N7 (pw 1.0) train with it after N4's eval releases the binary.

    all on the v5 ruler, ~800 turns from the champion unless noted
    N1  teeth                         148/81/23   NAP  0%  ceasefire  0%   (wall)
    N2  + per-kind guard, no pull     110/84/15   NAP  0%  ceasefire  0%   (wall)
    N3  + pull                        139/85/23   NAP 17%  ceasefire 100%
    N4  + pull, 2,026 turns            88/65/ 4   NAP 90%  ceasefire 100%
    N5  + pull, pw 1.0                112/73/17   NAP 60%  ceasefire 100%
    champion (no retrain)             148/87/21   NAP  0%  ceasefire  0%

## 35e — N6/N7: the ceasefire credit moves the kind, but not to "conditional"

    model                          rating  surv  worst   ceasefire yes      NAP yes
    N6  credit, pw 2.5, 841 t        76     57      0    0/55, 0/27   0%     56%
    N7  credit, pw 1.0, 1127 t      139     78      8    13/13, 21/21 100%   62%
    champion                        148     87     21    0%                   0%

N6's Sweden died on all three seeds; the ceasefire kind went from "always"
back to "never" (training-time marginal 0.593 — mixed — yet 0/55 at eval).
N7's stayed at "always". Per-decision H is 0.000 in both: deterministic per
state, and at eval every state gets the same answer. One reading of N6 is
that it is RIGHT: in a vs-script eval the script asks for a ceasefire when
the script is losing, so the net is winning, so the credit says refuse.
"to stronger 0%" does not settle it — army ratio is not the credit's test.
The eval cannot tell a refusal-while-winning from a refusal-while-losing.
Instrumenting exactly that split (asked/yes by the credit's own
winning/losing/even classification) and re-evaluating N6 and N7 on the new
binary; no retrain needed for the question.

Standing back: every retrain from the champion this evening lands at or
below 148 on the v5 ruler. Diplomacy fixes are about survival and about not
looking stupid; they have not produced rating and the arithmetic says they
cannot: 300 means holding three times par on average, and the champion's
best seat is at 2.5× with the rest at 1.3–1.9×. Rating lives in the war and
economy heads (conquest rate), and in decision-time search.

## 35f — trade rules (user decision): never cede at a loss, always take a gift

User, mid-loop: the AI sometimes gives up territory for nothing, and it
should always accept deals clearly in its favour. Both are rules, not
rewards, and live in `decideDiplomacy`'s trade block ahead of the head, so
they bind every model, the scripted cohort, and the network:

  1. A trade that takes any province of ours and nets below zero at our own
     prices is refused by rule (`tradeRuleRefusals`).
  2. A gift (we give nothing, receive something) or a deal at least one
     province's worth (120) in our favour is accepted by rule
     (`tradeRuleAccepts`), counted as a yes for the trade kind.

The head still decides the open middle: deals between −120 and +120 that do
not cede land. The −120 floor, the ruinous gate, and the treasury clamps
stay. Eval prints a "by rule" line under the trade kind. Verification: a
champion-vs-script probe after the build; the bench cannot see this (no
player in it, and the champion refused every trade before).

Not covered, on record: ceasefire terms can still cede land at a loss while
NOT losing the war — the ceasefire credit trains against it, but no rule
stops it; and there is no truce after a ceasefire, so declare / take terms /
re-declare is legal. Both are the user's call (assessment given 19:5x).

## 35g — the credit made a wall, not a judge; ceasefire rules

N6 re-evaluated with the war-state split (net column, four maps):

    winning 0/34  losing 0/12  even 0/9
    winning 0/12  losing 0/10  even 0/5
    winning 0/26  losing 0/9   even 0/9
    winning 0/26  losing 0/4   even 0/3

Zero while losing. The credit did not teach "refuse when winning, accept
when losing"; it taught "refuse", because the asks are mostly from losers
(the net is winning in 60–75% of them) and −0.8 on those outweighed +0.6 on
the few losing ones. The head is deterministic per state and the states it
conditions on are not the credit's. The script column reads 0/0 by
construction — scripted countries answer before the counting site.

That closes the reward-side thread with a measured verdict: four retrains,
two weights, two lengths, one shaped credit; the ceasefire kind ended at
100% or 0%, never conditional. The two ends are now RULES, mirror of the
trade rules the user decided: (A) losing and the ceasefire costs nothing →
accept; (B) winning and nothing offered → refuse. The head keeps the middle
(even wars; losing but asked to cede; winning but paid). Eval prints a
"by rule" line. Rules bind the rung too, so the ruler moves: v6.
Chain: rules probe → v6-base / v6-best.

**N7 split:** 4/4, 9/9, 10/10, 19/19 winning; 3/3, 6/6, 6/6, 5/5 losing —
yes in every state. N6 no in every state. The credit found the two corners
and never the middle. Rules it is (35g).

**Trade-rule probe is VOID:** in a vs-script eval nobody proposes a trade —
"trade 0 offer(s) made" on both maps; the champion's politics head never
picks propose_trade and the script never does. The per-kind trade line does
not even print. The rules cannot be verified by eval; they need a direct
probe that constructs terms and asks `decideDiplomacy` — writing one as a
server flag.

**Search at play, first cost/benefit number** (`--quick`, one seed, the two
France seats, champion, v5 binary):

    control        195   FRA rung 15.1   rush 11.0    11.0 s
    OD_MCTS_SIMS=4 210   FRA rung 17.7   rush 10.5    15.8 s   (+43% time)

One seed, so a hint and not a result — but 4 sims cost 43% and not the 30×
that killed the 24-sim arm (journal 22). Full three-seed benches at 4 and 8
sims queued behind the v6 ruler. If +15 holds on 18 games it is the single
cheapest rating gain of the day, and it needs no retrain.

**Ceasefire rules verified** (champion vs script, two maps, net column):

    map   winning        losing        even     by rule: accepted-losing / refused-winning
    1     0/16           4/5           0/2      4 / 15
    2     0/10           5/6           0/4      5 / 8

The head was asked 4 of 23 and 7 of 20 times; the rest was settled by rule.
The one refusal while losing on each map is the head's, on terms that ceded
land or ran negative — the middle it keeps. The scripted cohort shows the
same rules acting (6 and 5 accepted while losing, 2 and 4 refused while
winning). Conditional by construction, which four retrains could not learn.
v6 ruler running.

## 35h — trade rules proven; search at play is greedy play; rule B scoped

**Trade probe (`--probe-trade 1914:FRA:rung`, Afghanistan asked by the
British Empire): PROBE_OK 5/5.** Gift of a province → accept by rule
(net +120). Province for 1 gold → refuse by rule (cedes at a loss, −119).
Province for nothing → refuse (−120). Province for its price + 200 → accept
by rule (+200). 300 gold for nothing → accept (gift). Head's-call cases:
"pay 50 for nothing" → refused by the ruinous gate (500% of a 10-gold
treasury); a swap at a 1-gold premium → the head accepted (+1). Both rules
fire, at the right thresholds, ahead of the head.

**Search at play, full bench, v6 ruler:**

    champion, no search      142 / 76 / 23    ~180 s
    OD_MCTS_SIMS=4           110 / 59 /  0     219 s
    OD_MCTS_SIMS=8           110 / 59 /  0     269 s

4 and 8 sims produced IDENTICAL per-seed numbers on all 18 games. At this
budget the visit-count argmax is the prior's argmax every time; what the
search actually changed is that the action became greedy instead of sampled
at T = 0.18 — and greedy play is 32 points worse, with Sweden and China
dead. Two things follow. Search at play at a shippable budget is closed
(journal 22 closed the expensive budget; this closes the cheap one). And
the sampling temperature is load-bearing at play: the policy's stochasticity
is doing work that its argmax cannot. The temperature itself has not been
benched; it is a one-env-var experiment.

**Rule B scoped.** v5 → v6 per seat, champion: FRA 251→225, SWE 127→23,
USA 145→205, CHN 187→211, rush 159→156, NOR 21→33; shipped model 108→78.
A winning Russia that used to grant Sweden a free peace now refuses it by
rule. Correct play for Russia, death for the seat, and not what rule B was
for. B now applies only when the winner holds a claim on the asker's land —
a winner with something to take. A winner with nothing to take lets the
head weigh weariness against pride. v7 ruler running with a re-probe.

**Two experiments queued behind v7.** (1) Play temperature: `OD_PLAY_TEMP`
(new, bench-only override of the difficulty profile's sampling temperature;
the seat plays at the rung profile's 0.35 by default). Champion at 0.20,
0.60, 0.90 on the v7 ruler; v7-best is the 0.35 control. Greedy scored 32
below sampled, so this is where that signal points. (2) N8 = N4's recipe
(two maps, ~2,000 turns, the length that collapsed to 88) at `OD_LR_SCALE=0.5`.
If the collapse past ~1,000 turns is the optimiser oscillating, half the
step should land near N1's 148 instead of N4's 88; if it is something else
(league drift, reward non-stationarity), it will collapse the same way.

## 35i — v7: the champion's best number of the day; temperature is not a lever

    v7 ruler = teeth + book bloc rule + trade rules + ceasefire rules (B scoped to claims)
    model                 rating  surv  worst    FRA  SWE  USA  CHN  rush  NOR
    v7-base (shipped)       74     64      3      58  130  130    4    97   23
    v7-best (champion)     163     89     36     264  140  239  140   157   38

    v6 → v7, champion: 142/76/23 → 163/89/36. Scoping rule B gave Sweden
    back (23 → 140) and cost nothing elsewhere. Highest rating, survival
    and floor the champion has posted on any ruler tonight.

**Ceasefire re-probe after scoping:** winning 0/9 and 0/15 (only 1 by rule
now — the rest are the head's refusals), losing 4/4 and 4/4 by rule, even
0/4 and 0/8 by the head. The head refuses when it is not losing; the rule
guarantees the escape when it is. That is the shape wanted.

**Play temperature, champion, v7 ruler** (0.35 is the rung profile):

    T      rating  surv  worst
    0.20    178     89     36
    0.35    163     89     36
    0.60    173     91     44
    0.90    169     90     41

Everything sampled sits within 15 points, non-monotonic, inside the bench's
seed noise (USA seeds 7.4 / 12.3 / 20.4 on the control). Greedy at 110 was
the outlier: the policy needs SOME sampling and is otherwise indifferent to
how much. Not a lever; not adopting a change to the shipped profile on one
bench. `OD_PLAY_TEMP` stays as a knob.

The shipped model at 74 on v7 (118 on v2) is the other side of every rule
added today: each one makes the game harder for a model that never trained
under it. The champion, retrained under none of them either, gained. The
difference is that the champion has the war and economy heads to use the
room the rules open; the shipped model does not.

## 35j — N8: half the step does not stop the decline; the sampling is not the temperature

    model                                turns   rating  surv  worst
    N1   1 map, LR 1.0                     791     148     81     23
    N4   2 maps, LR 1.0                   2026      88     65      4
    N8   2 maps, LR 0.5                   1693     107     86     33

Half the learning rate at the collapsing length: 107 — better than N4's 88
on floor and survival, still 40 below the ~800-turn point. One run each, so
±15 is noise; the direction is not. The decline past ~1,000 turns is not
(only) the optimiser's step size. Remaining suspects: the opponent mix
(league drift — the model learns to beat its own recent selves, not the
script the bench is), and the reward's non-stationarity. A vs-script-only
training arm at the same length is the clean test of the first.

**Temperature sweep, per seat:** FRA rung 264 / 264 / 264 / 264, rush 156
×4, CHN 140 ×3 — IDENTICAL games at T = 0.2, 0.35, 0.6, 0.9. Only SWE and
USA moved. The champion's policy is so peaked that sampling almost never
leaves the argmax on three seats. So what made greedy play (the 4/8-sim
search, 110) lose 32 points was not the temperature. The search path also
bypasses the ε random-action roll (2% at the hard profile). Hypothesis:
the 2% random actions are what a collapsed policy is living on — they are
the only way it ever attacks / builds / signs when its argmax says hold.
`OD_PLAY_EPS` added; sweep at 0 / 0.05 / 0.10 running. If ε = 0 reads ~110,
that is the finding; if 0.05 or 0.10 beats 163, it is also a lever — and a
damning one, because it says random beats the head on those decisions.

## 35k — the trainer never met the bench's opponent

Reading the training population: a map holds the learners, a league share
(LEAGUE_SHARE 0.33, frozen earlier selves plus the capped exploiter), and a
"control cohort" (`m_randomCids`). That cohort plays DICE unless
`--vs-script` is passed, in which case it plays the hand-written rung — the
same rung every bench seat is scored against. Every retrain tonight (N1–N8)
used the N1 recipe, which does not pass it. The model has been training
against random countries and its own past selves, and being scored against
a scripted opponent it never saw. That is the user's question of 19:xx
("the trainer puts the AI against its earlier self — how else can it
learn?") answered from the code: the other opponent was one flag away.

Two arms, N1's recipe plus `--vs-script`, from the champion: N9 (one map,
~800 turns) and N10 (two maps, ~1,700 turns — the length that declined to
88 and 107 against dice/league). If N10 holds up where N4/N8 fell, the
decline past ~1,000 turns was drift against a moving opponent, not the
optimiser. The caveat is on record: a model trained against the rung is
being taught the test; the bench will flatter it relative to human play.

## 35l — ε is not it either; training against the script ties the champion; map 2 is the cliff

**Random-action sweep, champion, v7:** ε = 0.00 → 163/89/36, identical to
the 0.02 control to the seat; 0.05 → 155; 0.10 → 172/88/33. Noise. So
35j's hypothesis is wrong too: neither the temperature nor the ε roll is
what search-at-play lost. The champion plays effectively deterministically
and the search's 110 came from the SEARCH — visit counts through the latent
dynamics model picking differently from the prior. The latent model
misguides at play; that is why it must not ship, not because greedy is bad.
(Correction to 35h/35j's attribution.)

**N9 — N1's recipe + `--vs-script`, 747 turns: 165 / 84 / 36.** USA 363,
rush 182, FRA 237, SWE 110, CHN 64, NOR 38. Ties the champion (163) from one
short step, where every dice/league retrain landed 139–148. The opponent
the model trains against matters as much as anything changed today.

**N10 — same, two maps, 1,593 turns: 81 / 70 / 4.** Collapsed exactly like
N4 (88) and N8 (107). So the decline is not the opponent mix, not the step
size. Look at the pattern instead of the turn count:

    one map:   N1 148  N3 139  N5 112  N7 139 (1,127 t)  N9 165
    two maps:  N4  88  N8 107  N10  81

Every two-map run collapsed; every one-map run held. A map ends when its
game ends (~750–1,100 turns), so "past ~1,000 turns" and "on the second
map" have been the same thing all along. Something happens at the map
switch — or the model learns its first world and the second is a
different game. Reading the between-map code. Meanwhile the production
question is direct: N11 = a fresh one-map vs-script step FROM N9 (new
seed). If it climbs, the cliff is in-process state and the ladder is the
path; if it falls, it is the lineage (memory: self-play erodes).

**League is not the map-2 difference.** Every run (N1, N4, N9, N10) wrote
exactly one league checkpoint, at start, slot 0 = the champion it began
from (270.6M updates). One-map and two-map runs face the same frozen
opponent throughout. Struck from the list. Left: the terminal update at
the end of map 1 (victor/dead outcomes fire for every country at once), or
plain non-transfer — a model fitted to one world meeting another. N11
(fresh process, N9's weights, new map) separates them: if it falls, it is
the weights meeting a new world; if it climbs, it is in-process state.

**Map 2 was the same world every time.** The trainer's scenario table is
indexed by map number: map 1 is always `pangaea`, map 2 always
`continents`, map 3 a shipped map. With one base seed (20260910) N4, N8
and N10 all played the identical continents world (seed 1551539546, land
0.35, 4 continents, 37 countries) as their second map. So the "cliff" has
three candidate causes and the runs so far cannot tell them apart: that
one world, continents worlds in general (naval geography the heads may
mishandle — the bench's shipped maps are Earth, also multi-continent), or
any second map. N12 = two maps on base seed 20260912 (a different
continents world) is running beside N11. Note the design gap regardless: a
one-map recipe never trains on anything but a pangaea, and is scored on
Earth.

**Not the navy, not the guard.** N10's progress lines: map 1 (pangaea)
embarks 1,929 / landings 0 by turn 700; map 2 (continents) 1,749 / 5 by
turn 600. Same pattern on both worlds, so naval behaviour is not what map
2 changes. The guard's marginals at the end of each map are alike too
(war 0.99 → 0.80, navy 0.76 → 0.45 of ceiling, nothing under a floor).
Whatever the cliff is, it is not visible in the per-head entropy and it is
not a continents-specific naval failure. N11 and N12 decide between
weights, process state, and that one world.

Standing oddity for the backlog: ~2,000 embarks and zero hostile-shore
landings per map, on a pangaea. Either "embark" means ferrying between own
ports (then the count is fine and the name is wrong) or the navy head
boards armies onto ships that never land them. Worth one look; it is not
tonight's question.

## 35m — the embarkation sink

From N3's full eval, one number that has been sitting in every eval log:

    [EVAL] amphibious  0% of 8,999 embarkations reached a hostile shore
           (152 came home, 6 landing orders dropped as out of range)
    per map: embarks 2,785 / 2,435 / 2,356 / 1,423 -- landings 0 / 1 / 0 / 3
    troops embarked per map: net 628 / 495 / 446 / 333  vs script 317 / 335 / 279 / 318

An embark order (navy action 3) takes HALF the port's garrison off the map
and turns it into boat crew (100 men = 1 crew). Nine thousand of those per
eval, and none of them ever land on an enemy. The script does it too, at
about half the rate. Whatever the boats are doing, the men on them are not
fighting, and the war head is left defending with what the navy head did
not ship out. On a pangaea this is pure loss; on Earth (every bench seat)
it may be the single biggest leak in the army.

Two questions, in order. Does it cost rating? `OD_NAVY_BIAS` (new, mirror
of `OD_WAR_BIAS` for the navy head) with embark at −20 on the champion,
queued behind N11/N12: if the rating rises with embark off, the sink is
real money. Why do boats never land? Reading the land action and its mask
next — the answer decides whether the fix is "embark only when a landing
is possible" (a mask rule) or a resolver bug in sailing/landing.

**Boat mechanics read.** An embark takes half a port garrison into a "boat"
(100 men = 1 crew). Every turn the navy's automatic routine sails each
loaded boat toward the nearest REACHABLE enemy port and lands when the
boat is within landing range of that province's CENTRE; with no enemy
port it sails home and unloads (the "152 came home"). Boats do move
(range 200 px/turn, destroyers 350). So a 0% landing rate over nine
thousand embarkations means loaded boats are stuck somewhere between
"sailing" and "allowed to land". Added a counter that splits loaded
boat-turns into "still under a move order" and "parked with no order and
still out of landing range". If the second number is the fleet, the sail
target (the port) and the landing test (the centre) disagree and the fix
is one line. Eval queued behind the embark-off bench.

## 35n — N12: the cliff was one world

    two maps, vs-script, from the champion
    N10  base seed 20260910 (map 2 = continents seed 1551539546)   81 / 70 /  4
    N12  base seed 20260912 (map 2 = a different continents world) 169 / 83 / 44

Same recipe, same length, different second world: 81 versus 169. Every
"training degrades past ~1,000 turns" measurement in this journal and in
memory was made on the fixed base seed 20260910, whose second map is that
one world. N12 is the best retrain of the day (N9 165, champion 163) and
has the best floor of any model on the v7 ruler (44). Two conclusions.
Longer training is back: the length ceiling was an artefact of one seed.
And that world is worth a look on its own — a map the model cannot learn
without losing what it knows is a map with a rule in it the model has not
met (memory: the worst seats are the ones it was never trained on).

Launched: N13 = three maps on the good seed, from the champion — the third
map is a SHIPPED Earth map (the trainer's cadence is every third), which is
what the bench plays. N14 = two more maps from N12 on seed 20260913, the
ladder. N11 (the one-map ladder step from N9) is still in its bench.

N12 per seat: FRA 252, SWE 210, USA 359, CHN 64, rush 91 (seeds 6.6 / 1.2 /
10.4), NOR 46. Growth seats strong, the two survival seats and the rush
seat weak — the same shape as N9. And the two second-maps are near twins
on paper: N10's toxic world is continents, land 0.35, 4 continents, jag
0.22→0.38, 37 countries; N12's is continents, land 0.35, 4 continents, jag
0.36, 42 countries. Not the world type, then — something that happens in
that particular game. Backlogged as a test case; not tonight's lever.

## 35o — notice from the roadmap session; reference frozen

Another session (drafting `docs/design/community-roadmap-2026-09.md`, a
PLAN, nothing built, pending the user's scope decision) warned that the
economy and combat rules are likely to change: per-province industry
capacity, a goods economy, economic systems, fuel/munitions repricing, a
13th politics action, combat width. Each moves the seat bench without
touching a weight; the economy head is full (ECON_ACTIONS = 12 =
MAX_MODULE_ACTIONS) and the feature vector will grow. Its advice, taken:

- **Reference frozen** at `build/loop/reference/`: champion (i10-final),
  N12, the shipped loop-base, the v7 server binary, the v7 source diff
  (all PENDING COMMIT work), the bench JSON, sha256 of each, and a README
  with the v7 numbers. A post-change regression can now be attributed.
- No long economy-dependent training starts from here. N13/N14 are short
  (≤ 25 min) and were already running; they answer a question about
  training length and the shipped maps, which survives the rule change.
- Instrumentation, rules-in-resolvers, and the bench harness — the work
  that survives — is what this loop has mostly been doing today anyway.

Not done, on purpose: no git tag (the user's rule: nothing committed or
tagged without their word), and no production-economy readouts in
od_bench — there is nothing to read until Phase 2 exists.

**Roadmap session, second exchange.** It found a real defect in the
widening I had offered to absorb: the dynamics net's input one-hot is
strided by MAX_MODULE_ACTIONS, so 12 → 16 re-indexes every module block
after economy and deserialize's tail zero-fill cannot see it. Verified at
AISystem.cpp:796 / :1177. Decision: reset m_dynamics at the widening and
let the 200,000-update warm-up re-earn it — search at play is closed and
train-with-search is the only consumer. The production-economy readouts
in od_bench stay with this loop, to be written when Phase 2 exists. Both
recorded in BACKLOG.md.

## 35p — scope landed: Phases 1+2 together; the tree is shared from here

The roadmap session reports the user's decision: capacity AND the goods
economy in one batch, four goods (consumer, machinery, fuel, munitions),
existing over-cap provinces grandfathered (refuse the NEXT level, never
clamp the current). It is editing src/Game.h, Game_TurnLogic.cpp,
AISystem.h/.cpp, Game_Loading.cpp, Game_Render.cpp, Game_Multiplayer.cpp
now, additively, around my uncommitted lines.

What this loop did about it, immediately:
- Killed its two chains that would have rebuilt build/ (embark-off bench,
  boat diagnostic) — a build from a half-edited tree would either fail or,
  worse, produce a binary my benches then measured as v7.
- Asked the other session to compile in a separate build directory until
  my running benches drain (N11, N13, N14 on the v7 binary), and not to
  touch build/ until told; I will not rebuild build/ until Phase 1 is done.
- Froze my own edits to the four claimed files. My lines are in
  build/loop/reference/v7-source.diff.

Consequences for the plan: the embark-off measurement and the boat counter
wait for the tree to settle (they need a build). No new training starts:
Phase 1 re-prices the economy head and Phase 2 widens the action space, so
anything trained now would be trained for a game that is about to change.
The v8 re-baseline happens once, after both phases, with the reference
models in build/loop/reference/ measured first.

**N11 correction.** N11 is still TRAINING — turn 1,500 on its single map
at 21:26; seed 20260911's pangaea has not ended, so a "one-map" run is
running to the 3,000-turn cap. By accident it is the experiment 35n asked
for: a long run on ONE world. My earlier "bench running" reads were wrong
— pgrep matched the chain wrapper's own text, which contains the bench
command. Real processes are matched on the expanded binary path from now
on. N13 is on map 3 (the shipped Earth map), N14 on map 1.

## 35q — N11: 200. The ladder works; the shipped map does not

    lineage (all vs-script, from the champion, v7 ruler)
    N9   champion -> 1 map,   747 turns                 165 / 84 / 36
    N11  N9       -> 1 map, 1,612 turns (seed 20260911) 200 / 87 / 23   <- first 200
    N12  champion -> 2 maps, 1,682 turns                169 / 83 / 44
    N13  champion -> 3 maps, 1,895 turns (map 3 = SHIPPED 1914)  145 / 82 / 36

N11 is the first model at 200 on any ruler today, from a chain of two
short runs, each a fresh process on a new world. Its single map ran to
1,612 turns without ending — a long run on ONE world — and gained 35 over
its parent. So: length on a world the model can learn from is fine; the
ladder (fresh process, new seed, from the last best) compounds; and the
"collapse" was that one world all along.

N13's third map was the shipped 1914 map, and it cost 24: the two 1914
seats fell (FRA rung 252 → 116, SWE 210 → 60) while CHN rose (64 → 232).
Training on the very world a seat is scored on made that seat worse.
One run — but it is the one kind of map the trainer plays every third
round, and it points the wrong way. Recipe for now: pangaea/continents
worlds, fresh seeds, chained short runs; avoid the shipped-map round.

N15 launched: from N11, one map, seed 20260914 — ladder step 3. Runs on
the v7 binary still on disk; no rebuild while the other session edits.
N11 copied into build/loop/reference/ as N11-200.bin.

N11 per seat vs its ancestors:

    seat        champ   N9    N11
    FRA rung      264   237   241
    SWE rung      143   107   233
    USA rung      239   362   285
    CHN rung      140    65   239   <- the rebellion seat, ×3.7 over N9
    FRA rush      156   182   183
    NOR hood       36    36    23

The most even model yet: five seats over 180. Norway remains the floor
on every model (dies to Sweden's rush regardless of who plays it).

N13's result is in its log but not in od_bench_results.json — its bench
and N11's finished within the same minute and one save lost the race.
Making od_bench's save a locked read-modify-write.

**od_bench save race fixed.** Both save sites now go through
`save_label()`: re-read the results file under an exclusive lock, merge
the one label, write a temp file, rename. N13-v7 restored from its log.

## 35r — N14: the second-map worlds are the problem, and the navy is why

    step                          worlds trained on            result
    N9   champ -> 1 map           pangaea                      165
    N11  N9    -> 1 map           pangaea (1,612 turns)        200
    N12  champ -> 2 maps          pangaea, continents(A)       169
    N14  N12   -> 2 maps          pangaea, continents(B)       121   (from 169)
    N13  champ -> 3 maps          pangaea, continents(A), Earth 1914   145
    N10  champ -> 2 maps          pangaea, continents(toxic)    81

Every pangaea-only step went up; three of four steps through a
continents or Earth world went down (N12 is the exception, +4). N14 lost
on every growth seat: FRA 252 → 170, SWE 207 → 107, USA 358 → 215.

Put next to 35m this has a mechanism. On a multi-continent world the navy
head acts far more, and its embark order takes half a port garrison off
the map for nothing — 0% of embarkations ever land. A world that
exercises that action trains the war head to fight with the men the navy
head did not ship away, and the value heads to expect it. A pangaea
never triggers it. The bench's Earth seats are mostly land wars, so a
pangaea-trained model looks better on them than a model that learned to
embark. That makes the embarkation sink the thing to fix BEFORE any
multi-continent training can pay, and it is blocked on the rebuild.

Recipe until then: one-map steps (pangaea only — the trainer's map 2 is
always continents), fresh seed, from the last best, vs-script. N15 is
that, from N11, at turn 600.

**Ladder step 3 runs two candidates.** N15 (seed 20260914) and N16 (seed
20260915), both one map from N11, both vs-script, on the v7 binary. The
better of the two becomes the next parent; the single-run spread between
siblings is the noise estimate the ladder has been missing. Both are
pangaea worlds by construction (map 1 always is).

## 35s — N16: a 546-turn step turned 200 into 67

    N16  N11 -> 1 map (pangaea, seed 1301715238, 32 countries), 546 turns   67 / 58 / 0

Sweden annihilated on all three seeds, every seat down, guards healthy
throughout, 28 of 32 countries alive at turn 500 — and the map ended at
546. Same recipe as N11 (which gained 35), same parent, different world.
So the ladder's step distribution has a catastrophic tail: two worlds out
of the eight trained on today (N10's map 2, this one) wrecked the model
in under 600 turns, and nothing in the guard's readouts saw it coming.
Sibling N15 (seed 20260914) is at turn 1,100 on its world and still
running; the bench gate keeps N16 out of the lineage either way.

What ends a map at 546 with 28 alive is the question — if it is the
learner cohort being wiped out, the model's last few hundred turns were
nothing but dying, and the terminal updates would teach exactly that.

## 35t — every map ends frozen, and the frozen share predicts the result

The trainer rotates a map after STAGNATION_TURNS = 400 turns in which no
province changed hands between real countries. Every training map today
ended that way. So a run's last 400 turns were always played in a world
where nothing moves — and the share of the map spent frozen lines up with
the bench:

    run   map ended   frozen share   bench
    N11     1,612        25%          200
    N1        791        51%          198
    N12   776 / 906    52% / 44%      169
    N9        747        54%          165
    N10   912 / 681    44% / 59%       81
    N14   884 / 562    45% / 71%      121
    N13  815/541/539  49%/74%/74%     145
    N16       546        73%           67

The two "toxic worlds" were the two shortest games. A world that freezes
at turn 150 gives the model 400 turns of evidence that holding is as good
as anything, and the terminal update on top. That is the passivity the
bench punishes (Sweden dies when it does not fight). STAGNATION_TURNS is
now 80 in the training loop (`OD_STAGNATION_TURNS` overrides; the eval
loop's 1,500 is untouched). Lives in Game_AITrain.cpp — not one of the
files the roadmap session claimed — but it needs a build, which waits
for Phase 1 to finish. First runs after that: the ladder from N11 (or
N15 if it wins) at 80, two seeds.

## 35u — Phase 1 is in the tree; interim ruler v7+P1

The roadmap session reports Phase 1 complete, compiling and tested in its
own build directory; the four shared files are stable until it announces
Phase 2. Its first numbers (40 turns, 65 countries): world capacity 25%
utilised, the heaviest builder at 86% of its own ceiling, 3 grandfathered
provinces — the cap binds for a hard industrialiser and nowhere else.
It also caught a calibration defect before shipping (density and area
terms cancelling) and added a test for it.

This loop's response: rebuild build/ now from the Phase 1 tree — that
binary also carries the stagnation cut (80), the navy bias knob and the
boat counter — and re-measure the shipped model, the champion and N11 on
it as `v7p1-*`. Two things are expected and will not be chased: the
economy will read low (Phase 1 is a mask change on econ action 1 and the
model never trained under it), and the number is an interim gate, not
the v8 baseline, which comes once after Phase 2. `[CAPACITY]` lines are
parsed by od_bench from now on (world utilisation and grandfathered
provinces per run, stored with each label).

**N15 trained: 1,407 turns** (frozen at 1,407 under the old 400 rule →
~1,007 live turns, 28% frozen — N11's shape). Its bench started after the
21:53 rebuild, so it is measured on v7+P1; the chain's label `N15-v7` is
wrong and will be renamed `N15-v7p1` when it lands. Gate for the ladder
from here: N11-v7p1 (running).

**N15 on v7+P1: 131 / 70 / 7** (label corrected to N15-v7p1). Per seat:
FRA rung 281 (best France yet), SWE 250, USA 139, CHN 8, rush 84, NOR 31.
Modern China collapsed to 8 — the seat that industrialises hardest, on
the first binary where industry has a cap. Whether that is the ruler
(expected: a mask change on econ action 1 read against a model that never
saw it) or N15 is exactly what the gate says: N11-v7p1 and v7p1-best are
running on the same binary. Until they land N15 is unjudged.

**Ladder step 4 (first under stagnation 80), two seeds from N11 on the
v7+P1 binary:** N17 (seed 20260916, pangaea, 45 countries) and N18 (seed
20260917). Gate: N11-v7p1 (running). Started before the gate lands because
N11 is the parent either way unless N15 beats it on the same ruler, in
which case a step from N15 follows.

## 35v — Phase 2 has started; the shared files are closed again

The roadmap session is now writing the goods economy into Game.h,
Game_TurnLogic.cpp, AISystem.{h,cpp} and Game_AITrain.cpp (additively,
beside the [CAPACITY] block). No edits or rebuilds from this loop until it
says complete. Phase 1 passed its determinism check (6 runs agree over 25
turns), so any spread in the v7+P1 numbers is the model.

Two things Phase 2 changes for the v8 plan. It ships behind a per-world
feature flag, so the old money economy runs on the new binary: v8 gets
measured twice — new binary + old economy (isolates the action-space
widening and the dynamics reset from the rules), then the new economy.
And raw-material surplus sale is a dial, `OD_AUTOSELL_PCT` (100 = today's
economy, 60 = intended default, 0 = nothing sells itself; ships at 100).
At 0 the trade head becomes load-bearing for the first time. The v8 sweep
is {100, 60, 0} on the reference models before any conclusion about
weights. Meanwhile the ladder keeps running on the v7+P1 binary.

## 35w — the embarkation sink is a resolver that deletes men

The boat diagnostic (champion vs script, 2 maps × 200 turns):

    5,204 embarkations, 0% reached a hostile shore, 86 came home,
    loaded boats: 27 boat-turns parked out of range, 25 still sailing

Fifty-two loaded-boat-turns in 400 turns of play against five thousand
embark orders. Loaded boats barely exist. The parked-boat hypothesis
(35m) is wrong: nothing is parking, because almost nothing gets aboard.

The resolver (Game_TurnLogic.cpp, the m_pendingEmbarkations loop) removes
the men from the port garrison FIRST, then looks for a boat within 50 px;
failing that it flood-fills for a sea pixel next to the province with at
least 200 water cells behind it and spawns a boat there; failing THAT it
does nothing — the men are already gone. And in every branch the crew
added is `totalRemoved / 100` in integer division, so an order of fewer
than 100 units adds zero crew and deletes the lot. Half a port garrison
per order, several thousand orders per eval, the script doing it too.

Fix (resolver, so it binds everyone — but Game_TurnLogic.cpp is closed
for Phase 2): remove men only after a boat exists (spawn or find first,
then transfer), keep the remainder ashore instead of dividing it away,
and count what is deleted until then. In the AI (AISystem.cpp, also
closed): embark only when a landing is possible. Both wait for the tree.
Meanwhile: v7p1-best-noembark scored 151 against a control still running.

**v7+P1 gate, first number: shipped model 99 / 65 / 4** (was 74 on v7).
The industry cap made the shipped model BETTER by 25 — not the low read
the mask change was expected to give; a model that barely industrialises
loses nothing to a cap that binds only hard industrialisers, and its
scripted rivals do. Not chased (agreed with the roadmap session); noted.
Champion and N11 on v7+P1 still running; the embark-off 151 waits on the
champion's control. The eval logs carry no [AI] decision lines, so the
men-deleted count needs the resolver instrumented — after Phase 2.

## 35x — v7+P1 gate

    v7+P1 ruler (Phase 1 industry cap on top of v7)   rating  surv  worst   cap util  grandfathered/run
    shipped (loop-base)                                  99     65      4     40%       44
    champion (i10-final)                                151     90     41     31%       21
    N11 (ladder, 200 on v7)                             165     91     46     31%       26
    N15 (from N11, 1,407 turns)                         131     70      7     --

N11 stays the parent: 165 vs N15's 131 on the same ruler, and the best
floor of anything measured today (46). The champion 163 → 151 and N11
200 → 165 across the ruler change, the shipped model 74 → 99; the cap
hurts hard industrialisers and spares the model that never was one.
N17 and N18 (from N11, stagnation 80) are the right runs and are in
flight. First `[CAPACITY]` readouts are in the bench JSON: the world runs
at 31–40% of capacity with 21–44 grandfathered provinces per seat-run.

**The embark-off bench is void.** v7p1-best-noembark is identical to the
champion's control on every seat to two decimals — the navy bias never
reached a decision. Reading the qbias path to see what gates it.

**The navy bias is inert.** One map, 100 turns, vs script, deterministic
world: with `OD_NAVY_BIAS` unset, at −20 and at +20 on embark, the model
cohort's counters are identical to the decimal — embark offered 1,955,
chosen 1,539 (78.7%), troops embarked 840.07. The env is parsed (the
"[AI] navy bias" line prints), the lambda returns the vector, pickAction
adds it before masking — and nothing changes. So the model cohort's navy
choices are not coming out of that call, or the bias is added to a
vector the choice does not use. Locating the actual navy decision path
before spending another bench on it. Not a Phase-2 file question: it is
in AISystem.cpp, which is closed, so the answer waits with the fix.

Established regardless: the head picks embark on 79% of the turns it is
offered, and 0% of those men ever land. That is the leak; the knob to
measure it with is broken, not the finding.

**Correction: the knob works; the head is saturated past 20 logits.** At
−200 on embark the model cohort's rate falls from 78.7% (1,539 of 1,955
offered) to 24.0% (753 of 3,137) — the residual being the opening book's
scripted turns, which embark by script, and the script cohort itself
embarks 100% of the time it can (879/879). Two things learned for free:
a ±20 logit bias is nothing to this policy (the temperature sweep said
the same), and the embarkation habit lives in the SCRIPT too, so the rung
leaks men at 100%. Bench at −200 on the champion running (control 151).

## 35y — N17: a low frozen share is not enough

    step from N11 (165 on v7+P1)     world              live / frozen   result
    N15  seed 20260914               pangaea, 1,407 t   1,007 / 400     131 / 70 / 7
    N17  seed 20260916, stag. 80     pangaea, 622 t       542 / 80      102 / 81 / 46
    N16  (on v7, from N11 = 200)     pangaea, 546 t       146 / 400      67 / 58 / 0

Three steps from the 200 model, three drops. N17 spent 13% of its turns
frozen and still lost 63. So the frozen share is not the whole story —
it explained N16 and the multi-map runs, and it is still worth having
cut, but a step's outcome is dominated by the world drawn. Read as a
distribution: of the eight one-map steps today, three went up (N9 +2,
N11 +35, N1-type +~), five went down, and the drops are large. The
ladder climbs only through the gate, and slowly; two or three seeds per
step is the honest cost. N18 (seed 20260917, frozen at 776) is in its
bench; N19 (seed 20260918) launched. N11 remains the model of record.

## 35z — the embarkation sink, priced: +18 for the champion

    champion, v7+P1, same seeds          rating  surv  worst
    control                                151     90     41
    OD_NAVY_BIAS embark −200               169     90     41

Eighteen points from one env var that stops the navy head loading half
a port garrison onto boats that never land. Survival and floor
unchanged, so it is all growth seats. This is decision-time only — the
model still WANTS to embark (79% of offers) and the rung still does at
100% — and the proper fix is the resolver (men only leave when a boat
exists; whole crews; remainder stays) plus the mask (embark only when a
landing is possible), both in files closed for Phase 2. Until then the
knob is the measurement, and it goes into the bench recipe as a
documented flag, not silently. N11 with embark off is running.

Per seat, champion with embark off: USA 251 → 355 and every other seat
identical to the decimal. The whole gain is the one seat with an ocean
between it and its wars — the 1939 United States ships its army away
turn after turn and never lands it. France, Sweden, China and Norway
never embark as the seat, so their games did not change. On Earth the
tax is geographic; in training worlds with coasts it is general.

## 36 — N18: 58. Four steps from N11, four drops; is the 80 rule the cause?

    from N11 (165 on v7+P1)     stagnation   turns   result
    N15   seed 20260914            400        1,407   131 / 70 /  7
    N16   seed 20260915 (on v7)    400          546    67 / 58 /  0
    N17   seed 20260916             80          622   102 / 81 / 46
    N18   seed 20260917             80          776    58 / 43 /  0

The two runs under the 80-turn rule are the two shortest games and two
of the three worst results. The rule may be cutting worlds at an
ordinary mid-war lull rather than at a real freeze, so the model trains
on 600 turns instead of 1,400 and ends on a lull. Paired test: N20 =
N18's exact world (seed 20260917) at 400. If N20 lands near N11 the 80
rule goes back to 400 (or to a test that distinguishes a lull from a
freeze: e.g. no province changed hands AND no war in progress). N19
(seed 20260918, at 80) is the third 80-rule sample.

Either way the ladder from N11 is 0 for 4. N11 stays the model of
record, and its embark-off bench is running.

## 36a — Phase 2 core: no widening, no dynamics reset; the flag is exactly neutral

From the roadmap session: factory-to-good allocation became a resolver
heuristic (Game::autoAssignOutputs), not a policy decision, so
MAX_MODULE_ACTIONS, ECON_ACTIONS, FEATURE_COUNT, GUARD_HEADS and
Experience::visits are untouched and m_dynamics keeps its training —
the widening and reset move to Phase 3 (planned-vs-market is the neural
decision). With OD_GOODS unset a 40-turn eval is byte-identical to the
Phase-1-only build; on, survival 67.9 → 66.0%, rebellions 71 → 120/1k,
14% of factories idle, median living standards 1.00. Determinism holds
both ways. New lines `[LIVING]` and `[GOODS]` (silent unless on) are
parsed by od_bench from now (living standards mean, idle share, per-good
produced/consumed/demand, stored under "goods").

v8 plan, revised: (1) new binary, OD_GOODS unset = Phase 1 alone — the
v7+P1 gate already IS that measurement, so nothing to redo; (2) goods on
at OD_AUTOSELL_PCT 100 / 60 / 0 on the reference models; (3) the four
hand-tuned production rates in GameStructs.h (GOOD_PER_LEVEL,
EXTRACT_PER_AMOUNT, CONSUMER_PER_CAPITA, RAW_FLOOR_PRICE) are unswept and
undefended — sweep them before anyone trains against them; (4)
SHORTAGE_UNREST_PCT is 6 on purpose (the AI has no lever yet). Two
design errors it caught from evals: consumer goods needed rubber or
gemstones that 27 of 64 countries do not have (93% idle, rebellions
tripled), and the allocator ignored whether the materials were held.

**N19 (from N11, stagnation 80): frozen after 268 turns → 126 / 86 / 44.**
The 80-turn rule ended a world at turn 268 — 188 live turns, which is
not a frozen world, it is a lull in the opening. Three runs under 80:
622, 776, 268 turns; 102, 58, 126. The rule cuts far too early. The
paired control N20 (N18's world at 400) is still the clean test, but
the direction is not in doubt; the recipe goes back to
`OD_STAGNATION_TURNS=400` (an env override — no rebuild needed) and the
lull-vs-freeze test in the backlog is the real fix. Three seeds from N11
at 80 are therefore not evidence about N11 as a parent.

**N11 with embark off: 167 / 91 / 46 (control 165 / 91 / 46).** Two
points. The sink's price at play depends on the model: the champion
paid 18 (all on the USA seat), N11 two. N11 simply ships fewer men
out. The resolver fix is still right — it is the rung's 100% embark
rate and the training worlds' coasts that the fix is for — but as a
play-time knob it is not where N11's next points are.

Ladder bookkeeping: five steps from N11 (two at 400, three at 80), all
down. N21 (seed 20260919, back at 400) is the sixth ticket; N9 is being
measured on v7+P1 to see whether N11's parent is the better base on
this ruler.

**N9 on v7+P1: 167 / 90 / 41** — level with N11 (165 / 91 / 46). N11's
+35 over N9 on v7 became +0 on v7+P1; the Phase 1 cap took back what
N11's extra 850 turns had bought on the growth seats. Parent stays N11
for the floor (46). Both are the top of the table on this ruler; the
champion is 151 and the shipped model 99.

## 36b — Phase 2 complete; v8 sweep launched

The roadmap session released the four files: persistence done (the save
wins over the environment, so a bench cannot mix economies), read-only
UI, three new tests, determinism both ways, and flag-off still
byte-identical to Phase-1-only — the v7+P1 gate stands as the
"new binary, old economy" measurement. Its goods-ON note worth a look:
industry building nearly triples (+865 → +2,273 levels) and bankruptcy
halves, most plausibly because auto-sold surplus raw is extra income the
old economy never had — in which case autosell 100 is "old economy plus a
subsidy", the 60 and 0 arms are the informative ones, and RAW_FLOOR_PRICE
is the knob, not the production rates.

Rebuilt build/ (goods off ≡ v7+P1, so N20/N21's benches are unaffected)
and launched the sweep: shipped, champion, N11 × OD_AUTOSELL_PCT
{100, 60, 0} with OD_GOODS=1, labels `<model>-v8g-a<pct>`, goods
readouts in each. Nine benches, ~40 min. The embarkation resolver fix
comes AFTER the sweep so the sweep's ruler is Phase 2 alone; v8 proper
(goods setting + embark fix) is one rebuild later.

## 36c — the embarkation resolver, root cause and fix

The roadmap session read processEmbarkations and found the level under
mine: the no-boat path spawns a hull by scanning m_provincePixels, an
index built lazily (128 MB) and only outside training — the same
"worked by accident" gate the conquest overlays had. In every training
and eval run the scan misses, no boat is ever spawned, and each
embarkation without a hull already within 50 px deletes its troops;
a player sees it intermittently (fine after opening the Claims view).
With the integer-division crew and the missing-centre path that makes
four deletion routes, and the eval's "0% of N embarkations" line had
been printing the fact all along.

Rewritten boat-first (Game_TurnLogic.cpp): whole crews from what the
garrison holds; a hull within 50 px or the nav grid's water cell by the
province (portApproach — no pixel index, water by construction, refused
if land); deduct exactly crews × 100 only after a boat exists; a failed
order is a no-op and counted (`m_navEmbarkNoBoat`, `m_navEmbarkTooSmall`,
`m_navMenEmbarked`, printed under the amphibious eval line). Compiles;
not built — the goods sweep runs on the Phase 2 binary first, then the
diff goes in after the roadmap session's read. It will move the ruler:
a rung that embarks whenever it can, and whose men now survive, is a
stronger opponent, and amphibious invasions become possible at all.

## 36d — first goods bench: a rules bug, not a tuning question; N21

**Shipped model, goods ON, autosell 100 (18 runs summed):** consumer
produced 1,350 / consumed 1,136 / demand 103,306 — 1% of demand met;
machinery, fuel and munitions demand 0.00 (nothing consumes them before
Phase 4); living standards mean 0.36; 22% of factories idle. Rating
71 / 44 / 0 against 99 / 65 / 4 with goods off on the same binary:
Sweden 223 → 0, China 4 → 0. A 76:1 demand-to-production gap is a
units mismatch (people vs goods, or per-turn vs per-window), not a
margin; reported to the roadmap session with the numbers, with the
advice not to tune any rate until the denominator is right. The other
eight benches of the sweep still run and will say whether 60 and 0
change the shape.

**N21 (from N11, seed 20260919, stagnation 400, 1,395 turns): 114 / 57 / 4.**
Six steps from N11, six down, at both stagnation settings. N11 is a
peak the ladder cannot leave upward by this recipe; the step variance is
the finding. N20 (N18's world at 400) is the last one running and
answers only the 80-vs-400 question.

**Embark fix, reviewed:** the roadmap session confirmed the restructure
and added three things, all applied — the 50 px hull search is gated on
the same sea body (an isthmus is narrower than 50 px), the spawn is
gated on isProvinceCoastal (navCellNear would find a sea beyond a
mountain range for a landlocked province), and a nav-grid cell reported
as land is logged and counted as a grid/raster disagreement rather than
folded into "no boat". Sequencing agreed: sweep → embark fix → re-gate →
their army-maintenance fix (maintenanceCostPct is summed and applied
nowhere — four research nodes and four doctrines advertise a discount
that never existed) → re-gate.

## 36e — paired test: the 80-turn rule cost 92 points on the same world

    from N11, seed 20260917 (identical world)   stagnation   turns   result
    N18                                             80          776    58 / 43 /  0
    N20                                            400        1,164   150 / 82 / 23

Same parent, same world, one env var: 92 rating points. The 80 rule was
cutting at ordinary lulls (N19's world ended at turn 268). Default back
to 400 in Game_AITrain.cpp with the numbers in the comment; the
lull-vs-freeze test stays in the backlog as the real refinement. The
three 80-rule steps (N17, N18, N19) are struck from the ladder record;
the honest count from N11 is three steps at 400 (N15 131, N16 67 on v7,
N21 114) and N20's 150 — still 0 for 4, but a different distribution.
N11 remains the model of record (165 on v7+P1).

**Shipped model, goods ON, autosell 60: 112 / 65 / 5** (off 99 / 65 / 4;
autosell 100: 71 / 44 / 0). Living standards 0.39 and 1% of consumer
demand met at 60 exactly as at 100 — the starvation is the demand
denominator and does not move with the dial. What moves is money:
France 70 → 79 → 205 across off/100/60, Norway 23 → 28 → 74, Sweden
223 → 0 → 43. So autosell reshuffles who profits while everyone starves;
none of these numbers is a basis for choosing a setting until the
consumer demand bug is fixed. The sweep runs on for the record (champion
and N11 arms will show whether trained models are hit the same way).

**Shipped model, autosell 0: 89.** The shipped arm of the sweep in full:
goods off 99 / a100 71 / a60 112 / a0 89 — non-monotone in the dial,
with living standards pinned near 0.4 and 1% of consumer demand met in
every arm. The dial reshuffles seats; the starvation is constant. Nothing
to choose here until the demand denominator is fixed. Champion and N11
arms follow.

## 36f — the famine was a population runaway; goods arms discarded

The roadmap session traced the 76:1 gap: consumer demand was the first
reader of absolute population that is LINEAR in it, and the world's
population runs away — 2.1 billion at load, 110 billion implied by turn
120, fifty-two times over a run. Industry capacity is logarithmic in
population, income a bonus, manpower a fraction, so nothing had ever
shown it. Goods reported it as a famine. Fixed on its side by making
every term in the chain scale with population (factory output = workers
× level/capacity; ore = richness × people), and a test now asserts scale
invariance; the ratio is flat across the horizon and living standards
rise as countries industrialise. The population runaway itself is on
the user's desk as a separate issue — and it matters to this loop
regardless of goods, because manpower comes out of the same number.

The four shipped goods benches (a100 71, a60 112, a0 89, and the six
never run) measured a world nobody could feed after turn 60: discarded,
labels kept in the JSON for the record only. Sweep aborted; the v8
chain proceeds to the embark-fix build and gate. Autosell is still worth
sweeping later; the rates that matter are now GOOD_OUTPUT_SCALE and
EXTRACT_SCALE, and CONSUMER_PER_CAPITA is a shared denominator (changing
it alone does almost nothing).

## 36g — v8 gate restarted on a clean tree; the instrument lands before the fix

The roadmap session reverted its mobilisation levers (policies.json at
HEAD; maintenanceCostMod inert; goods off), so the gate measures Phase
1+2 plus the embarkation fix and nothing about armies. The user decided
to bound population growth; it lands as one "army and people" batch with
the upkeep fix and the lever spread, followed by one re-gate.

Added before that batch, at the roadmap session's request: `[POP]` lines
— raw per-country population (sum of province populations), army units
and provinces, plus a world total — printed with the end-of-run summary
and parsed by od_bench into a "people" record per label. The v8 chain
rebuilds with it, reads the champion's world at 40 / 80 / 150 turns on
one map (the before-curve of the runaway), runs the amphibious check,
then gates shipped / champion / N11. Two ladder tickets (N22 from N11,
N23 from the champion) start on that binary once the marker appears.

## 36h — v8 is the whole batch; the before-curve was not captured

My v8 chain never built: its "wait for the running cmake" loop matched
its own script text and waited forever (the same pgrep trap as 35q,
from the other side). Meanwhile the roadmap session landed the "army and
people" batch on disk: bounded population growth (the runaway was a
DUPLICATE growth path — the old per-minority block left standing beside
the per-province rule, applying growth once per minority name, ~9.5×,
for an effective 3.35%/turn = 52× over 120 turns; plus a taper that
aimed at a flat 1e10 and never bit), maintenanceCostMod applied to army
upkeep (its first wiring went into the dead copy of the formula —
refreshIncomeCache has its own upkeep line and is what the game reads;
the third two-homed formula in that file), and the mobilisation lever
spread. Its proxy: implied world population at turn 120 141.6 → 1.82
billion; survival at 120 turns 58.5 → 66.0% (bound and upkeep together).

So there is no clean embark-only gate and no printed before-curve; the
roadmap session's consumer-demand inference stands as the before, and
my `[POP]` instrument prints the after. Ruler v8 = Phase 1+2 (goods
off) + embark fix + army-and-people batch, in one build, running now:
after-horizon at 40/80/150, amphibious check, then shipped / champion /
N11. Tickets N22/N23 start on it. One ruler, one gate, as intended —
just without the intermediate reading.

## 36i — v8 built: population bounded; embarkations now harmless but never sail

Champion vs script, one map (seed 20260801), the `[POP]` instrument's
first numbers, after the bound:

    turns   world population   army units
      40      1.99 billion      45.5 M
      80      2.45 billion      53.8 M
     150      3.41 billion      63.9 M

Bounded — +70% over 150 turns where the old rule gave 52× — and still
rising toward the taper's ceiling (~8.5 billion on the 1939 world).
Different map and seed from the roadmap session's proxy (1.67 / 1.85 /
1.82 billion at 40 / 80 / 120), so the level differs; the shape agrees.

The embarkation resolver, same runs:

    turns   embark orders   loaded   dropped (no boat/water)   dropped (<1 crew)
      40        1,357          0            2,674                   0
      80        3,197          0            6,200                   0
     150        7,017          0           12,724                   0

Every order is dropped; nothing is loaded; landings 0%. The deletion is
gone (the garrison stays), which is the part that mattered for training
and for the rung — but no boat is ever found or spawned, so amphibious
operations remain impossible. Either isProvinceCoastal is false for
every port in eval (if it reads the absent pixel index) or the nav grid
is not built in eval and portApproach fails. The grid/raster counter
never printed, so the approach itself is not returning land. The v8
gate runs on this binary (a ruler with no leak and no landings); the
spawn fix follows as v8.1.

**Why every order dropped:** isProvinceCoastal reads m_provincePixels
and returns false for every province while that index is unbuilt — the
same absent index, one function over. My coastal gate therefore refused
every embarkation in eval. Repaired: a harbour is coastal by
construction, so the resolver asks the port table (m_provincePorts)
first and keeps the pixel test only for non-port coastal provinces.
v8.1 rebuild and re-gate queued behind the v8 gate, with an 80-turn
resolver check first.

Caveat for training worlds: buildNavGrid is called from the ships.json
load in Game_Loading.cpp. Scenario maps have that file; generated
training worlds may not, in which case m_nav is never ready there,
portApproach returns false, and embarkations in training stay harmless
no-ops rather than becoming invasions. The [TRAIN] progress line's
landings count on N22/N23 will say.

**Training worlds have no nav grid.** buildNavGrid() runs only inside
the ships.json block of the loader, and the generator writes ships.json
only for maps that have ships. So generated training worlds — every
one-map ladder step — most likely never build the grid: portApproach
fails, seaBodyOfPort is −1, and embarkations there are no-ops even with
the resolver fixed. Asked the roadmap session (its file) to build the
grid whenever the raster is loaded. Until then the navy exists on the
shipped maps only, which is where the bench lives; training keeps a
harmless navy rather than a leaking one.

**v8 gate, first number: shipped model 104** (v7+P1 99, v7 74). The
"army and people" batch and the no-leak resolver together leave the
shipped model about where Phase 1 put it. Champion and N11 follow.

**v8 gate: champion 126** (v7 163, v7+P1 151). The batch costs the
champion 25 on top of Phase 1's 12. N11 follows; per-seat below once
both are in.

    champion per seat      v7    v7+P1    v8
    FRA rung              264     245    233
    SWE rung              143     103     67
    USA rung              239     251    121
    CHN rung              140      99    173
    FRA rush              156     164    102
    NOR hood               36      41     59
    survival / worst    89/36   90/41  88/59

The batch moves growth seats down and survival seats up: the best floor
any model has posted (59), and the USA seat halved. Bounded population,
a live upkeep discount, opposed-sign mobilisation levers and a rung that
no longer loses half a garrison per embark are four changes in one
ruler; the honest reading is "different game", not "worse model". N11
on v8 is running; v8.1 (spawn repair) re-gates all three after it, and
that is the ruler the next tickets are judged on.

## 36j — v8 gate complete

    v8 (Phase 1+2 goods off, embark no-leak, bounded population,
        live upkeep, mobilisation levers)     rating  surv  worst
    shipped (loop-base)                          104     62      1
    champion (i10-final)                         126     88     59
    N11                                          146     --     --   (detail below)

N11 stays the model of record, 20 clear of the champion on the fourth
ruler in a row. Every model lost rating to the batch and gained floor;
the game got harder and less lethal at once. v8.1 (embark spawn repair)
builds now and re-gates all three; that is the ruler the tickets are
judged on.

N11 on v8 in full: 146 / 92 / 62 — survival 92 and worst seat 62 are the
best either number has ever been; Norway at 90 for the first time on any
model. Per seat, v8: shipped / champion / N11 = FRA 215/233/205, SWE
237/67/247, USA 96/121/152, CHN 1/173/120, rush 64/102/62, NOR 8/59/90.
The batch turned the hood seat from a certain death into a held line.

## 36k — boats sail: 9% of embarkations land

v8.1 resolver check (champion vs script, one map, 80 turns):

    embarkations 1,771 -- 9% reached a hostile shore, 1,465 came home
    resolver: 159,683,500 units loaded, 0 orders dropped, 0 under one crew

Against every prior reading of 0% and "0 loaded, all dropped", the
port-table coastal gate plus the boat-first resolver put men on boats
and some of them onto enemy coasts. Most still come home (no landable
enemy port in range on that map) — that is the AI's target choice and
the mask's job, not the resolver's, and it is now measurable. The v8.1
gate (shipped / champion / N11) runs on this binary; it is the ruler of
record from here. The nav-grid gap for generated training worlds stands
until the roadmap session moves buildNavGrid out of the ships block.

**And now the parked boats are real:** the same check reads "loaded
boats: parked out of landing range 1,326 boat-turns, still sailing 2".
With men actually aboard, the 35m geometry shows up: the boat sails to
the harbour's approach cell, the landing test measures to the province
centre, and a coastal province whose centre is more than one hull's
range inland can never be landed on — the boat parks. Fix written in
the auto-landing routine (AISystem.cpp): only ports whose centre is
within LAND_RANGE of their approach count as targets; the nearest
landable one wins. Compiled, not linked; it goes into the build after
the v8.1 gate and the tickets' benches, as v8.2, with the same check.

## 36l — N22, and the v8.1 gate begins

**N22 (from N11, seed 20260920, 850 turns) on v8.1: 141 / 84 / 15.**
Its bench started 26 seconds after the v8.1 binary landed, so it is a
v8.1 number (label corrected). Judged against N11-v81 when that lands;
against N11's v8 146 it is a near-tie with a worse floor (Norway back to
15 from 90). N23 (from the champion) is at turn 1,100.

v8.1 gate: shipped 98 (v8 104). Champion and N11 follow.

**v8.1 gate: champion 90** (v8 126). The first ruler on which the
scripted rung's boats actually land — 9% of its embarkations, where
every earlier ruler had 0% — and a model trained under a navy that
never arrived pays for it. Per seat below; N11 follows.

    champion per seat    v8   v8.1
    FRA rung            233    238
    SWE rung             67      0
    USA rung            121    136
    CHN rung            173     59
    FRA rush            102     89
    NOR hood             59     15
    survival / worst  88/59   60/0

Sweden annihilated, China and Norway cut to a third: the three seats
that sit across water from their enemies. The rung embarks every time it
can (100% in every measurement) and, with the resolver honest and the
port-table gate in, a tenth of those boats now land. Armies at turn 120
fell from 97 M to 61 M units world-wide — men are dying on beaches
instead of vanishing on docks. The model has never seen an enemy arrive
by sea, because in every training world it ever played no boat ever
landed. Two things follow. The ladder must retrain on worlds where the
navy exists, which needs the nav grid in generated worlds (asked of the
roadmap session); and the balance of amphibious warfare is now a real
question for the game, since every scripted coastal country has become
an invader overnight.

## 36m — v8.1 gate complete: N11 holds under landings

    v8.1 (landings real)        rating  surv  worst
    shipped (loop-base)            98
    champion (i10-final)           90     60      0
    N11                           145     --     --   (v8: 146)
    N22 (from N11, v8.1)          141     84     15

The champion lost 36 to the rung's landings; N11 lost one point. Per
seat below. N11 remains the model of record on the fifth ruler in a
row, and it is the model that copes with an enemy arriving by sea
without ever having seen one — which is worth understanding before the
next retrain, because it is the difference between the two lineages.

    v8.1 per seat     shipped  champ   N11   N22
    FRA rung             204    238    216   203
    SWE rung             260      0    253   243
    USA rung              90    136    163   174
    CHN rung               3     59    149   123
    FRA rush               1     89     76    89
    NOR hood              28     15     15    15
    survival / worst    54/1   60/0  82/15  84/15

The whole difference between the champion and its descendants is
Sweden: 0 against 253 and 243. Sweden faces Russia across the Baltic;
the champion is annihilated by landings there, N11 and N22 hold. Both
descendants were trained against the scripted rung (the champion was
not), and the script defends its own coasts by reflex — so a model that
learned to beat the script may have learned to keep garrisons where the
script attacks from, which is also where boats arrive. Norway at 15 for
all three is the seat every model still loses to a seaborne Sweden.

## 36n — training worlds have a navy; the tempo question goes to the user

The roadmap session moved buildNavGrid out of the ships.json block: on
a generated world, 60 turns, 7% of 463 embarkations landed and the
router stopped 143 of 269 ship moves at land. Generated training worlds
now have a nav grid, so the next tickets can learn coastal defence. It
also corrected the attribution: the 0 → 10% on the shipped maps is this
loop's resolver fix (those maps always had a grid); its move affects
only maps without ships.json.

On the rung's embark tempo its view matches mine: a scripted country
that empties half a port garrison on a reflex is doing something no
player would, so seats are measuring resistance to a tic; a doctrine
gate (a national share of the army, a target that could be held, never
from a province under threat) would make landings rarer and better.
Put to the user as "doctrine with preconditions, or constant attempts
and learn to defend". The embark rule is in AISystem and is this loop's
to write once the answer lands; nothing changes before then. Its other
caveat stands: a harder rung is not a bad outcome, and the answer may be
a retrain rather than a gentler rung.

**N23 (from the champion, seed 20260921, 1,394 turns) on v8.1: 83 / 72 / 15**
— below its parent's 90. The champion lineage does not hold coasts and
a fresh step from it does not learn to; N22, from N11, sits at 141.
Lineage of record stays N11. The next tickets train on v8.2 worlds,
which have a nav grid, so a landing can be met for the first time in
training. v8.2 waits for the roadmap session's Phase 3 headers to
compile (marker GO_V82).

## 36o — Phase 3 complete; v8.2 building; first tickets on navigable worlds

Phase 3 (planned-vs-market economies off the compass: directable
factories, a planned economy that sells a fifth of a market's surplus and
pays for factories partly in machinery, a player button to direct one)
is in and verified, inert with goods off. It needed NO new AI action:
the economic system is a consequence of doctrines the politics head
already enacts, so ECON_ACTIONS, MAX_MODULE_ACTIONS, FEATURE_COUNT and
m_dynamics all stay. Whether the politics head can learn to use the
compass as an economic lever is a real question for a later ticket, with
goods on.

v8.2 build released (landable-port targeting in the auto-landing
routine, nav grid in generated worlds, Phase 3 inert). Chain: 80-turn
shipped-map check, 60-turn generated-world check, then the gate. N24 and
N25 (from N11, seeds 20260922/23, stagnation 400) start on that binary —
the first tickets ever trained where an enemy can arrive by sea.

## 36p — v8.2 check: the landable-port filter did nothing

Shipped-map check on v8.2: 1,771 embarkations, 9% landed, 1,465 home,
1,326 parked boat-turns, 2 sailing — identical to v8.1 to the unit. So
no enemy port fails the "centre within one hull's range of its
approach" test; the 35m geometry is not why loaded boats sit with no
order. The other way a boat ends up with no move order is the router
erasing it as STUCK (Game_TurnLogic.cpp, the ship move loop: "arrived
|| stuck → erase"); the reflex then re-issues the same order next turn,
it sticks again, and every check finds the boat orderless and out of
range. That is a pathing question, and the instrument for it is a
count of arrived vs stuck erasures for loaded boats. Reading the router.

## 36q — the amphibious doctrine (user decision) is written

User's decision, relayed: a landing is a doctrine with preconditions.
Written in bestEmbarkPort — the chokepoint every cohort's embark passes
through — with the constants beside the other rung numbers in the
header:

  1. A national share (AMPHIB_ARMY_SHARE 0.25): men aboard plus the
     force being loaded may not exceed a quarter of the army. Binds
     every cohort. Six ports are not six invasions.
  2. A target it could hold (AMPHIB_ODDS 1.5 over the weakest hostile
     port garrison on the port's sea): SCRIPTED COHORT ONLY. Whether a
     landing that might lose is still worth making is the war head's
     judgment; a mask would stop it ever being tried, scored or learned
     — the diplomacy-head trap. Recorded as a deliberate split.
  3. Never from a port with hostile troops on its border. Binds every
     cohort: emptying the garrison about to be attacked is not a
     strategy anyone chooses.

The roadmap session argued the split of condition 2 and is right by
this loop's own rule (ends are rules, the middle is the head's). Sent
for review; builds as v8.3 after the v8.2 gate and N24/N25. Expected:
the rung weaker at sea, coastal seats recovering some ground — the
intended effect, not tuned to any number, and v8.2 stays as the
"constant attempts" reference. Also in v8.3: router counters for
loaded-boat orders (arrived vs erased-as-stuck), the instrument for the
1,326 parked boat-turns the landable-port filter did not touch.

**Doctrine gate, review round.** The roadmap session found the cohort
test for condition 2 covered one of the four scripted cases (the
vs-script control cohort only; not the tutorial, the script duel or the
exploiter league) — now m_scriptedThisCountry, the canonical flag, set
in takeTurn before either path into bestEmbarkPort. It also caught the
old "guard tried, measured, removed" comment sitting directly above the
re-added guard; the measurement stays and the conclusion now says the
guard is back by the user's decision and its measured cost (a third of
the AI's gains, landings 8% → 3%, when the 8% was never a landing) is
expected. Fail-closed share cap now speaks under aiDebug; the odds
test's looseness (weakest garrison on the sea, target chosen later) is
stated. v8.3 chain picks all of it up.

**Signed off by the roadmap session; one correction to my own framing.**
The "about a third of the AI's gains, landings 8% → 3%" that the old
guard comment records was measured while the resolver was deleting the
embarked men — the 8% was never a landing rate — so the real cost of
condition 3 is UNKNOWN, not "about a third". The only honest measurement
is v8.3 against v8.2, and a smaller cost there means the old number was
taken in a broken world, not that the doctrine is cheap. Written into
the code comment so it cannot calcify. The tree is at a finished state
on the roadmap side (both binaries, determinism both economies, four
suites green); Phases 4–6 await the user's pick and will be announced
with a file list before they start.

## 36r — v8.2 gate: identical to v8.1

    v8.2        shipped 98   champion 90   N11 145   (v8.1: 98 / 90 / 145)

The landable-port filter is measurably nothing; v8.2 is v8.1 with a nav
grid in generated worlds, which the bench (shipped maps) cannot see. It
stands as the "constant attempts" reference for the doctrine. v8.3
builds when N24/N25 finish.

**Landings in training, for the first time:** N24's world shows 73
landings by turn 600 and N25's 84 by turn 800 (every earlier training
log read "landings=0" to the end). N25 finished at 870 turns and is in
its bench; N24 at turn 600. Whatever they score, they are the first
models that had an enemy arrive by sea while learning.

**N25 (from N11, seed 20260923, 870 turns, the first world with landings —
84 of them): 106 / 74 / 26 on v8.2** against N11's 145. One ticket, the
usual spread; its floor (26) beats N11's 15. N24 still training. Whether
either lands above N11 on the same ruler decides the next parent, on
v8.3, once both are re-benched there.

## 36s — Phase 4 announced; v8.3 parked on a marker

Phase 4 (armies and artillery priced in fuel and munitions; real demand
for machinery, fuel and munitions; doctrines as the exchange rate;
deliberately NOT touching resolveAssault while the amphibious work is
measured on it) is starting in Game.h, Game_TurnLogic.cpp and
AISystem.cpp among others. Goods off stays neutral, but a half-written
edit at build time is not neutral, so the v8.3 build now waits for a
GO_V83 marker that I create when the roadmap session confirms the
shared files are held or compiling. Requested the hold; N24's bench is
the only other thing before the build.

## 36t — v8.3 built; the doctrine trims, it does not halve

Doctrine check (champion vs script, shipped map, 80 turns), v8.2 → v8.3:

    embarkations   1,771 → 1,545     landed   9% → 7%     came home 1,465 → 1,355
    units loaded   159.7 M → 134.5 M   dropped 0 → 0     parked boat-turns 1,326 → 1,273

An eighth fewer embarkations, not the halving a quarter-of-the-army cap
suggested: the script's loads mostly fit under the cap and mostly come
from ports with no enemy on the land border. The gate says whether the
coastal seats recover anything. The new routing counters read "0 orders
arrived, 0 erased as stuck" against 1,273 parked boat-turns — so loaded
boats never have a move order reach the router's end at all, which is
either a counter bound to the wrong variable or the reflex never issuing
one. Reading the router loop. Phase 4 released into the shared files.

**Parked boats, narrowed but not solved.** The router's counter binds
the right ship and runs in processNavyMovement; loaded-boat orders are
erased only there (0 arrived, 0 stuck) or by the reflex's own
re-target. So the reflex is erasing and re-issuing every turn, or no
order ever reaches the router. The next step is a per-boat aiDebug
trace, which needs AISystem.cpp — just released to Phase 4. Parked in
the backlog with the chain of reasoning; it costs landings, not the
rating gate, and it waits.

**Doctrine per cohort** (same run, v8.2 → v8.3):

    cohort   embark chosen/offered        troops embarked
    MODEL    1,539/1,955 (79%) → 1,545/2,641 (59%)    840 → 910
    SCRIPT     740/876   (85%) →   422/830   (51%)    438 → 249

The script's loads fall by 43% (the odds condition binds it; the
national share and the threatened-port rule bind both), the model's
rate falls but its men shipped rise — it loads from quieter, fuller
ports now. The doctrine did what it was written to do: the rung invades
less and from the right places; the model keeps its judgment. The
eval's headline "embarks" is the model's count, which is why the total
fell only an eighth.

**v8.3 gate, shipped model: 75** (v8.2 98). Down, not up: the doctrine
binds the shipped model's own embarks too (conditions 1 and 3), and a
rung that invades from the right places may be a harder rung than one
that throws garrisons into the sea. Per seat above; the champion and
N11 decide the reading.

The shipped model's loss is one seat: Sweden 260 → 113. Under constant
attempts the shipped Sweden was the invader across the Baltic; under the
doctrine its own loads are gated (a quarter of a small army is not an
invasion, and its ports face Russia) and the offensive is gone. The
doctrine trims the seat's sea power as much as the rung's — as a rule
should. Whether the coastal DEFENCE seats recover is the champion's and
N11's number.

**v8.3 gate, champion: 98** (v8.2 90, v8 126 before landings). Per seat
above. The doctrine gives the champion back eight of the thirty-six the
landings took; the rest is a model that never learned to hold a coast.
N11 follows.

    champion per seat   v8.2   v8.3
    FRA rung             238    202
    SWE rung               0     47
    USA rung             136    183
    CHN rung              59     91
    FRA rush              89     48
    NOR hood              15     15
    survival / worst    60/0   67/15

Coastal defence seats recover (Sweden 0 → 47, China 59 → 91, USA 136 →
183); France's two seats fall (238 → 202, 89 → 48) — the seat's own
loads are gated too, and France was an invader. Norway unchanged at 15:
a seaborne Sweden still takes it under any doctrine.

## 36u — v8.3 gate complete: the doctrine's price is on the invader

    v8.3 (amphibious doctrine)    rating   (v8.2)
    shipped (loop-base)             75      98
    champion (i10-final)            98      90
    N11                            126     145

The doctrine costs N11 19 and the shipped model 23 — both were sea
INVADERS on Sweden's seat — and gives the champion 8 back on its coastal
defences. Per seat above. So "the real cost of condition 3 is unknown"
now has a number for the model of record: about a fifth, and it is the
offensive half that pays, not the defensive. N11 remains the model of
record on the sixth ruler (126 vs 98). The parent choice for the next
tickets runs on v8.3 now (N24, N25 re-benched there).

N11 on v8.3 in full: 126 / 88 / 61 — Norway 15 → 69 under the doctrine
(a seaborne Sweden that must keep a quarter of its army home and cannot
load from a threatened port no longer takes it), survival back to 88
and the floor at 61. The rating fell on Sweden's offence (253 → 163)
and rose on Norway's defence; the doctrine made the game less lethal
for the small seats and less lucrative for the invader, which is what a
doctrine is for.

## 36v — Phase 4 complete; ruler stands; N24's long world

Phase 4 (recruits cost munitions, artillery and bombardment cost fuel
and munitions, a standing army burns fuel with shortfalls bought at a
premium, real demand for the three military goods, doctrines as the
exchange rate) is in; goods-off is byte-identical, so v8.3 stands and
N11 stays the model of record without a re-gate. The roadmap session
also unified seven copies of the artillery price list — one of them was
validWar's mask, priced differently from the executor, so the head had
been learning from artillery choices the executor then refused — and
made the recruit and artillery masks check materials. Its ARMY_FUEL
constant was chosen on "is the mechanic live" after three runs came out
non-monotonic; the Phase 4 rates are unswept and go through the
three-seed harness when goods-on is benchable. All of this loop's
pending work survived the edits (markers checked).

N24 (from N11, seed 20260922) ran 2,362 turns on a world that would not
freeze — the longest single-world run of the day — and is in its bench.
Per-boat trace (OD_BOAT_TRACE) added to the amphibious reflex; a
behaviour-neutral rebuild and a 40-turn trace run follow N24's bench.

## 36w — N24: 169 on v8.3. The ladder climbs again

    v8.3            rating  surv  worst   turns   world
    N11               126     88     61
    N24 (from N11)    169     86     23   2,362   pangaea, seed 20260922, 79 landings in training

The first step to pass the gate since N11 itself, by 43 points. Its
world ran 2,362 turns without freezing — the longest of the day — and
it is the first model trained where an enemy could arrive by sea (the
nav grid in generated worlds landed this evening). Its bench started
after the v8.3 build, so the number is v8.3 (label corrected). Per
seat above: it trades N11's Norway floor (69 → 23) for growth
everywhere else. The parent-choice chain re-benches N24 and N25 on
v8.3 and picks the parent from {N11, N24, N25}; N24 will win it, and
N26/N27 start from N24. Copied into the reference as N24-169-v83.bin.

## 37 — the boat trace: no boat ever has an order

40 turns, champion vs script, shipped map, one line per loaded boat per
turn (646 lines, 291 boats):

    order present      2 of 646 boat-turns
    sailingTo target   1 of 646
    enemyPid < 0       0        (every boat has a reachable enemy port)
    enemyD <= range    0        (none is ever within landing range)
    enemyD             48 .. 219 degrees   against a hull range of 8.79
    per boat           target static for 66 of 83, distance unchanged turn to turn

So a loaded boat learns a target, the reflex pushes a sail order, and
by the next turn the order is gone and the boat has not moved. The
router's counters read 0 arrived / 0 stuck for loaded boats, and the
only other place an order dies is the reflex's own re-target — which
would then re-push it. Something consumes the order between the push
and the router's end-of-order line, or the router never runs for AI
countries' orders. Also worth an eye: enemyD of 200+ degrees is either
a genuinely antipodal "nearest reachable" port or a longitude
difference computed without wrapping at the dateline. Second trace
running with the push and the router entry/exit both printed.

**Trace 3 and the phase order.** Per country and per turn the game
runs: AI decisions (the reflex's push) → processShipDisembarks →
processNavyMovement (the router) → processEmbarkations. So a pushed
order is routed in the same phase, which the t2 push / router "t1"
pair confirms (the two counters differ by one). Boat 78 sailed 7.8°
and 6.5° on its first two turns, re-targeted at t4 as a nearer port
appeared, and was then never routed again; by t10 the country's boats
carried crews of 196, 80 and 10 and were pushing orders every turn
that the router never saw. Both ship-removal sites re-index the queue
correctly, and nothing reorders m_ships. Between the push and the
router sits processShipDisembarks, which ERASES a hull after it
unloads and drops its orders — the one remaining way an order can
vanish in-phase. Trace 4 prints the queue's contents at router entry.

**Trace 4: the queue at the router's door.** Over 8 turns and every
country's phase, 5,949 pending-order entries were listed at router
entry; exactly 2 pointed at the routing country's own LOADED boat. 190
pointed at its empty hulls (warship moves), the rest at other
countries' hulls (the queue is global). For cid 41: at t2 the pushed
order was routed (7.8°), at t3 again (6.5°); at t4 the reflex re-pushed
for boat 75 ("orders now 14"), and the router in the same phase listed
13 orders with index 75 now owned by country 43 and no order for a
41-owned loaded boat at all. Between the push and the router only
processShipDisembarks runs for that country, and it is the one site
that erases a hull mid-phase; trace 5 prints every erase with the
hull's owner and crew.

## 37a — found: the navy head unloads its own invasion at home

Trace 5, boat 75 of country 41: turn 4, the reflex pushes a sail order
toward a target 41° away ("orders now 14"); in the same phase a
DISEMBARK order for ship 75 executes at province 231 — the boat's HOME
port — crew 1,748 → 0, the hull erased, the order dropped. The
disembark came from the navy executor's action 4 ("land"): land at an
at-war port within one hull's range, ELSE unload at any own port
within range. For a boat loaded this turn, still off its harbour, the
else always fires. The script prefers action 4 whenever it is valid and
the head often picks it, so every loaded boat was emptied and scrapped
one turn after loading. "1,465 came home" of 1,771 embarkations; 646
loaded-boat-turns with 2 orders; 438 pushed, 2 routed. Not a lazily
built index this time — a fallback in this loop's own file.

Fix: the fallback is deleted. The amphibious reflex already sails a
boat with no reachable target home and unloads it there; a head with
nothing in range now does nothing. This changes the ruler (the rung's
boats will sail and land under the doctrine), so v8.4 re-gates four
models — shipped, champion, N11, N24 — then N26/N27 train from N24
(169 on v8.3, the model of record) on the fixed binary. The parent
chain's redundant re-benches were cancelled.

## 37b — Phase 6 announced (not flag-gated); v8.4 built ahead of it

Phase 6 = combat width in resolveAssault (only so many men engage,
set by true area and fortification) and attrition on foreign stacks.
Not goods-gated: it changes existing rules, so every number moves and
there is no byte-identical arm. Its design choice, agreed: a repulsed
assault loses its engaged men and the rest fall back to where they came
from (a fallback province passed by processArmyMovement); a failed
LANDING still drowns — the sea does not give them back — which keeps
the amphibious measurement surface intact. The roadmap session will
send a goods-off before/after pair at 40 and 120 turns as the stated
baseline for the next re-gate.

v8.4 (the boat fix) is building now, before Phase 6 touches Game.h and
Game_TurnLogic.cpp; it is the last gate on the pre-width rules. Four
models, then N26/N27 from N24.

## 37c — v8.4: the amphibious system works

    check (champion vs script, shipped map, 80 turns)   v8.3      v8.4
    embarkations                                        1,545      154
    reached a hostile shore                               7%       74%
    came home                                           1,355       60
    loaded boat-turns: sailing / parked                 2 / 1,273  407 / 224
    router: loaded-boat orders arrived / stuck           0 / 0     10 / 158

Deleting the "unload home" fallback did it. Men now stay aboard, so the
doctrine's national share fills and loads fall tenfold; the boats
sail, and three-quarters of them land. The 158 stuck orders are the
next thing (a straight-line leg stopping at a coast, most likely) but
they are not tonight's. Nine thousand embarkations at 0% for the life
of the game came down to two lines: a resolver that deleted the men,
and an action that sent the survivors home. The v8.4 gate runs on four
models; the rung invades for real now, under the doctrine.

**Near-miss on the v8.4 build, and a protocol.** The roadmap session had
written one Phase 6 edit (a fifth parameter on resolveAssault's
declaration, definition unchanged) before my "hold" arrived, and
reverted it on reading it. Had my compile caught that window it would
have FAILED loudly (declaration/definition mismatch), not produced a
bad binary; the chain reported BUILD_DONE with no error lines and a
fresh binary, so v8.4 is sound. Agreed protocol, both directions: a
file claim is not live until the other session acknowledges it. Its
pre-Phase-6 baseline was measured before the boat fix and is being
re-taken on the current tree, so width and attrition are not credited
with 7% → 74%.

**v8.4 gate, first number: shipped model 121** (v8.3 75). With boats
that sail and land, the shipped model's own invasions work again — per
seat above. Three benches to go.

    shipped per seat   v8.3   v8.4
    FRA rung            194    177
    SWE rung            113    277   <- the Baltic offensive is back, by sea that works
    USA rung             93    186   <- across the Atlantic likewise
    CHN rung              4      4
    FRA rush             17     48
    NOR hood             31     33

The two seats that face their wars across water double; the land seats
barely move. Boats that sail are worth more to the seat than to the
rung on these maps — for the shipped model at least. Champion, N11 and
N24 decide whether that holds for trained models.

**v8.4 gate: champion 116** (v8.3 98). N11 and N24 to go; per-seat table
once all four are in.

**v8.4 gate: N11 160** (v8.3 126). N24 to go.

## 37d — v8.4 gate complete: boats that sail favour N11

    v8.4 (boats sail and land under the doctrine)   rating   (v8.3)
    shipped (loop-base)                               121      75
    champion (i10-final)                              116      98
    N11                                               160     126
    N24                                               147     169

Every model gains from an amphibious system that works except N24, and
the ranking flips: N11 160 over N24 147. Per seat above. N24 was the
one model trained on a world where landings happened — under the OLD
executor, where every loaded boat came home a turn later, so what it
learned about the sea was learned against boats that never arrived.
N11 is the model of record again; N26/N27 (from N24) are already
training on v8.4 and stand as N24's tickets. Ruler of record: v8.4
until Phase 6 lands.

## 37e — Phase 6 complete: width ships on correctness, attrition removed; v9

From the roadmap session. Attrition was built at 1.5%/turn, measured
against 0%, and produced BYTE-IDENTICAL 120-turn output — in a
deterministic sim, proof it never fired once: a carried assault leaves
the attacker on his own soil and a repulsed one is destroyed or falls
back, so persistent foreign stacks barely exist. Removed rather than
shipped inert. Combat width fires on 20.3% of assaults (7,364 of
36,306) and its aggregate effect is NOT separable — a three-seed A/B
on one tree is mixed in sign on survival and concentration. Shipped
on correctness (numbers ceasing to be decisive past a frontage);
NOT a balance win, and this journal will not call it one.

Its method note is the one to keep: its first reading looked like a
ten-point survival drop until the width-off arm was re-run on the
current tree and gave most of the "drop" back — the shared tree had
moved between baseline and test (my boat fix). Rule adopted both ways:
an A/B is quotable only if both arms ran in one session on one tree.

Surface changes: resolveAssault takes an optional fallback province
(processArmyMovement passes it; the landing path does not, so a failed
landing still drowns); noteAssaultRepulsed receives the ENGAGED count.
New line `[WIDTH] assaults=N bound=N (P%)`, parsed by od_bench.

v9 = v8.4 + width + navy mask parity + a navRoute exit trace. Chain:
build, check (landings, width, NOROUTE reasons), gate on four models.
The three tickets in training will bench on v9 (labels to correct).

## 37f — v9 check: width caps one assault in six; routes never fail

    check (champion vs script, shipped map, 80 turns)     v8.4     v9
    [WIDTH] assaults / capped                              --     24,413 / 3,874 (15.9%)
    embarkations / reached a hostile shore              154 / 74%   191 / 64%
    came home                                              60        87
    routing: loaded-boat orders arrived / stuck          10 / 158   29 / 71
    NOROUTE (navRoute failed for a loaded boat)            --         0

navRoute never fails for a loaded boat whose reachability passed, so
the "stuck" erasures are routed orders that moved zero on their first
leg — the coast-stop on a nav-grid leg that clips a land corner, most
likely — and the waypoint-skip logic already in the router (MAX_SKIPS)
does not catch them. A refinement for v9.1, after the gate; landings
are at 64% regardless because the reflex re-pushes and most boats get
there in the end. Also seen: a destroyer sinking a loaded boat with
its crew, which is the sea giving nothing back as designed. Four-model
gate running.

**Stuck orders, mechanism and fix.** The router steps a partial leg
pixel by pixel and stops at the first land pixel; a hull parked at a
coastal pixel with a spit of land between it and its own nav cell
moves zero on that first leg, the skip rule spends its four skips on
the following legs, and the order is erased as stuck — with navRoute
never having failed (0 NOROUTE lines). Fix in the router: a leg no
longer than two cells to a non-final waypoint is taken as a jump, as a
completed intermediate leg already is; the land test keeps guarding
long legs and the final approach. Queued as v9.1 (build, check, gate)
behind the v9 gate; the "arrived / stuck" line says whether it worked.

## 37g — memory incident; Phase 5 plan

The v9 gate had been crawling: 66 MB free, 5.0 of 6.1 GB swap in use,
three trainers (N26, N27 from N24; N28 from N11) plus bench seats under
the 5-game lock, plus the other session's evals. Killed N26/N27 — the
N24 lineage was demoted on v8.4 anyway — and kept N28. Lesson for the
protocol: the lock caps games, not memory; three trainers and a gate
is one too many on 16 GB when another session is also running evals.

Phase 5 (releasing nations), the roadmap session's proposal, agreed:
the release RULES and the player action now, plus release as the last
rung of the bankruptcy cascade (budgets → policies → minority spending
→ ships → troops → shed the ungovernable province) — a rules reflex
aimed at the seat that dies to rebellion on every ruler; the strategic
politics action later, in a migration window this loop schedules with
a retrain budgeted, since it is the MAX_MODULE_ACTIONS widening. Its
claim on Game.h / Game_TurnLogic.cpp / Game_Policies.cpp / Game_Render.cpp
is acknowledged only after v9.1 builds (the short-hop router fix is in
Game_TurnLogic.cpp). Told it m_stats is per-turn (one-turn staleness on
a mid-turn release, same as rebellion) and to go through
createRebelCountry's path.

**v9 gate, first number: shipped 116** (v8.4 121). Width barely moves
the shipped model. Three benches to go, faster now that the machine is
no longer swapping under three trainers.

**v9 gate: champion 165** (v8.4 116) — the biggest single-ruler gain
the champion has had; with width, a rung's ten-million-man stack no
longer beats a defended province by numbers alone, and the champion
defends. N11 and N24 to go; per-seat table when all four are in.

**Phase 5's rule half is in (no shared files):** src/ReleaseRules.h and
a 19-check test. Its test caught a rule worth remembering here too —
minorities partition a province to 100%, so a country's own people are
just another entry in that list, and the first version offered to
release a homogeneous nation's heartland; the largest group by
population is now the titular one and is never releasable, even where a
minority has outgrown the founders. Two of its test worlds were wrong,
not the code ("the test failed, change the code" would have removed a
right rule). The remaining half touches Game_TurnLogic.cpp (a peaceful
flag on createRebelCountry; release as the last bankruptcy rung),
Game.h, Game_Policies.cpp, Game_Render.cpp — acknowledged after v9.1
builds. AISystem untouched.

**v9 gate: N11 139** (v8.4 160). Width takes 21 from N11 and gives the
champion 49: the champion defends behind forts where numbers no longer
decide, N11 attacks with them. First ruler since v6 on which the
champion leads. N24 to go.

## 37h — v9 gate complete: N24 229, the highest number of the project

    v9 (v8.4 + combat width)        rating   (v8.4)
    shipped (loop-base)               116      121
    champion (i10-final)              165      116
    N11                               139      160
    N24                               229      147

Combat width reshuffles everything and N24 comes out 64 clear of the
champion and 90 clear of N11, on the ruler with all six roadmap phases
in. Per seat above. N24 is the model of record; its tickets (N26/N27,
killed in the memory incident) are relaunched from it on v9.1 once
that builds; N28 (from N11) runs on. Width is 22–24% of assaults capped
in these benches. The ruler is v9.1 (router short-hop) next, then
holds while Phase 5's rules land.

## 37i — v9.1: the short-hop rule never fires; gate skipped; Phase 5 released

v9.1's check is identical to v9 to the unit (191 embarkations, 64%
landed, 29 arrived / 71 stuck), so the two-cell hop never applied: the
stuck first legs are longer than two cells, or the zero-move happens on
a leg that is not the first. v9 numbers stand for v9.1; the gate was
cancelled rather than spent on an identical binary. The stuck-boat
question keeps its instrument and waits. Phase 5's claim on Game.h,
Game_TurnLogic.cpp, Game_Policies.cpp and Game_Render.cpp is
acknowledged; no rebuild until it completes. N28 trains; N29/N30 from
N24 follow it.

**N28 (from N11, seed 20260926, 1,457 turns, 325 landings seen) on v9:
156 / 71 / 15** — 17 over its parent (N11 139 on v9), 73 under N24.
Label corrected (its bench ran after the v9 builds). The N11 lineage
climbs a little; the N24 lineage leads by a lot. N29/N30 from N24 now
training on v9.1 (≡ v9).

**N29/N30 relaunched.** Cancelling the v9.1 gate by `pkill -f
REBASE91_COMPLETE` also killed the tickets chain that was waiting on
that marker (its script contains the word). Relaunched both from N24
on the v9.1 binary with no marker word in the script; protocol now
says kill by PID. Two trainers, no gate: within the memory rule.

**noteAssaultRepulsed after Phase 6:** it feeds only two eval counters
(attacksRepulsed, troopsLostAttacking), no reward, so receiving the
ENGAGED count rather than the committed one makes the counter mean
"men who fought and died", which is the better number. Nothing to
change on the AI side.

## 38 — Phase 5 complete: all six phases in; v10

Releasing nations landed as rules plus the player action, and release
as the last rung of the bankruptcy cascade — gated on a FIVE-TURN
bankruptcy streak, because "still short after the cascade" fired zero
times in 120 turns with 46 bankruptcies (disbanding men clears any
single shortfall); the failure it aims at is chronic insolvency. It
fires once (two provinces) per 120-turn eval, by design. Peaceful
releases do not count as revolts, so the policy is not taught that a
choice was a loss. Its value on CHN — the seat that dies of exactly
this spiral on every ruler — is untested and is this bench's to
answer: survives smaller, or disintegrates. A probe (OD_PROBE_RELEASE)
prints why the cascade does or does not fire; kept.

The roadmap session also corrected its own Phase 6 note: its
three-seed aggregate A/B saw nothing, this loop's seat bench saw N24
at 229 and the champion +49; a change can be invisible in the world's
aggregate and decisive in one country's decisions, and the reverse
holds too. Both readings now sit in Game.h and the roadmap.

v10 = v9.1 + Phase 5 + a stuck-boat anatomy trace (leg length in
cells, route size, first-step land). Chain: build, check with the
release probe, gate on shipped / champion / N11 / N24 / N28. N29/N30
(from N24) train alongside — two trainers and a gate, the ceiling.

## 38a — stuck boats, anatomy: off-grid water

v10 check, every stuck loaded-boat order: hull on WATER, first step on
WATER, first leg 9.5–12.9 cells long, route 17–90 waypoints. A nav
route's consecutive waypoints are one cell apart, so a first leg of
ten cells means the hull is in water the 32-px grid does not cover —
a bay narrower than a cell — and its route begins at the nearest grid
cell, up to eight cells away (navCellNear's search radius); the
straight leg to it crosses the bay's shore, the coast-stop rule moves
the hull zero, the four skips burn on the far-away next waypoints, and
the order dies as stuck. The two-cell hop of v9.1 could never fire.
Fix: the grid-entry hop is eight cells — only that leg can be longer
than one cell to a non-final waypoint, so nothing else matches.
v10.1 check queued behind the v10 gate; a gate follows only if the
"arrived / stuck" line moves. Also from the check: the release probe
finds 8 of 95 countries with a releasable region (23 regions; 643
provinces fail dominance, 417 alignment, 187 pass both).

**v10 gate, shipped model: 116** — identical to v9 in rating. Per seat
above; the CHN line is the one the bankruptcy-release rung was for.

    shipped per seat   v9   v10
    FRA rung          233   233
    SWE rung          170   170
    USA rung          232   229
    CHN rung            3     7    held 0.07 → 0.17 of par, seeds [0.0 0.4 0.1]
    FRA rush           27    27
    NOR hood           31    31

Five seats identical to v9: Phase 5 is neutral where nothing goes
bankrupt for five turns. China moves from 3 to 7 — the shipped model's
China still disintegrates; on one seed it keeps a sliver. The rung
fires rarely by design (one release per 120-turn eval); whether a
trained China survives smaller is the champion's, N11's and N24's line.

**v10 gate: champion 177** (v9 165). Per seat above. N11, N24, N28 to go.

    champion per seat   v9   v10
    FRA rung           245   245
    SWE rung           240   240
    USA rung           324   324
    CHN rung            96   171   <- the bankruptcy-release rung
    FRA rush            71    71
    NOR hood            13    13

Five seats identical to the unit, one seat up 75: the seeds are
deterministic, so the difference is the rung and nothing else. A
trained China that used to bankrupt itself placating minorities and
then disintegrate now sheds the ungovernable region and survives
smaller. That is what Phase 5's cascade rung was written for, and it
is the cleanest causal read this bench has produced.

**N29 (from N24, seed 20260927, 1,286 turns, 778 landings seen) on v10:
120 / 62 / 13.** Label corrected. A large step down from a 229 parent
(N24's own v10 number pending) — the ladder's usual spread, on the
lineage's first ticket since the memory incident. N30 still training.

**v10 gate: N11 139** (v9 139) — unchanged; its China line above says
whether the rung fired for it. N24 and N28 to go.
N11's China holds 2.0 of par 2.5 on both rulers: it never ran a
five-turn bankruptcy streak, so the rung never fired for it — the rung
touches only a country already in the spiral, which is the intended
shape (a lever of last resort, not a subsidy).

**v10 gate: N24 228** (v9 229) — the model of record holds; its China
line above. N28 to go, then the table.

## 38b — v10 gate complete: the ruler of record with all six phases

    v10                         rating  surv  worst   (v9)
    shipped (loop-base)           116     61      7    116
    champion (i10-final)          177     81     13    165
    N11                           139     82     13    139
    N24                           228     80     13    229   <- model of record
    N28 (from N11, v9 156)        156
    N29 (from N24)                120     62     13

Phase 5 moved exactly one thing: the champion's China (96 → 171). N24
holds at 228 and is the model of record on the ruler that carries the
whole roadmap. Per-seat table above. The v10.1 routing check (eight-cell
grid entry) runs next on this tree; N30 (from N24) is training.

**N30 (from N24, seed 20260928, 1,365 turns, 826 landings seen) on v10:
147 / 74 / 16.** Both tickets from N24 fell (120, 147 against 228). The
ladder from the model of record is 0 for 2 on this ruler; N24 stays.
The v10.1 routing check runs now; the next tickets wait for it (two
trainers, no gate) and for a decision on the parent — N24, or the
champion lineage whose China now survives.

## 38c — v10.1 identical; the entry leg is longer than a turn's sailing

v10.1's check matched v10 to the unit (29 arrived / 71 stuck). The
eight-cell hop required the leg to be completable in one turn (take ≥
legDist), and the observed entry legs are 9.5–12.9 cells = 13–18°,
longer than a hull's 8.8° turn. So v10.2 sails the grid-entry leg BY
BUDGET without the coast stop — the hull is in real water the grid
cannot see and the waypoint is a navigable cell — up to sixteen cells,
partial legs ending the turn. Check running; N31/N32 from N24 train
beside it and will bench on whatever binary exists then (relabel).

## 38d — v10.2: the entry leg sails; the routing line moves at last

    check (champion vs script, shipped map, 80 turns)    v10 / v10.1     v10.2
    embarkations / reached a hostile shore               191 / 64%      243 / 54%
    landings (absolute)                                    122            131
    came home                                               87            120
    routing: loaded-boat orders arrived / stuck           29 / 71        43 / 48
    loaded boat-turns sailing / parked                    407 / 224      867 / 199

Sailing the grid-entry leg by budget frees a third of the stuck
orders and doubles the boat-turns at sea; more boats load (the share
cap refills as boats deliver), and the landing SHARE falls while
absolute landings rise. Behaviour changed, so the ruler moves: v10.2
gate on shipped / champion / N11 / N24 running (two trainers and a
gate, the ceiling). The remaining 48 stuck are a different leg.

**v10.2 gate, shipped model: 91** (v10 116). Boats that reach their
grid cost the shipped model 25 — per seat above; the rung's boats
arrive as much as the seat's. Champion, N11, N24 follow.

    shipped per seat   v10   v10.2
    FRA rung           233    134   <- boats across the Channel and the Mediterranean arrive
    SWE rung           170    200
    USA rung           229    173
    CHN rung             7      8
    FRA rush            27      4
    NOR hood            31     28

France pays most: on v10 the rung's boats from Britain and the
Mediterranean rarely reached their grid; on v10.2 they do. Sweden
gains (its own Baltic boats arrive too). The trained models decide
whether that is a harder ruler or a different one.

**v10.2 gate: champion 160** (v10 177). N11 and N24 to go.

**N32 (from N24, seed 20260930, 723 turns, 62 landings seen) on v10.2:
143 / 82 / 21.** Label corrected. Judged against N24's v10.2 number
when it lands; N31 still training.

**v10.2 gate: N11 167** (v10 139) — N11 gains 28 where the champion
lost 17 and the shipped model 25; boats that arrive suit the model
that trained where they never did. N24 to go.

## 38e — v10.2 gate complete: boats that arrive reshuffle the table

    v10.2 (entry leg sailed by budget)   rating   (v10)
    shipped (loop-base)                     91      116
    champion (i10-final)                   160      177
    N11                                    167      139
    N24                                    185      228   <- still the model of record
    N32 (from N24, 723 turns)              143

Per seat above. Every model loses France and gains Sweden as the
rung's boats arrive; N24 keeps the lead but by 18 over N11 instead of
90. The ruler moved because more boats reach their grid — a real
change in the game, and the boat-fix sequence is now four rulers deep
(v8.4, v9, v10, v10.2). Next: the stuck trace's remaining 48 orders,
then a pause on ruler changes so the ladder can be judged on one.

## 38f — the last stuck leg: a planned leg that clips a land corner

Extended trace, 48 of 48 stuck orders: hull on water, first pixel on
water, route planned, and the router's own first stride (a 32nd of the
turn's take along the leg) on LAND. The straight line between two
navigable cell centres clips a land corner — ordinary on a 32-px grid
— and the pixel-level coast stop treats it as a wall, moves zero,
burns four skips and erases the order. v10.3: every planned leg to a
non-final waypoint is sailed by budget; the coast stop keeps only the
final approach and unplanned straight fallbacks. Build, check, and a
four-model gate running. After this the ruler HOLDS: the ladder needs
one ruler to be judged on, and the boat-fix sequence has moved it five
times in a night.

## 38g — v10.3 check: stuck orders 71 → 3

    check (champion vs script, shipped map, 80 turns)   v10    v10.2   v10.3
    embarkations / reached a hostile shore            191/64%  243/54%  270/51%
    landings (absolute)                                 122      131      138
    came home                                            87      120      128
    routing: arrived / stuck                            29/71    43/48    39/3
    loaded boat-turns sailing / parked                 407/224  867/199  1117/207

Three stuck orders left of 71. Boats sail; the share cap refills as
they deliver, so more load and the landing share settles near half
while absolute landings keep rising. The four-model gate is running;
after it the ruler holds.

**v10.3 gate, shipped model: 65** (v10.2 91, v10 116). Every boat that
now arrives lands on a model that never learned to hold a coast. The
trained models follow.

**v10.3 gate: champion 152** (v10.2 160). N11 and N24 to go.

**v10.3 gate: N11 158** (v10.2 167). N24 to go; the parent for the held
ruler is whichever of N24 / N11 leads on v10.3.

## 38h — v10.3 gate complete; the ruler holds here

    v10.3 (boats sail and land; stuck 3/80 turns)   rating  surv  worst
    shipped (loop-base)                                65     40      0
    champion (i10-final)                              152     73     13
    N11                                               158     83     13
    N24                                               187     --     --   <- model of record

Per seat above. This is the ruler the ladder is judged on from here:
every rule and resolver holds; changes queue in the backlog. Parent:
N24 (187). N31 (from N24) is still training and benches on v10.3; N33
starts from N24 now, N34 when N31 finishes — two trainers, no gate.

## 38i — audit: discarded queue results in the AI

Prompted by the roadmap session's UI bug (the Declare War button
ignored queueDiplomaticAction's refusal). In AISystem.cpp five calls:
four check the bool (politics requests, trade, break-NAP, ceasefire);
the war head's declaration discarded it — but pre-checks the same
rule (pending talks with the pair, or any declaration pending this
turn) and returned early, so nothing was ever queued silently. What it
did wrong was smaller: the early return was a plain string, not
didNothing, so a refused declaration was not counted as a no-op. Fixed:
both the pre-check and the queue result now return didNothing, which
only sets m_execNoop (noopChosen counters, training bookkeeping); play
behaviour is unchanged, so the held ruler stands. Rides into the next
build.

**N31 (from N24, seed 20260929, 2,332 turns, 169 landings seen) on
v10.3: 120 / 78 / 18** (parent 187). Label corrected. Six tickets from
N24 across rulers, none above it; the ladder on the held ruler is now
N33 (training) and N34 (starting behind N31).

## 38j — the ladder's step is too big

    tickets from N24     N26/27 (killed)  N29 120  N30 147  N31 120  N32 143  N33 (training)
    tickets from N11     N15 131  N16 67  N17 102  N18 58  N19 126  N21 114  N28 156

Twelve steps from the two best parents; one exceeded its parent (N28,
+17 on v9), the rest fell 30–130 points. The only large climbs were
first steps of a lineage (N9 → N11 +35, N9 → N24 +4 → +60 on width).
A full-strength retrain from a strong parent mostly overwrites what
made it strong. Paired test on the held ruler: N34 (seed 20261002,
standard recipe) and N35 (same seed, same parent, OD_LR_SCALE=0.25).
If N35 lands near or above the parent where N34 falls, the ladder's
step size is the problem and every later ticket runs at the lower
rate. Two trainers at a time throughout.

**N33 (from N24, seed 20261001, 2,422 turns, 302 landings seen) on
v10.3: 111 / 54 / 0** (parent 187). Seven from N24, none above it. N34
(standard) and N35 (quarter rate), same seed, now run the paired test.

## 38k — the quarter-rate step holds the parent: N35 186 vs N24 187

    from N24 on v10.3 (held)      rate   turns   rating  surv  worst
    N33  (seed 20261001)          1.0    2,422    111     54     0
    N35  (seed 20261002)          0.25   1,670    186     80    15
    N34  (seed 20261002)          1.0    (running)  -- the paired arm

The first ticket in twelve that did not fall away from a strong
parent. Per seat above: N35 keeps N24's shape almost seat for seat.
Whether the quarter rate can CLIMB is the next question; that it
stops the erosion is settled if N34 (same seed, full rate) falls the
way every other full-rate step did. Waiting on N34 before adopting.

## 38l — paired verdict: the quarter rate wins by 36 on the same world

    from N24 (187), seed 20261002, v10.3      rate   turns   rating  surv  worst
    N34                                       1.0    1,480    150     78    26
    N35                                       0.25   1,670    186     80    15

Same parent, same world, one env var: +36. With twelve full-rate steps
behind it (one above parent) and the first quarter-rate step at parity,
the ladder's recipe changes: OD_LR_SCALE=0.25 for every ticket. The
open question is whether a quarter-rate step can climb rather than
hold; N36 (from N35) is the first test, N37 (from N35, another seed)
the second. Two trainers, held ruler.

## 38m — longitude never wrapped in the movement math (roadmap session)

The 213° "nearest enemy port" of trace 1 was real and I misread it:
navRoute's BFS wraps at the antimeridian, but everything that
measured or followed a route did not — `legLon - ship.lon` unwrapped,
so a one-degree hop across 180° measured 359° and pointed the long way
round the world. Hulls near the Aleutians burned a turn's range sailing
east across the Pacific and reported stuck. The roadmap session fixed
the mover and navLineClear (helpers lonDelta / wrapLon / seaDistanceDeg
on Game), measured ship moves stopped by land 24–31% → 8%, and caught
and fixed a regression its own change exposed: my trusted-leg mover
never checked where the hull STOPPED, safe only while the chord
measured 359°; the resting place is now walked back to the last water.
No cos(latitude): these maps are equirectangular with w = 2h, range is
pixels, the player's circle is the contract.

AI side: seven `hypot(lon − lon, lat − lat)` sites (range gates,
target picking, the landing search) rewritten on Game::seaDistanceDeg;
compiled, not built. Near the antimeridian the AI believed everything
unreachable. v11 = wrap fix + these + the counters-only edits, built
after N36/N37 finish on v10.3, then a five-model gate.

## 38n — the quarter-rate ladder climbs: N37 224 on the held ruler

    v10.3 (held)                      rate   turns   rating  surv  worst
    N24  (parent of the lineage)                     187     81     13
    N35  N24 -> 1 map                 0.25   1,670    186     80     15
    N37  N35 -> 1 map                 0.25   1,162    224     78     15

Two quarter-rate steps: the first held the parent, the second climbed
37 points above the lineage's best on the same ruler. Per seat above.
Thirteen full-rate steps produced one climb; the second quarter-rate
step produced the largest climb since N9 → N11. Model of record: N37.
N36 (the other quarter-rate ticket from N35) is still training; the
v11 build waits for it, then gates shipped / champion / N11 / N24 /
N35, and N37 is benched on v11 right after. The next ladder step runs
from N37 at 0.25 on v11 once the gate has its number.

## 38o — quarter-rate ladder, four steps; v11 built

    v10.3 (held)                   rate   turns   rating  surv  worst
    N35  N24 -> 1 map              0.25   1,670    186     80     15   held
    N36  N35 -> 1 map              0.25   3,000    188     88     26   held (best survival and floor of the lineage; hit the turn cap)
    N37  N35 -> 1 map              0.25   1,162    224     78     15   climbed +38
    N38  N37 -> 1 map              0.25   1,739    126     80     10   fell −98

Three of four quarter-rate steps held or climbed against one of
thirteen at full rate; the fourth fell as hard as any full-rate step.
The rate improves the odds, it does not remove the world's variance.
Policy stands: quarter rate, two seeds per step, keep climbs only.
Model of record: N37.

v11 (longitude wrap; the AI's nine distance sites on
Game::seaDistanceDeg; counters-only edits) built: 67% of 244
embarkations landed, 34 orders arrived / 5 stuck, 0 of 78 hulls
beached. Five-model gate running, N37 on v11 right after; N39/N40 from
N37 at 0.25 start on v11 now.

**v11 gate, shipped model: 66** (v10.3 65) — the wrap fix is neutral
for a model that cannot hold a coast either way. Four to go.
Its survival went 40 → 60 and its worst seat 0 → 13 at the same
rating: with distances measured the short way round, the rung's boats
land where they can be met instead of arriving from nowhere.

**v11 gate: champion 166** (v10.3 152). N11, N24, N35 to go, then N37.

**v11 gate: N11 170** (v10.3 158). N24 and N35 to go, then N37.

**v11 gate: N24 214** (v10.3 187). The wrap fix gives the trained
models back what the coast-stop fixes took: every model is up on v11.
N35 to go, then N37.

**N40 (from N37 at 0.25, seed 20261007, 1,372 turns) on v11: 144 / 72 / 10.**
Judged against N37-v11 when it lands (N37 was 224 on v10.3; N24 is 214
on v11, so the bar is high). N39 still training; N35 and N37 still to
bench.

## 38p — v11 gate: N35 235, the project's highest number

    v11 (longitude wrap; the ruler holds here)   rating  surv  worst   (v10.3)
    shipped (loop-base)                             66     60     13      65
    champion (i10-final)                           166     86     15     152
    N11                                            170     80     13     158
    N24                                            214     74     13     187
    N35  (N24 -> 0.25)                             235     --     --     186
    N40  (N37 -> 0.25)                             144     72     10
    N37  (N35 -> 0.25)                            pending                224

Per seat above. Distances measured the short way round lift every
trained model 8–49 points; N35, which merely held its parent on v10.3,
leads on v11 by 21 over N24. The parent for the next pair is the best
of N35 / N37 on v11. The ruler holds at v11.

## 38q — ghost claims (roadmap session): v12 queued behind the v11 pair

A conquered map country kept its claims for ever: eliminateDefeatedCountries
cleared treaties and rebels' claims but never a dead map country's, so
every province a dead state had claimed carried permanent unrest (2, or
6 at war) from a corpse nobody could answer. Fixed on disk: same seed,
same tree, 80 turns — claims 1,015 → 310 (69% ghosts), rebellions 141
→ 59 per 1k country-turns. The AI's claim readers (bloc opening,
reconquest goal, credibility, trade terms) were seeing a graph that was
mostly noise, worst late in the game. v11 holds until N37's v11 bench
and the N39/N41 pair land; then v12 (claims clear) and a six-model
gate. The ruler moves again, on a change that is plainly right.

**N39 (from N37 at 0.25, seed 20261006, 1,250 turns) on v11: 181 / 85 / 13.**
Its sibling N40 read 144; N37's own v11 number is still benching, N41
(from N35) training. The v12 build waits on those two.

## 38r — v11 complete with N37: N35 is the model of record

    v11             N24    N35    N37    N39    N40
    rating          214    235    217    181    144
    survival         74     86     76     85     72

N35 leads on v11 by 18 over N37 and 21 over N24; per seat above (N37's
China 319 is the lineage's best single seat; N35's rush 176 its
best on the seat every model loses). Parent for the next pair: N35.
N41 (from N35) training; N42 (from N35, another seed) starts now; v12
builds when N41 finishes and re-gates six models.

**N41 (from N35 at 0.25, seed 20261008, 618 turns) on v11: 167 / 83 / 21** — a fall
of 68 from the parent, the ladder's worst quarter-rate step. N42 (same parent,
seed 20261009) is the other arm. v12 now also carries `OD_DISBAND_TRACE`
(peer request, no behaviour change): one stderr line per disband order pushed
(panel, Disband All, load, client orders) or fired/skipped in the resolver.

## 38s — v12 built (03:23): v11 + ghost-claims clear + OD_DISBAND_TRACE

Post-build check (same eval, same model, same seed as the v11 gate):

    v11: amphibious 67% of 244 reached a hostile shore, 1 dropped out of range; routing 34 arrived, 5 stuck
    v12: amphibious 55% of 268 reached a hostile shore, 0 dropped out of range; routing 49 arrived, 9 stuck

More embarkations, more arrivals, a lower landed share and more stuck
orders. The longitude wrap was already in v11, so the only simulation
change here is the claims clear (different war targets, hence different
embarkations); 3 scenarios x 80 turns is too small to read a share from,
and no v11 server binary was archived (v7 and v12 are), so
the gate's six-model table is the read. Ruler held at v12; game binary
also v12. Peer told BUILD_DONE.

**v12 ruler (shipped model): 131 / 63 / 1**, from 66 / 47 on v11. Per seat
v11 -> v12: FRA 88 -> 146, SWE 13 -> 240, USA 138 -> 318, CHN 72 -> 1,
rush 61 -> 57, NOR 26 -> 21. The claims clear alone moved the ruler by 65;
the ghost claims were feeding scripted wars against the 1914/1939 seats.
Modern China now dies everywhere. World aggregates barely moved
(width 24.2%, pop 2.32e9, overcap 61) — seat-level again where the
aggregate is blind. Six-model table follows.

## 38t — v12 gate tabled: N24 leads (232), N35 falls to 201

    v12        shipped   champ     N11     N24     N35     N37
    FRA rung       146     247     182     197     261     253
    SWE rung       240     203     260     373     357     280
    USA rung       318     356     194     338     368     346
    CHN rung         1     164     187     375     133     303
    FRA rush        57       3     112      93      72      83
    NOR hood        21      18      18      18      15      15
    rating         131     165     159     232     201     213
    survival        63      70      86      85      81      83
    worst            1       3      18      18      15      15
    v11 -> v12: shipped 66->131  champ 166->165  N11 170->159  N24 214->232  N35 235->201  N37 217->213

The claims clear reordered the lineage. N24 gains 18 and leads; N35 loses
34 (its China 229 -> 133 and rush 176 -> 72 are the whole fall; France and
the USA went up). N37 holds (213). The shipped model doubled, the champion
did not move. Two rulers, two leaders: N35 by 21 on v11, N24 by 31 on v12.
Per seat N24 wins SWE, CHN and ties NOR; N35 wins FRA and USA. Both stay
parents: N42 (N35, seed 20261009) is training; N43 (N24, seed 20261010)
starts now. v13 = v12 + PUSH-ai trace + the peer's split-remainder fix
(every odd garrison stranded men on a split; the AI's 50% moves now empty
the source exactly) + the two disband findings; it re-gates these six when
the peer's landings are in. Modern China on the shipped model reads 1.

**N42 (from N35 at 0.25, seed 20261009, 2,586 turns) on v12: 165 / 83 / 15** — a
fall of 36 from N35's 201 on the same ruler. Both quarter-rate children of N35
fell (N41 167 on v11, N42 165 on v12); N35 looks like a peak that does not
continue. N44 (from N24, seed 20261011) takes the free slot beside N43.

## 38u — v13 cut (peer's three fixes + PUSH-ai)

v13 = v12 + orders die with the ground (captureProvince drops pending
disband AND move orders naming the lost province; the AI's queued moves out
of a lost province are dropped rather than skipped) + split remainder
(shares telescope; the AI's 50% moves empty the source exactly) + disband
marker prints the number (UI) + PUSH-ai in the disband trace. Peer verified:
determinism 6/6 at 4242, suites green, tutorial walk 108/0. Building,
then ruler + six-model gate; N43/N44 (from N24) train under v12 meanwhile.
Peer's point taken for reading the tables: the shipped model's 66 -> 131 says
it was reading a claim graph two-thirds held by dead countries, and lineage
reorderings on a moved ruler need a second look before any single model's
move is believed.

**N43 (from N24 at 0.25, seed 20261010, 3,000 turns) on v13: 228 / 81 / 18** vs
N24's 227 / 86 / 15 on the same ruler — a hold, not a climb. Per seat N43
took SWE 217 -> 417 and USA 401 -> 432 and gave back CHN 375 -> 216 and rush
116 -> 67: growth on the easy seats bought with the two seats the lineage
keeps losing (the self-play-erodes-rush-defence shape). N24 stays the
parent; N44 (N24, seed 20261011) still training.

## 38v — v13 gate tabled: N24 227 leads; N43 228 hold, N44 170 fall

    v13        shipped   champ     N11     N24     N35     N37     N43
    FRA rung       220     249     184     239     209     229     221
    SWE rung       233      93     283     217     247     347     417
    USA rung       321     314     218     401     347     333     432
    CHN rung        72     233     179     375     159     207     216
    FRA rush        41      52     118     116      68      69      67
    NOR hood        21      21      15      15      18      18      18
    rating         151     160     166     227     174     200     228
    survival        72      78      86      86      81      81      81
    worst           21      21      15      15      18      18      18
    v12 -> v13: shipped 131->151  champ 165->160  N11 159->166  N24 232->227  N35 201->174  N37 213->200

Check line: amphibious 67% of 244 (v11) -> 55% of 268 (v12) -> 49% of 342
(v13) reached a hostile shore; routing stuck 5 -> 9 -> 9. More boats
launched, fewer arriving, across the two builds that changed war targets
(claims clear) and dropped orders out of lost provinces (v13). Sent to the
peer as a candidate for orders-die-with-the-ground.

N35 keeps falling as the world is fixed (235 -> 201 -> 174): it was the
best fit to the ghost-claims world, not the best model. N24 is the leader on
two rulers running and the reference model. N44 (N24, seed 20261011) read
170 / 86 / 15, a fall of 57; with N43's hold that is 0 climbs from 2 seeds
at quarter rate from N24, and 0 from 2 from N35. The quarter-rate ladder has
stalled at ~230 with the rush and NOR seats structural (every model 15-21 on
Norway, 41-118 on the rush).

Next: (a) N45 from N24 at OD_LR_SCALE=0.1 (seed 20261012) and N46 from N43
at 0.25 (seed 20261013), (b) a play-time sweep on N24 (OD_PLAY_TEMP 0.7 /
0.5, OD_PLAY_EPS 0) — no retrain, seat-level read, (c) the disband trace
re-run with the gate's eval arguments (an 80-turn difficulty-1 eval printed
no [DISBAND] line at all).

**Correction to 38v (peer):** the landed share fell because the denominator
grew. 67% of 244 = 164 landings, 55% of 268 = 147, 49% of 342 = 168: flat
across a 40% larger fleet; stuck 5 -> 9 -> 9 on the same growth. Not a
regression and not the peer's v13 change (captureProvince drops disband and
army move orders; voyages and embarkations are other containers). The check
line now prints `N landed = X% of M` so the ratio is never read alone again
(source only; lands in v14). Same shape as the width lesson: a ratio whose
denominator is an AI decision cannot separate fewer-arrive from more-set-out.

**N45 (from N24 at OD_LR_SCALE=0.1, seed 20261012, 1,563 turns) on v13:
219 / 80 / 18** — inside noise of N24's 227 on rating, 6 under on survival.
Three children of N24 (228, 170, 219): no climb at either 0.25 or 0.1.
The ladder is finished as a lever until something structural changes.

**Play-time sweep on N24 (v13, no retrain):** default 227; OD_PLAY_TEMP 0.7
-> 211, 0.5 -> 209; OD_PLAY_EPS 0 -> 227 (identical: play epsilon is
already 0). Cooling the policy at play time costs 16-18. Nothing here.

**Norway seat, what actually kills it (N24, seed 20260801, v13):** Sweden
declares at turn ~3; Norway goes bankrupt at turn ~13 (short 16.32: 3
doctrines repealed, 1 minority cut) and again soon after (2 minority cuts);
East Norway then Central Norway secede by rebellion; it ends with 3-4
provinces. Score 23. Norway's army is 36% of its population at turn 120
(197k of 555k), the fifth-highest ratio on the map, but army upkeep is
0.01 per 10k men = 0.2/turn, not the 16 short. Goods are off in the bench
world, so the fuel charge is not it either. v14 adds OD_ECON_TRACE=<cid>
(the ledger per turn) to find the expense; trace-only build at 04:36.

## 38w — two AI rules from the Norway ledger (v15, 04:47)

The ledger (OD_ECON_TRACE=20, N24, seed 20260801, hood): turn 0 income
46.5, expenses 18 (navy 10, doctrines 7). Turns 1-3 the AI funds research
(18-22), raises minority programmes 0 -> 7.5 and at turn 4 enacts doctrines
7 -> 19, all against a projected budget. Sweden takes the industrial
provinces in the same turns: income 48 -> 36 -> 12.8. Turn 5: expenses 32.7
vs income 12.8, bankrupt (short 16.3), minority cut, "+20% rebellion while
broke", rebellion the same turn; East Norway secedes at turn 18, Central
later; from turn 20 the sliders scale to exactly zero net and Norway sits on
income 12.66 for a hundred turns. Without the turn-3/4 additions the books
balance (11 vs 13) and the cascade never starts. The austerity reflex did
fire, one cut a turn (policy 19 -> 15 at turn 5), which cannot answer −20.

Rules (src/ai/AISystem.cpp, resolver side, AI only):
1. LOSING GROUND (`losingGround`, `AI_LOSS_FREEZE_TURNS=8`): a province lost
   within 8 turns blocks every action that adds upkeep -- `enactablePolicy`
   returns nothing, conciliate's headroom is 0 (free options still allowed),
   the calming doctrine must fit current headroom (it had no hard gate).
2. CRASH AUSTERITY (`AI_AUSTERITY_MAX_CUTS=6`): when treasury + net < 0 this
   turn, the cascade repeats on the recomputed books until it balances or
   nothing is left; pending scraps counted by hand. Otherwise one cut a turn
   as before.
Mask note: v[1] (enact) is now invalid while losing ground -- a frozen policy
may read low here (mask-changes-need-a-retrain). Measured: N24 on v15 vs
N24-v13 227 / 86 / 15 (v14 is rules-identical to v13).

**N24 on v15 (loss freeze 8 + crash austerity): 154 / 81 / 15** — a fall of 73
from 227. Per seat: FRA 239 -> 249, SWE 217 -> 80, USA 401 -> 318, CHN 375 ->
168, rush 116 -> 90, NOR 15 -> 15. The blanket freeze (no doctrine, no paid
conciliation for 8 turns after any loss) is the wrong shape: the journal's
own earlier measurement said capping conciliation costs 8.5 paired because
alignment is what keeps provinces in. Both rules are now env knobs on one
binary (OD_LOSS_FREEZE turns, OD_CRASH_CUTS) and are benched separately.

**Why Norway secedes (OD_UNREST_TRACE=20, v15 trace, seed 20260801):** every
term of the rebellion chance is ~0 for every Norwegian province on every
turn -- base 1.0, political 0, ethnic 0.2, claims 0, weariness 0, minus
the loyalty floor 6 -- except turn 7, the one bankrupt turn, when the
bankruptcy charge puts each province at 15.2%. Four provinces at 15% rolled
the rebellions that became South, West and Central Norway; the country was
otherwise perfectly stable. Rule-side question sent to the peer: a one-turn
cash shock (provinces lost inside the same resolution, nothing the country
could see) charges the same +20% as a chronic bankrupt; ramping the charge
with m_bankruptStreak would leave chronic bankruptcy exactly as punishing
and stop a single shock fracturing a stable country. Proposed, not landed.

**N46 (from N43 at 0.25, seed 20261013) read 166 / 78 / 23 on the v15
default-knob binary** (freeze 8 on, the arm that scored N24 154), so it is
not comparable with N43's 228; re-benched under control knobs after the A/B.

## 38x — the v15 rules by arm (N24, one binary, knobs)

    control (freeze 0, cuts 1)   209 / -- 
    crash cuts only (cuts 6)     181      -28: the deep cascade destroys more than it saves
    loss freeze only (8 turns)   211      +2: neutral, and it blocks paid conciliation
    both (04:47 binary)          154

Defaults set to control (OD_LOSS_FREEZE=0, OD_CRASH_CUTS=1); the code stays
as knobs. The calming-doctrine headroom gate is also a knob now
(OD_CALM_GATE, default on) because it was in every arm including control.
Control reads 209, not N24's 227 on v13, on a bench that is deterministic
(same seat twice: 2.2 / 2.2). The binary carries the peer's seed change
(chooseWorldSeed in startNewGame; the eval reseeds with the CLI seed
after the load, so the load itself may have drawn a different world stream)
and the calm gate; the v13 reference binary on the same seat settles which.
The AI cannot pre-empt a same-turn cash shock; the fix for Norway is the
bankruptcy-unrest ramp (peer, game rule, landing after this arm).

**N46 (from N43 at 0.25, seed 20261013) re-benched under control knobs: 188 / 78 / 10.**
N43 is being benched on the same binary for the paired read. On the seat
1914:SWE seed 20260801 at 60 turns, the v13 reference binary and the current
one both score 2.2, with or without the calm gate: the first 60 turns of that
world are identical across the builds. The 227 -> 209 gap is somewhere in the
other seeds or after turn 60; being located.

**Located: the 227 -> 209 was the calming-doctrine headroom gate.** v13
reference binary vs current, 1914:SWE at 120 turns: seeds 20260801 3.4 vs
2.4, 4242 0.6 vs 2.3, 90210 2.5 vs 2.8 (identical at 60 turns); the current
binary with OD_CALM_GATE=0 gives 3.4 on the first. The gate was in every arm
including control. Default now OFF; the binary is v13 rules exactly, plus
traces and the three knobs. Third time the same lesson measures: capping
conciliation -8.5 paired, the loss freeze blocking conciliation, the calm
gate -18 -- the unrest levers are never where to save money. Not the
peer's seed change: worlds are identical across binaries for 60 turns.

**N46 (from N43 at 0.25, seed 20261013) on v13 rules: 175 / 79 / 21** vs N43
228 / 81 / 18 — a fall of 53. The quarter-rate ladder from the N24 line is
now hold / fall / fall / fall (N43 228, N44 170, N45 219, N46 175). N47
(N24, seed 20261014) is the last quarter-rate ticket before the ramp ruler.

**N48: a different lever.** From N24 at 0.25, seed 20261015, trained with
`--vs-script --vs-exploit 3`: the scripted opponents play the blitz variant
the rush and hood seats are benched against. Every model loses those two
seats (rush 67-118, NOR 15-21); self-play has been eroding rush defence
(memory). Measured on the full bench like any ticket; the rung seats may pay.

**N48 (N24 trained vs an all-blitz world, 1,209 turns in 5.3 min): 149 / 76 / 18.**
Per seat vs N24: FRA 239 -> 194, SWE 217 -> 250, USA 401 -> 283, CHN 375 ->
112, rush 116 -> 36, NOR 15 -> 18. The seat it was meant to fix fell by 80:
five games in an all-blitz world end in annihilation inside 250 turns, and
the model learned from being annihilated (passivity-is-load-bearing shape).
Not the lever, at least not at this dose. Next: trace the rush seat itself
(what actually takes France's land, turn by turn) before touching anything.

## 38z — v16 gate tabled (bankruptcy ramp): the ruler fell ~30 for everyone; N47 leads at 218

    v16        shipped   champ     N11     N24     N35     N37     N43     N47 N24cuts
    FRA rung       182     256     180     209     218     212     189     222     232
    SWE rung       307     223     237     273     260     233     273     353     297
    USA rung       132     255     192     358     307     246     354     378     360
    CHN rung        57     239     173     221     169     232     165     284     292
    FRA rush        38      14      27      74      26      69      88      45      23
    NOR hood        18      26      26      23      23      26      23      26      23
    rating         122     169     139     193     167     170     182     218     204
    survival        69      73      75      83      75      82      85      78      74
    worst           18      14      26      23      23      26      23      26      23
    v13 -> v16: shipped 151->122  champ 160->169  N11 166->139  N24 227->193  N35 174->167  N37 200->170  N43 228->182

The ramp is a new world, and a harder ruler: every model reads 20-45 lower
than on v13 (N24 227 -> 193, N43 228 -> 182, shipped 151 -> 122) except the
champion (160 -> 169). The reason is in the seat definition: with far fewer
shock rebellions the SCRIPTED countries also keep their provinces, so the
seat's share of the world -- the score -- shrinks even when it holds every
province it had. Norway's hood seat did NOT move (0.3 on every model, 23-26):
the ramp took the third-of-a-charge off its one bankrupt turn and it still
dies; traced next on v16. N24-v16-cuts (OD_CRASH_CUTS=6): 204 vs 193, the
interaction the peer predicted -- but survival 74 vs 83 because the rush seat
falls 74 -> 23 while FRA/SWE/CHN rise; default stays off, noted as a
seat-specific lever. N47 (N24 at 0.25, seed 20261014): 218 / 78 / 26 vs
N24 193 / 83 / 23 -- FRA +13, SWE +80, USA +20, CHN +63, NOR +3, rush -29:
five seats of six, the first quarter-rate climb from N24 in six tickets.
Parent: N47. Ruler held at v16 (binary 05:27, --binary explicit from now
on). Rush trace on v16 (seed 20260801): France GROWS 83 -> 114 provinces by
turn 90 and then loses 47 in the last 30 turns (income 827 -> 333); the v13
seed's early collapse is gone from this world, seed 90210 annihilates now.

**Norway on v16 (N24, seed 20260801, OD_ECON_TRACE=20 with [CAPTURE]):** it
is a rout, not a ledger. Norway starts with 17 provinces; Sweden (cid 19)
takes 13 of them in turns 1-5, all contested (7 on turn 3 alone); Norway
has 4 from turn 7 to the end and lives on income 9.46 for a hundred turns.
The one bankrupt turn (turn 5, now charged a third: 2.1% per province) no
longer matters; the economy was never the cause on this seat, only the
last act. Army expense reads 0.05 -> 0.21 -> 0.07 -> 0.01 over turns 0-5:
Norway recruited to ~210k men by turn 1 and lost them all by turn 5.
Next: trace the battles (attacker/defender strength per capture) to see
whether the men are spread thin and beaten piecemeal.

## 39 — v17: the maps' doctrine data was never what the game read

Peer found `data/policies.json` is not what the game reads: every .odmap
embeds its own copy and the embedded copy wins, so the mobilisation levers
(army upkeep / recruitment cost on 14 doctrines) had been inert and the
shipped maps disagreed with the repo. Regenerated and re-embedded (7 maps);
their eval moved 71.7 -> 62.3 survival on the data alone. v17 = the v16
binary on the re-embedded maps; re-gated with --binary explicit. Norway's
v16 opening (the turn-1 assault on 1046) does not happen on v17 -- the map
data reaches the policy's choices, not only the costs -- so every trace
reading is taken from v17 on.

**N49 (N24 at 0.25, seed 20261016) benched on v17 data: 204 / 71 / 2**;
**N50 (N47 at 0.25, seed 20261017): 164 / 79 / 26.** Both fall from their
parents on v17 (N24 222; N47 pending).

## 39a — two AI-side knobs from the Norway decision stream (v17)

OD_ECON_TRACE now also prints [ACTION] (every module decision), [BATTLE]
(every assault made or received: attackers, engaged, defenders, width,
fort, powers, outcome), [CAPTURE] and [ATTACK-ORDER]. Norway, v17, N24,
seed 20260801, the turn Sweden declares: econ head takes research funding
0 -> 40% in eight goes (45% next turn), upgrades industry in four provinces
(two fall two turns later), never fortifies; war head recruits 154k in one
province; politics asks Sweden for a NAP; navy spends seven goes on
"disembark: nearest enemy port 7 deg". Sweden's stack: 174,800 at province
824 (93k defenders), repulsed three times at width 35,631 (each repulse
also kills 9-34k defenders), carried on the fourth.

Knob 1, OD_WIDTH_MARGIN: attackCandidates scores an assault as
resolveAssault scores it (both sides capped at combat width). Peer's
reading, agreed: above the frontage the resolver's comparison is
modifiers-only, so the gate learns "never assault an above-width defender
with a defensive edge" -- correct against today's resolver, one line to
change when reserves start replacing losses within a fight (theirs, held
for the user).

Knob 2, OD_SIEGE_REFLEX: while an adjacent enemy army exceeds one of our
garrisons (st.worstDeficit > 0): research funding steps down to 10% and
the fund-up action is refused; the most threatened under-fortified frontier
gets a fort through execEconomy(cid, 2); and `siegeEarmark` holds the fort's
cost back from the head's other lump sums (industry, port, spec, ships),
because the treasury runs at zero every turn and a 20-cost fort was
"cannot afford" on every turn it mattered. With it Norway buys a level-1
fort at 826 on turn 2 ($18), holds research at 10%, repulses one assault
there -- and still has 4 provinces by turn 7 (was 4 by turn 7 without):
Sweden's adjacent mass is 574,500 against a 93k garrison; a level-1 fort is
+10%. The seat may be unwinnable by play under this resolver; the bench
decides whether the reflex helps anywhere. Arms queued on N24 (v17):
width, siege, siege+width.

## 39b — v17 gate tabled: N24 leads again (222); N47's v16 lead did not survive the maps

    v17        shipped   champ     N11     N24     N35     N37     N43     N47     N49     N50
    FRA rung       117     225     197     247     264     208     238     251     219     228
    SWE rung       207     257     247     350     260     283     310      97     230     237
    USA rung       191     246     215     373     318     371     302     370     398     265
    CHN rung        73     192     121     309     185     236     172     375     351     179
    FRA rush        41      52      24      27      54      51      63      13       2      48
    NOR hood        31      23      28      26      26      26      23      26      23      26
    rating         110     166     139     222     185     196     185     188     204     164
    survival        74      79      75      75      80      79      81      73      71      79
    worst           31      23      24      26      26      26      23      13       2      26
    v16 -> v17: shipped 122->110  champ 169->166  N11 139->139  N24 193->222  N35 167->185  N37 170->196  N43 182->185  N47 218->188

N24 is the model that holds across rulers (v13 227, v16 193, v17 222);
N47 (218 on v16) reads 188 here, N49 204 with a worst seat of 2. Reference
model stays N24. Ruler held at v17 until the peer's multi-war map change
(v18), which lands on this ping. Calls to arms are issued only inside
declareWar (issueCallsToArms), so a pact made mid-war brings nobody: a
"seek a guarantor" rule for a besieged country cannot work under today's
rules -- proposed to the peer as a diplomacy rule (allies callable to an
ongoing war, the cooldown map already exists).

**Width-margin arm, N24 on v17: 196 / 84 vs 222 / 75.** Per seat: FRA 247 ->
215, SWE 350 -> 187, USA 373 -> 407, CHN 309 -> 264, rush 27 -> 83, NOR 26 ->
21. The seat it was written for tripled and survival rose 9; the three
conquest seats paid for it -- the frozen policy keeps choosing "attack" and
the gate now refuses the assaults it used to win by numbers (mask change on
a frozen policy). Test per the standing rule: N52 trains from N24 with
OD_WIDTH_MARGIN=1 on, benched with it on.

**Width-margin arm, N47 on v17: 215 vs 188 (+27)**, the opposite sign to
N24 (−26). Same gate, two models, two answers: the gate removes assaults
the resolver would repulse, and whether that helps depends on how much of
a model's conquest came from numbers the resolver no longer honours. Not a
rule to switch on blind; N52 (trained with it on) is the fair test.

## 39c — v18: the multi-war doctrine maps landed at 06:20:13, before the ping

Peer re-embedded the seven maps at 06:20:13 (`war_on_several_fronts`,
16/turn, +2 declarations, -20% maintenance, -8% defence; 46 doctrines, 7
maps in sync). Maps are read per bench process, so every seat run started
after that timestamp is v18 data: the v17 gate (complete 06:19) and
N24-v17-wm (finished ~06:20) stand; N47's width arm (06:21-06:33) is v18
and is relabelled N47-v18-wm; the siege arms, wm2 arms, N51 and N52 benches
are v18. Peer's 60-turn eval at seed 4242 is identical to the digit to
v17 -- nobody enacted the doctrine in 60 turns -- so v18 may read flat.
v18 gate (shipped, champion, N24, N37, N47) queued behind the arms.

**Siege-reflex arm, N24 (v18 data): 204 / 88** against N24-v17 222 / 75.
Rush 27 -> 179, survival 75 -> 88 (the highest survival any model has
posted), FRA 247 -> 240, SWE 350 -> 290, USA 373 -> 324, CHN 309 -> 168,
NOR 26 -> 26. Provisional until N24-v18 separates the map change from the
reflex; if it holds, the reflex is the first thing to move the rush seat by
more than noise, and the China cost (forts and a research cap while any
adjacent enemy outnumbers a garrison) is the thing to narrow -- trigger on
a deficit that is a real share of the army, not any single garrison.

**Siege + width-v2 arm, N24 (v18 data): 224 / 88** against N24-v17 222 / 75.
FRA 247 -> 187, SWE 350 -> 360, USA 373 -> 392, CHN 309 -> 181, rush 27 ->
196, NOR 26. Same rating, survival +13, the rush seat from lost to held
with growth. The two knobs together give back what each cost alone (the
width gate's SWE, the reflex's USA); China and France pay. Provisional
until N24-v18; if it holds, both knobs go on by default and the next
tickets train with them on (N52 already trains with the width gate).

**N52 (N24 at 0.25, seed 20261019, trained WITH the width gate; benched on
the v2 gate, v18 data): 181 / 88 / 26.** Survival 88 again with the gate;
rating below N24's own gated arm (196). Training with the gate on did not
recover the conquest seats in one ticket. Slot goes to N53: N24 trained
with BOTH knobs on (siege + width), the combination that measured 224 / 88.

**N53 (N24 at 0.25, seed 20261020, trained with siege + width on, 1,822
turns): 202 / 83 / 28** vs N24 with the same knobs 224 / 88. Worst seat 28
is the lineage's best floor, but the ticket lost 22 on rating: a short
training run (five games ending early) that did not improve on its parent
under the same rules. The knobs' value is in the frozen N24 (224/88); the
tickets from it keep failing to climb.

**Width v2 alone (above-width certainty), N24, v18 data: 193 / 85** — the
same as v1 (196 / 84); the certainty rule moved nothing measurable on its
own. The table so far on N24: plain 222/75; width 193-196/84-85; siege
204/88; siege + width 224/88. The pair is the only arm that keeps the
rating; both knobs go on by default if N24-v18 (gate running) confirms the
plain number did not move with the maps.

**N47 with width v2 (v18 data): 240 / 87** — the highest rating on any
ruler so far (N35's 235 was v11), from a model that reads 188 plain on v17.
N47 + siege + width queued behind the v18 gate.

**N51 (N24 at 0.25, seed 20261018, plain knobs, v18 data): 179 / 81 / 26** — a
fall from N24's 222. Next pair from N47, the model the width gate lifts
most: N54 trained and benched with the width gate; N55 with both knobs.

## 39d — v18 gate: identical to v17 for every model

    v18   shipped 110   champion 166   N24 222   N37 196   N47 188   (v17: 110 / 166 / 222 / 196 / 188)

Nobody enacts the 16/turn multi-war doctrine in 120 turns, so v17 and v18
are one ruler; every knob arm benched after 06:20:13 compares directly with
the v17 table. Knob table (plain -> arm):
    N24  222/75 -> width 193-196/84-85, siege 204/88, siege+width 224/88
    N47  188/73 -> width 215-240/87-88, siege+width benching

**N55 (N47 at 0.25, seed 20261022, trained with both knobs, 855 turns): 189 / 81 / 26**
vs N47 on the width gate 240 / 88. Five games ended inside 855 turns; the
ticket learned little and lost 51 against its parent's gated number. The
gates are worth more on the frozen models than any quarter-rate ticket has
been; the training recipe, not the rules, is now the bottleneck (short
games: OD_MAX_GAMES=5 ends a session when five games end, and gated play
ends games faster).

**Two recipe faults found while reading N55's log.** `OD_MAX_GAMES=5` in the
recipe was never a game count: it is odlock's slot count, so the memory
gate has been letting five processes through since the recipe was written
(harmless only because at most three ever ran). Dropped. And sessions end
by STAGNATION_TURNS (400 turns with no progress) -- N55's world froze from
turn 500 (embarks flat) and the single-map session ended at 855. Gated play
freezes worlds sooner. Tickets use three maps from here so a frozen world
rotates instead of ending: N56 (N47, width gate, 3 maps, seed 20261023).

**N54 (N47 at 0.25, seed 20261021, trained with the width gate, 2,200 turns): 187 / 87 / 23**
vs N47 on the gate 240 / 88. Fall of 53. Every ticket trained under a
knob has fallen from its parent's knob number (N52 181, N53 202, N54 187,
N55 189): the quarter-rate step erodes what the frozen parent has. Keep
the frozen models; the gates are the deliverable.

**N47 with siege + width: 225 / 88 / 28** vs width alone 240 / 88 / 26 and
plain 188 / 73 / 13. On N47 the siege reflex costs 15 of rating for +2 on
the floor; on N24 it is worth +28 over the width gate alone. The width
gate is the consistent win (N24 +9 survival, N47 +52 rating +15 survival);
the siege reflex depends on the model. Width-only arms queued for N37,
N43, N35 beside their both-knob arms to settle the siege default.

**Both-knob arms on the frozen models (v18):** N37 196 -> 232 / 88, N43
185 -> 192 / 71 / worst 0, N35 185 -> 214 / 71 / worst 0; with N24 222 ->
224 / 88 and N47 188 -> 225 / 88. Five of five up on rating, but modern
China is ANNIHILATED on N43 and N35 with the knobs on (CHN 0) and falls on
every model (N24 309 -> 181, N47 375 -> 277, N37 236 -> 127). On N24 and
N47 the width-only arms keep China (264-280, 336), so the siege reflex is
the China killer: any frontier deficit triggers forts and a research cap,
and a large power at war always has some outnumbered garrison. Narrowed:
OD_SIEGE_SHARE (default 0.25) -- the reflex fires only when the worst
deficit is at least that share of the country's own army (Norway 574k
against 210k fires; a Chinese frontier of 100k against a 5M army does not).
Width-only and width+narrow-siege arms queued on all five models.

## 39e — the knob table across five frozen models (v18, rating/survival/worst)

    model      plain        width        siege        both
    N24    222/75/26    193/85/23    204/88/26    224/88/26
    N47    188/73/13    240/88/26        -        225/88/28
    N37    196/79/26    172/77/23        -        232/88/26
    N43    185/81/23    169/87/26        -        192/71/ 0
    N35    185/80/26    183/87/23        -        214/71/ 0

Width alone: rating up on one model (N47 +52) and down on four (−29, −24,
−16, −2); survival up on four of five. Both: rating up on five of five, but
China annihilated on N43 and N35. Neither knob is a rule on its own
evidence; the narrow siege (a quarter of the army) with width is the arm
that decides, on all five, running now.

## 39f — knob table on five frozen models, and the defaults (v19)

    model                  plain                 width                  both                narrow   (rating/survival/worst, CHN)
    N24     222/ 75/ 26 CHN 309     193/ 85/ 23 CHN 280     224/ 88/ 26 CHN 181     215/ 86/ 23 CHN 181   
    N47     188/ 73/ 13 CHN 375     240/ 88/ 26 CHN 336     225/ 88/ 28 CHN 277     218/ 80/ 21 CHN 320   
    N37     196/ 79/ 26 CHN 236     172/ 77/ 23 CHN 239     232/ 88/ 26 CHN 127     255/ 87/ 21 CHN 281   
    N43     185/ 81/ 23 CHN 172     169/ 87/ 26 CHN  95     192/ 71/  0 CHN   0     200/ 80/ 26 CHN  60   
    N35     185/ 80/ 26 CHN 185     183/ 87/ 23 CHN 259     214/ 71/  0 CHN   0     167/ 87/ 21 CHN  99   
    mean    195/ 78/ 23 CHN 255     191/ 85/ 24 CHN 242     217/ 81/ 16 CHN 117     211/ 84/ 22 CHN 188   

Narrow siege (deficit >= a quarter of the army) + width gate: mean rating
195 -> 211, survival 78 -> 84, floor 23 -> 22, no annihilations; the wide
siege scored higher on rating (217) but annihilated China on two models.
Width alone is a survival lever (85) that costs rating on four of five.
N37 on the narrow pair reads 255 / 87 / 21, the highest rating recorded.
China is the cost on every arm with the reflex (mean 255 -> 188): traced
on N24 seed 20260801, China's own reflex never fires (siege-only, zero
firings, score 7.8 -> 0.0) and its action stream is identical until turn
3 while the ledger differs from turn 2 -- the knob is global, every
model-cohort country plays it, and the world moves under China. Seeds
split (4242 up, 90210 and 20260801 down). Not understood to the mechanism;
recorded, not fixed.

Defaults set: OD_WIDTH_MARGIN=1, OD_SIEGE_REFLEX=1, OD_SIEGE_SHARE=0.25
(knobs remain; 0 turns each off). v19 = v18 + these defaults; shipped and
champion benched on it for the record. Reference: N37-255-v18-knobs.bin.

**v19 on the peer's world aggregate (seed 4242, 60 turns, difficulty 2):**
survival 62.3 -> 69.8, research 194.40 -> 177.99 (-8.4%: the reflex not
paying for research while a stack sits on the border -- the mechanism,
not only the outcome), rebellions per 1k 48.19 -> 50.16 (+4%: the reflex
crowds out pacification and minority spending; stability traded for
survival). Watch line for later arms: survival flat with rebellions up is
that trade coming apart.

**N56 (N47 at 0.25, width gate, THREE maps, 5,934 turns) on v19: 153 / 79 / 26**
vs N47 on the same defaults 218 / 80 / 21. A fall of 65 with three times
the turns: length is not the missing ingredient. Since N43 every ticket
from a strong parent has fallen or held (N43 hold, N44-N46, N48-N56 fall;
N47 the one climb). The ladder is exhausted as a lever; the slot waits
for N57 (from N37) and then the training objective gets the next look.

**Why tickets erode, read from N56's table and the reward.** N56's war head
chose reinforce 299,584 times, hold 152,380, recruit 93,192, attack 5,871
(the width gate removes most candidates); advantage on recruit −0.35, on
attack +0.28 from a thin sample. The reward pays land (PHI_PROV 2.4 plus
2.0 in the war line), a sufficiency-shaped army term (ARMY_SHAPING_WEIGHT
2.0 up to the larger of the adjacent enemy and peacetime parity, zero
beyond), treasury 0.35 and net income 0.55: a recruit past sufficiency
costs treasury and earns nothing, so the head learns to stop recruiting,
worlds freeze (three rotations in N56), and the seat bench -- which pays
for growth above par -- reads the quieter policy as worse. A reward change
is the lever the journal says finds only corners; not taken. The frozen
models with the v19 rules are the deliverable today.

**v19 record: shipped 110 -> 131, champion 166 -> 129.** The 1.1.2a model
gains 21 on the rules; the old champion (i10-final, v7 lineage) loses 37 --
it was the model whose conquests most relied on assaults the resolver no
longer honours. The defaults stand on the five-model table; the champion
is history, not a reference. v19 ruler: shipped 131, champion 129, N24
215, N47 218, N37 255, N43 200, N35 167.
Per seat the rules move the weak models the same way as the strong: the
shipped model's rush 41 -> 105 and USA 191 -> 288, but CHN 73 -> 4 and
survival 74 -> 71; the champion's SWE 257 -> 13. On models that were
never trained beside these rules the floor can fall to an annihilation on
one seat. Shipped model with width only queued to split the two knobs for
the shipping decision (the user's; model.bin is never overwritten here).

**Shipped model, the two knobs split (v19 binary):** plain 110 / 74 / 31
(CHN 73); width only 129 / 71 / 28 (CHN 48); both 131 / 71 / 4 (CHN 4).
On the shipped model the width gate is worth +19 with the floor intact and
the siege reflex adds +2 while taking China from 48 to 4. For a release
that keeps the 1.1.2a model, width-only is the safer default; for N37/N47
the pair is. Recorded for the user's shipping decision.

**N47 vs N56 on the 80-turn eval (v19 rules):** model survival 70% vs
52%; battles won per attack issued 0.64 vs 0.50; war spend 0.84 vs 2.46;
rebellions per 1k 53 vs 82; recruit waste 4% vs 12%. The ticket trained
under the gate attacks about as often, wins fewer of them, recruits more
and wastes more, and its worlds rebel half again as much: the attack
chooser (chooseAttack, trained from the thin post-gate sample) got worse,
not the gate. A ticket needs many more assault samples than a 6,000-turn
session gives once the gate has removed the easy ones.

**N57 (N37 at 0.25, v19 defaults, 3 maps, 4,804 turns): 172 / 85 / 26** vs
N37 on the same rules 255 / 87 / 21. A fall of 83, the largest yet. The
quarter-rate ladder is stopped: from N43 to N57, one climb (N47) in
fifteen tickets, and every ticket trained beside the new rules fell hard.
No more tickets from the strong models until the training objective is
changed; the frozen models with the v19 rules are the state of the art.

**N37 vs N47 head-to-head, v19 rules, 120 turns:** seed 20260801 55.6% N37,
4242 56.1% N37, 90210 48.2% N37 (mean 53.3%, maps 2-1). With the seat
bench (255 vs 218 on the defaults) and the duel agreeing, N37 is the
recommended model: build/loop/reference/N37-255-v18-knobs.bin. data/ai/
model.bin is untouched; shipping it is the user's decision.

**Siege reflex split, five models (rating/survival/worst, China):**
width only 191/85/24 C242; full narrow siege 211/84/22 C188; fort-only
(no research cut) 192/79/18 C155. The fort and its earmark are the China
cost AND worse everywhere; the research cut is the whole benefit. Next
arm: research cut only (OD_SIEGE_FORT=0), same five models.

## 39g — siege reflex settled: the research cut alone is the rule (v19 final)

    model                 plain                width           full siege            fort-only        research-only
    N24     222/ 75/ 26 C 309   193/ 85/ 23 C 280   215/ 86/ 23 C 181   176/ 75/ 18 C 169   211/ 87/ 23 C 284 
    N47     188/ 73/ 13 C 375   240/ 88/ 26 C 336   218/ 80/ 21 C 320   207/ 74/ 18 C 229   230/ 87/ 23 C 300 
    N37     196/ 79/ 26 C 236   172/ 77/ 23 C 239   255/ 87/ 21 C 281   206/ 84/ 18 C 229   233/ 87/ 26 C 312 
    N43     185/ 81/ 23 C 172   169/ 87/ 26 C  95   200/ 80/ 26 C  60   194/ 83/ 21 C  80   165/ 75/ 26 C  60 
    N35     185/ 80/ 26 C 185   183/ 87/ 23 C 259   167/ 87/ 21 C  99   176/ 81/ 18 C  67   193/ 84/ 23 C 157 
    mean    195/ 78/ 23 C 255   191/ 85/ 24 C 242   211/ 84/ 22 C 188   192/ 79/ 18 C 155   206/ 84/ 24 C 223 

Research-only (no fort, no earmark): mean 206 / 84 / 24, China 223 -- the
same survival as the full reflex, the best floor of any arm, China within
32 of plain, and simpler. Three of five models prefer it or tie (N47 230
vs 218, N35 193 vs 167, N24 211 vs 215); N37 (255 vs 233) and N43 (200
vs 165) prefer the fort. The fort half is the China cost on every model
and worse on average; a level-1 fort bought with an earmark that starves
the industry head is not what a besieged country needs, the research
slider is. Default: OD_SIEGE_FORT=0. v19 final = width gate + research
cut while besieged (deficit >= a quarter of the army). Record on this
binary: N37 233/87/26, N47 230/87/23, N24 211/87/23, N35 193/84/23, N43
165/75/26; shipped benched next. Recommended model stays N37 (255 with the
fort, 233 without; beats N47 2-1 head-to-head).

## PENDING COMMIT — 2026-09-05 (AI session; nothing committed, nothing staged)

AI code (src/ai/AISystem.cpp, .h):
- attackCandidates scores an assault as resolveAssault does (both sides
  capped at combat width; above-width edge = certain carry). Default on;
  OD_WIDTH_MARGIN=0 restores the old score.
- siegeReflex: while an adjacent enemy army exceeds a garrison by >= a
  quarter of the country's army (OD_SIEGE_SHARE), research funding steps
  down to 10% and fund-up is refused. Default on; OD_SIEGE_REFLEX=0 off.
  The fort/earmark half exists behind OD_SIEGE_FORT=1 (off: China cost).
- losingGround / crash austerity kept as knobs, default off (measured
  neutral / −28). Calming-doctrine headroom gate behind OD_CALM_GATE=1
  (off: −18).
- Traces (stderr, env-gated, no behaviour): OD_ECON_TRACE=<cid> (ledger,
  [CAPTURE], [BATTLE], [ATTACK-ORDER], [ACTION], [SIEGE]),
  OD_UNREST_TRACE=<cid> (rebellion-chance terms), OD_DISBAND_TRACE
  ([DISBAND] with PUSH-ai).
Eval/bench: [EVAL] amphibious line prints the absolute landings count;
tools/od_bench.py header note (share of the world); docs/ai/LOOP.md
corrections (OD_MAX_GAMES is odlock's slot count; three maps per ticket).
Reference: build/loop/reference (N37-255-v18-knobs.bin recommended;
OpenDoctrinesServer-v13/v16/v19; README).
Not for commit: build/loop/*, data/ai/model.bin untouched.

## 39h — correction: the fort half stays (peer's reading of my own table)

39g said "the fort half is worse on average". The table does not say that:
fort-only 192 is the fort WITHOUT the research cut (not sufficient alone);
the comparison that decides the default is both 211 vs research-only 206,
and the fort adds +5 there. The peer's aggregate (one binary, seed 4242,
60 turns): no reflex 62.3 survival / 48.19 rebellions; reflex with fort
69.8 / 50.16; research cut only 66.0 / 48.01. The fort half carries half
the world-survival lift (+3.8) and all of the rebellion tick (+2.2), and
the seat ruler taxes a rule that helps every country defend itself (share
of the world). The explicit trade: fort ON = +5 mean rating, +3.8 world
survival, +2.2 rebellions/1k, −35 on the China seat, floor 22 vs 24.
Taken: fort on. v19 = width gate + siege reflex (research cut + fort with
earmark), OD_SIEGE_FORT=0 to drop the fort. 39g's record line is
superseded: the five-model record on v19 is the "full narrow siege"
column (N37 255, N47 218, N24 215, N43 200, N35 167; mean 211/84/22).

**39i — the 09:13 rebuild had changed one gate.** While splitting the
reflex, the research fund-up refusal moved from "a fort is owed"
(siegeEarmark > 0) to "besieged at all" (underSiege), so the 09:30 binary
refused fund-up in more states than the 08:18 binary the v19 record was
taken on. Peer's aggregate caught it: 69.8 world survival on the 08:18
build, 67.9 on 09:30, same seed, deterministic. Reverted to the earmark
gate; the binary on disk (09:4x) is the 08:18 semantics again and the
five-model record ("full narrow siege" column: N37 255, N47 218, N24 215,
N43 200, N35 167) stands for it. Lesson repeated: a knob's default and
its gate are two different things; changing the gate is a new build.

**v19 verified by the peer:** the reverted binary reproduces their record
to the digit (survival 69.8, rebellions 50.16 per 1k, research 177.99;
largest power 19.3 -- their earlier "19.9" was a v18 figure carried over,
their correction). Recorded on both sides as: v19 = width-aware attack
gate + siege reflex (research cut while a fort is owed, fort with earmark,
share 0.25). Reference binary: build/loop/reference/OpenDoctrinesServer-v19.

**v19 record reproduced on the reverted binary:** shipped 131, champion
129, N37 255 -- all to the digit against the 08:18 numbers. The v19
binary in build/loop/reference is the one both records describe.

## 40 — the pacification reflex (OD_PACIFY_REFLEX, knob, off until measured)

Modern China, N24, seed 20260801, OD_UNREST_TRACE=3, turn 1: all 96
provinces at 4-10% rebellion chance -- war weariness 6.8 on every one
(national), base ~2.1, ethnic ~1.3, claims 0.1, minus the loyalty floor 6;
pacification 0.00. China loses 95 -> 59 provinces by turn 11 to rebels
while the politics head conciliates Chechens and Ukrainians (the ethnic
term, 1.3 of the sum). Suppression is pac x 50 subtracted from the chance,
so 0.2 zeroes the worst province for a fifth of income. The reflex raises
the slider by worst/50 (+0.25 max a turn) while any province is at risk
and lets it fall 0.05 a turn when none is. With it: China holds 96 -> 99
provinces through turn 11 at 15-38 a turn. Five models + shipped, v19 +
reflex, benching.

**Pacification reflex measured: N37 255 -> 171, N47 218 -> 179 (chain
stopped, two of two conclusive).** Per seat on N37: SWE 453 -> 213, USA
442 -> 280, CHN 281 -> 211 -- China itself ends LOWER although it keeps
its provinces through turn 11. Under this ruler the early rebellions are
cheaper than preventing them: the parent auto-claims every rebel province
and declares war on it, the war head takes them back (plain N24: 43
provinces at turn 20, 80 at turn 100), and a fifth of income for 120 turns
buys nothing that reconquest does not. Knob kept, default off; the
memory's rule holds in this direction too: the unrest levers are not where
the AI's money goes.

## 40a — the coalition bar (OD_COALITION_BAR, knob)

France, rush world, seed 20260801, N37 on v19: score 0.1. The war head
declares on Belgium at turn 1 (the "book" war), Britain honours its
guarantee, and the British Empire (334 provinces) takes 172 French
provinces from turn 1 on; France has 3 provinces by turn 40. The learned
war chooser's features count a target's guarantors (out[6]) but not their
size, so Belgium looks tiny. The knob adds the armies of the target's
guarantors and allies not already at war with us to the army the war bar
is measured against. Five-model bench follows.

**Coalition bar, France rush seed 20260801, N37: 0.1 -> 10.4.** France's own
turn-1 declaration on Belgium still passes (Britain's standing army at
turn 1 is not yet the blitz monster, so BEL + GBR clears the 2.0 bar);
the world changed around it -- Britain ends far smaller. Attribution by
the declaration diff and the five-model bench, running.

**Coalition bar measured: N37 255 -> 179, N47 218 -> 174 (chain stopped).**
Both models lose 40-80; the rush seat itself did not move on N37 (104 ->
about the same) and the rung seats fell: counting a target's guarantors
and allies against the bar removes most declarations in the rung world
(every scripted country is in some pact), and less war means less land.
The seed-20260801 rescue was a butterfly. Knob kept, default off. Same
lesson as the crash cuts and pacification: a rule that makes every model
country more cautious reads as a loss on a growth ruler.

**PENDING COMMIT addendum (10:1x):** further knobs in src/ai/AISystem.cpp,
all default OFF and measured as losses on rating: OD_PACIFY_REFLEX
(pacification slider to the worst province's chance), OD_COALITION_BAR
(guarantors' and allies' armies counted against the war bar),
OD_SIEGE_RESEARCH=0 / OD_SIEGE_FORT=0 (halves of the siege reflex, both
ON by default in v19). Reference binary and record unchanged (v19).

## 40b — the guarantor bar (OD_GUARANTOR_BAR, knob): N37 255 -> 267

Narrower than the coalition bar: an UNCLAIMED declaration is refused only
when a guarantor of the target (not an ally; guarantors join at the
declaration) holds more provinces than we do. N37: 267 / 87 / 21 -- FRA
230 -> 305, CHN 281 -> 316, rush 104 -> 141, NOR 21, SWE 453 -> 397, USA
442 -> 420. Four seats of six up and the highest rating recorded. N47
benching; five models if it holds.
**N47 with the guarantor bar: 264** (218 on v19): two of two models up by
12 and 46. N24, N43, N35 and the shipped model follow.

## 41 — v20: the guarantor bar goes on by default

    model              v19     v19+guarantor bar   per seat with the bar (FRA SWE USA CHN rush NOR)
    N37       255/ 87/ 21     267/ 87/ 21          305  397  420  316  141   21
    N47       218/ 80/ 21     264/ 87/ 21          267  413  419  323  139   21
    N24       215/ 86/ 23     196/ 82/ 23          296   77  449  240   92   23
    N43       200/ 80/ 26     212/ 80/ 21          267  293  450   59  181   21
    N35       167/ 87/ 21     203/ 87/ 21          261  313  355  115  153   21
    shipped   131/ 71/  4     206/ 80/ 23          317  383  298   57  156   23
    mean(5)   211/ 84/ 22     228/ 84/ 21

An unclaimed declaration is refused when a guarantor of the target holds
more provinces than the declarer. Four of five frozen models up (N37 255
-> 267, N47 218 -> 264, N43 200 -> 212, N35 167 -> 203; N24 215 -> 196),
mean 211 -> 228 with survival 84 -> 86 and the floor unchanged; the
SHIPPED model 131 -> 206. The narrow rule keeps what the coalition bar
lost: it only bites on the wars that summon a bigger power, which are the
ones no player would start. v20 = v19 + this default. Reference binary
OpenDoctrinesServer-v20; N37-267-v20.bin is the recommended model (267 /
87 / 21). Reproducibility of the arm numbers on v20: the knob code is
unchanged, only its default, so the arm labels ARE the v20 record.

**PENDING COMMIT addendum (v20):** OD_GUARANTOR_BAR default ON in
src/ai/AISystem.cpp (findWarTarget); OD_COALITION_BAR stays off. Nothing
staged.

**v20 on the peer's aggregate, three seeds, one binary, bar on/off:**
survival 67.9/69.8, 69.8/71.7, 64.2/69.8 (lower with the bar, 3/3);
concentration 0.085/0.082, 0.082/0.079, 0.092/0.080 (higher, 3/3); wars
per 1k 49.7/59.5, 54.2/46.2, 54.8/45.9 (up on two seeds: the bar reduces
suicide, not war); rebellions mixed. Their reading, agreed: the blunder
the bar removes used to destroy the aggressor; without it aggressive
powers survive their own aggression, keep conquering the unguaranteed,
and the map concentrates. Better play, a less crowded world. Recorded as
a design decision for the user beside the model choice; default stays on
for the AI-quality target, and the two instruments disagree for a true
reason this time.

**Rush seat under v20, N37, seed 90210: 14.7** (0.2-0.5 under v19). France
declares on Siam at turn 1 instead of a guaranteed neighbour, grows 83 ->
165 provinces, loses 54 to Brazil and 22 to Romania late and still holds
the seat. The rush seat is no longer a loss for N37 on any seed (mean 141
= 9.4%); Norway (21 on every model) is the one seat left that no AI rule
reaches -- it is a 4:1 military rout that needs the peer's held game rules
(callable allies, reserves above the frontage).

## 42 — v21: the peer's depth rule, and the gate's one line

User approved the land-war work; the peer landed depth: men beyond the
frontage are the reserve, `Game::depthFactor` = 1 + 0.20 per doubling above
the frontage, capped at 1.5, applied to both sides (their tuning: caps of
2.0 and 3.0 are worse than no depth at all on every seed; 1.5 beats the
baseline on 3/3 with fewer rebellions). Province 824's repeats now flip
from repulsed to carried at narrow margins.

The width gate mirrors the resolver, so it carries the same term now:
attacker min(sent,width) x atkMod x depthFactor(sent,width), defender
min(defG,width) x depthFactor(defG,width) x fort x defMod. The
"above-width means a certain carry" shortcut is deleted -- depth separates
the sides until both reach the cap. Everything measured on v19/v20
describes the old resolver: the whole five-model table is being re-run.
The guarantor variants measured just before this (N37 by-army 281, N24
by-army 215, N37 claimed-too 243, N24 claimed-too 220) are also stale;
by-army is re-tested on v21 after the re-gate.

**The v21 abort was a mid-edit tree, not the gate.** The 14:12 binary
aborted after the map load on every seat (exit 134); the peer's own build
of the same rule ran clean, and a rebuild from the settled tree at 14:25
runs clean too with the identical gate source. Lesson for a shared tree:
a build taken while the other session is mid-edit can compile and still
be incoherent; rebuild before bisecting your own diff.

**Peer withdrew the 15-point gate claim** (it compared a v20 gate against a
v21 gate through the incoherent build). Their reproducible pair on the
settled depth tree, my gate on vs off: survival 62.3/66.0, 67.9/66.0,
67.9/62.3 (2 of 3 favour the gate), rebellions consistently higher with
it. Recorded as unestablished, not as a cost.

**Supply and cut-off landed (v22 = depth + supply).** Hops from ports and
the largest industrial province over own/allied ground: two free, then -8%
a hop to 0.55, no land route 0.45, landings a flat 0.85. They chose the
constant by FIRING RATE, not outcome: at four free hops only 3-4.5% of
sides were penalised (indistinguishable from inert); at two, 20% -- the
same order as the frontage's 25% bind rate, the rule that is invisible in
world aggregates and decisive on the seat bench. The v21 gate table is
being finished on the depth-only binary first, so the two changes stay
separable; v22 re-gates after it.

## 43 — v21 table (depth resolver): the ruler moved, the floor rose

    model     v20 (old combat)     v21 (depth)   per seat on v21
    N37          267/ 87         206/ 84/ 23   FRA  295 SWE   80 USA  364 CHN  339 rush  138 NOR   23
    N47          264/ 87         215/ 88/ 26   FRA  239 SWE  353 USA  357 CHN  203 rush  113 NOR   26
    N24          196/ 82         221/ 88/ 33   FRA  294 SWE  193 USA  482 CHN  227 rush   96 NOR   33
    N43          212/ 80         189/ 83/ 28   FRA  334 SWE   93 USA  450 CHN   77 rush  151 NOR   28
    N35          203/ 87         163/ 81/ 26   FRA  269 SWE   93 USA  321 CHN   68 rush  199 NOR   26
    shipped      206/ 80         128/ 72/  1   FRA  232 SWE  120 USA  260 CHN    1 rush  124 NOR   28
    mean(5)      228/ 84         199/ 85

Depth lowers rating (mean 228 -> 199) and raises survival (84 -> 85) and
the floor: Norway, the seat no AI rule reached all day, reads 23-33 on
every model against 21 before, and the worst seat on four of five models
is now Norway at 26-33 rather than a collapsed rung seat. Sweden is the
seat that fell (453 -> 80 on N37): a small country's stack far above the
frontage no longer wins on modifiers, and Sweden's neighbours are big.
This is a ruler change, not a model regression -- the models are the same
files. N24 gains on it (196 -> 221), so the lineage's ordering changed
again: on v21 the order is N24 221, N47 215, N37 206, N43 189, N35 163.
Gate-on vs gate-off arms on this binary follow, to settle whether my
width gate still earns its place now that the resolver it mirrors pays
for depth.

**Gate on vs off, same depth binary, three models:** on 214 / 87, off
212 / 88; per model 206/199, 215/193, 221/242; floors 23/28, 26/26,
33/36. The width gate was worth 20-50 points on the OLD resolver, where
an above-frontage repulse deleted the engaged men and numbers past the
frontage bought nothing. Depth pays for the reserve, so the mistake the
gate existed to prevent is mostly gone -- and on N24 the gate now COSTS
21. N43, N35 and the shipped model are benching gate-off before the
default is decided.

## 43a — the width gate is retired by the depth rule (v22)

    model             gate on        gate off
    N37           206/ 84/ 23     199/ 88/ 28
    N47           215/ 88/ 26     193/ 88/ 26
    N24           221/ 88/ 33     242/ 89/ 36
    N43           189/ 83/ 28     206/ 87/ 28
    N35           163/ 81/ 26     210/ 81/ 28
    shipped       128/ 72/  1     133/ 80/ 18
    mean(5)       199/ 85/ 27     210/ 87/ 29

Five models, one binary: gate off 210 / 87 / 29 against gate on 199 / 85
/ 27, better on four of five models and on the shipped model (128 -> 133,
floor 1 -> 18). The gate was written because an above-frontage repulse
deleted every engaged man while numbers past the frontage bought nothing,
so an even-numbers assault was a coin flip that cost `width` men. Depth
pays for the reserve, and the mistake is gone; what is left is a gate
that refuses attacks the resolver would now carry. Default OFF
(OD_WIDTH_MARGIN=1 restores it). This is the right ending for it: it was
a compensation for a resolver bug, and the bug is fixed.

v22 = depth + supply + the gate off. Full re-gate on v22 next; the v21
gate-off column is the closest estimate meanwhile (supply is the only
change between them).

**Standing battles are being built (peer, user-approved).** A repulsed
assault will no longer end: the engaged men are lost as now, but the
reserve holds position as a standing battle, fights a round a turn, is
reinforced by a move order into that province and left only by a new
withdraw order. Committed attackers live in the battle record, not in
m_provinceArmies, so the occupation invariant holds. Consequences here:
the war head has no withdraw action, so a frozen model will bleed into a
losing battle where automatic fallback used to save it. The v22 table
(running) is therefore the reference ruler; v23 needs a RETRAIN, not a
re-gate, and an AI withdraw rule of the siege-reflex shape ("losing this
battle N rounds running, pull out") that is mine to design once the peer
gives me the call and the record's fields. Do not build from the tree
while it churns.

## 44 — v22 reference table (depth + supply, no width gate)

    model        v21 nogate   v22 (+supply)   per seat on v22
    N37          199/ 88      237/ 79/ 15   FRA  281 SWE  417 USA  438 CHN  213 rush   57 NOR   15
    N24          242/ 89      234/ 80/ 38   FRA  307 SWE  373 USA  393 CHN  253 rush   39 NOR   38
    N47          193/ 88      202/ 86/ 18   FRA  254 SWE  213 USA  329 CHN  264 rush  134 NOR   18
    N43          206/ 87      214/ 83/ 21   FRA  257 SWE  387 USA  419 CHN   75 rush  125 NOR   21
    N35          210/ 81      205/ 75/ 15   FRA  267 SWE  423 USA  365 CHN   36 rush  123 NOR   15
    shipped      133/ 80      131/ 75/ 24   FRA  282 SWE   97 USA  256 CHN   24 rush  102 NOR   28
    mean(5)      210/ 87      218/ 80/ 22

This is the ruler that stands through the standing-battles work. N37 is
the model again (237), N24 234, N43 214, N35 205, N47 202, shipped 131.
Supply BUYS RATING AND SELLS SURVIVAL: mean 210/87/29 -> 218/80/22, and
the seats it takes are the ones where the seat is the one being invaded
-- the rush seat falls to 57 on N37 and 39 on N24 (from 127 and 187), and
the floor drops from 26-36 to 15-38. That is the rule working as written:
an attacker deep in someone else's country is now weaker, so the seats
that expand gain and the seats that are invaded by a supplied neighbour
lose. Worth the peer knowing before the sea-cell fix; the amphibious
seats are the same shape. Reference binary OpenDoctrinesServer-v22, N37
the recommended model (N37-237-v22.bin).

## 45 — v23: standing battles, and the AI learns to withdraw

Peer landed standing battles: a repulse ABOVE the frontage leaves the
reserve standing as a battle that fights a round a turn, reinforced by a
move order and left only by a withdraw order; committed men live in the
battle record (the occupation invariant holds) and, after their fix, are
counted by `countryTroops` for fuel, munitions and upkeep. Battles form
from 18.6% of repulses; 97% of assaults are walk-ins.

`withdrawReflex` (OD_WITHDRAW_REFLEX, on by default; OD_WITHDRAW_ROUNDS,
default 2): pull out when the resolver's own last comparison has gone
against us for 2+ rounds AND our losses are not smaller than theirs.
Reads Battle::lastAtkPower/lastDefPower/lastAtkLosses/lastDefLosses, so
frontage, fort, depth, supply and both sides' research are already in the
number. Landings (fromProvince < 0) are never withdrawn: they fight or
they drown. Smoke on 1914:FRA, 20 turns: battles 29 started, withdrawn
0 -> 14. Five-model arms next.

**Supply is not what broke the rush seat.** Battle trace extended to print
both sides' supply. France on the rush world (N24, seed 20260801, v23):
as defender 147 of 186 weighed fights at full supply, 23 at 0.92, 11 at
0.84, four below 0.76; of the 123 assaults that carried against her, 34
had her below 1.00 -- and 0.92 is not what loses a province. As attacker
2,940 of 3,753 at full supply. The peer's aggregate said the same thing
from the world side (defenders penalised 12.8-19.1%, below the average
for all sides); the seat trace agrees. Whatever moved the rush seat on
v22 was not the supply factor in the seat's own fights.

## 46 — v23: standing battles, and my withdraw reflex is WRONG as written

    model            v22    v23 +reflex   v23 no reflex   per seat, v23 no reflex
    N24       234/ 80     250/ 86/ 18      257/ 88/ 28   FRA  314 SWE  403 USA  401 CHN  229 rush  163 NOR   28
    N37       237/ 79     211/ 85/  8      249/ 88/ 28   FRA  223 SWE  407 USA  401 CHN  305 rush  131 NOR   28
    N43       214/ 83     201/ 86/ 15          --
    N47       202/ 86     195/ 83/ 31          --
    N35       205/ 75     184/ 83/  8          --
    mean(5)   218/ 80     208/ 84/ 16

Both pairs say the same thing: withdrawing costs. N24 250 -> 257 without
it, N37 211 -> 249 without it, and the floor is far better without (18/8
-> 28/28). The reason is the 824 lesson from this morning: a repulse
grinds the DEFENDER too, so a battle that reads as losing this round is
often winnable by persisting -- the peer's own numbers show battles
usually ending by exhaustion. My rule reads "losing now and not improving
this round" and pulls out of exactly the fights that would have been won
two rounds later. Default OFF (OD_WITHDRAW_REFLEX=1 restores it).

Standing battles themselves cost frozen models 10 mean (218 -> 208) and
buy 4 survival (80 -> 84), as the peer predicted; the models have no
withdraw action and no notion of a commitment. That is a retrain, not a
regression. On the no-reflex arms N24 reads 257 / 88 / 28 and N37 249 /
88 / 28 -- both above their v22 numbers, so the resolver change is good
for play once the AI is simply left alone to fight.

**Withdraw rule, second attempt (peer added the whole-battle fields).**
Battle now records totalAtkLosses, totalDefLosses and openingDefPower.
The rule quits only when BOTH questions say no: the grind is not working
(lastDefPower is not below OD_WITHDRAW_GRIND=0.9 of the opening defence)
AND the exchange is not ours (totalDefLosses < totalAtkLosses over every
round). Total losses rather than the last round because a rising
lastDefPower can mean the enemy is feeding a fight it is losing, which is
a fight worth staying in. Still OFF by default until it measures.

## 47 — v23 final table (standing battles, no AI withdraw rule)

    model             v22    v23 (final)   per seat on v23
    N24        234/ 80     257/ 88/ 28   FRA  314 SWE  403 USA  401 CHN  229 rush  163 NOR   28
    N47        202/ 86     233/ 88/ 31   FRA  270 SWE  343 USA  482 CHN  175 rush   98 NOR   31
    N35        205/ 75     216/ 88/ 26   FRA  254 SWE  300 USA  435 CHN  181 rush  102 NOR   26
    N37        237/ 79     249/ 88/ 28   FRA  223 SWE  407 USA  401 CHN  305 rush  131 NOR   28
    N43        214/ 83     174/ 78/ 32   FRA  221 SWE  263 USA  366 CHN   32 rush  127 NOR   33
    shipped    131/ 75     161/ 75/ 18   FRA  294 SWE  260 USA  265 CHN   64 rush   67 NOR   18
    mean(5)    218/ 80     226/ 86/ 29

The first withdraw rule was worse on four of five (N43 was the exception,
201 vs 174) and is off. On the settled resolver the frozen models read
better than on v22 for three of five, the shipped model gains 30 (131 ->
161), and the floor holds at 28 on the leaders. N24 is the model of
record again at 257 / 88 / 28; N37 249. Reference: OpenDoctrinesServer-v23,
N24-257-v23.bin.

**Second withdraw rule also measures worse: N24 229 vs 257, N37 190 vs
249, N47 245 vs 233 (one of three up).** Two rules, two shapes, the same
answer: an AI that leaves a standing battle does worse than one that
grinds. On this resolver a repulse costs the defender as much as the
attacker, the attacker's reserve is intact, and the province falls to
whoever stays -- so withdrawing forfeits a fight the rules make winnable.
Withdrawal is a PLAYER tool for a fight they can see is hopeless; the
value of the AI's version is at best a floor effect (N47 +12, N43 +27 on
rule 1) and at worst 60 points. Both rules stay as knobs, default off.
The lever to try instead is not withdrawing but NOT COMMITTING: the
attack chooser deciding how many men a battle is worth before it starts.

## 48 — commitment size: the default is already right

    model      1.25 (default)      2.0         3.0
    N24         257/88/28      233/85/26   248/86/15
    N37         249/88/28      253/88/26   222/86/18

ATTACK_SAFETY is the odds each prong is sized to clear; raising it sends
more men at fewer targets. Neither larger setting beats the default on
both models and 3.0 is worse on both, so the sweep that chose 1.25 back
in the old resolver still holds under standing battles. Surplus decides a
battle (peer's 356 rounds: median 0.73 power, 0.74 exchange, and the
attacker wins anyway), but the AI is ALREADY sending enough: the prong is
sized off the same margin the resolver uses, so a winnable target gets
what it needs and the rest of the garrison opens a second front. Knobs
kept (OD_ATTACK_SAFETY, OD_ATTACK_MAX_COMMIT), defaults unchanged.

Standing at v23: N24 257 / 88 / 28 is the model of record, N37 249, the
shipped model 161. Nothing in the AI's action space is now a measured
loss; the remaining seat gaps (modern China on three models, Norway at
26-33) need either a retrain or the land-war design decisions that are
the user's.
(Floors corrected from the JSON: 3.0 halves the floor on N24, 28 -> 15.)

**Sea supply landed (peer, my suggestion; v24 when built).** A province
with no land route reads 0.85 while a friendly or allied hull is within
`shipMaxRangePx` -- the same distance a hull may land men over, so one
constant serves both -- and 0.45 when none is. Their measurement: of
forces with no road home, 13-34% are held up by a fleet. Guaranteed by an
ORDERING test (no supplied position is worse than under the old flat
constant; a landing at the moment it lands is unchanged) rather than by a
benchmark, which is the stronger guarantee and worth copying.

My build/OpenDoctrinesServer is still the 16:14 v23 binary and N58 is
training against it, so N58 stays comparable with N24's 257. I do not
rebuild until N58's bench is in; then v24 = v23 + sea supply, and the
amphibious doctrine is re-read there. Two AI behaviours that were free
are now priced: landing and immediately re-tasking the fleet, and landing
beyond where the navy can loiter. The doctrine's odds check
(AMPHIB_ODDS, bestEmbarkPort) may need the fleet's staying power in it,
which is the same shape as the width gate mirroring the resolver.

**Build-hold slip (17:02).** Clearing the duplicate `callsIssued` (my
counter collided with the pre-existing one the peer's noteCallIssued
plumbing already had; the tree did not compile for a few minutes and the
peer flagged it rather than touching my file) rebuilt the binary I was
holding. N58 was still training, so its bench will run on v24, not v23.
Rather than discard the ticket, N24 and N37 are re-benched on v24 as
controls: v24 becomes the reference and the pair stays honest. The
lesson from this morning applies to me twice now -- a build is a
side effect, so a hold has to cover every command that can trigger one.

**The call picker's acceptance term was inert, and the trace says why.**
Peer caught it as byte-identical output with OD_CALL_PICK=army and
without. [CALL-PICK] shows predictAcceptance returning the SAME 0.277 for
every candidate (GBR 6.4M and RUS 7.9M both 0.277): the call_to_arms head
is saturated by AI_CALL_RELUCTANCE, so the score collapsed to log1p(army)
and the two pickers were the same function. Replaced with the fact that
actually decides the answer -- whether the friend is free. The scripted
rung answers `!atWar` outright and the trained head's own feature 83 is
"already busy at home"; a busy friend now scores 0.2 of its army rather
than being dropped. Third time today that a rule looked like it was
working because a probability was constant; the [CALL-PICK] trace is kept
for the next one.

**The call picker cannot matter at this scale, and now that is measured
rather than inferred.** The eval prints "call picker: N decisions with a
choice, M reordered": 23/0 on seed 4242 and 15/0 on 777. So of ~500
country-turns with a callable friend, only 15-23 have more than one, and
in none of them does "is the friend free" pick a different friend than
raw army would -- the strongest friend is also the free one, or all
candidates are equally busy. The picker is not inert this time; there is
nothing for it to reorder. Identical world figures are therefore the
CORRECT result, and the peer's instinct to count reorderings rather than
outcomes is what separated the two cases.

**A class of bug, not an incident:** the econ head's dead actions and
the saturated call_to_arms head failed the same way -- not a wrong
answer, the SAME answer for every input (0.277 for a 6.4M army and a
7.9M army alike). The tell is invariance across inputs that should
differ, and the instrument is a per-candidate trace plus a counter of
how often the decision CHANGES, never the outcome.

## 49 — N58, the retrain on the new resolver: 163 / 60 / 3

From N24 at 0.25, three maps, 5,754 turns (48 min), benched on v24:
163 / 60 / 3 against N24's 257 / 88 / 28. Per seat FRA 238, SWE 207, USA
470, CHN 3, rush 31, NOR 28: modern China annihilated and the rush seat
collapsed, the two seats where the model is the one being invaded. The
retrain the peer predicted would be needed did NOT rescue the frozen
models -- it made the same lineage worse, exactly as the previous
fifteen quarter-rate tickets did, and for the same reason: the reward
pays land and the sufficiency-shaped army term, so a model that learns
to survive a grinding defence is not rewarded for it. Standing battles
did not change that; they changed the world the reward is measured in.
N24 remains the model of record at 257 / 88 / 28.

Conclusion for the day's arc: the AI's gains came from RULES read off
traces (guarantor bar, siege reflex, the resolver work), not from the
ladder. Sixteen tickets, one climb.

**The call reflex triples rebellions (peer, seed 4242, 60 turns):** 54.53
-> 156.24 per 1k country-turns, with survival 64.2 -> 62.3 and calls 3 ->
54. Mechanism is war weariness: calls answered drag countries into wars
they did not start and each carries the weariness home. Arguably the rule
working -- alliances that actually pull people in SHOULD destabilise --
but tripling a stability metric is a decision, not a side effect, and the
seat bench cannot see it because it is spread over everyone. This is the
fourth rule trading world crowding for seat competence and much the
steepest, so it goes to the user with the other three; it is a knob
(OD_CALL_REFLEX) either way. My seat arms decide only whether it earns
its place on the bench.

## 50 — v24 tabled: sea supply + callable allies; my call reflex is off

    model    v23         v24      v24 with the call reflex
    N24   257/88/28   249/88/28   230/88/28
    N37   249/88/28   225/88/26   224/88/26

The peer's two resolver rules cost the frozen models 8 and 24 on rating
with survival and the floor unchanged -- landings that lose their fleet
now fight at 0.45, and no frozen model knows to keep hulls on station.
My call reflex costs 19 on N24 and 1 on N37 AND triples rebellions
(54.53 -> 156.24 per 1k, peer). Off by default; it is a knob and the
question of whether alliances should really drag countries into wars is
the user's, alongside the other three rules that trade world crowding
for seat competence. N24 remains the model of record: 249 / 88 / 28 on
the current binary, 257 on v23.

## 51 — the navy's bombard action was 99.9% no-op (v25)

N24 on v24, 80-turn eval: `navy bombard 1123 / 1124 (99.9% wasted)`. The
mask asked only "do we own a warship"; the executor then needs researched
ammunition it can afford AND an enemy port inside a hull's range, and
almost never has both. Same waste the doctrine action had before
enactablePolicy (97.4%), same fix: `bombardAvailable(cid)`, cached per
turn, offered only when the executor would fire. After: 3 / 4 attempts.
That is roughly a quarter of the navy head's decisions returned to it.
Knob OD_BOMBARD_GATE=0 restores the old mask. Five-model arms next.

## 52 — masking out a wasted action is worse than the waste (both gates off)

Isolated on ONE binary, gate on vs off:

    shipped   89 / 48 /  0   vs  148 / 82 / 31   (SWE and CHN annihilated with it)
    N24      239 / 88 / 28   vs  249 / 88 / 28

The bombard gate does exactly what it claims -- no-ops 1123/1124 -> 3/4 --
and three of five trained models read higher WITH it, which is why the
first table looked like a win. On one binary against its own control it
costs 59 on the shipped model and 10 on N24. The file already carried
this lesson from the reinforce experiment: "mask out the head's favourite
safe action and it takes the next safe one" -- the freed probability does
not go where the mask designer expects. A wasted action is not free, but
it is cheaper than the action that replaces it.

Both mask gates (OD_BOMBARD_GATE, OD_REINFORCE_GATE) are off by default.
The finding worth keeping is methodological: three-of-five "up" against
mixed historical baselines was noise; the isolated pair on one binary is
the measurement. Ruler unchanged: N24 249 / 88 / 28 is the model of
record.

**Two standing rules out of today (both sessions agree).** (1) A per-call
INVARIANT proves a change is what you think it is and says nothing about
how a learned policy redistributes around it: resolver arithmetic needs
only the property, anything the heads choose among needs the property AND
an isolated bench. The mask gate would have passed its invariant and
still cost 59. (2) Comparison against a historical baseline is unsound in
this tree by default -- five separate confusions today came from it, and
every real finding came from one binary with its own control.

## 53 — planning: two knob experiments and one architectural change (user-approved)

Diagnosis, all three from the code rather than the models: the credit
horizon is 12 turns against a 120-turn bench; the latent MCTS exists and
is off; and nothing the AI does persists, so a plan cannot be expressed
and therefore cannot be rewarded. Running now: search arms (32 and 128
sims, N24 and N37, controls 249 and 225) and N59, a retrain from N24 with
OD_N_STEP=32. Designed: docs/ai/CAMPAIGNS.md -- one new war action that
opens a multi-turn commitment (target, staging province, budget,
deadline) executed by a resolver, so one decision owns twelve turns of
consequences and the EXISTING reward covers the whole decision. Evidence
it is the right shape: standing battles, the only change today that made
a decision persist, raised the frozen models' floor from 15-21 to 26-33.

## 54 — the AI is named and versioned: ParrotZero 8.1.0

User's name and joke: it has AlphaZero's shape and uses little of it, so it
imitates thinking. Version is independent of the game's 1.1.2a --
ARCH.RULES.PATCH, where ARCH IS the model file's format byte (static_assert
in saveModel so they cannot drift), RULES bumps whenever a bench-visible
behaviour changes, PATCH for traces and off-by-default knobs. Model files
tagged parrotzero-8.1-N24 so "which model" and "which rules" stop being the
same question. The eval prints the version; od_bench stores it per label and
prints MIXED VERSIONS if the binary changed mid-run, which happened twice
today and silently invalidated two tables. Docs: docs/ai/VERSIONING.md,
definition in src/ai/AIVersion.h.

## 55 — the two planning knobs, measured

**Search at play time costs.** N24 249 -> 228 at 32 simulations and 227 at
128; N37 225 -> 207 at 32. The header's own design note said this would
happen: the payoff of the latent MCTS is training the policy toward the
visit distribution, not re-ranking at inference. Its value is untested and
remains so; what is now measured is that switching it on for a frozen
policy is a loss. Knob unchanged (off).

**A longer credit horizon costs too, on one ticket.** N59 (from N24, 32
turns of credit instead of 12, 4,860 turns): 134 / 74 / 13 against N24's
249 / 88 / 28. Reading it honestly: a longer window with the SAME
bootstrap discount and the same 400-turn stagnation rotation means each
update carries more variance and no more real information, because the
worlds still freeze before the horizon pays. The horizon was never the
only thing missing -- what is missing is something for the credit to
attach to, which is the campaign design.

Both knob experiments therefore point at the same conclusion the design
already argued: the AI cannot plan because a plan cannot be EXPRESSED,
and lengthening credit or adding search to a policy that has no
multi-turn commitment to make changes nothing. Campaigns next.

## 56 — campaigns built (ParrotZero, OD_CAMPAIGNS, off by default)

Phase 1 per the amended design: opened by a reflex rather than a ninth war
action, because the war policy head is {TRUNK_OUT, WAR_ACTIONS=8} and a
ninth action is an ARCH bump that refuses every model file we have.

Game::Campaign holds countryId, targetCountry, current objective, staging
province, budget and deadline; Game::processCampaigns closes on BEATEN (no
reachable enemy ground), peace, deadline or spent force, with a four-turn
grace so a campaign is not closed on the turn its staging province marches.
While one is open it steers the ORDINARY actions: recruitment raises men at
the staging province, reinforcement sends there first, and the attack
chooser prefers any province of the victim.

First implementation targeted a PROVINCE and was pointless -- 84 of 89
closed the turn they opened, because an adjacent beatable province falls in
one turn (97% of assaults are walk-ins). Re-targeted at a COUNTRY: 58
opened, lifetimes 1-12 turns, 46 ending BEATEN. The lesson is in the design
doc: the grain of a plan must match the grain of the game's slowest real
process, which here is finishing an enemy rather than taking a tile.

Inert with OD_CAMPAIGNS unset (verified: identical survival and land held,
twice, seed 4242). Benching now against same-binary controls; a retrain
follows only if the mechanism earns it, and the ARCH bump that gives the
head the choice follows only after that.

## 57 — campaigns work: the first thing all day to lift the floor

Same binary, controls reproducing their earlier numbers exactly (249, 225):

    N24  off 249 / 88 /  28    on 265 / 100 / 110
    N37  off 225 / 86 /  18    on 238 /  88 /  31

N24 with campaigns is the highest any model has read, and its survival is
100 with a worst seat of 110: EVERY seat above par, which has not happened
before on any ruler. Norway -- the seat no rule reached all day, 15-33 on
every model since the depth resolver -- goes 28 -> 110. The mechanism is
visible in the trace: Norway opens a campaign against the one neighbour
that is beating it, and instead of the war head re-deciding every turn
between recruiting, reinforcing and attacking somewhere else, the
commitment keeps men flowing to the staging province and attacks pointed
at the same victim until it is BEATEN.

That is exactly the argument in docs/ai/CAMPAIGNS.md: the reward did not
change, the credit horizon did not change, the model did not change. One
decision now owns twelve turns, so the existing twelve-turn credit window
covers a whole decision. Three more models and the shipped one are
benching; if it holds, campaigns go on by default and ParrotZero becomes
8.2.0.

**ParrotZero 8.2.0: campaigns on by default** (user's instruction, on the
two-model evidence). RULES bumped 1 -> 2 per docs/ai/VERSIONING.md, since
this is behaviour a bench can see. OD_CAMPAIGNS=0 restores 8.1.0 behaviour.
The remaining three models and the shipped one are re-benched with the knob
set EXPLICITLY on both arms, because flipping a default silently changes
what an unset-knob control measures -- the same class of error as comparing
against a historical baseline.

**Stage 5 of unit types is starting (peer).** Research nodes unlock militia,
assault infantry and mechanised; the recruit order carries a kind; every
inert piece from stages 1-4 goes live at once (manpower, money and munitions
multipliers, and the three combat columns). Ruler HELD at ParrotZero 8.2.0:
the reference binary is build/loop/reference/OpenDoctrinesServer-8.2.0 and
the 8.2.0 arms now running are the last numbers taken before it. Their two
points, both agreed: OD_TYPE_INVARIANT stops proving inertness the moment a
second kind exists (by design -- it still proves all-line fights untouched),
so from there the instruments are the seat bench and their world eval on ONE
binary; and this is a retrain rather than a re-gate, because recruitment
gains a kind and a frozen model has no vocabulary for it.

Plan when it lands: (1) gate the five frozen models on the new binary to
price the world change alone; (2) retrain from the best of them with the
action available; (3) report whether the retrained model USES the kinds or
converges on line infantry anyway -- their question, and either answer is a
finding: convergence means the first-draft TROOP_TYPES columns are wrong or
the choice is not worth making.

## 58 — campaigns on five models: a wash on rating, a big lift on the floor

    model       campaigns off      campaigns on   delta
    N24         249/ 88/  28      265/100/ 110     +16
    N37         225/ 86/  18      238/ 88/  31     +13
    N47         232/ 90/  38      202/ 85/  41     -30
    N43         202/ 84/  23      241/ 88/  31     +39
    N35         235/ 89/  36      212/ 85/  18     -22
    mean        229/ 87/  29      232/ 89/  46      +3

The two-model result that justified the default does not generalise on
RATING: three models up (+16, +13, +39), two down (-30, -22), mean +3,
which is noise. What does generalise is what the mechanism was for:
survival 87 -> 89 and the worst seat 29 -> 46, with N24 reaching a floor
of 110 (every seat above par). A commitment stops a country being picked
apart while its war head re-decides; it does not make a strong country
stronger, and on N47 and N35 it costs growth seats (N47's rush 187 -> 69).

Kept ON per the user's instruction, and reported to them as this: campaigns
buy survivability and the floor, not rating. The knob is OD_CAMPAIGNS=0.
The shipped model's pair is still benching and matters most for a release.

**Campaigns and the SHIPPED model: 148 / 82 / 31 -> 137 / 71 / 1.** The
1.1.2a weights lose 11 rating, 11 survival and their floor (modern China
annihilated) with campaigns on. So the default interacts with which model
ships:

    ship the 1.1.2a weights   -> campaigns OFF (148 vs 137)
    ship N24                  -> campaigns ON  (265 vs 249, floor 110)

Recommendation to the user: ship N24 with campaigns on, which is the best
combination measured today by a wide margin, and the one the default is
set for. If the release keeps the current model.bin, set OD_CAMPAIGNS=0.
Both are one line, and data/ai/model.bin remains untouched by this session.

## 59 — troop kinds: the AI uses them and they cost (knob stays off)

    N24  campaigns only 265 / 100 / 110    with kinds 231 / 88 / 26
    N37  campaigns only 238 /  88 /  31    with kinds 210 / 87 / 23

The rule works as designed -- 24% of China's 322M recruits are militia --
and the seats say it is the wrong trade. Militia is 0.7 attack and 1.1
defence at half the money and 1.2 frontage, so a defending country buys
more bodies per dollar and gets LESS power on the ground it has to hold,
because frontage is the binding constraint in this resolver (25% of
assaults are width-bound) and militia occupies 20% more of it per man.
The cost per man was the wrong denominator: it should be power per unit
of FRONTAGE, not per unit of money, wherever the fight is width-bound.

Kept behind OD_TROOP_KINDS, default off, and the finding sent to the peer
with the recruit distribution they asked for: the AI does reach for the
kinds, and the first-draft columns make militia a bad buy for the case
the rule picks it for. That is evidence about the columns, not only about
my rule -- a defensive kind that is worse at holding a frontage than line
infantry has no situation it is right for.

## 60 — lookahead, done exactly rather than learned (user's request)

Three facts settled first. The forward model IS trained (20.8M updates,
10,415% of warmup), so the latent MCTS is not hallucinating; it simply does
not pay at PLAY time, exactly as its own header says (the payoff is training
the policy toward the visit distribution). And the bench's difficulty rung
has searchDepth 0 -- only the self-play rung searches -- so no bench number
today has ever included search.

So: (a) N60 trains from N24 with OD_MCTS_SIMS=64, testing the operator the
header says actually pays; and (b) the AI now looks ahead EXACTLY, in the
one place where a plan exists to look ahead about.

`projectCampaign` plays the war forward in closed form over the campaign's
whole deadline using the resolver's own quantities -- frontage, depth beyond
it, fort and supply, the attacker's and defender's modifiers, and the
defender's replacement rate from its population. It answers the two
questions a commitment poses: can we finish them inside the deadline, and
what is left of the committed force. A war of choice is opened only if it
finishes with at least a quarter of the force intact, and the score prefers
sooner and cheaper. A DEFENSIVE campaign -- against a neighbour already on
our ground -- skips the finishability test, because declining to focus on a
war we are already losing does not make it go away.

That last clause exists because the first version collided with the recall
rule and cost Norway everything: recall closed any campaign while home was
threatened, and the projection refused any unfinishable war, so a besieged
country did neither. Norway is the case both rules were built from. Now
recall fires only when the threat is somebody OTHER than the campaign's
target, and Norway opens against its invader again.

**N60, trained WITH the latent search (64 sims, 6,808 turns): 87 / 72 / 23**
against N24's 249 without campaigns and 265 with. The operator the header
argues for -- pulling the policy toward the MCTS visit distribution -- did
not merely fail to help, it destroyed the policy: 87 is below the shipped
1.1.2a model. Most likely the cross-entropy pull (MCTS_POLICY_WEIGHT 0.5)
is far too strong against visits from a latent rollout, or the visit
distribution is poor enough that imitating it is worse than the prior.
Either way the claim in the header is now measured and it is negative in
the form it is written.

That is FOUR failures of foresight today in four different forms: search at
play time (-21), a longer credit horizon (-115), an exact closed-form
projection (-20 mean, floor 50 -> 14), and now training toward search
visits (-162). Against one success for commitment (campaigns, floor 30 ->
50). The pattern is consistent enough to write down as a finding rather
than a run of bad luck: in this game, at this level of play, deciding well
about the CURRENT turn and then sticking to the decision beats reasoning
about future turns -- because the futures the AI can model are dominated by
what its dozens of opponents do, and it models them badly.

## 61 — recall measured and rejected; 8.2.0 stands

    model     campaigns (8.2.0)      + recall
    N24           265/100/ 110    222/ 87/  23
    N37           238/ 88/  31    196/ 87/  23
    N47           202/ 85/  41    238/ 88/  26
    N35           212/ 85/  18    204/ 87/  21
    mean          229/ 90/  50    215/ 87/  23

Recall closes a campaign when home is threatened by somebody other than
its target. It does what it was written for -- N47, the model that lost
most to campaigns, recovers 202 -> 238 -- and it costs more elsewhere:
mean 229 -> 215, worst seat 50 -> 23, N24 265 -> 222 with its floor
falling 110 -> 23. Default OFF.

The reason is the same sentence three times today: a commitment that is
abandoned when things get difficult is not a commitment, and the floor
was the entire gain. Campaigns, recall, the projection and the withdraw
reflexes are one family -- three of the four try to make the AI let go of
something, and all three measured worse than holding on.

ParrotZero 8.2.0 stands as measured: campaigns on, everything else off.

## 62 — the commitment size was already right

    share      N24              N47
    0.20    200 / 84 / 15    205 / 87 / 26
    0.35    265 /100 /110    202 / 85 / 41   (the default, unchanged)
    0.50    227 / 88 /  26   189 / 85 / 18

A third of the army is the best of the three on both models, and the shape
is a peak rather than a slope: too small and the commitment cannot finish
anything, too large and it strips the country that made it. 0.35 was a
guess written with the feature and the sweep says the guess was right,
which is worth recording precisely because it is the boring outcome --
the parameter I most suspected turned out not to be the problem.

Knob kept (OD_CAMPAIGN_SHARE). Nothing changes.

**N61 (retrained from N24 in a world where campaigns exist, 7,106 turns):
212 / 88 / 23** against N24's 265 / 100 / 110 on the same rules. The
retrain that finally had a reason -- the world now rewards persistence, so
a policy fitted to it should do better -- lost 53. That is the eighteenth
training ticket today and the seventeenth to fall.

Recovered by hand: the ticket's shell chain died after training (killed by
one of my pkill sweeps for bench processes), leaving the weights in
dataN61 and no bench. Lesson for the loop's own plumbing: a pkill pattern
broad enough to catch bench processes catches the chains that launch them,
so a training chain should write its model out itself rather than trust a
successor step.

The day's training record: 18 tickets, 1 climb (N47). Every gain came
from a rule.

## 63 — the campaign's three parameters were all already right

                       N24              N47
    default          265 /100 /110   202 / 85 / 41
    share 0.20       200 / 84 / 15   205 / 87 / 26
    share 0.50       227 / 88 / 26   189 / 85 / 18
    two at once      219 / 87 / 23   199 / 85 / 26
    deadline 24      211 / 86 / 23   190 / 85 / 21

Three sweeps, six arms, and the guesses written with the feature beat every
alternative on the model campaigns help. A third of the army, one war at a
time, twelve turns to win it. The shape is the same each time: the value is
in COMMITTING and the alternatives all dilute the commitment -- half the
army is not more committed, it is just more exposed; two wars is not twice
the commitment, it is half of each; a longer deadline does not buy patience,
it delays the point at which a lost war is abandoned.

All three stay knobs (OD_CAMPAIGN_SHARE, OD_CAMPAIGN_MAX,
OD_CAMPAIGN_DEADLINE) with the numbers recorded, and nothing changes.

## 64 — the ceasefire action is dead, so peace becomes a reflex

France in the rush world, 120 turns, war-head actions: reinforce 726,
recruit 99, attack 94, declare 5, stage 36, ceasefire ZERO. It fights five
wars at once and never once asks anybody to stop. The action is not masked
out -- the policy simply never chooses it, exactly as the econ head never
chose fortify.

`peaceReflex` (OD_PEACE_REFLEX): while a campaign is open, offer white
peace to every OTHER enemy, weakest first, one a turn. It is the
complement of the campaign and the two together are a doctrine rather than
two tricks: commit to one war and end the others. The offer goes through
the ordinary diplomatic queue, so the resolver's own rules decide whether
it is accepted -- this adds an ASK, not an outcome.

Fires as intended (France offers peace to two rebel states on turns 3 and
4 to concentrate on cid 46). Four models benching.

**The peace reflex is a clear loss on 4 of 4: N24 265 -> 199, N47 202 ->
166, N37 238 -> 188, N35 212 -> 168.** Default off.

Why it fails is the sharpest version of today's recurring lesson. Ending
the wars a campaign is not about sounds like concentration, and it is
actually SURRENDER: a white peace gives up every claim and every gain
against that enemy, and the seats are scored on land held. The AI was not
fighting five wars badly -- it was holding five fronts, and three of them
were feeding it. "One war at a time" is a maxim from a different game,
where wars cost upkeep and peace is free; here a war you are winning on a
quiet front is just land arriving slowly.

That is now the fifth rule in the family (withdraw x2, recall, projection,
peace) that tries to make the AI GIVE SOMETHING UP and measures worse. The
one rule that measured better -- campaigns -- takes something on instead.
For this game the rule of thumb is: add commitments, never subtract them.

## 65 — the war economy: the campaign reaches the industry head

Following the one principle the day supports -- ADD commitments, never
subtract them -- the campaign now also steers the economy: while one is
open the next factory goes up in the province the war is fed through
(OD_CAMPAIGN_ECON, off until measured). `industryBuyAt` asks
nextIndustryBuy's own question about one named province, so a campaign
cannot order an upgrade the ordinary rule would refuse -- over the
national cap, over the province's capacity, or already pending.

This is the fourth thing a campaign steers (recruit, reinforce, attack,
now industry). Four models benching.

**War economy measured: N24 265 -> 195, N47 202 -> 217, N37 238 -> 190,
N35 212 -> 152. Mean 229 -> 189. Default off.**

So "add commitments, never subtract" is not a licence either -- it is a
description of what worked, not a law. The campaign steers force, and
force is fungible: men raised at the staging province fight at the front
this turn. A FACTORY is not: it takes IND_TURNS to build, it pays back
over the rest of the game, and putting it in the province a twelve-turn
war is being fought through means putting it where the war might be lost.
The campaign's horizon is twelve turns; industry's is a hundred. Steering
a long-horizon decision with a short-horizon commitment is the mistake,
and it is the mirror image of this morning's: there I gave a short
decision a long horizon (n-step 32, -115) and here a long decision a short
one.

Rule of thumb, sharper than the last: a commitment may steer decisions
whose payback is INSIDE its own deadline, and must not steer decisions
that outlive it.

## 66 — research: the navy focus never worked, and formations had no focus

The user changed research (a new "formations" category whose three nodes
unlock militia, assault infantry and mechanised). Reading the AI's picker
to teach it the new branch turned up an older bug in the same three lines.

FOCUS is {"buildings", "army", "navy"} and the picker compared `want`
against the node's CATEGORY. "navy" is a SUBCATEGORY -- its category is
"army" -- so the third research action has never selected a navy node:
it always fell through to "cheapest node anywhere". Same silent
degradation as the capped-industry bug recorded a few lines above it in
the same function, and the same symptom: a branch that advances only by
accident.

Fix (OD_RESEARCH_FOCUS, off until measured): `want` matches the category
OR the subcategory, and the army focus also covers "formations" --
formations are what an army is made of, and this keeps three actions
rather than needing a fourth and an ARCH bump. Four models benching.

(The rebuild during the guns/labs arms is behaviour-neutral: focusFix is
read once into a static and the off path is the original expression.)

**Both campaign extensions fail: artillery-at-the-target N24 265 -> 234,
N37 238 -> 218; research-held-during-a-campaign N24 265 -> 223, N37 238 ->
220. Both default off.**

The horizon rule passed them and they still lost, so it is necessary and
not sufficient. What the two have in common with the war economy is that
they take a resource the ordinary rule was allocating WELL and hand it to
the campaign: shells were already going to the frontier that needed them,
and research funding was already being cut by the siege reflex when it
mattered. The campaign's value is in what it does with force, which no
other rule was allocating coherently -- the war head re-decided its target
every turn. Steering things that were already steered well is subtraction
by another name.

Refined once more: a commitment should own the decisions NOTHING ELSE
owns. Four extensions tried (industry, artillery, research, and peace),
four losses; the three original ones (recruit, reinforce, attack) are the
three the war head was thrashing over.

**Research focus fix measured: N24 265 -> 220, N37 238 -> 189, N47 202 ->
198, N35 212 -> 200. Off by default, and the BUG IS STILL REAL.**

This is the mask-gate result again in a different costume, and it is the
cleanest example of it yet. The navy focus genuinely never worked -- it
compared a category against a subcategory -- and repairing it makes every
frozen model worse, because those models were fitted in a world where the
third research action meant "buy the cheapest node available" and they
learned to use it that way. Fixing the action changes what a third of the
econ head's research decisions DO, and a frozen policy has no way to know.

So the fix ships with a retrain or not at all. It is knobbed off with the
numbers recorded, and it is the first item on the list for whenever the
model is next trained -- unlike the four campaign extensions, this one is
a defect rather than an idea, and leaving it unfixed for ever is not the
answer.

The pattern in one line, now with five instances (bombard mask, reinforce
mask, research focus, plus the two horizon rules): CHANGING WHAT AN ACTION
MEANS COSTS A FROZEN POLICY, however wrong the old meaning was.

## 67 — the retrain that is actually justified (N62)

Five results today say the same thing: a frozen policy is fitted to what
its actions MEAN, and today two of those meanings changed for good reasons
-- the research focus was repaired (navy had never worked; formations had
no focus at all) and the recruit order grew a troop kind. Both measured as
losses on frozen models and both are correct changes. That is precisely
the case where a retrain has a reason to help, and it is the case that has
been missing from the eighteen tickets that failed: those re-fitted a
moved WORLD, this one refits a moved ACTION SPACE.

N62 trains from N24 with OD_RESEARCH_FOCUS=1 and OD_TROOP_KINDS=1, and is
benched with the same two on -- the model and the rules it was trained
under, measured together, which is the only honest way to read it. Its
control is N24 at 265 under the shipped rules.

(The chain copies the model out itself this time: N61's chain was killed
by a pkill sweep after training and left the weights unbenched.)

**N62 (9,000 turns from N24, trained AND benched with the research focus
fix and troop kinds): 200 / 84 / 26.** Against N24 under the corrected
rules (220) it is 20 lower; against N24 under the shipped rules (265),
65 lower. The retrain that had a genuine reason -- the action space moved,
not the world -- still lost, and it lost to the same model it started
from playing under the same new rules.

Its recruit mix is 93% line, 7% militia after 9,000 turns of being able
to choose, which is the answer to "does a trained model use the kinds":
barely, and less than the hand-written rule did (24%). Nothing here is
learning to exploit the new options; it is learning to avoid them.

That closes the training question for this session at 19 tickets and one
climb. Every remaining item is a rule the models cannot be taught to use
without a training regime this project does not have -- longer episodes,
worlds that do not freeze at 400 turns, and a reward that pays for
something other than land.

## 68 — campaigns hold on every difficulty rung, and the rungs are different games

    rung                         campaigns off    campaigns on   delta
    1 (easy)                      165/ 76/  15    175/ 88/  31     +11
    2 (normal)                    220/ 99/  95    240/ 89/  44     +19
    3 (hard, the bench)           249/ 88/  28    265/100/ 110     +16

Every rule measured on 2026-09-05/06 was measured on rung 3 alone, and
the rungs are not the same game: difficulty decides which faculties are
switched on at all (the aim head, coalitions, the action budget, and --
per the DIFFICULTY table -- search depth). The same model reads 165, 220
and 249 on rungs 1, 2 and 3 with campaigns off, which is the AI's own
difficulty setting working as designed.

Campaigns gain on all three: +10, +20, +16. That is the first result today
verified outside the rung it was developed on, and it is the one that
matters for a release, because almost nobody plays on the hardest rung.

tools/od_bench.py now takes OD_BENCH_DIFFICULTY and records the rung in
every stored label, so two rungs can never be compared by accident -- the
same discipline the ai_version field added for binaries.

## 69 — validating the OTHER shipped rules on the rungs players use

Campaigns hold on all three rungs (+11, +19, +16). The two other rules
that ship on by default -- the guarantor bar and the siege reflex -- were
also chosen on rung 3 alone, and the same risk applies: a rule that helps
on hard and hurts on normal makes the game worse for nearly everyone who
plays it. Both are switched OFF at rungs 1 and 2 against the 165 and 220
baselines taken with them on. Campaigns are held off in all four arms so
the two rules are read alone.

**Rung validation, first two rungs (N24, campaigns off in every arm):**

    rung        both on         no guarantor bar    no siege reflex
    1 easy    165 / 76 / 15     203 / 89 / 67       157 / 88 / 26
    2 normal  220 / 99 / 95     220 / 81 / 38       253 / 89 / 33

Two rules that ship ON by default, and neither survives the rungs people
actually play:

  GUARANTOR BAR: -38 at easy, exactly neutral at normal, +17 mean at hard
  (where it was chosen). Refusing a war whose target has a bigger protector
  is only worth it when the protectors are dangerous; on the easy rung they
  are not, and the AI is declining wars it would have won.

  SIEGE REFLEX: +8 at easy, -33 at NORMAL, and it was chosen at hard.

My rung-3 column in the first pass reused stale labels from other binaries
(the width-gate and bombard-gate arms) -- the exact error this session has
recorded five times. Rerunning both arms cleanly on the current build
before drawing any conclusion about the defaults.

## 70 — a false alarm, diagnosed: the tree moved, not the AI

Sequence, all at rung 3 on N24:

    265   8.2.0 at 20:0x  (campaigns on)
    212   8.3.0 tonight   (per-rung rules)   -> looked like a 53-point regression
    212   same, with BOTH rules FORCED on    -> so the per-rung profile was innocent
    224   with defensive campaigns gated off -> a real 12-point fix, still not 265
    201   campaigns OFF on the same binary   -> THE CONTROL

The 265 was a historical baseline and the peer landed unit types stage 5
and the research rework after it, so the game itself changed underneath.
Measured properly on one binary, campaigns are still worth +23 (201 ->
224) and the day's conclusion stands. Nothing regressed; I compared
against a number from a different game, which is the exact error this
journal has recorded five times and which I repeated at 05:00.

Two real fixes came out of the false alarm, so it was not wasted:

  * DEFENSIVE CAMPAIGNS were left ON by accident when the projection and
    the recall rule collided over Norway. A threatened country opening a
    campaign against its attacker costs 12 points; at the 265 measurement
    it did not open one at all. Now OD_CAMPAIGN_DEFENSIVE, off.
  * The DIFFICULTY table gained two fields and five rows were never
    updated. C++ zero-fills missing initialisers silently, so every rung
    read both rules as OFF -- a "78-point regression" that was a missing
    brace. The header now says every row must initialise them.

The per-rung idea itself remains unproven: its ablations were measured
with campaigns OFF and do not transfer to the shipped configuration
(with campaigns on, the guarantor bar HELPS at easy: 175 vs 147). Left in
place with hard/insane on and easy/normal off is NOT justified, so the
next tick re-measures it in the configuration that ships.

## 71 — settling the per-rung question in the configuration that ships

The per-rung table was justified by ablations taken with campaigns OFF,
and campaigns are on in the shipped configuration. The first check
already contradicts it: at easy, with campaigns on, the guarantor bar is
worth +28 (175 with, 147 without) -- the OPPOSITE sign to the -39 the
ablation reported. So the two arms that decide it are "both rules forced
ON" at easy and normal, against the per-rung defaults of 147 and 240 on
the same binary. If all-on wins, the table goes back to all-true and the
per-rung claim is withdrawn.

**Per-rung gating withdrawn: both rules TRUE on every rung.** In the
shipped configuration (campaigns on) all-on beats the per-rung table by 52
at easy (199 vs 147) and 34 at normal (274 vs 240). The ablations that
suggested otherwise were taken with campaigns OFF, and effects do not
compose: an ablation is evidence about the configuration it was taken in
and nothing else.

The machinery stays (DifficultyProfile::useGuarantorBar/useSiegeReflex)
because per-rung rules are a legitimate idea and this is where such a
decision belongs; nothing currently justifies setting either false. Also
in 8.3.0: defensive campaigns off (on by accident, -12), and the bench
records its rung per label.

Two process notes, both mine: a missing brace in the difficulty table read
as a 78-point regression because C++ zero-fills silently, and a stale
20:0x baseline read as a 53-point regression because the peer's game
changes landed in between. Every conclusion tonight that looked like a bad
rule was a measurement fault, and each was found by noticing a number that
matched an earlier one too exactly.

**8.3.0 final, N24, one binary: easy 199, normal 274, hard 224.** Every
figure reproduces its forced-on arm exactly (199, 274), so the difficulty
table now does what it says. Campaigns remain worth +23 against a
same-binary control at hard.

Note for anyone reading the numbers cold: hard (224) scores LOWER than
normal (274) because the seat score is a share of the world and the
scripted opponents are stronger on the higher rung -- the same
share-of-the-world effect recorded in journal 43. Rungs are not
comparable with each other, only with themselves.

## 72 — re-establishing the reference table on the current game

Only N24 has been measured since the peer's unit-types and research work
landed; every other model's number predates it, which is exactly the stale
baseline that produced tonight's false alarm. The five other entries are
being re-run on the current binary at the bench's own rung so the table
describes one game. N24 reads 224 there.

This also re-decides the model of record: N24's lead was established on a
game that no longer exists.

## 73 — the honest table on the current game, and a new model of record

    ParrotZero 8.3.0, one binary, rung 3 -- the honest table
    model     rating  surv  worst   per seat
    N43          235    86     18   FRA  362 SWE  320 USA  390 CHN  165 rush  152 NOR   18
    N24          224    88     28   FRA  347 SWE  300 USA  391 CHN  165 rush  114 NOR   28
    N47          209    86     15   FRA  268 SWE  197 USA  357 CHN  283 rush  134 NOR   15
    N37          206    88     26   FRA  270 SWE  217 USA  377 CHN  208 rush  136 NOR   26
    N35          193    86     15   FRA  336 SWE  173 USA  327 CHN  152 rush  152 NOR   15
    shipped      120    72      4   FRA  216 SWE  120 USA  219 CHN    4 rush  134 NOR   28

Every entry measured on one binary, ParrotZero 8.3.0, after the peer's
unit-types and research work. N43 leads at 235 and N24 is second at 224 --
the ordering changed, as it has every time the game moved, which is why
the table had to be re-run rather than quoted.

RECOMMENDED MODEL: N43 (build/loop/model.N43.bin), 235 / 86 / 18. N24
remains a defensible choice: 11 points behind on rating, better on the
floor (28 vs 18) and better on the seat the whole project has struggled
with (rush 114 vs 152 favours N43, Norway 28 vs 18 favours N24). If the
release wants the safest AI rather than the strongest, N24 is it.

The shipped 1.1.2a model reads 120 on the same binary. So the day's work
is worth about +115 rating and +14 survival to a player, and the choice
between N43 and N24 is worth 11.

## 74 — verifying the recommendation itself

N43 is now the model of record but every claim behind the recommendation
was measured on N24: campaigns worth +23, and the rung sweep. A
recommendation whose evidence comes from a different model is the same
mistake as a baseline from a different binary, so N43 is being measured
with campaigns OFF at hard (its 235 is with them on) and on both rungs
below.

Results (N43, binary 8.3.0):

    arm                 rating  surv  worst
    hard, campaigns ON     235    86     18
    hard, campaigns OFF    169    88     38
    easy (rung 1)          183    81     13

Two things the N24 measurements did not show.

1. Campaigns are worth +66 on N43, not the +23 they were worth on N24.
   The size of a rule's benefit is a property of the pair (rule, model),
   not of the rule. Quoting "+23" as the value of campaigns was wrong in
   the same way as quoting a baseline from another binary.

2. Campaigns trade the floor for the ceiling: +66 rating, -20 worst
   seat. Committing to an offensive wins more where the seat can afford
   one and loses more where it cannot. That is the same shape as every
   caution rule this session, run backwards, and it means the
   N43-vs-N24 choice is not "stronger vs safer model" but the same
   ceiling/floor dial appearing twice.

Rung sweep for N43: 183 (easy) / 224 / 235 (hard). N24 reads 199 at easy,
so the two models change places by rung and "which model is better" has
no rung-free answer.

## 75 — home first: yielding the reinforcements without abandoning the aim

A campaign steers three decisions -- where new men are raised, where men
are moved, and who is attacked -- and recall treated them as one thing,
closing all three when the home front was losing. That gave back more
mean than it bought floor. But only the third is the commitment. The
first two are just a queue the campaign happens to be at the head of.

OD_CAMPAIGN_HOMEFIRST (off) keeps the campaign aiming at its victim
while returning recruit and reinforce to the ordinary threat rule
whenever the country is losing ground at home. If the floor damage comes
from men walking abroad past a burning frontier, this recovers it at no
cost to the constancy that earned the +66. If the floor damage comes
from the attacks themselves, it will do nothing, and that is worth
knowing too: it would mean the ceiling and the floor are the same
decision and the dial cannot be split.

Version 8.3.1 (a knob defaulting to off is PATCH). Both models get their
own control on this binary; the 235 predates the patch and a re-gated
binary is a new build.

Result (N43, one binary, 8.3.1):

    arm          rating  surv  worst
    control         235    86     18
    home first      217    86     18

The control reproduces 235 exactly, so the knob is inert when off. With
it on, rating falls 18 and the floor does not move AT ALL -- survival and
worst seat identical to the digit.

This answers the question the experiment was built to ask, in the
negative I said was worth having. The floor damage campaigns do is not
reinforcements walking abroad past a burning frontier; redirecting every
one of them home changed nothing about which seats collapse. It is the
ATTACKS. A seat under pressure that keeps assaulting its victim dies of
the assaults, not of the men it sent away.

So the ceiling and the floor are one decision, and the dial cannot be
split by re-routing men. The only remaining split would have to be in
the aim itself -- which is recall, already measured, already costing more
than it buys. Two different decompositions of the same commitment have
now failed the same way, which is the point at which the trade should be
treated as a property of the game rather than a knob waiting for a
better implementation.

Keeping the knob off. It stays in the tree as measured evidence, at the
cost of one branch on a decision that is already conditional.

N24 confirms and strengthens it: 224 -> 215, and its floor fell too, 28
-> 23. So home-first is not even a trade on the second model -- it loses
rating AND floor. Both controls reproduced their prior numbers to the
digit (235, 224), so the binary is sound and the knob is genuinely inert
when off. Permanently off.

## 76 — sweeping the parameters of the thing that actually works

Every rule this session was measured as on-or-off, and the two numbers
INSIDE the one rule that works have never been touched. Both were
guesses written the day campaigns were: a 12-turn deadline, and a 0.35
share whose own comment calls it "the parameter most likely to be
wrong". Campaigns are worth +66; a 10% error in their duration is worth
more than most of the rules that were tried and rejected today.

Sweeping deadline 8/18 and share 0.20/0.55 on N43, against the 235
control already established on this binary. Share is really a duration
knob -- it only feeds the "spent" close threshold -- so both arms ask
the same question from opposite ends: is a campaign currently too short
or too long?

Sweep results (N43, one binary, control 235 / 86 / 18):

    arm             rating  surv  worst
    deadline 8         209    86     15
    deadline 12 (def)  235    86     18
    deadline 18        216    86     15
    share 0.20         227    89     36
    share 0.35 (def)   235    86     18
    share 0.55         227    85      8

Two different shapes, and the second one matters.

DEADLINE is a peak. 12 turns beats both 8 and 18 by 19 and 26 points
with the floor flat across all three. The guess was right, and it is now
evidence instead of a guess. I am NOT going to sweep 13 or 14 for the
last few points: that is fitting three seeds, which this project has
paid for before.

SHARE is the dial. Both directions cost the same 8 rating points, and
the floor moves 28 points between them, monotonically: 0.20 gives 36,
0.35 gives 18, 0.55 gives 8. Survival tracks it too (89 / 86 / 85). So
the size of the committed force is exactly the ceiling-floor exchange,
and at a far better rate than anything else tried -- 8 rating for 18
floor, where recall wanted 14 for less and home-first wanted 18 for
nothing.

This CORRECTS entry 75. The trade could not be split by re-routing men
or by abandoning the aim, and I concluded it could not be split at all.
It can: not by changing WHERE the commitment goes or WHETHER it holds,
but by how BIG it is. A third of the army is a spearhead that costs the
seat its defence; a fifth is a spearhead that does not.

Confirming on N24 and N37, each against its own control, before the
default moves. One model is where the last two corrections came from.

N24 at share 0.20: 224 -> 253, survival 88 -> 90, worst seat 28 -> 41.
Not a trade on this model at all -- better on every axis, and 253 is the
best rating anything has posted this session. On N43 the same change was
-8 rating for +18 floor; on N24 it is +29 for +13.

So the committed share was not merely mistuned, it was mistuned in the
direction that hurt every model, and how much it hurt depended on the
model. That is the same lesson as the +66/+23 campaign gap, arriving
again: a knob's value is a property of (knob, model), never of the knob.
N37 with its own control decides whether 0.20 becomes the default.

N37: 206 -> 250, floor 26 -> 31, survival flat. Its control reproduced
206 exactly.

    model   control  share 0.20   floor
    N37         206         250   26 -> 31
    N24         224         253   28 -> 41
    N43         235         227   18 -> 36
    mean        222         243

Three models, three own controls, one binary. Two gain 44 and 29; the
third pays 8 rating for doubling its floor. No model's worst seat gets
worse. **0.20 is the new default: ParrotZero 8.4.0.**

Worth stating plainly what this was. Nineteen training tickets produced
one climb. Rules produced a handful. The single largest improvement of
the session came from sweeping a number that had been sitting in a
header since campaigns were written, with a comment on it that said it
was probably wrong. Nobody swept it because it was not a hypothesis --
it was a default, and defaults do not look like experiments.

Now verifying the new default reproduces 253 on N24 (a changed default
is a new build), and testing 0.12 to learn whether the curve keeps
falling or 0.20 is a peak. If 0.12 also wins on both, the honest
reading is that the right share is "as small as still takes ground" and
the next question is what the lower bound is -- not another sweep step.

## PENDING COMMIT

Campaign share default 0.35 -> 0.20 (8.4.0), the home-first knob (off,
measured negative on two models), and journal entries 74-76.

Direction test at 0.12: N24 246 (vs 253 at 0.20), N37 224 (vs 250). Both
below 0.20, and N37 gives back most of its 44 points. The new default
reproduced 253 on N24 exactly, so 8.4.0 is what it claims to be.

    share      N24    N37    N43
    0.12       246    224      -
    0.20       253    250    227
    0.35       224    206    235
    0.55         -      -    227

0.20 is a peak on both models that gained, not a point on a slope. The
curve has a genuine interior optimum: too small a commitment cannot take
ground, too large a one cannot hold home. Stopping the sweep here --
0.16 and 0.24 would be fitting three seeds for noise-sized differences,
which is the mistake this journal has recorded twice.

N43 remains the exception, best at 0.35 and worst-floored everywhere. It
is also the model that was fitted latest. Its 235 at the old default and
227 at the new one are within noise of each other, while the other two
models move 29 and 44 points, so the default follows the majority.

**Model of record changes: N24 at 253 / 90 / 41 under 8.4.0.** It leads
on rating, survival AND floor simultaneously -- the first time this
session any model has led on all three -- so the "stronger vs safer"
choice between N24 and N43 has collapsed in N24's favour.

Rungs at 8.4.0 (N24): 209 / 247 / 253, survival 91 / 90 / 90, floor
49 / 38 / 41. The change holds everywhere.

## 77 — the gate that never binds, and what it led to

AI_CAMPAIGN_MIN_MARGIN swept 1.05 / 1.15 / 1.35: 253 / 253 / 253, with
survival and floor identical to the digit at every setting. A constant
that does nothing across a 3x range is not tuned, it is INERT -- the
chosen candidate always clears it. Sweeping a constant is therefore two
experiments in one: what the best value is, and whether the code even
consults it.

Chasing why led somewhere better. OD_CAMPAIGN_TRACE on the Norway seat,
297 closed campaigns:

    closed on the turn they opened     96   (all BEATEN, 90 took 1 province)
    BEATEN later                      116
    spent                              54
    deadline                           26
    at peace                            5

A third of all campaigns commit to a country with ONE province: the
ordinary attack takes it, the victim is beaten, the campaign closes
having decided nothing. This is the same failure the design notes say
was fixed when campaigns moved from province grain to country grain --
it survived the move, because log1p(1) is still positive and a
one-province neighbour has an enormous margin, so it OUTSCORES the
targets a commitment is for.

The gate: a victim must have at least two provinces (OD_CAMPAIGN_MIN_-
VICTIM, default 2). Trace after: opens 303 -> 175, same-turn closes
96 -> 46. Measuring on N24 and N37 with their own controls. 8.5.0.

Note that this was invisible to every measurement made all session. The
bench sees a rating; only the trace sees that a third of the mechanism
was idling. Aggregate and seat numbers are both blind to it.

Result -- and it is a clean reversal of the diagnosis:

    model  control  min-victim 2      floor
    N24        253           229   41 -> 18
    N37        250           193   31 -> 15

Both models, every axis, large. Reverted to default 1 (off); the gate
stays in the tree as evidence. Back to 8.4.1.

So the one-province campaign is not waste. It takes a province, it takes
it inside the grace window, and it removes a country from the map --
and the slot it "wastes" would otherwise be spent on a harder target
the model does WORSE against. The trace showed a mechanism idling; the
bench showed the idling was profitable.

This is the third time this pattern has cost points (masking a 99.9%
no-op action: -59; heuristics that outlived their bug; now this). The
generalisation is sharper than "waste can be load-bearing". It is:

  A TRACE SHOWS WHAT A MECHANISM DOES, NEVER WHAT IT IS WORTH. Every
  "this is obviously pointless" reading of a trace this session has been
  wrong, because the counterfactual is not "the same play without the
  pointless part" -- it is "whatever the policy does instead", and that
  alternative is chosen by a network nobody inspected.

Cost: four bench runs. Worth it -- the alternative was shipping a
plausible-looking gate that would have cost 24 to 57 points, and the
reasoning behind it ("a campaign should own decisions nothing else
owns") is one I had already written into the journal as a PRINCIPLE.
The principle is wrong, or at least it does not imply this.

## 78 — the cap, read in the direction the last result pointed

The min-victim arm cut opens 303 -> 175 and lost 24 and 57 points. Read
as a dose-response rather than as a defect fix, it says: MORE campaigns
was better. The one remaining campaign constant is the cap on how many
a country may hold open at once, OD_CAMPAIGN_MAX, default 1 -- and one
is the most restrictive value it can take.

Sweeping 2 and 3 on N24, after verifying the revert reproduces 253.
This is the same sweep-the-defaults move that produced the session's
biggest gain, aimed by the failure that immediately preceded it.

Cap sweep (N24, 8.4.1, control 253 / 90 / 41):

    cap 1 (default)   253   90   41
    cap 2             259   90   41
    cap 3             259   90   41

Cap 2 and cap 3 are identical on all three metrics, so a second campaign
is sometimes wanted and a third never is. The gain is small (+6) but it
is not sampling noise -- the bench is deterministic on fixed seeds, so
253 and 259 are two different games, not two draws from one. What is
unknown is whether it generalises, which is what N37 and N43 are for.

Recording the shape of the day's arithmetic while those run: share was
+21 mean across three models, the cap is +6 on one. Both came from
constants, neither from a new idea. Every new mechanism attempted since
campaigns -- projection, recall, home-first, peace, industry, artillery,
research holds, min-victim -- has lost. The tree does not want more
machinery; it wants the machinery it has to be pointed correctly.

    model  control  cap 2        floor
    N24        253    259    41 -> 41
    N37        250    266    31 -> 31
    N43        227    231    36 -> 33
    mean       243    252

Three models, three own controls, positive on all three, floors flat.
**Cap 2 is the default: ParrotZero 8.5.0.** Cap 3 was byte-identical to
cap 2, so two is the whole of it -- the AI wants a second war and never
a third.

N37 now leads at 266. That is the third change of model-of-record today
(N43 -> N24 -> N37), each time caused by a rule change rather than by a
better model, which is worth stating plainly: the models are close
enough together that the CONFIGURATION decides which one wins. Ranking
models under a stale config is measuring the config.

8.5.0 verified: N37 266 / 88 / 31, N24 259 / 90 / 41, both reproducing
their arm numbers exactly. N37 rungs: 173 / 244 / 266, with survival 95
and floor 85 at rung 1 -- at the easy rung it barely loses a seat.

RECOMMENDED MODEL, 8.5.0:

    N37  build/loop/reference/N37-266-850.bin   266 / 88 / 31   best rating
    N24  build/loop/reference/N24-259-850.bin   259 / 90 / 41   best floor

Seven points apart on rating, ten apart on floor, in opposite
directions, and they swap places by rung (N24 209 vs N37 173 at easy).
There is no rung-free winner; N37 for the hard rung, N24 if the release
wants the seat that never collapses.

Where the day stands: shipped 1.1.2a reads 120 on this binary. The best
model reads 266. None of that came from training -- the model files are
the same ones that were on disk this morning, and every point of the
climb from 235 came from three constants and a cap.

## PENDING COMMIT

8.5.0: campaign share 0.35 -> 0.20, campaign cap 1 -> 2, the home-first
and min-victim knobs (both off, both measured negative on two models),
journal entries 74-78.

## 79 — the constant that was measured in a different game

An inventory of AISystem.h found 34 tuning constants, most never swept.
AI_MAX_CONCURRENT_WARS = 1 stands out, and its comment says it WAS
measured at 2: "the gate almost never fired, total declarations fell
just 11%".

That measurement is not usable now. It was taken before campaigns
existed, and 8.5.0 just raised the campaign cap to 2. A country may now
hold two commitments while being permitted one war it chose -- so the
second campaign can only ever point at a country already fighting it.
The cap gain (+6/+16/+4) may be exactly what leaks through that
contradiction, in which case lifting the war limit compounds it, or the
one-war rule may be load-bearing the way the one-province campaign was.

OD_MAX_WARS added (8.5.1, knob defaults to the existing constant, so
off). Testing 2 on both leaders with a fresh control.

This is the same trap as a stale baseline, one level up: not a stale
NUMBER but a stale MEASUREMENT, whose conclusion is still sitting in the
tree as a comment that reads like settled fact. The comment is honest
and was right when written. It is the game underneath it that moved.

Result:

    model  8.5.0  wars 2   survival      floor
    N37      266     277    88 -> 88   31 -> 31
    N24      259     270   90 -> 100   41 -> 97

N24 does not lose a seat anywhere on the bench: survival 100, worst 97.
The only other time that happened was N24 the day campaigns landed.

The stale measurement was not wrong when it was taken. It said the gate
"almost never fired" and that declarations fell only 11%, and that was
true of a game with no campaigns in it. What it could not see is that
the wars the gate blocks are exactly the ones a COMMITMENT feeds -- the
mechanism that would make a second war worth having was written months
later. A measurement's conclusion has a shelf life set by the parts of
the game it did not mention.

**Two chosen wars is the default: ParrotZero 8.6.0.** Verifying, adding
N43, and probing 3 to find where it stops -- the cap said two and never
three, and it would be a nice symmetry, but symmetry is not evidence.

## PENDING COMMIT

8.6.0: campaign share 0.35 -> 0.20, campaign cap 1 -> 2, concurrent wars
1 -> 2, the home-first and min-victim knobs (off, measured negative),
OD_MAX_WARS, journal entries 74-79.

wars 3: N37 261, below 277 and below the 266 it had at one war on the
old cap. Two, never three -- the same answer the campaign cap gave, from
an unrelated gate.

N43 at 8.6.0: 219, down from 231. It is the third campaign-related
change N43 has disagreed with (it wanted share 0.35, gained least from
the cap, loses on the war limit). Two of three models gain, the two
that gain are the two leaders, and one of them stops losing seats
entirely -- the default follows them. But N43 is not noise: it is a
consistent minority of one, and the honest description of 8.6.0 is that
it is tuned for the models that like commitment, not that it is
uniformly better.

FINAL STATE, 8.6.0:

    N37  build/loop/reference/N37-277-860.bin  277 /  88 / 31
    N24  build/loop/reference/N24-270-860.bin  270 / 100 / 97
    N43                                        219 /  83 / 28
    shipped 1.1.2a                             120 /  72 /  4

N24 is the better ruler even at 7 points less rating: it did not lose a
single seat on the bench. Rating is a share of a world; survival 100
with a worst seat of 97 means every seat it was handed came through.

N24 at 8.6.0 across rungs: 241 / 248 / 270 (was 209 / 247 / 253 at
8.4.0). The change lifts every rung, and the easy rung most.

## 80 — a constant that could not fire until an hour ago

AI_WAR_BAR_SECOND_FRONT (0.50) is added to the superiority bar when a
country is already at war. It was UNREACHABLE: with AI_MAX_CONCURRENT_-
WARS at 1, `if (myWars >= AI_MAX_CONCURRENT_WARS) return false` came
first, so any country with a war already left the function before this
line. 8.6.0 made it live for the first time.

So the number now governing how often a second war happens is a value
chosen for a game in which it did nothing. That is a different flavour
of staleness again -- not a measurement whose game moved, but a constant
that never had a measurement at all, sitting behind a gate that hid it.

Sweeping 0.20 and 1.00 on N37 against a fresh control. 8.6.1.

Worth naming the general shape, because it has now produced three gains
in a row: RAISING A LIMIT EXPOSES THE CONSTANTS BEHIND IT. Cap 1->2 made
the second campaign real; wars 1->2 made the second-front bar real. Each
lifted gate turns dead parameters live, and those are exactly the
parameters nobody has ever had a reason to tune.

Result -- and it is not a tuning curve:

    bar 0.20   289   87   23
    bar 0.50   277   88   31   (default, control)
    bar 1.00   286   88   26

BOTH directions beat the default, by 12 and 9. A U-shape with the
shipped value sitting at the minimum is not evidence that 0.20 is
better; it is evidence that something is wrong with the reading. Two
candidate explanations:

  1. The bar barely matters, and 277 / 286 / 289 are three different
     games inside one band -- the bench is deterministic per seed, so
     these are not draws from a distribution, but three seeds is a very
     small sample of WORLDS and a rule can be worth +12 on one world and
     -12 on another.
  2. 0.50 lands exactly on a threshold that many decisions straddle, and
     both moves away from it are improvements for unrelated reasons.

Either way the honest action is the same: do not ship a U-shape.
Replicating all three points on N24. If N24 also puts the default at the
bottom, something real is happening at 0.50 and it is worth finding. If
N24 puts them in a different order, the answer is that this constant is
inside the noise floor of a three-seed bench and should be left alone --
which is itself worth knowing, because it bounds how much any of today's
smaller gains (+4, +6) can be trusted.

    bar    N37    N24   N24 surv   N24 floor
    0.20   289    283         91          44
    0.50   277    270        100          97
    1.00   286    238        100         100

The U-shape does not replicate, and what replaces it is more useful.
On N24 the bar is a clean monotone ceiling-floor dial: cheaper second
fronts buy rating and sell survival, all the way to a bar of 1.00 where
N24 holds EVERY seat at 100 and scores 238. N37's 286 at 1.00 is
contradicted by N24's 238 at the same setting.

Two conclusions, and the second is the important one.

1. Keep 0.50. The +13 at 0.20 is real on both models, but it costs N24
   the only perfect bench any ruler has posted (100/97 -> 91/44). For a
   release AI that is the wrong side of the trade, and 0.50 is already
   sitting at the knee.

2. A ~10-point difference at this scale does NOT survive a change of
   model. N37 said 1.00 was worth +9; N24 said it was worth -32. That
   is the resolution limit of a three-seed bench, measured rather than
   assumed, and it retroactively bounds today's smaller results: the cap
   (+6 / +16 / +4) is trustworthy because all three models agreed in
   SIGN, not because 6 is a meaningful number. Any future single-model
   result under ~15 points should be treated as unmeasured until a
   second model agrees.

## 81 — the next constant behind the lifted gate

AI_WAR_WEARINESS_BLOCK (6.0) refuses a chosen war while the home front
is unhappy. It was set when only one chosen war was possible; two wars
generate weariness faster, so a threshold that was a backstop under the
old limit may now be the thing actually deciding whether the second war
happens. Same shape as the second-front bar: a lifted gate promotes the
constant behind it from backstop to binding.

Testing 10 on BOTH models from the start rather than one -- the
resolution limit measured in entry 80 says a single-model result under
~15 points is not evidence, so running one model first would just mean
running the second afterwards anyway. 8.6.2.

Result: N24 270 -> 271, N37 277 -> 277 (unchanged to the digit).

Inert. The weariness block was NOT promoted to binding by the second
war -- countries choosing a second war are not the ones with unhappy
home fronts, so the threshold sits in a region nothing reaches. Keeping
6.0 and the knob.

That is the "lifted gate exposes the constant behind it" idea failing on
its second application, which is useful: the pattern found the
second-front bar (a real dial) and then this (nothing). Being behind a
lifted gate makes a constant WORTH CHECKING, not likely to matter. Two
checks, one hit -- and the check is cheap, so that rate is fine, as long
as the prior is not mistaken for a prediction.

## 82 — the main aggression dial

AI_WAR_BAR_UNCLAIMED (2.00) is the superiority a country demands before
declaring a war of pure opportunity, and its comment records the one
retune it has had: 2.50 produced ZERO declarations per thousand
country-turns and the model held 36% of the world to a random control's
64%, so it came down to 2.00.

That tuning was done in a game with no campaigns, one chosen war, and a
second-front bar that could not fire. It is the single most consequential
number in the war module and the most stale. OD_UNCLAIMED_BAR scales it
and the naval variant together, preserving the ratio between them.

Sweeping 0.80 (bar 1.60 / 1.76) and 1.25 (2.50 / 2.75) on both models.
1.25 restores exactly the setting that produced zero declarations before
campaigns existed, which makes it a direct test of whether the old
failure was a property of the bar or of the game around it.

    N24  bar x1.00 (2.00)  270  surv 100  floor 97   (control)
    N24  bar x0.80 (1.60)  268  surv  88  floor 31
    N24  bar x1.25 (2.50)  322  surv  90  floor 38

322. The setting that was ABANDONED for producing zero declarations per
thousand country-turns is now worth +52 over the default, and it is the
highest number this project has recorded.

The old measurement was not wrong. At 2.50 with no campaigns, a country
that cleared the bar had no mechanism to press the advantage, so a rare
war was a wasted war and refusing to fight lost the map. With campaigns,
clearing the bar starts a commitment that keeps attacking for twelve
turns -- so the SAME bar now selects few wars and finishes them, which
is a different strategy wearing the same number.

That is the third and largest instance of the day's real lesson: the
value of a constant is a property of the mechanisms around it, and every
mechanism added since silently re-tuned every constant that touches it.

Cost: survival 100 -> 90, floor 97 -> 38. So this is the aggressive end
of the same dial as the second-front bar, and the question the release
has to answer is whether 322/90/38 beats 270/100/97. Not obvious --
N24 at the default does not lose a seat.

Waiting on N37 before anything moves; per entry 80, +52 on one model is
suggestive, not settled.

N37 at 1.25: 266, against its 277 control. So the models disagree in
SIGN: +52 on N24, -11 on N37. Entry 80's rule blocks this as a default.

But it does not block it as a RELEASE CONFIG. A release ships one model
and one configuration, so tuning the configuration to the model being
shipped is correct, not overfitting -- the overfitting risk is elsewhere,
and it is serious: every number today has been read off the same three
seeds, and a +52 chosen on those three seeds is exactly what fitting to
them looks like.

So OD_BENCH_SEEDS was added to od_bench (a confirmation tool only) and
the claim is being scored on three worlds it was never chosen against:
31337, 777001, 20251225. N24 control, N24 at 1.25, N37 at 1.25.

If the gain survives the hold-out, the release has a decision to make
between two genuinely different rulers, and I can put real numbers on
both. If it does not survive, then a +52 that evaporates on new worlds
is the most important negative result of the day, because it would mean
several of today's smaller decisions deserve the same scrutiny.

## 83 — the +52 was fitted, and everything today is now suspect

    N24, hold-out seeds (31337, 777001, 20251225)
      control (bar 2.00)   241
      bar 2.50             213

On the three seeds it was chosen against, raising the bar was worth +52.
On three worlds it was never chosen against, the same change is worth
-28. It does not shrink, it REVERSES. The 322 was not a discovery; it
was the bench being fitted, by me, one arm at a time.

Note the control alone: 270 on the familiar seeds, 241 on the new ones.
The seeds this project has used all day flatter N24 by about 29 points,
which means even the absolute numbers reported today describe those
three worlds and not the game.

The uncomfortable part is that nothing distinguished this experiment
from the ones that produced the shipped defaults. Same tool, same
method, same care about controls, same replication across models. The
share change, the campaign cap and the war limit were all selected by
looking at these three seeds and keeping what scored well -- which is
the definition of fitting, however careful each individual step was.
Replication across MODELS does not help, because all three models were
scored on the same three worlds.

So all three defaults are now being validated together on the hold-out
seeds against this morning's settings (share 0.35, cap 1, wars 1). If
they hold, the day stands and the discipline was adequate. If they do
not, 8.6.0 has to be reverted to 8.3.0's behaviour and today's real
output is one lesson rather than four rules -- which would still be
worth the day, but it must be reported as what it is.

This is why the hold-out was run before telling anyone 322 was real.

## 84 — the validation, and the revert

    hold-out seeds (31337, 777001, 20251225)
    model  config                      rating  surv  floor
    N24    share .35 / cap 1 / wars 1     233   100    154
    N24    share .20 / cap 2 / wars 2     241    85     13
    N37    share .35 / cap 1 / wars 1     195    88     31
    N37    share .20 / cap 2 / wars 2     200    69      3

Rating +8 and +5, both inside the resolution band entry 80 established.
Survival and floor collapse on both models: N24 100/154 -> 85/13, N37
88/31 -> 69/3. On worlds it was not tuned against, this morning's
configuration holds its seats and today's loses them.

REVERTED. share 0.35, cap 1, wars 1; version 8.3.4 (8.3.0 behaviour plus
every knob, all defaulting to the old values). The knobs stay so none of
today's work has to be redone to be re-tested.

What today's headline numbers actually were:

    claimed (fitted seeds)        real (hold-out)
    N24 270 / 100 / 97            241 / 85 / 13
    N37 277 /  88 / 31            200 / 69 /  3
    "322, past the target"        213, and -28 against its own control

Every one of those was measured carefully -- own controls, same binary,
replicated across models, inert-when-off knobs verified to the digit.
None of that helped, because all of it happened on the same three
worlds. Cross-model replication looked like independent evidence and was
not: three models scored on three shared worlds is one sample, not
three.

The finding is not "today failed". It is that THE BENCH HAS BEEN FITTED
FOR AS LONG AS IT HAS BEEN USED THIS WAY. Every constant in AISystem.h
carrying a comment like "2.50 was too high" was chosen by this same
procedure. The reference table, the model rankings, the 235/253/266/277
progression -- all of it describes three worlds.

NEXT, and it should come before any further tuning:

  1. Decide the bench. Either widen to ~6 seeds for every decision, or
     keep 3 for iteration and hold a permanent, never-tuned-against set
     for confirmation. The second is cheaper and stricter; it only works
     if the hold-out is genuinely never used to choose anything.
  2. Re-test today's three defaults INDIVIDUALLY on hold-out worlds.
     They were validated as a bundle; one of them may be good and be
     carrying the other two. The share change is the best candidate --
     it was the only one that improved the floor on every model.
  3. Re-derive the reference table on the widened bench before any
     release recommendation. The current recommendation (N24 or N37) is
     not supported.

## PENDING COMMIT

Nothing to ship from 8.4-8.6. What is worth keeping: OD_BENCH_SEEDS in
od_bench.py, the knobs (OD_CAMPAIGN_HOMEFIRST, OD_CAMPAIGN_MIN_VICTIM,
OD_MAX_WARS, OD_SECOND_FRONT_BAR, OD_WEARY_BLOCK, OD_UNCLAIMED_BAR, all
defaulting to prior behaviour), and journal entries 74-84.

## 85 — decomposition on hold-out worlds

    N24, hold-out seeds, each change ALONE against the reverted control

    config           rating  surv  floor
    control             233   100    154
    share 0.20          217    99     97
    cap 2               237   100    154
    wars 2              232    86     18

Clean, and it locates everything.

  * WARS 2 is the survival collapse. Rating unchanged (-1), survival
    100 -> 86, floor 154 -> 18. On worlds it was not tuned against,
    letting the AI choose a second war is what loses seats -- exactly
    the failure mode the original ONE WAR AT A TIME comment described,
    which is why that comment existed.
  * SHARE 0.20 is simply worse: -16 rating and -57 floor. The session's
    headline result ("mean 222 -> 243, no model's floor got worse") was
    an artefact of three worlds.
  * CAP 2 is inert where it matters: survival and floor identical to the
    digit, +4 rating inside the band. Harmless, unproven, not worth a
    default change.

None ships. The revert stands, and it is now justified per-change rather
than as a bundle.

Worth noting what the fitted bench did to the SHAPE of the conclusions,
not just their size. On the fixed seeds the story was "commitment is
undervalued: more campaigns, more wars, smaller spearheads". On unseen
worlds the story is "the existing restraint is correct and the AI's
floor is fragile". Those are opposite readings of the same mechanism.
Fitting did not add noise to a true answer; it produced a coherent,
plausible, wrong one, and every replication I ran made it look stronger.

PROTOCOL, adopted:

  1. Iterate on the three fixed seeds. They are fine for finding
     candidates and for checking a knob is inert when off.
  2. NO default changes without OD_BENCH_SEEDS confirmation on worlds
     the change was not chosen against.
  3. The hold-out set must never be used to choose anything. The moment
     a knob is tuned against 31337/777001/20251225, those seeds are
     burned and a new hold-out is needed.
  4. Cross-model agreement is not independent evidence when the models
     share worlds. Cross-WORLD agreement is.

## 86 — the reference table, re-derived on worlds nobody tuned against

    8.3.4 (this morning's configuration), hold-out seeds

    model            rating  surv  worst
    N24                 233   100    154
    N47                 199    88     31
    N37                 195    88     31
    N43                 166    86     28
    N35                 166    89     33
    shipped 1.1.2a      133    69     23

    for comparison, the fitted-seed table from entry 73:
    N43 235, N24 224, N47 209, N37 206, N35 193, shipped 120

The orderings disagree. N43 led the fitted table and was RECOMMENDED to
the user this morning at 235; on unseen worlds it is fourth at 166. N24
was the runner-up "safe" choice; it is first by 34 points and does not
lose a seat anywhere, with a worst seat at 154% of par -- a better
profile than it posted on the seeds it was selected with.

So model SELECTION survived the fitting even though rule TUNING did not,
and it is worth being precise about why rather than treating it as luck.
A model is a large object evaluated on six seats x three seeds; a knob
is one number chosen by comparing runs that differ in that number alone.
The knob search had many candidate values, a tiny effect size, and I
kept whatever scored best -- textbook conditions for fitting. The model
comparison had few candidates, large gaps, and no iterative selection
against the score. The bench was equally fitted in both cases; the
SEARCH PROCEDURE differed.

REAL RESULT OF THE SESSION, stated conservatively and confirmed on
worlds it was not chosen against:

    N24 (build/loop/reference/N24-233-holdout.bin) beats the shipped
    1.1.2a model 233 to 133, survival 100 to 69, worst seat 154 to 23,
    under the CURRENT shipped configuration with no rule changes at all.

That is worth having and it is safe to release: it needs no code change,
only the model file. Everything else from today stays in the tree as
knobs defaulting to old behaviour.

## 87 — the model ranking does not survive either

    seeds                          N24   N47
    31337, 777001, 20251225        233   199    N24 +34
    555001, 888002, 20240704       199   214    N47 -15
    mean of six unseen worlds      216   206

The ranking FLIPS. Entry 86 said "model selection survived the fitting
even though rule tuning did not", and gave a procedural reason why that
should be expected. The reason was plausible and the conclusion was
wrong: it was drawn from ONE hold-out set, which is the same error as
before wearing different clothes -- I validated a claim on three worlds
and then generalised from three worlds.

The honest statement is now: differences among the trained candidates
(N24, N37, N43, N47, N35) are comparable to world-to-world variation,
and no ordering among them is supported. N24's mean edge over N47 (216
vs 206) is real but small and rests on six worlds.

What might still survive is the only comparison with a large gap: every
trained model against the SHIPPED 1.1.2a model, which read 133 on set A
against 166-233 for the candidates. Testing shipped and N37 on set B
now. If the gap holds, the release has a defensible claim ("this model
is better than what is live") without any claim about WHICH candidate is
best -- and that is the only thing today's work needs to support.

Method note: a third correction of my own conclusion today, each one
caused by generalising from three worlds. The pattern is not that the
numbers were bad; it is that I kept forming a conclusion at the exact
moment the evidence became suggestive and then looked for confirmation
rather than for the boundary of the claim. The fix that worked, every
time, was to ask "what set of worlds was this chosen on, and what would
a different set say" BEFORE reporting -- not after.

## 88 — six unseen worlds, and the metric that does not flip

    model     rating A/B   survival A/B   floor A/B
    N24         233 / 199      100 /  94   154 /  79
    N47         199 / 214       88 /  88    31 /  28
    N37         195 / 205       88 /  84    31 /  28
    N43         166 /   -       86 /   -    28 /   -
    shipped     133 / 120       69 /  68    23 /  28

RATING flips between world sets and is not resolvable: N24 leads set A
by 34, N47 leads set B by 15. SURVIVAL and FLOOR do not flip. N24 holds
97% of its seats against ~86% for every other candidate, on both sets,
and its worst seat is three to five times theirs on both sets.

So entry 87's "no ordering is supported" was too strong -- it was true of
the metric I happened to be leading with. Rating is a SHARE of a world
and therefore depends on what the world's other countries do, which is
exactly what changes between seeds. Survival and floor are properties of
the seat, and they survive the change of worlds. The right instrument
was in the output all along, in the two columns I kept quoting and not
deciding on.

TWO CLAIMS, both confirmed on six worlds nothing was tuned against:

  1. Every trained candidate beats the shipped 1.1.2a model by a wide
     margin: rating 166-233 against 120-133, survival 84-100 against
     68-69. This is the release claim and it is safe.
  2. Among the candidates, N24 is the one to ship, on survival and floor
     rather than rating. build/loop/reference/N24-233-holdout.bin.

Both hold under the CURRENT shipped configuration -- no rule changes, no
version bump, model file only.

Session output, honestly: one model worth releasing (already trained
before today), one methodological defect found and documented, one
protocol added to the bench, six knobs added defaulting to old
behaviour, and four rule changes proposed, measured, believed, and
correctly withdrawn. No rule change from today survives.

## 89 — auditing the rule that predates today

Campaigns (8.2.0, on by default) were selected on the fitted seeds like
everything else, and they are live in the model being recommended, so
they are the highest-value audit in the tree.

    N24, campaigns ON vs OFF        rating   surv   floor
    set A   on                         233    100     154
    set A   off                        208     87      23
    set B   on                         199     94      79
    set B   off                        211     88      28

Rating flips (+25, then -12), as it now always does. Survival and floor
do not: campaigns are better on both sets, and the floor difference is
131 and 51 points. N24's entire advantage over the other candidates --
the reason it is the recommendation -- IS campaigns.

So the pre-existing rule survives the audit that today's four did not,
and it survives on the metric that generalises. That is a real
distinction and not luck: campaigns changed the floor by more than a
hundred points, which is far outside the world-to-world variation, while
today's changes moved rating by 6 to 50 points, which is not.

The practical rule this suggests for the project: a change worth
shipping should move survival or floor by more than the ~35 points that
absolute scores swing between world triples. Rating differences smaller
than that are descriptions of three worlds.

## 90 — siege audit, and the instrument problem stated cleanly

    N24, siege reflex ON vs OFF     rating   surv   floor
    set A   on                         233    100     154
    set A   off                        203     88      26
    set B   on                         199     94      79
    set B   off                        229     86      15

Rating says +30 on set A and -30 on set B. The SAME rule, the same
binary, the same model, opposite verdicts of equal size, decided
entirely by which three worlds were scored. Survival and floor agree
across both sets: +12/+8 survival, +128/+64 floor.

Two audits, two rules, one shape. Combined with entry 89:

    rule        rating A   rating B   floor A   floor B
    campaigns       +25        -12      +131       +51
    siege           +30        -30      +128       +64

Both rules are real and both are large -- on the floor. Neither is
distinguishable from noise on rating. And every rule I proposed TODAY
was judged on rating.

That is the whole session in one table. The failure was not carelessness
about controls; the controls were right, the binaries were right, the
knobs were verified inert when off. The failure was using an unstable
instrument to read small differences, and then reading them across
models that shared the instrument's worlds. Rating is a SHARE of a
world, so it moves when the world's other countries do; survival and
floor are properties of the seat.

od_bench prints all three and I led with the unstable one all day,
because it is the one the tool prints first and the one every previous
journal entry quotes.

RECOMMENDED CHANGE TO THE INSTRUMENT (not made yet, it is the user's
call): od_bench should lead with survival and floor, and print rating as
context. That is a change to how every future measurement gets read, so
it should not be slipped in as part of a loop iteration.

## 91 — third audit, and the audit table complete

    N24, guarantor bar ON vs OFF    rating   surv   floor
    set A   on                         233    100     154
    set A   off                        216     96      77
    set B   on                         199     94      79
    set B   off                        202     88      31

    THE AUDIT, all three shipped rules, both hold-out sets:

    rule           surv A/B    floor A/B    rating A/B
    campaigns       +13 / +6   +131 / +51    +25 / -12
    siege           +12 / +8   +128 / +64    +30 / -30
    guarantor bar    +4 / +6    +77 / +48    +17 /  -3

Three for three on survival and floor, on both sets, no exceptions. Zero
for three on rating consistency.

Also re-checked today's four rejected changes against the STABLE metric,
in case any had been rejected for the wrong reason:

    share 0.20   floor  97  (control 154)   correctly rejected
    wars 2       floor  18  (control 154)   correctly rejected
    cap 2        floor 154  (control 154)   inert, correctly not shipped
    bar 2.50     floor  38 fitted / control 154 hold-out   rejected

The verdicts do not change; only the reasoning does. That is a useful
check to have run -- it would have been easy to assume the new framework
overturns the old decisions, and it does not.

STATE AT THE END OF THIS STRETCH:

  tree      8.3.4 -- this morning's behaviour, six new knobs all
            defaulting to prior values, nothing committed
  release   N24 (build/loop/reference/N24-233-holdout.bin), model file
            only, no code change; beats shipped 1.1.2a on six unseen
            worlds by ~90 rating and ~28 survival
  bench     OD_BENCH_SEEDS added; hold-out protocol documented in the
            tool; sets A and B are now BURNED for tuning purposes and
            may only be used for confirmation
  open      whether od_bench should lead with survival and floor -- a
            change to how everything is read, left to the user

## 92 — per-seat diagnosis: the changes were size-dependent, not wrong

Same model (N24), same hold-out worlds (set A), the two configurations
side by side, par-relative per seat:

    seat                        8.3.4    8.6.3
    1939:NOR:hood (small)       153.8     12.8
    1914:SWE:rung (small)       200.0    100.0
    1914:FRA:rush               233.8    160.7
    modern:CHN:rung             240.0    345.3
    1914:FRA:rung               249.3    325.9
    1939:USA:rung (large)       323.8    500.0 (capped)

Today's configuration is not worse. It is BETTER for every large seat
(USA +176, CHN +105, FRA +77) and catastrophically worse for every small
or exposed one (NOR -141, SWE -100, FRA-rush -73). Two campaigns, two
wars and a smaller spearhead let a great power expand faster and leave a
small country with nothing at home.

Rating averaged the two halves into a wash (+8), which is why it looked
like noise. The floor caught it because the floor IS the small seat.
That is the strongest argument yet for reading floor first: it is not
merely more stable, it is the metric that notices when a change helps
one kind of country by hurting another.

HYPOTHESIS, with a mechanism rather than a curve: the aggressive
settings should be conditional on the country being big enough to
afford them. A great power with forty frontier provinces can hold two
commitments; Norway cannot.

PROTOCOL for testing it, per entry 85: sets A and B are now BURNED --
this hypothesis was DERIVED from them, so confirming on them would be
the same circularity as before. Iterate on the fixed seeds, confirm on a
fresh set C (909091, 20230115, 42424242) that nothing has touched.

## 93 — the size gate, tested properly

Fitted seeds (iteration only): control 224, threshold 8 -> 241,
threshold 15 -> 259. Set C (909091, 20230115, 42424242, untouched):

    model  config     rating   surv   floor
    N24    control       229     88      28
    N24    big15         267     85      10
    N37    control       244     88      31
    N37    big15         294     87      21

The first change today to gain on worlds it was not derived from, and it
replicated across two models: +38 and +50 rating, floor -18 and -10,
survival -3 and -1.

Two things to be careful about before calling this a win.

FIRST, one untouched set is how this morning went wrong. A DIFFERENCE
measured within one set can flip between sets just as an absolute score
can -- the war bar was +52 within set A and -28 within set B. Set D
(606060, 19171105, 88888) is running to see whether the difference
itself replicates across world sets.

SECOND, and more interesting: the gate did NOT fix the floor, which is
what it was designed to do. It was built from the diagnosis that small
countries over-commit, so gating the second commitment on owning 15
provinces should have protected Norway. Norway got worse anyway (-18 on
N24). That falsifies the mechanism as stated.

The likely reason is one this journal already knows: an AI rule is
GLOBAL. Norway's floor does not fall because Norway over-commits; it
falls because Norway's large neighbour is now permitted two wars and two
campaigns. Gating on the actor's own size cannot help the victim of a
bigger actor. If that is right, no gate on the committing country will
ever protect the small seats, and the trade is structural: this rule
makes big AI countries stronger, and every small seat is measured
against big AI countries.

Which reframes the decision. It is not "does the AI play better" but
"should great powers in this game be more dangerous". That is a design
question about the game the user is shipping, not a tuning question, and
it is theirs to answer.

## 94 — set D, and the first result that earns belief

    pair          rating         surv        floor
    N24 set C   229 -> 267 (+38)   -3          -18
    N37 set C   244 -> 294 (+50)   -1          -10
    N24 set D   205 -> 272 (+67)   -7          -43
    N37 set D   210 -> 258 (+48)   +3           +7
    mean               +51         -2          -16

Rating positive in 4 of 4 pairs, mean +51, across two models and two
world sets that had no part in deriving the rule. Compare the war bar:
+52 on the set it was chosen on, -28 on the next. This is a different
kind of object -- derived on A/B, iterated on the fitted seeds,
confirmed twice on untouched worlds, and positive every time.

Survival is flat (-2). Floor is mixed: -18, -10, -43, +7, mean -16, with
the large loss on one pair. Under the strict floor criterion from entry
90 this does not pass. But that criterion was written for changes whose
rating gain was inside the noise; here the rating gain is +51 with 4/4
consistency, and the floor cost is neither consistent in size nor
consistent in sign.

So the honest summary is a TRADE, measured properly for once:

    the AI plays large countries substantially better, and its weakest
    seat gets somewhat worse, by an amount that varies a lot by world.

DEFAULT STAYS OFF. Not because the evidence is weak -- it is the best of
the day -- but because it is a game-design decision rather than a tuning
one: it makes great powers more dangerous, which changes what the game
FEELS like for a player in a small country, and that is the user's call.
The knob is OD_BIG_PROVINCES=15 with OD_CAMPAIGN_MAX=2 OD_MAX_WARS=2.

Method note worth keeping: the mechanism behind this rule was FALSIFIED
(entry 93 -- it was supposed to protect small seats and did not), and
the rule still works. The diagnosis pointed at the right knob for the
wrong reason. That is worth remembering as a counterweight to today's
other lesson: a plausible mechanism is not evidence, but neither is a
broken mechanism a refutation of a measured effect.

## 95 — the threshold is a position on one dial

    N24, mean of the two untouched sets (C and D)

    threshold          rating   floor
    off (one front)       217      58
    25 provinces          238      39
    15 provinces          270      27

Monotone in both columns. A stricter gate does not buy the rating gain
more cheaply -- it buys less of it, and returns floor in proportion.
There is no setting that escapes the trade, which is what "structural"
means and is much stronger evidence for it than the falsified mechanism
in entry 93.

So the deliverable here is not a recommended value; it is a measured
curve. The user picks a point on it:

    "great powers restrained"      217 rating, 58 floor
    "great powers dangerous"       270 rating, 27 floor

and every intermediate threshold interpolates. Default stays at the
restrained end, which is the current shipped behaviour.

This is also the first time today that a negative result and a positive
one turned out to be the same measurement. Entry 84 reverted cap 2 and
wars 2 because they cost the floor; entry 94 confirmed the same two
knobs, size-gated, gain +51 rating. Both are true. The knobs move one
dial, and which end is "better" is not a fact the bench can supply.

## 96 — the dial has one knob

    N24, campaign cap alone (size-gated), untouched sets

    set C   229 -> 232   floor 28 -> 28
    set D   205 -> 204   floor 87 -> 89

Inert. That is the fourth independent measurement of the campaign cap
(fitted seeds, hold-out A, set C, set D) and it has never moved the
floor by more than 2 points in any of them. The whole of the +51 and the
whole of the floor cost belong to AI_MAX_CONCURRENT_WARS.

So the campaign cap should simply be dropped from consideration -- it
was in every bundle today and contributed nothing to any of them. It
survived this long because it was measured once, on fitted seeds, as
+6/+16/+4 and never questioned again.

THE COMPLETE PICTURE, one knob, all on untouched worlds:

    second war for big countries     rating   floor
    off (shipped)                       217      58
    on, gated at 25 provinces           238      39
    on, gated at 15 provinces           270      27
    on, UNGATED (hold-out A)           ~233     ~18   <- no gain, floor gone

The last row is the important one. Ungated, the second war produced NO
rating gain and destroyed the floor -- that is why it was reverted this
morning. Gated on size, the same knob produces +51 and costs a fraction
of the floor. So the size gate does not protect small seats (entry 93
falsified that), but it is what makes the aggression PROFITABLE: a
second war is worth having only for a country large enough to fight it,
and letting small countries take one was pure loss.

That is a satisfying place for this thread to end. One knob, one curve,
a mechanism that is now correctly stated, and a design choice left with
the user.

## 97 — correcting entry 96: the cap is unresolved, not inert

Entry 96 said the campaign cap contributes nothing and should be
dropped. That was inferred from N24 alone. N37, directly:

    set   control   wars only   wars + cap
    C         244         273          294
    D         210         256          258

The cap adds +21 on set C and +2 on set D. On N24 alone it was +3 and
-1. So its contribution is somewhere between zero and about twenty,
depending on the world and the model, which is precisely the resolution
limit -- unresolved, not zero.

What IS established, on two models and two untouched sets:

  * the war limit carries the bulk of the gain (+29 and +46 on N37) and
    ALL of the floor movement (31 -> 21 and 21 -> 28, identical to the
    bundle's)
  * the cap is not separately justified by any measurement, but claiming
    it is inert overstates the evidence

The corrected recommendation is unchanged in substance -- the dial is the
war limit -- but the reasoning is now supported rather than inferred
from one model. Entry 96's table stands; its conclusion about the cap
does not.

This is the fifth correction of my own claim today and they all have one
cause: stating a general conclusion from the narrowest evidence that
happened to support it. The measurements were never wrong. Every single
error was in the sentence written about them.

## 98 — the econ head, measured rather than remembered

OD_ACT_HIST added: counts what each module PICKED and what the mask
OFFERED, which separates "the policy does not want this" from "the
policy is never asked". N24, Norway seat, 59,032 econ decisions:

    action                offered   taken   rate
    a7  research up        35,454  33,869  95.5%
    a8  research down      58,262  17,181  29.5%
    a10 research branch     2,105   1,903  90.4%
    a1  industry            3,447   1,878  54.5%
    a9  research branch     2,105     120   5.7%
    a2  fortify             5,675      41   0.7%
    a4  specialize          3,473       9   0.3%
    a3  PORT                1,914       0   0.0%
    a5  SHIP                3,535       0   0.0%
    a6  SHIP                2,485       0   0.0%
    a11 research branch     2,105       0   0.0%

86% of every economic decision the AI makes is moving the research
slider up or down. It is offered a port 1,914 times and a ship 6,020
times across a game and takes neither, ever. It fortifies once per 138
offers.

This is a POLICY collapse, not a mask problem -- the actions are on the
menu thousands of times. And a7 at 95.5% taken-when-offered is the
signature of a saturated head: whenever raising research is legal, it
raises research.

Consequences worth stating:

  1. Any rule that works by changing what the econ head CAN do is
     inert by construction. Four of its twelve actions are dead and two
     more are within rounding of dead.
  2. The AI never builds a port or a ship through the economy. Whatever
     naval capability the game has, the AI reaches it only through the
     navy module, if at all -- and the floor seat (1939:NOR) is a
     coastal country.
  3. "Research funding up" being taken 95.5% of the time while "down" is
     taken 29.5% of the time means the slider is being pumped, not set.

Next: whether this matters is a separate question from whether it is
true. Memory says forcing a saturated head to take its dead actions
measures WORSE (passivity is load-bearing; masking waste costs), so the
route is a reflex that acts outside the head, not a mask change. Reading
the port and ship paths before writing anything.

## 99 — a reflex for the dead actions

validEconomy already records the previous attempt: a savings reserve was
built so ports would be affordable, it produced 716 offers, and the
policy took the action FOUR times. Its comment concludes that the fix is
a retrain and warns not to re-add the reserve unless action 3's take
rate is above the floor. Measured today: 0.00%. That path stays closed.

A reflex is a different intervention -- it does not tempt the head, it
decides, and then calls execEconomy(cid, 3) and (cid, 5) so that every
constraint stays in the ONE copy that already exists: the port
candidate, the siege earmark, and the berth checked against projected
income that was added after the AI spent 144 turns of a thousand
bankrupt buying hulls it could not crew. If the executor refuses,
nothing happens.

Proven to fire before being measured: 24 ports and 62 destroyers bought
in one Norway game where the policy builds zero of each.

It is also a cheap answer to an expensive question. If handing the AI a
navy it cannot buy for itself is worth nothing, the retrain that would
unlock ports is not worth running either -- and that retrain has been
the standing recommendation for the econ head since before today.

OD_NAVAL_REFLEX, off by default, cadence 8 turns (OD_NAVAL_CADENCE).
Iterating on fitted seeds; confirmation on untouched worlds only if the
fitted result is worth confirming.

Result (fitted seeds, own controls):

    model  control   naval reflex   surv        floor
    N24        224            184   88 -> 87   28 -> 28
    N37        206            213   88 -> 87   26 -> 23

-40 and +7. No evidence of benefit, clear evidence of harm on one model,
floors flat to slightly worse. Knob stays off.

The valuable part is what it closes. The econ head's dead actions have
carried a standing recommendation -- "the fix is a retrain on the
corrected action space" -- written into validEconomy and into memory.
That retrain costs hours. This measured, for the price of four bench
runs, whether the capability it would unlock is worth having: handed 24
ports and 62 destroyers it would never buy, the AI does not play better.
So the retrain is not justified by the naval half of the argument, and
the head's refusal to buy hulls looks less like collapse and more like
the correct response to the berth costs that bankrupted it before.

Being precise about what was tested: "buy a port, else a destroyer,
whenever the executor allows, every 8 turns". Not "naval capability is
worthless". A rule that bought hulls only when a coastal threat existed
could still be worth something -- but the burden of proof has moved, and
nothing here suggests where such a rule would find its 40 points back.

KEEPING: OD_ACT_HIST. Picked-vs-offered per module action is the first
instrument this project has for "is this part of the AI alive", and it
answered in one run a question that had been carried as an assumption
for weeks. It is off by default and costs nothing when off.

## 100 — what the instrument says about the whole AI

POLITICS (9,134 decisions, France seat):

    a0  nothing                     64.4%
    a8  enact a CALMING doctrine    18.7%
    a2  pacification up             14.6%   (26.4% when offered)
    a6  propose NAP                  1.2%   ( 3.6% when offered)
    a9  conciliate a minority        0.8%   ( 1.5%)
    a5  propose alliance             0.2%   ( 0.5%)
    a1  enact ANY doctrine           0.0%   (0 of 4,966)
    a3  pacification down            0.0%   (0 of 4,618)
    a4  cancel a costly policy       0.0%   (0 of 4,189)
    a7  propose guarantee            0.0%   (0 of 3,149)
    a10 repress a minority           0.0%   (0 of 9,134 -- offered EVERY turn)
    a11 buy a province               0.0%   (0 of 818)

WAR is the healthiest head (recruit 89% when offered, reinforce 42%,
attack 61%) with one dead spot: a6, OFFER CEASEFIRE, 10 of 14,841.

Corrected before concluding: doctrines ARE enacted, through a8, 1,707
times -- but only ever the calming ones. The AI treats the doctrine
system exclusively as an unrest tool and never for economic or military
effect. So the politics head is a DOMESTIC UNREST head that happens to
have diplomacy actions attached.

THE SYNTHESIS, and it is the useful part of today:

Every action these heads refuse is one this project has independently
found to be BAD IN THE GAME'S RULES.

    ships          measured today: forcing them costs -40
    pacts/NAPs     measured before: breaking one is free, so they
                   prevent nothing; forcing signing cost -54
    ceasefire      the AI is usually winning its wars when offered it
    hulls          bankrupted the AI so badly that berth constraints
                   had to be added

The heads are not collapsed. They have correctly learned that several of
the game's mechanics do not pay. "The AI ignores diplomacy" and
"diplomacy has no teeth" are the same fact seen from two ends, and this
is the third independent confirmation.

WHAT THAT MEANS FOR DIRECTION. The AI cannot be made to use diplomacy,
navies or doctrines-for-effect by any amount of AI work -- every attempt
so far has measured negative, and the reason is that the actions are
genuinely worse than the alternatives. Making them worth using is a
RULES change (pacts that bind, hulls that earn their berth, doctrines
with effects worth paying for), which is the other session's domain.

That is worth telling the user plainly: the remaining large gains in
this AI are gated on game design, not on the AI.

## 101 — the reinforcement that ignored its own arithmetic

reinforceProvince moved a flat 50 men on every call. garrisonReflex
ranks frontier provinces by exactly how many men they are outnumbered BY
-- computes the deficit, sorts on it, uses it to choose the target --
and then calls reinforceProvince, which discards it and sends 50.

That is the mirror image of the defect this journal already names:
re-deriving a resolver's numbers makes a silent second copy; DISCARDING
a number you already computed makes the rule act on a decision it did
not make. The reflex knows a province is short two thousand men and
sends fifty.

OD_REINF_SIZED (off) sends what was asked for, capped at half the source
garrison -- a province emptied to feed its neighbour is simply the next
deficit -- and floored at the old 50 so no call becomes weaker than
before. The campaign and ordinary reinforce paths pass no deficit and
keep the flat behaviour, because they have no quantity to quote.

This is a defensive change, which matters: floor is the metric that
generalises, and every gain measured today came from the offensive side
of the same head.

Also noted, not fixed: the comment in reinforceProvince says it never
strips a province that is itself under threat, and the code does no such
check -- it takes the neighbour with the largest garrison. That is a
second instance of a comment describing an intent the code never had
(the first, st.frontiers[0], is already flagged a few lines above it).
Left alone for now: changing source selection at the same time as
quantity would confound the measurement.

Result: N24 224 -> 214, N37 206 -> 186, and on BOTH models the worst
seat went to ZERO (28 -> 0, 26 -> 0) with survival 88 -> 83 and 88 -> 69.
A floor of zero means a seat was wiped off the map.

The cause is the guard that was never implemented. Sizing the move to
the deficit takes up to half the source garrison; the source is chosen
as "the neighbour with the most men", threatened or not; so the reflex
now strips a defended province to feed a losing one and loses both. The
flat 50 had been doing the guard's job by accident for as long as the
function has existed -- another magic number that was load-bearing, and
the second one found today.

I deferred the source-selection fix explicitly to avoid confounding the
measurement. The result says the two are not separable: quantity and
source selection are one rule, and measuring half of it measured a bug.

Implemented the guard (OD_REINF_GUARD, off): skip any neighbour that has
an enemy stack adjacent to it. Now measuring guard alone, and guard with
sizing -- in that order, because if the guard alone is what matters then
sizing is a distraction, and if guard+sizing beats both then the flat 50
really was costing something.

## 102 — the guard, and what the flat 50 was hiding

Fitted seeds, own controls:

    model  control   guard only   surv        floor
    N24        224          258   88 -> 89   28 -> 33
    N37        206          225   88 -> 88   26 -> 26
    N24 guard + sizing      262   88 -> 88   28 -> 26

+34 and +19, with survival and floor flat or better on both models. No
trade -- the first change measured today that improves every axis at
once. Sizing on top adds 4 rating and gives back the floor gain, so the
GUARD alone is the configuration; the deficit-sizing that started this
was a red herring that happened to expose the real defect.

What the sequence actually was:

  1. noticed reinforceProvince throws away the deficit garrisonReflex
     computes, and sized the move to it
  2. that took the worst seat to ZERO on both models
  3. the cause was a guard the comment had promised for as long as the
     function existed and which was never written; the flat 50 had been
     performing it by accident
  4. writing the guard is worth +34 and +19 on its own, and makes the
     sizing harmless but pointless

So the bug was found by breaking something adjacent to it. The failed
experiment was not wasted, it was the diagnostic -- and the fix is not
the thing I set out to change.

Confirming on both untouched sets before believing any of it, which is
the step that has caught every false positive today.

Hold-out result -- the guard does not survive either:

    set      N24 rating   N37 rating   N24 floor
    fitted         +34          +19    28 -> 33
    set C          +24           +8    28 -> 23
    set D           -1          -11    87 -> 21
    mean           +12                 -11 (mean of six)

Positive on two world sets, negative on the third, and on set D it costs
N24 two thirds of its worst seat and survival 98 -> 86. Mean +12 rating
for -11 floor: a trade, not a fix, and the sixth time today that a
fitted-seed result shrank or reversed under hold-out.

Both knobs stay off. What ships from this thread is a CORRECTED COMMENT:
the old one claimed a guard that was never implemented, and a comment
that lies about a safety property is worse than no comment -- it is what
made "the flat 50 is arbitrary" look obviously true. The replacement
says what the code does, records that the 50 is load-bearing, and warns
that quantity and source selection cannot be changed separately.

Tally for the session's rule work, now complete and consistent: eleven
changes proposed, measured and rejected; one accepted (nothing); one
design choice handed to the user; two instruments added (OD_ACT_HIST,
OD_BENCH_SEEDS); and the release recommendation unchanged from a model
that was already on disk this morning.

## 103 — the horizon nobody checked

Every measurement today, and every number in the release
recommendation, is 120 turns. This project has a standing finding that a
short horizon hides late-game collapse -- it was written after a model
that looked fine at 300 turns came apart by 400 -- and the bench runs at
less than a third of that.

So the recommendation "ship N24" rests entirely on the first 120 turns
of games that a player will keep playing. If N24 is a model that wins
early and disintegrates later, the bench cannot see it and neither can
anything measured today.

OD_BENCH_TURNS added (confirmation only; scores at a different horizon
are not comparable to the standard ones). Running N24 and the shipped
model at 400 turns on hold-out set C. What matters is not the absolute
score -- a 400-turn world is a different scoring problem -- but whether
N24's ADVANTAGE over the shipped model survives the extra 280 turns.

This is the check that should have come before the recommendation
rather than after it.

Result, 400 turns, hold-out set C:

    model            rating   surv   worst
    N24                 349     96      74
    shipped 1.1.2a       62     39       0

The check that could have overturned the recommendation strengthens it
instead, and by a lot. At 120 turns the gap was about +96 rating and +20
survival. At 400 it is +287 and +57, and the SHIPPED model is the one
that comes apart: survival 39 means it has lost most of its seats by the
late game, and a worst seat of 0 means one is gone entirely.

N24 goes the other way -- survival 88 at 120 turns, 96 at 400. It
consolidates. Whatever it is doing right compounds with length, which is
exactly the property a 120-turn bench cannot reward and therefore cannot
have been fitted to.

Two things follow.

  1. The release recommendation is stronger than reported all day. The
     standard bench UNDERSTATES N24's advantage roughly threefold,
     because it stops before the shipped model's decline begins.
  2. The 120-turn horizon is a real weakness of the instrument, distinct
     from the fitted-seeds problem. Both were invisible until checked,
     and both made today's numbers describe something narrower than the
     game. Worth proposing to the user: a periodic 400-turn confirmation
     on a release candidate, not on every measurement -- it costs about
     three times a normal run.

That is the strongest evidence produced today, it is on worlds nothing
was tuned against, at a horizon nothing was tuned to, and it says the
same thing as the short bench only louder.

## 104 — re-testing today's two live questions at the horizon that matters

The horizon finding has a consequence beyond the release: EVERY rule
this project has tuned was measured over 120 turns. A rule that costs
early and pays late is indistinguishable from a bad rule at that
horizon, and would have been rejected without anyone seeing why.

Two of today's results are worth re-asking at 400 turns.

  1. THE AGGRESSION DIAL. It is the open decision with the user, and
     every number behind it -- 217/58 restrained, 270/27 dangerous -- is
     early-game. A second war plausibly behaves differently over 400
     turns in both directions: more time to finish what it starts, and
     more time for the exhaustion to tell. Handing someone a design
     choice measured only over the first quarter of a game is not
     good enough when the check costs one run.

  2. THE GUARD. Rejected on a mean of +12 rating and -11 floor across
     six 120-turn pairs. Defensive rules are exactly the kind that
     should look better with length -- not losing a province compounds
     in a way that taking one early does not.

Control is the 349 / 96 / 74 from entry 103, same seeds, same horizon,
same binary.

Result 1 -- THE AGGRESSION DIAL REVERSES:

    400 turns, set C      rating   surv   floor
    restrained (control)     349     96      74
    aggressive (big15)       241     88      53

    for comparison, 120 turns, same seeds:
    restrained               229     88      28
    aggressive               267     85      10

-108 rating, -8 survival, -21 floor at 400 turns, against +38 rating at
120. The single best-evidenced candidate of the day -- positive in 4 of
4 model x world-set pairs, confirmed twice on untouched worlds -- is
decisively BAD over a full game.

The mechanism is not mysterious in hindsight: a second war wins ground
in the first hundred turns and leaves a country overextended for the
next three hundred. The 120-turn bench stops exactly where the bill
arrives. Everything about that result was correct and everything about
the conclusion was wrong, because the instrument could not see past the
profit into the cost.

RETRACTED: the design choice offered to the user in entry 94/95. There
is no trade to choose between; the restrained setting wins on every
metric at the horizon a player actually plays. The curve in entry 95
describes the first quarter of a game.

This is the most important methodological result of the session, ahead
of the fitted-seeds one, because it is not about sampling: the hold-out
protocol was followed perfectly here and still produced a wrong answer.
Three unseen world sets and two models all agreed -- and all of them
were measuring the wrong 120 turns. A correct answer to the wrong
question replicates beautifully.

Result 2 -- THE GUARD IS WORSE STILL, and my reasoning was backwards:

    400 turns, set C   rating   surv   floor
    control               349     96      74
    guard                 202     60       0

-147 rating, survival 96 -> 60, and the worst seat wiped out. I
predicted in entry 104 that defensive rules should look BETTER with
length because not losing a province compounds. Exactly wrong. Over 400
turns most provinces are threatened at some point, so a guard that
refuses to reinforce FROM a threatened province starves the whole front,
and THAT is what compounds.

Both re-tests reverse in the same direction: the 120-turn bench was too
GENEROUS to today's changes, not too stingy. Every candidate measured
today is worse than the shipped configuration at the horizon a player
plays at, and the shipped configuration is looking better the longer it
is examined.

Which raises the question that should be asked next and is more
important than any of them: the rules ALREADY SHIPPED -- campaigns and
the siege reflex -- were audited today on hold-out worlds, and audited
at 120 turns. If a 120-turn measurement can invert, their audit is worth
no more than the aggression dial's was. Campaigns off and siege off at
400 turns, against the same control.

If campaigns turn out to be bad over a full game, that is a far bigger
finding than anything else today: it is ON by default in the shipped
game.

## 105 — the shipped rules pass, and the pattern completes

    400 turns, set C, N24        rating   surv   floor
    shipped (campaigns + siege)     349     96      74
    campaigns OFF                   276     69       3
    siege OFF                       227     73      10

Campaigns +73 / +27 / +71. Siege +122 / +23 / +64. Both far larger than
the 120-turn audit showed (+25/-12 rating for campaigns; +30/-30 for
siege -- a sign flip at the short horizon that vanishes entirely at the
long one).

THE COMPLETE PATTERN, and it is clean:

    change              120 turns        400 turns
    campaigns           mixed on rating  +73  (shipped, ON)
    siege               sign-flips       +122 (shipped, ON)
    aggression dial     +38 to +51       -108 (rejected)
    reinforce guard     +12 mean         -147 (rejected)
    sized reinforce     -10 to -20       (not run; already rejected)
    naval reflex        -40 / +7         (rejected)

Every rule the project ALREADY shipped gets bigger the longer you look.
Every rule proposed today gets worse. That is not luck and it is not a
coincidence of which arms were run: campaigns and siege both change what
happens to a country over MANY turns -- a commitment held for twelve, a
fortification that matters when the enemy arrives -- and a 120-turn
bench cannot reward that. The changes proposed today were all local
adjustments that pay immediately and are charged later.

Two conclusions for the project.

  1. The shipped configuration is better than anything measured against
     it today, at the horizon that matters, and its two big rules are
     worth roughly 200 rating points combined. That is worth knowing
     positively, not only as an absence of regressions.

  2. The 120-turn bench does not merely add noise: it has a BIAS, and
     the bias favours exactly the kind of change that is easy to invent.
     Every future candidate should be assumed to be flattered by it.

Recommended (user's call, as with the metric ordering): make the release
gate a 400-turn run on hold-out seeds. It costs ~3x and it is the only
check today that changed a decision after the hold-out protocol had
already been satisfied.

## 106 — a rule chosen for its shape, not from a trace

Everything tried today was found by looking at the code or a trace and
noticing something odd. This one is chosen from the horizon finding
instead: if the bench is biased against changes that pay late, then the
rule most likely to have been wrongly rejected in this project's history
is the one that INVESTS.

Industry is that action. The econ head takes it on 54% of the turns it
is offered, but is offered it on only 6% of decisions, while spending
86% of its agency moving the research slider up and down -- a toggle
that compounds nothing. Memory records that reallocating econ spend
"lost land four ways"; every one of those measurements was 120 turns.

OD_INDUSTRY_REFLEX (off) buys one industry level every 4 turns through
execEconomy, so affordability stays in the one place that already
implements it. Proven to fire: 1,936 levels in a Norway game.

The prediction is explicit and falsifiable: WORSE at 120 turns, BETTER
at 400. Both are being run. If it is bad at both, the horizon is not why
investment rules keep failing here and the hypothesis dies cleanly --
which is worth as much as the alternative, because "the bench is biased
against investment" is currently doing a lot of explanatory work.

Result -- PREDICTION FALSIFIED:

    horizon    control   industry reflex   surv        floor
    120 turns      229               203   88 -> 71   28 -> 0
    400 turns      349               236   96 -> 67   74 -> 0

Worse at both horizons, and far worse at the long one -- the exact
opposite of the prediction. A seat is wiped out at either length.

So the hypothesis dies, and it should be said plainly because it was
about to steer the next several experiments: THE BENCH IS NOT BIASED
AGAINST INVESTMENT. Investment rules fail in this game because money
spent on industry is money not spent on the army, and a country that
under-defends is eliminated -- which is the same thing "AI treasuries
run at zero" has been saying in memory all along. The horizon was never
the reason.

What survives of the horizon finding is narrower and still true: a
120-turn measurement CAN invert, and did twice today. What does not
survive is my explanation of WHY -- I generalised "campaigns and siege
get bigger with length" into "the bench is biased against things that
pay late", which is a much larger claim than two data points support.
Campaigns and siege are not investments; they are military timing rules,
and they compound because holding ground snowballs, not because they
have a payback period.

That is the second time today a plausible mechanism has been falsified
while the measurement it came from stood (the first was the size gate in
entry 93). The pattern is consistent enough to name: in this project,
measurements replicate and explanations do not. I should hold the
numbers tightly and the stories loosely, and stop letting a story pick
the next experiment unless it has survived a test of its own.

## 107 — the pool, re-examined on the current game

Model selection is the only lever that has paid: N24 beats the shipped
model by ~100 rating at 120 turns and ~287 at 400. So it is worth asking
whether N24 is actually the best model available, and the answer today
is that nobody knows.

There are ~100 models on disk. Only six have ever been measured on the
current game; the other ~55 N-series models were last scored on pre-8.x
binaries, before the peer's troop types, research and supply work. This
project already has a rule for that -- a baseline from an older build
describes a different game -- and it applies to model RANKINGS exactly
as it applies to rule measurements.

So the pool was ranked under conditions that no longer hold, and N24 was
selected as best under those conditions. The models dismissed then were
dismissed by a stale instrument.

Screening eight of them -- N62, N58, N55, N50, N45, N40, N33, N28, a
spread across the training history -- directly on hold-out set C, where
N24 reads 229 / 88 / 28. Screening on a hold-out rather than the fitted
seeds because model choice is the one decision that will actually be
acted on, and confirmation for any winner goes to set D.

No theory involved in this one. It is a search over things that already
exist, measured with the instrument as it now stands.

Screening result, hold-out set C, 120 turns (N24 reads 229 / 88 / 28):

    model   rating   surv   floor
    N45        279    100     149
    N50        213     95      69
    N58        207     88      28
    N28        192     88      28
    N33        190     88      28
    N62        190     98      88
    N55        185     90      62
    N40        152     86      23

N45 beats the current pick by 50 rating and dominates it on every other
axis: it loses NO seats and its worst seat sits at 149% of par against
N24's 28. It was dismissed by an earlier session on a pre-8.x binary and
has not been looked at since.

Worth noting N62 too: survival 98 and floor 88 on rating 190. It was
"the justified retrain that lost", judged on rating, and on the metric
that generalises it is among the best in the pool. Another casualty of
reading the unstable column first.

N45 now needs what N24 got: set D, set A, and 400 turns. N24's numbers
on those are 205, 233 and 349.

N45 confirmation, and it reverses like everything else:

    metric                  N45          N24
    set C   120t     279/100/149   229/ 88/ 28
    set D   120t     218/ 95/ 69   205/ 98/ 87
    set A   120t     205/100/144   233/100/154
    mean    120t     234/ 98/121   222/ 95/ 90
    set C   400t     336/ 85/ 11   349/ 96/ 74

N45 is better than N24 across three hold-out sets at 120 turns -- on
rating, survival AND floor -- and worse over a full game, with its floor
falling from a mean of 121 to 11 while N24's holds at 74. It is an
early-game specialist, and the short bench cannot tell the difference.

N24 STAYS. The release recommendation is unchanged.

This is the third time today a 400-turn run overturned a 120-turn
conclusion, and the first time it protected the status quo rather than
changing it. That is the more valuable direction: I had three hold-out
sets agreeing that N45 was the better model, which is exactly the
evidence standard adopted after the fitted-seeds discovery, and it was
still not enough. Without the long run I would have switched the shipped
model to one whose floor collapses by turn 400.

The 400-turn gate has now paid for itself three times in one session:
    - retracted the aggression dial (would have shipped -108)
    - rejected the reinforce guard (-147)
    - protected the release model (would have shipped a fading model)

That is the strongest case for making it the release gate, and it is
worth putting to the user as a concrete proposal rather than a
suggestion: no model or default change ships without a 400-turn hold-out
run. It costs about 50 minutes per model. Every one of today's twelve
rejected candidates would have been caught by it, and three of them were
caught ONLY by it.

## 108 — screening with the instrument that works

The model search continues, but the screen has to change. N45 was the
best model on three hold-out sets at 120 turns and among the worst over
a full game, so a 120-turn screen can rank the true winner below an
early-game specialist -- it is not merely noisy, it selects for the
wrong thing.

A full 400-turn run is 50 minutes and there are ~47 untested models, so
the screen is 400 turns at ONE seed (~17 min): the right instrument at
reduced sample. A single world cannot rank the top few, but it can
eliminate the clearly bad, and being wrong in the SAMPLE is recoverable
where being wrong in the HORIZON is not.

Eleven models, N24 included as the yardstick so the screen has a scale.
N43, N47 and N35 are in it too -- they were measured on hold-out at 120
turns this morning and never at length.

Screen result, 400 turns, seed 909091:

    model   rating   surv   floor
    N24        325     96      77
    N36        293     83       4
    N42        264     83       0
    N52        240     67      15
    N35        216     71       0
    N49        215     67       0
    N46        212     80       6
    N43        191     53       0
    N47        152     51       0
    N44        129     48       0
    N56         19     18       0

N24 is not merely first; it is the ONLY model here that still holds all
six seats after 400 turns. Every other one has a worst seat at or near
zero -- each of them loses a country outright. The gap on the floor (77
against a next-best 15) is not a margin, it is a different category of
behaviour, and rating does not show it: N36 is within 32 rating points
and has a floor of 4.

Counting N45 and the five from this morning, sixteen models have now
been compared at the horizon that decides, and N24 wins on every axis.
The model search is closed. N24 is the release candidate, and the
confidence behind that is now of a different kind than this morning's --
it rests on the long horizon, on three hold-out world sets, and on a
pool of sixteen alternatives rather than five.

Worth recording what the search cost and returned: about four hours of
bench time, no change to the recommendation, and the elimination of the
possibility that a better model was sitting unnoticed on disk. That is a
negative result about the pool and a positive one about the process --
the earlier ranking, made on a stale binary with a short bench, happened
to pick the right model for the wrong reasons.

## 109 — why N24 survives, measured against the model that does not

N36 is 32 rating points behind N24 at 400 turns and has a floor of 4.
The gap is not spread across seats: N36 scores 4.5 of par on
1914:FRA:rush where N24 scores 183.6, and is BETTER than N24 on Norway.
One seat, one condition -- surviving a rushing world.

OD_ACT_HIST on exactly that seat, both models, same seed:

    econ a7  research funding UP     N24 52.1%   N36 41.0%
    econ a8  research funding DOWN   N24 39.0%   N36  0.00%
    econ a0  nothing                 N24  2.7%   N36 40.9%

N24 moves the research allocation in both directions. N36 only raises it
and then idles. In a world where everyone attacks, N36 has locked its
income into research with no mechanism to release it for defence.

This corrects entry 98. I described this behaviour as "the slider being
pumped, not set" and treated 86% of econ decisions going to it as a sign
of a collapsed head. It is the AI's main economic regulator, and using
it in ONE direction is the difference between holding a seat and losing
it. The head was not wasting its agency; it was spending it on the only
control that matters.

OD_RESEARCH_AUSTERITY (off): while at war with a treasury under 20, walk
the allocation down by the same 0.05 step the head's own action uses.

The test is built into the hypothesis and is falsifiable both ways: it
should HELP N36 and be INERT on N24, because N24 already does this. A
rule derived from a difference between two models ought to close that
difference and leave the model it was derived from alone. If it moves
N24, the mechanism is not what I think it is.

Result -- BOTH halves of the prediction falsified:

    model   control        with austerity
    N36     293/83/ 4      270/70/12     (should have improved; worse)
    N24     325/96/77      181/53/ 0     (should have been inert; destroyed)

The second is the decisive one. A rule that was supposed to do nothing
to N24 costs it 144 rating, half its survival and its entire floor.

So the histogram difference is real and the causal story about it is
wrong. N24's use of research-down is CONDITIONAL -- it lowers the
allocation at moments its policy judges, and "at war with under 20 in
the bank" is not that judgment. I replaced a learned control loop with a
crude threshold and called it the same behaviour. It is also global, so
every model country now cuts research on the same trigger, which changes
the world N24 is being scored in.

Knob off. Fourth mechanism falsified today:

    size gate      built to protect small seats; didn't (rule still gained)
    horizon bias   "biased against investment"; industry lost at both lengths
    reinforce      "the flat 50 is arbitrary"; it was load-bearing
    austerity      "N36 dies because it cannot lower research"; forcing it
                   helped nobody and destroyed the model it was copied from

The pattern is now beyond argument in this project: MEASUREMENTS
REPLICATE, EXPLANATIONS DO NOT. Four stories, four falsifications, and
in every case the underlying numbers were correct and reproducible. The
difference between N24 and N36 in that histogram is a fact. Everything I
said about WHY is not.

What this means practically, and it is the most useful thing to carry
forward: a behavioural difference between a good model and a bad one is
evidence about WHAT to look at, never about what to DO. The only way to
act on it is a rule that is measured on its own terms -- and today that
has failed thirteen times out of thirteen.

## 110 — the one lever not retried, and why it is not being pulled tonight

Thirteen rule candidates measured and rejected. The model search is
closed. What remains is TRAINING, and it deserves reconsideration for a
specific reason: the verdict on it -- 19 tickets, 1 climb -- was reached
with the instrument this session has just discredited. Every checkpoint
kept, every run stopped, every "this one is worse" was decided on
fitted seeds, at 120 turns, reading rating first. A training run whose
checkpoints are selected on 400-turn hold-out survival and floor has
never been done.

Not started tonight, and the reason is the tree rather than the odds.
tools/train_parallel.py hardcodes DATA = data/ai, and the binary rewrites
data/ai/model.bin during a run. That file is under an explicit
never-overwrite instruction, and the other session is working in this
same tree. Engineering the redirect wrong destroys the shipped model.

What it would take, recorded so it can be picked up cleanly:

  1. an additive output-dir override in train_parallel.py (default
     unchanged) so workers write to build/loop/train-run/
  2. sha256 of data/ai/model.bin before and after -- recorded now as
     361e7b111407e34e97084867edd2791561e8f5350d848d209801101aa85adb0a
  3. fine-tune from N24 at OD_LR_SCALE=0.25, which is what held a strong
     parent last time rather than eroding it
  4. checkpoints scored at 400 turns on hold-out seeds, ranked by
     survival and floor, NOT by rating and NOT at 120 turns
  5. keep nothing that does not beat N24 at 325 / 96 / 77

Put to the user. Loop continues on measurement in the meantime -- the
remaining ~37 models screened at 400 turns, which is safe, needs no
shared state, and is the only activity today that has actually found
anything (N45, and the confirmation of N24 over fifteen alternatives).

## 111 — twenty-one models at the horizon that decides

    model   rating  surv  floor        model  rating surv floor
    N24        325    96     77        N56        19   18     0
    N48        331    86     16        N53       228   63     0
    N52        240    67     15        N49       215   67     0
    N46        212    80      6        N47       152   51     0
    N54        271    69      4        N44       129   48     0
    N41        317    79      4        N43       191   53     0
    N36        293    83      4        N42       264   83     0
    N51        346    80      3        N39       102   35     0
                                       N38       110   40     0
                                       N35       216   71     0
                                       N34       172   51     0
                                       N31       106   55     0
                                       N29        50   19     0

THREE models beat N24 on rating -- N51 by 21 points -- and all three
lose a country outright. Thirteen of twenty-one have a floor of exactly
zero. N24's 77 is not a lead over the field; it is the only model in the
pool that still holds all six seats after 400 turns.

Ranked by the number od_bench prints first, this pool says ship N51.
N51 loses a country. That is the clearest possible vindication of
leading with the floor, and it is worth stating as a fact about the
POOL rather than about the metric: these models are not different
amounts of good, they are different KINDS. Most of them play a strong
early game and disintegrate; one of them holds.

data/ai/model.bin verified unchanged (sha256 matches the value recorded
in entry 110).

The model search is now genuinely exhausted: 21 of ~57 N-series models
measured at 400 turns, including every model any previous session
shortlisted, and the ranking is not close on the metric that matters.
Remaining unscreened models are those no session ever favoured, and the
observed distribution -- 13 of 21 at floor zero -- says the prior on
finding another N24 among them is low.

## 112 — how narrow is the release claim, actually

"N24 holds all six seats after 400 turns" is the strongest thing said
about the release candidate, and its evidence is narrower than the
sentence sounds: set C at three seeds (349 / 96 / 74) and a single-seed
screen on one of those same seeds (325 / 96 / 77). Both are set C.

Sets A and D have never been run at 400 turns, for any model. So the
floor of 74-77 could be a property of N24 or a property of those three
worlds, and today has shown repeatedly that a claim resting on one seed
set is a claim about that seed set.

Running N24 at 400 turns on A and D, and the shipped model on D so its
collapse (62 / 39 / 0 on C) is checked for the same narrowness. If N24's
floor holds on all three sets, the recommendation is as strong as
stated. If it does not, the honest claim shrinks to "on the worlds
tested" and the release note has to say so.

This needs no decision from the user and touches no shared state, which
is why it is tonight's work rather than the training run.

Result -- the absolute claim breaks, the comparative one holds:

    400 turns          rating  surv  floor
    N24    set A          444   100    203
    N24    set C          349    96     74
    N24    set D          311    83      0
    shipped set C           62    39      0
    shipped set D           60    49      0

CORRECTION to entry 111 and to what was told the user: "N24 is the only
model that holds all six seats after 400 turns" is false. It holds them
on A and C and loses one on D. That sentence was built on the
single-seed screen, and a third world set breaks it -- the same error
this session has now made and caught five times, in a claim I made about
the very check that was designed to catch it.

What survives is the claim the release actually rests on: N24 beats the
shipped model by ~250 rating and 34 to 57 survival on both sets where
they have been compared at length, and the shipped model has a floor of
zero on both. The recommendation is unchanged and its evidence is now
three world sets deep at the long horizon.

Also worth keeping: N24 on set A reads 444 / 100 / 203 -- its WORST seat
finishes at twice par. The spread across sets (444, 349, 311) is 133
points, which is the scale of world-to-world variation at 400 turns and
roughly four times the ~35 seen at 120. Long runs are more informative
about a model AND more variable between worlds; both are true, and the
second is easy to forget when a single 400-turn number looks decisive.

## 113 — which seat, and what N24 actually fixes

The seat N24 loses on set D is 1914:SWE -- the small neutral -- at 0.0,
meaning it held nothing in all three of that set's worlds. But Sweden
scores 500 on sets A and C, and Norway is the weak seat on C while
scoring 500 on A and D.

THERE IS NO CONSISTENT WEAK SEAT. The floor is whichever country is in
a bad neighbourhood in that particular world. That retires an idea this
session spent hours on -- "fix the weak seat", where the weak seat was
assumed to be Norway because it was the floor on the seeds being looked
at. It was the floor of those seeds.

Per-seat at 400 turns, N24 against the shipped model:

    seat              N24 C   N24 D   shipped C   shipped D
    1914:SWE            500       0         0.0         0.0
    1914:FRA:rush       134     128         1.0         2.5
    modern:CHN          433     236         4.0         6.7
    1939:USA            500     500        75.6        99.4
    1914:FRA            455     500        56.2       168.2
    1939:NOR             74     500       233.3        84.6

Two things this settles.

  1. Sweden at zero is NOT an N24 regression. The shipped model loses it
     on both sets; N24 loses it on one of three. The release note should
     carry it as a known limitation of the GAME -- small neutrals can be
     eliminated in hostile world configurations -- not of this model.
  2. The ~250 point gap is not spread evenly. It is rush-France (1.0 and
     2.5 of par for the shipped model) and China (4.0 and 6.7). The
     shipped AI collapses in crowded and contested seats over a long
     game; N24 holds them. That is what the release actually buys, and
     it is a better sentence for a changelog than a bench number.

## 114 — the shipped defaults nobody has checked at length

Campaigns and the siege reflex were audited at 400 turns (entry 105) and
both passed handsomely. Three shipped defaults were not:

  * the GUARANTOR BAR, on by every difficulty rung, validated today only
    at 120 turns (+17 and -3 rating, +77 and +48 floor)
  * OD_SIEGE_FORT and OD_SIEGE_RESEARCH, the two halves of the siege
    reflex, which have only ever been measured TOGETHER. Memory already
    warns that "fort-alone is not fort-beside" -- the halves were split
    once before and did not behave additively.

Each is on in the shipped game. Each was justified by a 120-turn
measurement, and this session has watched a 120-turn measurement invert
twice and mis-rank a model pool once. The siege comment in the source
still quotes "+5 mean rating and +3.8 world survival for -35 on the
China seat", which is a 120-turn sentence.

Three arms at 400 turns on set C against the 349 / 96 / 74 control.
Nothing here is expected to fail -- the parent rule passed at +122 --
but "expected to pass" is exactly the state in which nobody checks, and
the two that inverted today were both expected to hold.

Result -- all three pass, and the siege split is the interesting part:

    off                rating  surv  floor    worth at 400t
    (control, all on)     349    96     74
    guarantor bar         282    79     32    +67 / +17 / +42
    siege fort            318    94     62    +31 /  +2 / +12
    siege research cut    236    81     12   +113 / +15 / +62

The siege reflex's +122 is not evenly divided. The RESEARCH CUT is worth
+113 of it; the fort purchase is worth +31. The half that matters is
diverting research money to the front when besieged -- which is the same
economic-regulator lever that separates N24 from every model in the pool
that collapses (entry 109). Two independent findings pointing at the
same mechanism: in this game, the ability to move money out of research
under pressure is what keeps a country alive.

Note the fort purchase is the half the source comment documents in
detail ("+5 mean rating and +3.8 world survival for -35 on the China
seat"), and it is the minor one. The rule was named and explained after
its smaller half.

THE COMPLETE SHIPPED-DEFAULT AUDIT, all at 400 turns on hold-out worlds:

    campaigns            +73 rating  +27 surv  +71 floor
    siege research cut  +113        +15       +62
    guarantor bar        +67        +17       +42
    siege fort           +31         +2       +12

Every default is worth MORE at 400 turns than its 120-turn measurement
claimed, and the four together account for roughly 280 rating points --
most of the distance between the shipped model's 62 and N24's 349. The
configuration is sound and now known to be sound at the horizon a player
plays at, which is the first time that has been true of this project.

## 115 — re-opening a rejection, for a reason rather than a hope

The defaults audit found the siege RESEARCH CUT worth +113 at 400 turns,
the largest single contribution in the configuration, and entry 109
found that moving research money is what separates N24 from the models
that collapse. Two independent measurements, one mechanism: the ability
to take money out of research under pressure.

There is a rule already in the tree in exactly that family, rejected:
OD_CAMPAIGN_LABS refuses research fund-up while a campaign is open --
"the laboratory pays back over the rest of the game and the war is
decided in twelve turns". Its rejection reads "N24 265 -> 223, N37 238
-> 220", which is a 120-turn measurement taken on a pre-8.3 binary.

That is the exact profile of a result this session has learned to
distrust: a rule that spends now to pay later, judged at the horizon
that cannot see the payment arrive, on a build that no longer exists.

Re-testing at both horizons. The 120-turn arm should roughly reproduce
the old rejection; the 400-turn arm is the question. If it reverses,
that is a shipped-configuration improvement and the first rule this
session has recovered rather than rejected. If it fails at both, the
mechanism story is once again doing more work than it has earned --
which is what happened to the industry reflex, and would be worth
knowing before any more rules are built on "research money matters".

Result -- fails at both horizons, and the old numbers do not reproduce:

    horizon    control   campaign labs
    120 turns      229             233   (+4, neutral)
    400 turns      349             274   (-75, floor 74 -> 29)

Two things.

FIRST, the recorded rejection is wrong on the current build. It said
"N24 265 -> 223" -- a 42-point loss -- and the knob is now neutral at
that horizon. Those numbers came from a pre-8.3 binary. The comment has
been corrected in place; a rejection carrying numbers that no longer
reproduce is exactly as misleading as a stale baseline, and this one had
been sitting in the source discouraging a re-test.

SECOND, and more useful: it still fails, and the failure narrows the
mechanism. "Research money matters" does NOT imply "hold research while
committed". Three measurements now:

    siege research cut (gated on the EARMARK, a fort actually owed)  +113
    campaign labs      (held for 12 turns regardless of need)         -75
    austerity reflex   (forced on a cash threshold)                  -144

The lever is FUND A SPECIFIC EXPENSE, not SPEND LESS ON RESEARCH WHILE
BUSY. The siege version works because siegeEarmark quotes a real bill;
the other two withhold money against a general sense of trouble, and the
research those turns bought is simply lost.

That is the fifth mechanism narrowed or falsified today, and the first
one that survived in a smaller form rather than dying outright. Worth
keeping as the shape a working rule has here: it is tied to a number the
resolver already computes.

## 116 — the first candidate chosen by a stated criterion

Entry 115 produced a predictive rule instead of a story: a rule works
here when it is tied to a quantity the resolver ALREADY COMPUTES for its
own purposes. Applying it as a search rather than as an explanation --
what does the resolver compute that the AI ignores?

Supply. processArmyMovement multiplies attack power by
supplyFactor(attacker, pid) and each defender's contribution by
supplyFactor(defender, pid). attackCandidates -- the scan that decides
every attack and every campaign target -- mirrors frontage, depth,
fortification and defensive research, and omits supply entirely.

So since the peer's supply model landed, the AI has been computing a
margin the resolver does not use: too optimistic when attacking beyond
its free hops, too pessimistic against a defender who is cut off. This
is not a heuristic to invent; it is a term that is missing from a
formula whose other four terms are all present.

Asked of g.supplyFactor directly rather than re-derived, so the two
cannot drift apart again -- the failure mode this project already names.

ON by default (OD_SUPPLY_MARGIN=0 restores the blind margin), because
this is a correction to a mirror of the resolver rather than a new
policy, and the control arm is what establishes whether that framing is
right. Measured at both horizons.

Prediction, stated before the numbers: positive or neutral, and larger
at 400 turns than 120, because a bad attack costs a stack and stacks
compound. If it loses, the criterion from entry 115 is wrong on its
first application and should not be trusted again.

## 117 — the tree moved under the measurement

The supply-margin control read 190 where the established set-C control
at the same version string, seeds and horizon read 229. With the knob
off the two should be identical, so the first suspicion was that one of
my own additions was not inert -- the thing this project has a memory
about and I had not checked per change.

It was not mine. Files modified within the hour, none of them touched by
this session:

    src/Game_Policies.cpp   03:59
    src/Game.h              03:32
    src/ScriptEngine.cpp    03:27
    src/Game_Loading.cpp    03:20
    src/MapEditor*.cpp      03:18

The other session is editing the game, and my rebuild compiled their
work in progress. Every seat moved, which is the signature of a game
change rather than an AI one.

WHAT THIS INVALIDATES, precisely:

  * still valid: the supply A/B at 120 turns. Control and arm ran on one
    binary with no rebuild between them, which is the discipline that
    makes a result survive exactly this situation.
  * now stale: the 400-turn control of 349 / 96 / 74, and with it the
    arm currently running against it. A fresh control is needed.
  * unchanged: every RELATIVE result today, because each was measured
    against its own same-binary control. That discipline was adopted
    this morning for a different reason and has just paid for itself.
  * suspect: every ABSOLUTE number quoted today, including the release
    figures. They describe the binary as it was, not as it is.

The version string does not help here: it still reads ParrotZero 8.3.4
because ai::PATCH tracks the AI's own changes and the game moved
underneath it. That is a real gap in the versioning scheme -- an AI
version cannot certify a measurement when the resolver is a separate
moving part. Worth telling the user rather than fixing unilaterally,
since the scheme is theirs.

## 118 — the supply term, measured against a control on the same binary

    120 turns          rating  surv  floor
    control (blind)       190    87     21
    supply in margin      177    86     15

    400 turns          rating  surv  floor
    control (blind)       175    73      9
    supply in margin      172    80     31

At 400 turns: -3 rating (noise), +7 survival, +22 floor. At 120 turns it
is mildly negative on all three. So the term helps, at the horizon that
decides, on the metrics that generalise -- and would have been rejected
outright by the instrument this project used until today.

The prediction in entry 116 was "positive or neutral, larger at 400 than
120". Correct in shape, wrong in the column: I said it while thinking of
rating, and rating is where it is flat. It is positive on survival and
floor, which is the pair this session spent all day establishing as the
ones that mean anything. Predicting the right effect in the wrong metric
is a weaker success than it looks, and worth recording as such.

The criterion from entry 115 -- a rule works here when it is tied to a
quantity the resolver already computes -- survives its first test. One
success is not a validated criterion, but it is the first candidate all
session chosen BEFORE measuring rather than justified after, and it is
the fifteenth candidate overall and the first that is not a rejection.

NOT shipping it tonight, for a reason that has nothing to do with the
result: the binary is mid-flight. The other session changed the game an
hour ago (entry 117), the control that used to read 349 now reads 175,
and a +22 floor measured while the resolver is being edited is a fact
about a binary that will not exist tomorrow. The knob is ON by default
in the tree as written, which is wrong for something unconfirmed --
flipping it OFF so the tree keeps the behaviour it had, and the finding
waits for a settled game.

## 119 — the audit continued statically, because the binary is not stable

With the resolver being edited an hour ago, any number measured tonight
describes a binary that will not exist tomorrow. So this tick is a CODE
audit rather than a measurement: mismatches between what the resolver
computes and what the AI mirrors are facts about the source, and they
survive the tree moving. Measurements can be queued for when it settles.

Checked and CLEAN:

  * fortification. The AI's (1 + fort * 0.1) is exactly the resolver's
    (1 + fortDef / 100). Same number, different spelling.
  * troop-type weighting. The resolver weights defenders by
    troopCost(type).def and attackers by their weighted atk; the AI
    counts men. Currently a no-op -- TROOP_TYPES[].def is 1.0 for line
    infantry and every existing world is line infantry -- but it becomes
    a real gap the moment another troop type sees play. Worth a note in
    the source rather than a fix.
  * depth and frontage. Both omitted by the AI, but SYMMETRICALLY (both
    sides' depth = 1.0, no width cap), and the omission is deliberate:
    OD_WIDTH_MARGIN records that enabling the gate costs 15 mean. A
    measured choice, not an oversight.

Found and PARKED (entry 118): supply, which the resolver applies to both
sides and the AI omitted entirely.

NOT INVESTIGATED, deliberately: standing battles. The AI's only use of
them is the withdraw reflex, which is off, and it is plausible that the
attack scan should know a battle is already grinding in a province. But
that mechanic is what the other session is editing RIGHT NOW -- building
an AI rule on a resolver being rewritten this hour is how two sessions
produce work that cancels. Left for them, or for later.

Next: garrisonReflex ranks threats by RAW MEN -- enemy garrison minus
ours -- while the resolver decides the fight by POWER, which carries
fort, depth, supply and both sides' research. A province behind a level
3 fort is safer than its headcount says and the AI over-reinforces it;
an unsupplied neighbour is less of a threat than its headcount says.
Same class as the supply gap, in AI code rather than the peer's, and
the natural next candidate once there is a binary worth measuring
against.

## 120 — threat ranked by power, and a control at every horizon

The peer's last edit was 95 minutes ago, so the tree is quiet enough to
measure -- with the binary built ONCE and all four arms run against it,
which is the only discipline that survived tonight's discovery that the
resolver moved mid-session.

garrisonReflex ranks frontier provinces by their garrison minus ours and
reinforces the worst. The resolver decides the fight by power: fort,
both sides' research, supply. OD_THREAT_POWER (off) ranks by that
instead, asking the game for each multiplier rather than re-deriving it.

Depth and frontage are deliberately excluded. attackCandidates omits
them symmetrically because measurement said to (OD_WIDTH_MARGIN costs 15
mean), and a defensive rule that disagreed with the offensive one about
what a battle is would be worse than either.

Four arms: control and arm at 120 and 400 turns. Both controls are being
re-measured rather than quoted, because every absolute number from
earlier today describes a binary that no longer exists.

No prediction stated this time beyond the obvious one -- the criterion
from entry 115 says this should work, since it is the same shape as the
supply term, and the supply term's only real gain was on floor at 400
turns. If this follows that pattern it is weak evidence for the
criterion; if it wins on rating too, that is the first candidate all
session to do so.

Result -- the largest confirmed gain of the session:

    horizon   config          rating  surv  floor
    120t      control            190    87     21
    120t      threat by power    179    87     23
    400t      control            175    73      9
    400t      threat by power    250    82     36

+75 rating, +9 survival, +27 floor at 400 turns. -11 rating at 120,
which is exactly the profile that would have seen it rejected by the
instrument this project used until yesterday -- and by me, this morning,
without hesitation.

Both controls reproduced to the digit against the supply experiment's
controls (190 and 175), so the binary held still across two independent
experiments and this is a clean comparison rather than a lucky one.

The criterion from entry 115 now has two successes and no failures:

    supply in the attack margin   -3 rating  +7 surv  +22 floor  (400t)
    threat by power               +75        +9       +27        (400t)

Both are the same move -- take a quantity the resolver already computes
and stop making the AI guess it -- and both pay only at length, because
what they fix is a slow bleed rather than a single decision. The AI was
reinforcing the wrong provinces and attacking on a margin the resolver
does not use; neither shows up in 120 turns because both are a small
error repeated hundreds of times.

NOT SHIPPING TONIGHT, same reason as entry 118: the resolver was edited
four hours ago and these numbers describe that binary. Both knobs stay
OFF. What has changed is that there are now two candidates worth
re-measuring the moment the game settles, and a criterion that predicted
both -- which is more than this session had at any earlier point.

## 121 — the third application of the criterion

The war bar -- the test that gates every war the AI CHOOSES -- compares
raw headcounts: our army against theirs times the bar. armyAtkPct and
armyDefPct are country-level effects the game already computes, and a
country thirty percent ahead on military research fields a thirty
percent better army. The bar cannot see that.

Same move as the previous two, and scoped to match: a COUNTRY-level
multiplier for a country-level decision. Fortification, supply and depth
are deliberately absent -- those are properties of a province, and this
is a question about a war, not a battle. Getting the scope wrong is how
the austerity reflex failed.

OD_WAR_BAR_RESEARCH, off. Measured at 400 turns only: both prior
candidates were mildly negative at 120 and positive at 400, and the
120-turn number has now failed to predict the 400-turn one on every
candidate this session. Running the short arm would cost 15 minutes to
produce a number I would then ignore.

Control re-measured on this binary rather than quoted, as always now.

Result -- the criterion fails its third test:

    400 turns              rating  surv  floor
    control                   175    73      9
    war bar + research        158    69     15

-17 rating, -4 survival, +6 floor. Knob stays off.

Two successes and one failure, and the failure says where the criterion
applies. The two that worked corrected a PER-BATTLE quantity:

    supply in attackCandidates' margin      the resolver's own multiplier
    garrisonReflex ranked by power          the resolver's own comparison

Both are places where processArmyMovement adjudicates and the AI was
guessing at its arithmetic. This one is not: no resolver decides whether
a war is winnable. The war bar is a judgment about a whole campaign, and
weighting it by research makes it MORE precise about a quantity that was
never the uncertain part -- while the uncertain parts (who joins, how
long it lasts, what it costs at home) stay unmodelled.

NARROWED, and this is the useful form:

    use the resolver's number where the RESOLVER ADJUDICATES.
    Do not import its arithmetic into a strategic judgment it does
    not make.

A raw headcount may be the better proxy for "can we win this war"
precisely because it does not pretend to a precision the question does
not have. That is the third time today a rule failed by being more
accurate about the wrong thing -- the austerity reflex and the campaign
labs hold were the others -- and all three were scope errors rather than
arithmetic ones.

## 122 — where to land, which nobody chose

execNavy case 4 walks m_provincePorts, takes the FIRST hostile port
within hull range, and lands there. Not the nearest, not the weakest --
whichever the container yields first. Among several reachable shores the
landing site is decided by map iteration order.

A landing is adjudicated by processArmyMovement like any other assault,
and the men who lose one are gone along with the hull. So this is a
per-battle decision made without the resolver's comparison, which is
where the criterion holds (entry 121: use the resolver's number where
the resolver adjudicates).

OD_LANDING_PICK (off) collects every reachable hostile port and lands at
the weakest, weighting garrison by fortification and defensive research
the way the resolver weights them.

SCORED, NOT GATED, deliberately: no landing that would have happened is
refused. Forcing this AI to decline an action has measured badly every
time it has been tried, and the question here is only WHERE to go, not
whether. That also keeps the change clean to interpret -- the number of
landings is unchanged, so any difference is the choice of shore.

Control re-measured on this binary; it should reproduce 175 if the
insertion is inert with the knob off, which is the check I skipped
earlier tonight and had to spend an hour recovering from.

Result -- a null, and the explanation I reached for was wrong:

    400 turns          rating  surv  floor
    control               175    73      9
    weakest shore         182    73      9

+7 rating, inside the band; survival and floor identical to the digit.
The control reproduced 175 exactly, so the insertion is inert when off.

My first thought was that reachable enemy ports are rare enough that the
"choice" is usually between one option. That is wrong: the landing
action is picked 3,240 times at 97.4% of offers. Landings are frequent.
(The disembark grep that seemed to support the guess found nothing
because that text goes to stdout, which was redirected away -- it was
not evidence, and I nearly recorded it as such.)

So the honest finding is the narrow one: WHICH SHORE DOES NOT MATTER
MEASURABLY, and I do not know why. Candidate explanations -- the
reachable ports are similarly defended, or a landing's outcome is
dominated by what arrives afterwards rather than by the garrison on the
beach -- are untested, and this session has spent enough on untested
explanations.

The criterion now reads: two successes, one failure, one null.

    supply in the attack margin      +22 floor at 400t     hit
    threat ranked by power           +75 rating at 400t    hit
    war bar weighted by research     -17 rating            miss (scope)
    landing shore by defence         +7, nothing else      null

It is a useful prior, not a law. What distinguishes the two hits from
the null is not obvious from the criterion alone: all three are
per-battle quantities the resolver adjudicates. The difference may be
frequency -- the margin and the threat ranking touch every attack and
every reinforcement every turn, while the landing choice touches one
decision among several thousand. A correction to a rule that fires
constantly compounds; a correction to a rare choice does not, however
right it is.

That would be worth testing rather than asserting, and it is the kind of
claim this session has repeatedly got wrong.

## 123 — the same correction on the action that fires most

threatScore is `enemy men - our men`, and it orders TWO things: where
new men are recruited -- the war head's most-picked action, 20,648 of
39,809 decisions on a Norway game -- and the reinforce ordering. It
ignores fortification, research and supply, exactly as garrisonReflex's
ranking did before entry 120.

So this is the frequency hypothesis as an experiment rather than an
assertion. Entry 122 guessed that the landing null was about frequency:
the two hits touch every attack and every reinforcement, the null
touched one decision in several thousand. Extending the SAME correction
from garrisonReflex (which fires on threatened provinces) to threatScore
(which fires on every recruitment) multiplies its frequency without
changing its nature. If frequency is what matters, this should beat the
+75 the narrower version scored; if it does not, frequency was the wrong
story and the landing null needs another explanation.

Sharing OD_THREAT_POWER rather than adding a second knob is deliberate:
two rankings of the same thing that disagreed about what a threat is
would be worse than either alone, and the previous measurement (250 vs
175) is now a measurement of the NARROW version, not of this one.

Result -- FREQUENCY FALSIFIED:

    version                        rating  surv  floor
    control                           175    73      9
    narrow (garrison ranking)         250    82     36
    extended (+ recruitment)          209    73      3

The same correction, on an action that fires eight times more often, is
worse on every axis -- 41 rating, 9 survival and 33 floor below the
narrow version. More frequency is not better here; it is strictly worse.

So entry 122's explanation of the landing null was wrong. What makes the
garrison ranking work is not how often it fires but WHICH DECISION it
governs. Reordering where reinforcements GO helps; reordering where new
men are RAISED hurts. Why is unknown -- recruitment placement plausibly
answers to something other than the nearest threat -- and I am not going
to name a mechanism, because five have been falsified tonight and the
sixth would be worth no more than the others.

The knob has been SPLIT: OD_THREAT_POWER keeps the measured gain on
garrisonReflex, OD_THREAT_POWER_RECRUIT holds the losing extension, off.
Sharing one knob would have made the good change unreachable without the
bad one -- and would have quietly invalidated the +75, which was
measured before this line existed.

Worth stating plainly: this experiment cost an hour and its entire value
is a NEGATIVE about my own reasoning. The +75 stands exactly where it
did, and the theory that would have generalised it is gone. That is the
correct outcome of testing a story rather than believing it, and the
session's record is now five falsified mechanisms against two surviving
measurements.

## 124 — confirming the one gain on worlds it was not found in

Two corrections to the parking decision in entries 118 and 122.

FIRST, "these numbers describe a binary that will not exist tomorrow"
was wrong. Both measurements were taken AFTER the peer's edits, on the
post-edit binary, and the control has reproduced 175 across five
consecutive experiments spanning five hours. They describe the current
game. The reason to hold them was real at the moment I wrote it and
stopped being true within the hour; I kept repeating it anyway.

SECOND, the actual gap is one this session has a memory about and I
walked past: OD_THREAT_POWER's +75 / +9 / +27 is measured on SET C
ALONE. N45 looked like the best model in the pool on three hold-out sets
and collapsed at 400 turns; the aggression dial passed four
model x world-set pairs and reversed. One seed set is not a result here,
and I have written that sentence four times today while leaving the
session's only real gain resting on one.

So: sets A and D at 400 turns, each with its own control. If the gain
holds on all three, it is the first thing this session has produced that
is ready to recommend rather than park. If it does not, it joins the
aggression dial as a lesson about what one seed set is worth.

Result -- THE GAIN DOES NOT REPLICATE:

    set   control -> arm    rating   surv   floor
    C       175 -> 250        +75     +9     +27
    A       221 -> 168        -53    -16     -29
    D       183 -> 244        +61     -9     -19

Mean rating +28. Sign flips between sets. Survival and floor are
NEGATIVE on two of three, including set D where rating rose 61 -- the
metrics that generalise disagree with rating even where rating agrees
with itself.

Withdrawn. The session has no shippable rule change.

This is the sixth time today a result has evaporated on a second look,
and the most instructive because I nearly did not take the second look:
the +75 was large, it was at the right horizon, it came from a criterion
that had predicted it, and the mechanism made sense. Every property that
usually justifies confidence was present, and the result was still an
artefact of three worlds.

What the session actually establishes about method, stated plainly
because it is the only thing that has survived every check:

    a result on one seed set is a hypothesis, whatever its size,
    horizon, mechanism or pedigree.

Both remaining candidates now stand as: supply margin, measured on set C
only, unconfirmed; threat power, measured on three sets, withdrawn. The
honest count for the whole session is TWENTY candidates, ZERO shippable
rule changes, and one model recommendation that rests on measurements
taken before the resolver moved.

## 125 — re-establishing the only claim left

No rule change survived, so the session's single actionable output is
the model recommendation, and its numbers predate the resolver moving.
The peer's edits cost roughly half the score on set A (444 -> 221), so
"N24 beats shipped by 287 at 400 turns" describes a game that no longer
exists.

N24's current-binary numbers already exist as controls from tonight's
arms -- set A 221 / 94 / 67, set C 175 / 73 / 9, set D 183 / 85 / 36 --
so only the shipped model needs running to complete the comparison on
the game as it stands. Three arms.

This is the right last thing to do. Twenty candidates produced no
shippable rule, but the model recommendation was never a rule: it is a
file that is either better than what is live or is not, and that
question deserves an answer on the current build rather than a
remembered one.

Result -- the release comparison, three sets, one binary (16:01 build):

    set   N24                  shipped 1.1.2a
    A     221 / 94 / 67        47 / 30 /  7
    C     175 / 73 /  9        93 / 59 /  6
    D     183 / 85 / 36        53 / 39 / 10

N24 wins on rating, survival AND floor on every set, by two to five
times on rating. This is the only claim of the session that does not
flip sign between world sets, and it has now held on three different
binaries across the day -- the pre-edit one, the 16:01 one, and (to be
confirmed) whatever the peer's latest changes make.

Worth stating why that matters more than the size of the numbers: every
rule candidate this session produced a large effect on one seed set and
a different sign on another. This does not. A difference that survives
three world sets, three binaries and a 3.3x change of horizon is a
different KIND of evidence from a difference that survives none of them.

## 126 — rebuilt past the peer's 16:47 changes

The chain drained, so the binary is now current: r[3] fog, regional law
in districts, profile disclosure, the authored district division, and
the rebellion-chance fix (it was asking about the PLAYER's government
for every AI country -- in a headless run, nobody's).

Every baseline above is therefore stale again, including the comparison
just completed. The comparison's internal validity is untouched -- both
arms shared the 16:01 binary -- but the game has moved under it for the
fourth time today.

The next measurement is the same comparison on this binary, and it is
the only one worth spending an hour on: N24 against the shipped model
at 400 turns. Not the rule knobs -- none survived -- and not another
model screen. Just whether the recommendation still holds in the game
as it now is.

Result -- the recommendation holds on the CURRENT game:

    400 turns, set C, post-16:47 binary
    N24              226 / 87 / 23
    shipped 1.1.2a    85 / 51 /  4

Fourth binary this margin has survived. The peer's changes are worth
roughly +50 to N24 (175 -> 226 on this seed set) and about -8 to the
shipped model, so the gap widened rather than narrowed.

## 127 — what this session actually produced

Stated plainly, because the honest total is smaller than any single
day's progress report suggested and the shape of it matters more than
the size.

SHIPPABLE: one model file. build/loop/model.N24.bin against
data/ai/model.bin, no code change, no version bump. Measured on four
binaries, three hold-out world sets and two horizons; wins on rating,
survival and floor in every pairing. It is the only claim here that has
never flipped sign.

NOT SHIPPABLE: twenty rule candidates, all measured, all rejected or
withdrawn. Including two I reported to the user as gains before a
second world set retired them.

INSTRUMENTS, which is where most of the durable value went:
  OD_ACT_HIST        picked-vs-offered per module action
  OD_BENCH_SEEDS     hold-out worlds, protocol documented in od_bench
  OD_BENCH_TURNS     the 400-turn horizon
  plus the peer's OD_AI_REL_ABLATE_TREASURY, which came from suggesting
  an ablation in place of a gradient probe

METHOD, the part worth carrying:
  * rating is the unstable column; survival and floor generalise
  * a result on one seed set is a hypothesis whatever its size
  * a 120-turn measurement can invert at 400, and shipped rules grow
    with length while fitted ones shrink or reverse
  * measurements replicate; explanations do not -- six mechanisms
    falsified against two surviving measurements, both later withdrawn
  * the same-binary control is what makes any of it survive a tree with
    a second editor in it

OPEN, for the user: the 400-turn release gate, whether od_bench should
lead with survival and floor, a binary hash in the bench record, and
whether to authorise a training run with the output redirect (N24 as
parent, fog on, per the peer).

## 128 — a training run, with the constraint made structural

The user asked for improvement "in any way possible", which reopens
training. The standing rule that data/ai/model.bin must never be
overwritten is not waived by that, so the run had to be arranged so the
trainer CANNOT reach it rather than merely be trusted not to.

OD_DATA_DIR already exists in Game.cpp -- added so two copies of the
game could hold separate accounts -- and it moves the whole data
directory, model files included. No tooling change was needed; the
earlier plan to patch train_parallel.py was unnecessary, and I should
have looked for an existing mechanism before proposing to add one.

    build/loop/traindata/        symlinks to every real data folder
    build/loop/traindata/ai/     a COPY of N24 as model.bin

The trainer writes into that tree. data/ai/model.bin is not in it, so
the constraint holds by construction and not by care. Hash verified
before launch and to be verified after.

Configuration follows the peer's advice: N24 as parent rather than
model.bin (which it beats 226/87/23 to 85/51/4), the r[3] fog ON so the
policy learns feature 3 is sometimes a proxy, headless through the
server binary so no window appears, gated through odlock for memory.
Six maps of 1,200 turns.

The child will be benched head-to-head against N24 -- same binary, same
env, 400 turns, more than one seed set. Every previous training verdict
in this project was reached on fitted seeds at 120 turns reading rating
first, which is the instrument this session spent a day discrediting, so
"training does not work here" is a conclusion that has never actually
been tested properly. That is the reason to run it, rather than any
expectation about the outcome.

Correction, immediately: the parent hash logged at launch (fa56fad5) is
not the one I copied in (adb32ac8). The aborted two-minute smoke test
had already trained on that file before it was killed, so the parent is
N24 plus a couple of minutes of self-play, not N24 exactly.

It does not invalidate the run -- the child is still compared
head-to-head against N24 itself, and any descendant of N24 is a valid
candidate -- but "parent = N24" is now approximate and the journal
should not carry the tidier claim. The lesson is small and familiar: a
process killed by a timeout has usually already done something, and the
file it was writing is not the file you put there.

## 129 — the ablation profile, and a caution that measured to zero

N24, 120 turns, fixed seeds, current binary, r[3] replaced by its mean
(0.1041):

    no ablation        187 / 90 / 38
    fogged only (16%)  189 / 90 / 38     +2
    published (84%)    182 / 90 / 38     -5
    all                182 / 90 / 38     -5

The peer's shipped-model profile: ~0 fogged, -5 published, -3 all. Same
shape and the same published cost, on two policies 65 rating points
apart un-ablated (187 against 122).

So the correction I pressed on them -- that before-controls belong to
the lineage being trained -- was right as an argument and worth nothing
as a fact. Two policies COULD read a feature by different amounts, and
had they, a cross-lineage control would have reported the gap as a
training effect. They do not. The variance estimate I asked for is
approximately zero, which is a better thing to know than the caution.

Worth keeping as a pattern: a methodological objection can be sound and
still cost more than it saves. The right response was the one taken --
measure it rather than argue it -- and the cheapest version of that was
one bench chain interleaved with a training run.

Two things fall out that are more useful than the original point:

  * survival and floor do not move AT ALL in any ablation mode. Whatever
    r[3] buys, it is not seat survival, and the whole effect is in the
    unstable column.
  * the -5 is identical across a large gap in policy strength, which
    suggests it is a property of the feature's information content
    rather than of how well a policy exploits it. If a fog-trained child
    shows a published loss much LARGER than 5, the natural reading is
    not "it learned to trust the real figure" -- there may be only five
    points there to win -- but that it has become dependent on an input
    that is a guess one read in six. That is fragility, not skill.

## 130 — ablating the whole relational slice

The peer built OD_AI_REL_ABLATE_FEATURE=<0..7> with a per-feature
constant and handed the running of it here, since their running it would
contend with the training the user asked for. Nine runs on N24: one
baseline and eight ablations, each feature replaced by its OWN mean over
98,420 reads.

    r0 army ratio 0.5760   r4 at war with them   0.2267
    r1 provinces  0.5784   r5 allied with them   0.0370
    r2 industry   0.5390   r6 their weariness    0.0125
    r3 treasury   0.1041   r7 their claims       0.5903

READ ON SURVIVAL AND FLOOR, not rating. r3's ablation moved rating by 5
and survival and floor by nothing at all, so a sweep read on rating
would produce eight numbers from the column that flips sign between
world sets -- and eight unstable numbers are worse than none, because
they look like a ranking.

Two predictions recorded before the numbers exist, both the peer's:

  1. r4 (at war) and r0 (army ratio) are the likeliest to move survival
     or floor, being the only two bearing directly on whether a country
     is about to be attacked and whether it can hold. All-zeros
     INCLUDING those two is a much stronger statement than eight quiet
     numbers from features nobody expected to matter.
  2. r5 (allied) has a mean of 0.0370 because alliances are rare, so it
     is nearly constant already and ablating it to its own mean should
     be a no-op BY CONSTRUCTION. If r5 moves anything, the sweep has a
     bug rather than a finding.

The second is the better idea of the two: a prediction that can only be
satisfied by correct machinery, embedded in the experiment. Nothing this
session has run has carried its own bug detector, and several results
would have been caught sooner if they had.

Result -- and it is the cleanest thing measured in two days:

    feature                 rating  surv  floor
    baseline                   187    90     38
    r0 army ratio              181    87     23   <-- moves the floor
    r1 provinces               177    90     38
    r2 their industry          212    90     38
    r3 treasury                182    90     38
    r4 at war with them        198    87     23   <-- moves the floor
    r5 allied (bug check)      188    90     38
    r6 their weariness         170    90     38
    r7 their claims            191    90     38

TWO of eight features touch survival or floor, and they are exactly the
two the peer named before the numbers existed: r0 (army ratio) and r4
(at war with them). Both by an identical amount, -3 survival and -15
floor. The other six move rating between -17 and +25 and leave survival
and floor untouched TO THE DIGIT.

The r5 no-op landed as predicted, so the machinery is doing what it
claims and the r0/r4 movements are real.

The rating column is meanwhile incoherent: ablating "their industry"
IMPROVES it by 25, and ablating "at war with them" -- a feature that
demonstrably matters, since it costs floor -- improves it by 11. A
ranking built on that column would put "destroy the enemy-industry
input" top of the list of improvements.

WHAT THIS SAYS

The policy's seat competence rests on two facts about a neighbour: how
big their army is relative to mine, and whether we are at war. Six of
its eight relational inputs contribute nothing to whether it keeps its
seats. That is a statement about where a strong model's advantage lives
-- and it is not in reading its neighbours in any detail.

It also retires the fog question as anything but a rating-column effect:
r3 is one of the six, so the disclosure mechanic cannot cost seat
survival however the training goes, and a retrain that improves the
r3 handling is optimising a column that flips sign between world sets.

The prediction-first design is what makes this readable. Eight numbers
in the rating column would have looked like a ranking; two pre-named
features moving the stable columns and a pre-named no-op staying flat is
a result. Worth adopting generally: state which arms should move and
which should not BEFORE running, and include one that must not.

## 131 — the floor is a magnifier, and it undercuts my own headline

The peer found it on their district law and it applies to my sweep
directly. score = held / par, so a seat with a small par multiplies
whatever happens to it. Norway's par is 1.3% of the world; France's is
6.7. The SAME movement in land reads five times larger on Norway.

My sweep, in raw land share rather than normalised score:

    seat            par    base     r0      r4
    1914:SWE        1.0    2.73    2.40    2.60
    1939:NOR        1.3    0.50    0.30    0.30
    modern:CHN      2.5    4.20    2.63    4.53
    1939:USA        5.6   16.90   18.13   18.47
    1914:FRA        6.7   14.43   14.73   13.73
    1914:FRA:rush   6.7    8.43   11.53   12.70

Norway goes 0.50% -> 0.30% of the world under both r0 and r4. That is
two tenths of one percent of the map, and the floor column reports it as
15.4 points -- a 77x amplification. Meanwhile r0 costs China 1.57 points
of actual land, five times more territory, and appears only in the
rating column I have been telling everyone to ignore.

So "exactly two of eight features touch the stable columns" is
substantially an artefact. What is true is narrower: r0 and r4 are the
two features that move the SMALLEST-PAR SEAT, and that seat is where the
floor metric points. The finding I reported -- that the policy's seat
competence rests on army ratio and at-war -- is not established by this
data.

WHAT SURVIVES

  * six of eight features leave Norway untouched at 0.50%, which is a
    real fact about those features and that seat
  * r0 and r4 move it identically, to 0.30%, which is still consistent
    with one fragile seat rather than two signals
  * the per-seat land table shows r0 and r4 are NOT alike elsewhere
    (China 2.63 vs 4.53), which stands -- that comparison is in land,
    not in normalised score, so the magnifier does not touch it

WHAT I GOT WRONG, AND HOW

I spent a day establishing that rating is unstable and floor is stable,
and concluded that floor should be read first. Stability is not
validity. The floor IS more consistent across world sets -- it kept
pointing at the same fragile seat -- and that consistency came from the
metric always magnifying whichever seat is nearest zero, not from it
measuring something more real. I mistook a magnifier for a microscope
because it kept giving the same answer.

Three aggregates have now hidden three findings in two sessions: the
rating mean, the floor minimum, and the survival mean. In every case the
per-seat table was already in build/od_bench_results.json.

## 132 — you cannot ablate a bit to nothing, only to a lie

The peer proved the comma knob fires both indices with a vector dump --
baseline against ablated, same binary -- which is a better control than
the bench arm I had running: it shows the mechanism firing instead of
inferring it from an outcome, at three turns instead of eighteen runs.
Reversed arm killed; it could no longer distinguish anything.

Their dump showed something neither of us was looking for. The baseline
vector has r4 = 0, and the ablated one has r4 = 0.227. Reading the
source:

    r[4] = rr->second.war      ? 1.0f : 0.0f;
    r[5] = rr->second.alliance ? 1.0f : 0.0f;

Both are BINARY. Their "means" are the fraction of country-pairs at war
(0.2267) and allied (0.0370). So holding r4 at its mean does not tell
the policy "you do not know who you are at war with" -- it tells it "you
are 23% at war with everybody", including the country it is fighting and
the five it is not. The sweep ran a different intervention from the one
it was designed for.

Two rows reinterpreted:

  * r4's +11 rating / -15 floor measures believing a uniform false war
    state, not losing war information.
  * r5's flatness is partly BY CONSTRUCTION -- 0.037 is near enough to
    zero that the ablation barely moves the input for the 96% of pairs
    that are not allied. The bug check passed, and my reading of WHY it
    passed was wrong.

For a one-bit feature there is no neutral constant. 0 means "believe you
are at peace with everyone", 1 means "believe you are at war with
everyone", and neither is ignorance. The technique does not have a
version that answers the question for binary inputs, which is a limit of
the instrument rather than a bug in it.

The six continuous features -- ratios and sums, where the mean is a real
central value -- are unaffected, so the rest of the sweep stands as run.

THREE INSTRUMENT DEFECTS IN ONE DAY, all found by someone checking the
thing rather than the number: the fitted seeds, the floor magnifier, and
now mean-ablation of a bit. Each was invisible in the output and obvious
in the mechanism. The standing check that would have caught all three is
the same one: print what the machinery is actually doing before
believing what it reports.

## 133 — the sweep again, with an ablation that means what it says

The peer built OD_AI_REL_ABLATE_MODE=rotate: each candidate's value for
the chosen feature moves to its neighbour, so the MULTISET is preserved
and only the PAIRING is destroyed. For a bit that is the ignorance the
constant could not express -- the policy still knows it is at war with
one of these six, and no longer knows which. Deterministic, since a
shuffle cannot be replayed; a rotation by one is a derangement for two
or more candidates. Verified over 1,908 rotations.

They also found a trap worth more than the knob: comparing a baseline
dump against an ablated dump ACROSS RUNS shows rotation apparently
deleting flags, because the ablation changes decisions, the runs diverge
immediately, and the same country is later dumped in a different world.
A correct rotation looked broken for ten minutes. Cross-run dump
comparison is invalid for the same reason cross-lineage controls were:
the thing being compared has moved underneath the comparison.

PREDICTIONS, before the numbers:

  1. r4 rotated should damage MORE than r4 held constant. The constant
     told a uniform lie about a state the policy can partly infer from
     elsewhere; the rotation destroys the pairing it actually uses.
  2. r5 rotated should remain near a no-op, but for an honest reason
     this time -- rotating a vector that is almost all zeros moves zeros
     among zeros. Still the bug check.
  3. r2 (their industry) rotated should NOT reproduce its +25 rating.
     That number came from replacing a whole distribution with one
     value; rotation keeps the distribution, so if +25 was the policy
     being helped by uniform industry it should vanish.

Read on RAW LAND per seat. The floor is a magnifier and the rating flips
sign; the land table is the only column not known to be broken.

## 134 — the training run finished

6,289 turns in 118 minutes, 358 million updates, saved as
build/loop/model.T1.bin. data/ai/model.bin verified untouched at every
check: the OD_DATA_DIR redirect held for the whole run, and the
constraint was structural rather than careful -- the trainer never had a
path to the file.

Parent was N24 plus the two minutes of self-play an aborted smoke test
had already put on the copy, which is recorded rather than tidied away.

The child is benched the way this session learned to bench, not the way
every previous training verdict in this project was reached:

    400 turns, not 120        -- the horizon that inverted six results
    two hold-out seed sets    -- one set is a hypothesis, whatever its size
    head-to-head against N24  -- its own parent, not data/ai/model.bin
    same binary, same env     -- both arms, no rebuild between

Queued behind the rotate sweep so the two do not contend.

Worth saying what would count as success before the numbers exist. N24
reads 226/87/23 on set C and 183/85/36 on set D at 400 turns on this
binary. A child that beats it on ONE set is not an improvement -- that
is exactly the evidence that produced the +75 I withdrew this morning.
It has to win on both, and I will read the raw per-seat land shares
rather than the floor, because the floor is a magnifier and the rating
column recommends blinding the AI to enemy industry.

Result -- THE TWO SWEEPS DISAGREE, and both my predictions failed:

    feature          const   rot    land change (rot)
    army ratio         181   182    +0.20%
    provinces          177   203    +4.00%
    their industry     212   179    -3.77%
    treasury           182   199    +4.37%
    at war             198   180    -2.33%
    allied             188   182    -0.43%
    weariness          170   188    +0.10%
    their claims       191   205    +2.93%

PREDICTION 1 WRONG. r4 rotated was supposed to damage MORE than r4 held
constant, since rotation destroys real pairing information. It damages
LESS: 180 with survival and floor AT BASELINE (90/38), against the
constant's 198 with 87/23. So the constant-mode "damage" to the stable
columns was entirely the uniform false war state. Losing WHICH neighbour
you are at war with costs nothing measurable.

PREDICTION 2 WRONG, instructively. r5 rotated is -5, not the no-op it
was under the constant. Rotation makes a RARE binary informative in a
way a mean cannot: when a country genuinely has an ally, rotating that 1
tells it the ally is somebody else -- a lie about a real fact. 0.037 was
too close to 0 to express that. So the control that made the first sweep
readable does not transfer to the mode that replaced it, and the rotate
sweep has no bug check at all.

PREDICTION 3 RIGHT. r2's +25 vanishes (212 -> 179). It was an artefact
of collapsing a distribution to one value, not the policy being helped.

WHAT TO CONCLUDE, WHICH IS LESS THAN EITHER TABLE SUGGESTS

Six of eight features change sign or magnitude substantially between the
two modes. Two ablations of the same feature, both defensible, give
opposite answers -- so neither table measures "what this feature is
worth". They measure what a particular corruption of it does, and the
corruption is doing most of the work.

And the land column says the AI GAINS territory when four of its inputs
are scrambled: provinces +4.0%, treasury +4.4%, claims +2.9%. Either
those features are actively harmful, or -- far likelier -- a single
120-turn fixed-seed run cannot resolve effects this size, and I have
been reading eight numbers off an instrument whose noise floor I never
measured. That is the same error as the fitted seeds, one level down: I
checked whether the SEEDS were representative and never whether a single
run of six seats is repeatable.

CORRECTED before running anything: the noise floor is ZERO and I already
had the data. sweep-base and N24-abl-none are the same configuration run
separately on the same binary, and their per-seat shares are identical to
four decimals on all six seats. The bench is fully deterministic.

Which sharpens the problem instead of dissolving it. These are REAL
differences in play, not sampling error -- but they are chaotic. One
changed decision cascades over 120 turns into a different world, so an
effect of +4% of the map can be a genuine consequence of a trivial
perturbation rather than a measure of what the feature was worth.
"Real" and "unrankable" are not in tension, and that is exactly why two
defensible ablations of one feature give opposite answers.

The honest conclusion from both sweeps together: this technique can show
that a feature does SOMETHING (r5 rotated moves play; r5 held constant
does not), and cannot say what a feature is WORTH. For the latter the
comparison has to be against many worlds, not many corruptions of one.

## 135 — re-reading the decisions in land, one that survives

The peer suggested the 0.20 share had been tuned to protect a phantom
floor. Checked from the stored results, free, no runs:

    share 0.35 (kept) vs 0.20 (rejected), hold-out set A, LAND

    seat              0.35    0.20   delta
    1914:FRA:rung    16.70   17.60   +0.90
    1914:FRA:rush    15.67    9.80   -5.87
    1914:SWE:rung     2.00    0.97   -1.03
    1939:NOR:hood     2.00    2.33   +0.33
    1939:USA:rung    18.13   22.03   +3.90
    modern:CHN:rung   6.00    5.57   -0.43
    TOTAL            60.50   58.30   -2.20% of the world

The rejection stands, and for better reasons than I gave. Share 0.20
loses 2.2% of the map, concentrated on rush-France (-5.87) and Sweden
(-1.03). And Norway GAINS 0.33% -- so the "-57 floor" I cited when
rejecting it was not merely magnified, it pointed the WRONG WAY. Norway
was the floor seat and it improved; the floor score fell because the
seat's score is held/par and something else about that run moved.

So: right decision, wrong reason, twice over. That is the first of
today's decisions to survive re-reading in land rather than dissolve,
and it survives on a number I never looked at -- total land held across
the six seats, which is neither of the two columns this project prints.

Worth noting what that column is. Rating is the mean of six par-relative
scores; total land is the sum of six raw shares. The first weights a
seat by how small it is, the second by how large. Neither is obviously
right, but the second is at least not a magnifier, and for "did this
change help the AI hold ground" it is the closer question.

The peer's defence of the phantom-chasing is fair and I am recording it
because I would not have said it myself: the recall and home-first
experiments were aimed at a floor cost that mostly did not exist, and
they produced the observation that redirecting every reinforcement home
moved rating and did not move the floor by a point -- which, read in
land, is now evidence that the floor row was inert to logistics because
it was inert full stop. A session that had not chased the phantom would
have had nothing to check the magnifier against.

## 136 — the training run failed, decisively and in land

    set C, 400 turns, one binary       N24      T1     delta
    1914:FRA:rung                    18.17    4.87   -13.30
    1914:FRA:rush                     8.00    0.07    -7.93
    1914:SWE:rung                     3.13    2.20    -0.93
    1939:NOR:hood                     0.30    0.33    +0.03
    1939:USA:rung                    18.67    2.17   -16.50
    modern:CHN:rung                   7.47    1.97    -5.50
    TOTAL                            55.73   11.60   -44.13

    rating 226 -> 73, survival 87 -> 53.

Not a metric artefact and not close. The child holds a fifth of the land
its parent does, and loses on five of six seats. The only seat it does
not lose is Norway, which it improves by 0.03% of the world -- the
magnifier seat, contributing nothing.

6,289 turns and 358 million updates of self-play from a strong parent
produced a substantially weaker policy. That reproduces this project's
standing finding that training degrades the model, and it is the first
time that finding has been established on an instrument I trust: hold-
out worlds, 400 turns, head-to-head against the actual parent, read in
land rather than in either broken column.

WHAT IT DOES AND DOES NOT SETTLE

It settles that a short unsupervised self-play run from N24, with the
current reward and the fog on, makes things worse. It does not settle
that training cannot work here -- one run, one configuration, no
checkpoint selection. The pool of 21 models was produced by training,
and N24 is in it.

What it does retire is the specific hope that drove this: that "training
does not work here" was an artefact of the old instrument. It was not.
Measured properly, the run is worse than measured sloppily would have
suggested -- 226 to 73 is not a marginal verdict that a better bench
might have rescued.

The honest summary of the day's largest single expenditure -- two hours
of compute and a session's worth of setup -- is a clean negative.

## 137 — no checkpoints, so no diagnosis

The peer asked the right question: monotonic decline, a cliff, or up-
then-down? Those need different fixes, and only the third makes
checkpoint selection sufficient rather than merely damage-limiting.

The answer is that I cannot tell. --train-ai checkpoints by OVERWRITING
model.bin every sixty seconds, so the run kept one moving file and the
curve is gone. Two league snapshots survive (17:41 and 19:24) and are
not loadable as models -- tried from the redirected tree and from a
normal path, both rejected.

Worth recording separately: the rejection message is WRONG. It reads

    [AI] Fresh model (no file at /.../model.LG1.bin)

for a file that exists and that I had just copied there. It is a format
rejection reported as a missing file, and it silently falls back to a
fresh model rather than failing. Anyone who hits that while chasing
something else loses an hour, and a bench run against a rejected model
file would silently measure an untrained net -- which is a way to
produce a very confident wrong number.

THE LESSON, which is the peer's and is worth more than the run:

    a training run that keeps no checkpoints cannot be diagnosed
    afterwards, only repeated.

Two hours of compute produced one number -- 226 to 73 -- and no way to
ask why. Keeping four snapshots would have cost 15 MB and would have
distinguished three hypotheses that now need a rerun to separate.

That is the actual failure of this experiment. The collapse is a
finding; the inability to explain it is a design mistake I made before
starting, by not checking what the trainer retains.

Set D confirms it, and worse:

    set   N24 land   T1 land    delta
    C       55.73%    11.60%   -44.13
    D       69.50%     9.60%   -59.90

    rating 226 -> 73 and 286 -> 63; survival 87 -> 53 and 90 -> 52.

Unlike every other result today this one does NOT reverse on the second
set -- it gets worse. Two hours of self-play from a strong parent cost
about fifty points of the world's land, on both sets, on five of six
seats each time.

So the record stands: the training run failed, and it is the first
conclusion of the session that survived a second seed set without
shrinking. The reason is worth noting -- effects this size do not need a
careful instrument. Everything that required careful measurement today
turned out to be noise or artefact; the one thing that did not is
visible from across the room.

## 138 — the three checks, all clean

    r6 rotate control     179   90 / 38
    r6 baseline           179   90 / 38     exact pass
    weariness fog OFF     187   90 / 38     +8 rating, stable columns flat

THE ROTATE CONTROL PASSES EXACTLY. r6 is constant across candidates
since the peer's weariness fog, so rotating it must return baseline, and
it does on all three columns. That is the first true no-op the
relational slice has ever had -- r5 only looked like one under the
constant mode, and stopped being one under rotation. It verifies the
rotate path, which retroactively supports the rotate sweep's numbers as
real measurements rather than artefacts of a broken mechanism.

It also retires the r6 row of that sweep: on this binary r6 IS the
control, so 188-vs-187 was measuring a feature that has since become
constant.

THE ENVIRONMENT MISMATCH IS REAL AND SMALL. T1 trained without the
weariness fog and would be benched with it; turning the fog off is worth
+8 rating with survival and floor identical. So the mismatch exists, sits
entirely in the column that flips sign between world sets, and is two
orders of magnitude short of explaining a 44 to 60 point loss of the
world's land.

That closes the training post-mortem as far as it can be closed without
checkpoints: the fog was not the cause, the parent was right, both of
the peer's corrections were correct, and the collapse is upstream of all
of it. What it was remains unknown and unknowable from this run.

## 139 — re-reading today's own rejections in land, and one changes

The floor correction applies to my own decisions, not only to the
peer's. Hold-out set A, 120 turns, land held across six seats:

    config                 land    delta
    control (as shipped)  60.50
    campaign cap 2        61.10    +0.60
    concurrent wars 2     71.20   +10.70
    share 0.20            58.30    -2.20

CONCURRENT WARS 2 GAINS 10.7% OF THE WORLD. I rejected it this morning
because rating was flat (-1) and the floor "collapsed" 154 -> 18 -- and
that floor is Norway, the magnifier seat, moving 2.00% -> 0.23% of the
map. In land the AI holds ten points more of the world with two wars
allowed.

Per seat:

    1939:USA      18.13 -> 30.83   +12.70
    1914:FRA      16.70 -> 21.10    +4.40
    1914:SWE       2.00 ->  1.83    -0.17
    modern:CHN     6.00 ->  4.77    -1.23
    1939:NOR       2.00 ->  0.23    -1.77
    1914:FRA:rush 15.67 -> 12.43    -3.23

So it is a real trade, not the artefact I rejected -- but the same shape
seen all day: the large safe seats gain, the exposed ones lose. The
difference is that in land the gains are an order of magnitude larger
than the losses, where in floor-score the single smallest seat dominated
the reading and reversed the verdict.

Re-testing at 400 turns on two hold-out sets with own controls, because
one 120-turn set decides nothing and I have said so about six other
results today. The share rejection stands on the same re-reading (-2.2%)
and the cap is within noise (+0.6%), so this is the only one of the
three that changes.

## 140 — the lineage caution was right after all, and entry 129 is wrong

The peer measured the weariness fog at +1 on the shipped model. I
measured the same change at +8 on N24, same binary, both with survival
and floor untouched. Same change, two policies, eight times the cost.

So entry 129 is wrong where it says the lineage caution "was right as an
argument and worth nothing as a fact". That was inferred from r3 alone,
where both models read -5 despite being 65 rating points apart, and I
generalised one feature's agreement into a property of the measurement.
r6 disagrees by a factor of eight.

What survives is the weaker and original claim: sensitivity to a feature
has to be checked PER FEATURE, and a number quoted without naming the
model is missing a column. Which was the a priori argument I made and
then helped retire.

Worth naming the shape, because I wrote it about the peer's retraction
four hours ago and then did it myself: A CORRECTION INHERITS THE
CONFIDENCE OF THE THING IT CORRECTS. Nobody re-examines a retraction as
hard as they examined the claim, and a retraction built on one feature's
evidence is exactly as fragile as a finding built on one seed set. This
is the third instance today -- the 09-04 training correction, the peer's
district-law reading, and now mine.

The practical consequence for the fog: its price is 1 point on the
shipped model and 8 on N24, and N24 is the bar. Still small, still
entirely in the unstable column, still not a reason against a mechanic
the user asked for on fairness grounds. But "costs nothing" was a
statement about a model nobody is shipping.

Registered before the re-test lands, from the peer and worth recording
as a condition rather than a hope: the +10.7% rests mostly on one seat,
the USA gaining 12.7%. NOR is at -1.77% and rush-France at -3.23%, and
those two are the seats that have carried the survival column all day.
At 400 turns everything gets bigger and the exposed seats have less room
to give, so the losses may grow faster than the gain.

What I will accept as a result: the total stays strongly positive on
BOTH sets, and the gain is not one seat's story. What I will not accept:
a narrow positive total that depends on the USA row, which is the same
structure as a +75 resting on set C.

If it narrows, the honest reading is that concurrent wars 2 moves land
between seats rather than adding it -- which the floor column said in
its own distorted way, and which would make my rejection right for the
wrong reason rather than wrong.

Result, set C at 400 turns -- the reversal does not survive:

    seat              1 war   2 wars    delta
    1914:SWE:rung      2.67    12.27    +9.60
    1939:NOR:hood      0.30     0.40    +0.10
    modern:CHN:rung    5.73     0.20    -5.53
    1939:USA:rung     19.93     9.63   -10.30
    1914:FRA:rung     18.17    23.77    +5.60
    1914:FRA:rush      7.83     6.73    -1.10
    TOTAL             54.63    53.00    -1.63

-1.63% of the world against +10.70% at 120 turns, and the composition
INVERTS: the USA goes from +12.70 to -10.30, Sweden from -0.17 to +9.60.
Not a narrowing -- a different result with the seat contributions
swapped.

So it fails the condition registered before the run, in the strongest
available way. The +10.7% was one seat's story, which is what the peer
said to watch for and what I agreed to treat as disqualifying.

MY ORIGINAL REJECTION WAS RIGHT, FOR THE WRONG REASON

I rejected concurrent wars 2 this morning because the floor collapsed.
That reasoning was invalid -- the floor was Norway moving 0.2% of the
map through a 77x magnifier. But the conclusion holds: at the horizon
that decides, two wars is worth -1.63% of the world, and the 120-turn
land gain was as unreliable as the 120-turn floor loss.

Which is the more interesting outcome than a clean reversal would have
been. The magnifier DID invalidate my reasoning; it did not invalidate
the decision. Those are separable, and today I have twice assumed a bad
instrument meant a bad verdict -- once about the peer's district law,
once about my own rejection. A broken metric can still point the right
way, and finding the break does not license re-opening everything it
touched.

Set D still running. The condition required both sets, so this is
already decided unless set D is large enough to reverse a negative,
which would itself be the disagreement-between-sets that disqualifies.

Set D completes it:

    set   1 war    2 wars   delta    USA alone
    C     54.63%   53.00%   -1.63    -10.30
    D     70.63%   71.90%   +1.27    +20.90

Both totals within +-1.6% of the world -- nothing -- while the USA seat
swings 31 points between the two sets. So concurrent wars 2 does not add
or remove land. It makes one large seat's outcome violently world-
dependent, and the total averages that into noise.

That is a third distinct reading of the same change in one day:

    floor score, 120t    "collapses the worst seat"   (magnifier artefact)
    land, 120t, set A    "+10.7% of the world"        (one seat's story)
    land, 400t, two sets "nothing, with huge variance" (what it does)

The rejection stands. It stood through a re-reading that appeared to
overturn it and a re-measurement that did not, which is three
independent looks at a decision made this morning for a reason that was
wrong.

I would rather have spent those four bench-hours elsewhere, but the
alternative was leaving a plausible +10.7% unexamined in the journal,
and it would have been the most cite-able wrong number in the file.

## 141 — the land column, printed

od_bench now prints total land held alongside the rating, survival and
worst seat. Additive: nothing reordered, nothing removed, and what the
tool LEADS with is still the user's call. The reasoning lives in the
source beside the line, not only here:

    land-line-smoke: OD BENCH 164
    land-line-smoke: land 43.90% of the world across 6 seats
                     survival 87   worst seat 23

Every measurement in this session required reconstructing that number by
hand from build/od_bench_results.json, and three separate conclusions
turned on it once reconstructed: the campaigns "trade" that was 0.27% of
the map, the concurrent-wars rejection that survived three readings, and
the ablation tables whose two modes disagreed on six of eight features.

Why it is worth a column rather than a note. Rating is a mean of
held/par, so it weights a seat by how SMALL it is -- Norway at par 1.3%
against France at 6.7 means the same tenth of a percent of land moves
Norway five times further. The worst-seat column is that effect at its
extreme, reporting whichever seat is nearest zero with the magnification
maximal. Land weights a seat by how LARGE it is, which is its own bias
and stated as such in the comment. The point is not that land is right;
it is that land is not a magnifier, and having both makes the
disagreement visible instead of leaving one of them to win by default.

That is the last of the instrument work. Three env knobs, provenance on
every row, a verified no-op control, and now the column the day's
findings actually turned on.

## 142 — checkpoint-selected training, at the user's direction

The previous run took the end state, which is not how anyone trains and
is why its failure could not be diagnosed. This one keeps the curve.

    parent      a CLEAN copy of N24 (adb32ac8). The last run's parent
                carried two minutes of self-play from an aborted smoke
                test; that is fixed rather than noted this time.
    training    8 maps x 1500 turns, headless, OD_DATA_DIR redirected
    snapshots   every 12 minutes into build/loop/ckpt/, with a torn-file
                guard: any copy under 3 MB is discarded rather than kept
    isolation   data/ai/model.bin is not in the tree being written to

The torn-snapshot risk is real -- the trainer rewrites model.bin every
sixty seconds and a copy taken mid-write is corrupt. Two things catch it:
the size guard here, and the peer's loader fix, which now REFUSES a bad
model file loudly instead of silently falling back to an untrained net.
Before that fix, a torn checkpoint would have benched as random weights
and produced an ordinary-looking score. This run would have been the
first thing to hit it.

PLAN AFTER IT FINISHES

  1. screen every checkpoint at 400 turns, one seed set, against N24
  2. take the best and confirm on two hold-out sets
  3. keep nothing that does not beat N24's 226/87/23 on set C

The curve is the point. Three hypotheses the last run could not separate
-- monotonic decline, a cliff, up-then-down -- predict visibly different
shapes, and only the third makes checkpoint selection sufficient rather
than merely damage-limiting.

Stated before the numbers: I expect monotonic decline. The previous run
lost 44 to 60 points of the world's land from the same parent, and if
there were a better model in the middle, the end state would have to
have fallen off a cliff rather than drifted. But that is a guess, and
the run exists because guessing is what the last one left us doing.

## 143 — a stream shift that does not appear in a diff

The peer moved properPlaceName into a shared header and added eight
demonyms. The move is inert; the additions are not, and the mechanism is
one that no diff shows: inside deriveTerritory the name lookup happens
before the first simRand(), so an irregular that HITS returns early and
consumes fewer draws. Every subsequent draw in that world shifts.

They measured it -- identical to four figures over 3 maps x 120 turns,
because none of those eight demonyms produced a breakaway in that run --
and flagged it anyway, on the grounds that "did not fire in 360
country-map-turns" is not "cannot fire in 12,000". My training is 8 maps
x 1500 turns and Russian and Italian minorities are all over the shipped
maps.

I took the rebuild rather than asking them to revert the feature. The
cost is one extra arm: N24 re-measured on the post-rebuild binary, which
I should run regardless, since every stored N24 number predates their
change and is therefore from a different stream whether the irregulars
fire or not.

TWO THINGS THAT ARE NOT THE SAME QUESTION

  * comparison validity -- all BENCHED arms on one binary. Satisfied by
    putting checkpoints and parent both on the new one.
  * environment match -- the checkpoints are produced under the old
    stream and judged under the new. Weaker, and accepted here: it is
    the same class as T1's weariness-fog mismatch, which measured 8
    points, and this is a name table that may never fire.

I have conflated those twice today and they need different remedies. The
first is a discipline; the second is a magnitude to bound.

SEQUENCE: training finishes on its current binary, then rebuild, then
re-measure N24, then screen the checkpoints. The trainer keeps one
binary for the whole run and every benched arm shares another.

## 144 — checking the setup before the curve, not after

The peer's warning is the right one: if the curve comes back monotonic
it AGREES with what we both expect, and that is exactly when a setup
error goes unexamined. A negative result that confirms a prior deserves
the scrutiny a positive one would get.

So, checked while the run is still going rather than after:

WHY IS A TRAINED CHILD SMALLER THAN ITS PARENT? T1 was 3,632,034 bytes
against N24's 3,693,162, which would be alarming if it meant the trainer
had started from a fresh net. It does not. The log says

    [AI] Model loaded from .../traindata/ai/model.bin (6.1 MB)
    [AI] Model saved (3745838 bytes, 354383050 updates)

Three things settle it. The in-memory model is 6.1 MB against a 3.69 MB
file, so the serialisation is compressed and its size is not a fixed
function of the weights. Consecutive saves in this run vary between
3,734,630 and 3,747,945 bytes on their own. And the update counter opens
at 354 million rather than at zero, so the parent's history is inherited
and the net is not reinitialised.

That rules out the most damaging setup error available -- training from
scratch while believing it started from N24 -- which would have produced
exactly the collapse we saw and would have been invisible in every
number I took.

WHAT I WOULD STILL CHECK IF THE CURVE IS MONOTONIC

  * a checkpoint that refuses to load (the peer's loader fix makes this
    loud rather than silent, and the size guard drops torn copies)
  * the first snapshot benching materially below N24 -- twelve minutes
    of training should not move much, so a large gap at ck-01 would
    point at the harness rather than at learning
  * the update counter advancing across snapshots, confirming they are
    distinct states and not the same file copied repeatedly

The third is worth doing regardless and costs nothing: if two
checkpoints have identical hashes, the snapshotter caught the same save
twice and the curve has fewer points than it appears to.

## 145 — the screening plan, decided before the checkpoints exist

The run is 8 maps x 1500 turns and snapshots every 12 minutes, so it
will produce roughly 15 to 18 checkpoints. Screening all of them at 400
turns is about five hours of bench, which is more than the question
needs.

The question is the SHAPE, and a shape does not need every point:

    screen ck-01, then a spread across the run, then the final state
    -- five points at 400 turns on one seed set, about 85 minutes

Five points separate the three hypotheses cleanly. Monotonic decline
shows every point below the parent and falling. A cliff shows a flat
stretch then a drop, and the two points bracketing the drop say where to
look. Up-then-down shows a peak, and the peak is the model worth
confirming on a second set.

Deciding this before the numbers exist so the sampling cannot be chosen
to flatter a story. If the curve turns out ambiguous between two shapes
I will bench the points BETWEEN my samples rather than re-reading the
ones I have -- the failure I want to avoid is picking a subset after
seeing the run and calling it a curve.

ck-01 matters most and is the cheapest guard against my own harness.
Twelve minutes of training from N24 should barely move; if ck-01 already
benches far below the parent, the fault is in the setup rather than in
the learning, and nothing after it is worth reading.

## 146 — the league record is already a curve, and my prediction was wrong

Training finished: 7 checkpoints, 7 distinct hashes, all full size, none
torn. data/ai/model.bin untouched. But the run logged its own curve
before any bench touched it -- the league slot pits the trainee against
a frozen earlier copy of itself:

    check   frozen self   trainee   maps lost
      1          1.0        2.3       0/1
      2          4.5        9.0       0/2
      3          6.8        1.0       1/3
      4         25.6       11.2       2/4
      5         42.3       32.0       3/5

It WINS the first two and loses the next three. That is up-then-down,
and I predicted monotonic decline in entry 142 on the grounds that the
previous run lost 44 to 60 points of the world from the same parent.

Two things follow if the bench agrees.

FIRST, checkpoint selection is sufficient rather than merely damage-
limiting. There is a better model in the early snapshots and taking the
end state threw it away -- which is exactly what the previous run did,
and why it looked like "training degrades the model" rather than
"training peaked and then degraded".

SECOND, the diagnosis was available for free. This log was written by
the previous run too and neither of us read it: the peer asked the right
question -- monotonic, cliff, or up-then-down -- and the answer was in a
file on disk while I was explaining that no checkpoints meant the curve
was unrecoverable. The curve was unrecoverable at BENCH resolution; the
league record is a coarser instrument that was recording all along.

Which is the day's shape once more: I built an instrument to answer a
question that a cruder one already answered, and the cruder one is the
game telling me about itself rather than me measuring it.

Benching all seven anyway. The league number is 5 maps against one
frozen opponent and is not a substitute for 6 seats x 3 seeds x 400
turns -- but it is now a prediction the bench can falsify, which is
better than the bench having no prior at all.

## 147 — the guard fired, and it caught my assumption rather than the harness

ck-01 benches 110 / 24.17% land against the parent's 221 / 57.47%. In
entry 145 I wrote that twelve minutes of training should barely move the
model, and that a large gap at ck-01 would point at the harness.

The gap is real and the harness is fine. The assumption was wrong:

    parent loaded at    354,383,050 updates
    run finished at     357,189,005 updates
    total added           2,805,955 in 80.3 minutes
    so twelve minutes is roughly 420,000 gradient updates

420k updates is not "barely moves". A guard is only as good as the prior
it encodes, and mine encoded a guess about wall-clock time that I never
checked against the update counter sitting in the same log.

WHAT THE NUMBERS NOW SAY

    parent   221 / 57.47%
    ck-01    110 / 24.17%   after ~420k updates
    T1        73 / 11.60%   after ~2.8M updates (the previous run)

Most of the damage happens in the first twelve minutes and the rest
accumulates. That is closer to a cliff-then-decline than to the
up-then-down the league record suggested, and the two instruments now
disagree: the league had the trainee WINNING its first two matches while
the bench has it at half the parent's land by the first snapshot.

Both can be true. The league opponent is a frozen copy of the trainee
itself, so early on it is comparing two damaged policies against each
other -- winning that says nothing about either being good. The bench
compares against N24. A model can beat its own recent ancestor while
both are far below where they started.

That is worth stating as a general caution, because I nearly read the
league record as a curve: SELF-PLAY LEAGUE STANDINGS MEASURE RELATIVE
PROGRESS WITHIN A RUN AND CANNOT SEE A RUN THAT IS DESCENDING TOGETHER.

The remaining six checkpoints are still running and will say whether the
decline continues monotonically from ck-01 or recovers.

## 148 — the curve: none of the three shapes

    point          rating   land %   surv  floor
    parent (N24)      221    57.47     89     36
    ck-01             110    24.17     73     12
    ck-02             162    35.47     72      0
    ck-03              59    10.23     40      0
    ck-04              86    12.00     53      0
    ck-05             106    21.77     70     41
    ck-06             106    17.10     50      1
    ck-07             193    44.17     88     31

Not monotonic, not a cliff, not up-then-down. It THRASHES: 24, 35, 10,
12, 22, 17, 44 percent of the world, a fourfold swing between snapshots
twelve minutes apart, with every point below the parent.

CHECKPOINT SELECTION IS NOT SUFFICIENT HERE, which was the question the
user asked me to answer. The best checkpoint is ck-07 at 44.17%, still
13 points of the world below where training started, and its immediate
neighbour twelve minutes earlier sits at 17.10%. Selecting a peak from
that series would be selecting noise: nothing suggests ck-07 is a better
POLICY rather than a luckier draw, and the only way to find out costs a
confirmation run on a second seed set.

THE THREE HYPOTHESES WERE ALL WRONG, INCLUDING MINE

I predicted monotonic decline. The peer's league record suggested
up-then-down. The truth is that a policy 354 million updates old, given
2.8 million more, does not decline or improve -- it becomes unstable,
and lands anywhere in a band between a fifth and three quarters of the
parent's territory depending on when you stop it.

That is a different failure from "training degrades the model", and it
points somewhere specific: at the step size. A policy that oscillates
fourfold between adjacent checkpoints is taking steps far too large for
where it is. Memory already carries OD_LR_SCALE=0.25 as the setting that
"held a strong parent" -- which is exactly the symptom this run has, and
I did not use it.

WHAT I WOULD RUN NEXT, IF ANYTHING

Same run at OD_LR_SCALE=0.25, same snapshots, same screening. If the
curve stops thrashing, the diagnosis is confirmed and checkpoint
selection becomes meaningful because there is a trend to select from. If
it thrashes anyway, the instability is not the step size and the reward
horizon is the next suspect.

That is one hypothesis, one knob, one measurable prediction -- which is
better than this run started with, and the run cost two hours to get it.

## 149 — the curve is straddled and is withdrawn

binary_mtime, added to the stored rows four hours ago, says the seven
checkpoint arms did not share a binary:

    curve-parent   23:02:47
    curve-ck01     23:02:47
    curve-ck02     23:33:46
    curve-ck03..07 23:46:08

Three builds across eight arms. So the "fourfold thrash" in entry 148 is
partly the binary moving under the measurement, and the entry is
withdrawn: 24, 35, 10, 12, 22, 17, 44 percent of the world is not a
policy oscillating, it is at best a policy oscillating PLUS two build
changes, and the two cannot be separated from these numbers.

I rebuilt at 23:02 and committed in writing to no further rebuilds until
the curve was done. Somebody else's builds at 23:33 and 23:46 landed
anyway -- which is not a complaint, the peer is entitled to build in
their own tree and I asked them to hold GAME LOGIC rather than to stop
compiling. My mistake was assuming that a promise I made to myself about
rebuilding controlled the binary a shared tree presents.

WHAT SAVED IT

The provenance field. I added binary_mtime this evening after four
baselines went stale the same way, and it is the only reason this is a
withdrawn entry rather than a finding. The numbers looked plausible --
they told a coherent story about instability, they agreed with a memory
about OD_LR_SCALE, and I had already written the follow-up experiment.

Nothing in the numbers themselves said anything was wrong.

WHAT NOW

Re-screen all seven checkpoints plus the parent on ONE binary, and this
time verify the mtime is constant across the finished rows rather than
promising myself I will not rebuild. The quarter-rate run the user asked
for waits until the full-rate curve is real, because comparing a
quarter-rate curve against a straddled full-rate one would inherit the
same defect one level up.

## 150 — a private build tree, at the user's instruction

The peer found the root cause and it is exact: tests/run_all.sh line 158
runs `cmake --build "$build" --target OpenDoctrinesServer`. They ran the
suite at ~23:32 and ~23:44, which produced my 23:33:46 and 23:46:08
stamps to the second.

So the protocol I asked for could never have worked. I asked them to
announce GAME LOGIC changes; they complied scrupulously; and the command
that contaminated my chain was them VERIFYING they had not made any. The
more careful they were, the more likely they were to run it. Neither of
us thought of a test run as a build.

The fix, at the user's instruction: build to build-loop/, which nothing
else writes to. od_bench already takes --binary explicitly, so it will
not go looking for the freshest tree -- which matters, because
binary_path() prefers exactly that and has silently won this race before
(see the memory about cmake-build-debug).

WHY A PATH BEATS A PROMISE

An announcement protocol requires someone to recognise that an action is
a build. A private path requires nothing of anybody. The peer put it
better than I did: a path only I write to cannot be forgotten by them on
a turn when they are concentrating on something else.

That generalises past this tree. Any coordination rule that depends on
correctly CLASSIFYING your own actions will fail on the action nobody
classified, and the failure will look like carelessness when it is
actually a taxonomy gap. Prefer a mechanism that does not require the
classification.

build/ is theirs from now on, permanently and without needing to tell
me. The unrun suite that was gating their finished feature can go.

## 151 — the quarter-rate run, matched to the full-rate one

Launched in parallel with the full-rate re-screen rather than after it,
since the two are independent: one produces checkpoints, the other
benches an existing set, and serialising them would cost six hours for
no gain.

Matched deliberately, one variable:

    parent          the same clean N24 (adb32ac8)
    maps/turns      8 x 1500, as before
    seed            20260908, as before
    snapshots       every 12 minutes, as before
    binary          build-loop, private
    CHANGED         OD_LR_SCALE=0.25

Snapshots go to ckpt-q/ so they cannot disturb the full-rate checkpoints
being benched right now out of ckpt/.

THE PREDICTION, stated before any snapshot exists

If the step size is the diagnosis, the quarter-rate curve should show a
TREND rather than a scatter -- monotone in some direction, with adjacent
checkpoints close to each other. The full-rate curve's defining feature
was adjacent points differing fourfold in land held, and that is what
"steps too large for where the policy is" looks like.

If the quarter-rate curve thrashes just as much, the step size is not
the cause and the reward horizon is the next suspect: N_STEP is 12 turns
and everything that matters here pays off over hundreds.

What I will NOT conclude either way: that a quarter-rate checkpoint
beating the parent means training works. That needs the same treatment
as any other candidate -- a second seed set at the horizon that decides
-- and this session has retired six results that skipped it.

Also worth writing down: the full-rate curve I am re-screening is not
yet a real measurement. Entry 148's numbers are withdrawn and the
replacement is still running. If the quarter-rate run finishes first I
will still not compare against the withdrawn numbers.

## 152 — the curve, re-measured and confirmed

Eight arms, one binary, verified from the stored rows rather than
promised: all eight report binary_mtime 00:59:03.

    point          rating   land %      (withdrawn run)
    parent (N24)      221    57.47      221 / 57.47
    ck-01             110    24.17      110 / 24.17
    ck-02             162    35.47      162 / 35.47
    ck-03              59    10.23       59 / 10.23
    ck-04              86    12.00       86 / 12.00
    ck-05             106    21.77      106 / 21.77
    ck-06             106    17.10      106 / 17.10
    ck-07             193    44.17      193 / 44.17

Every point identical. So entry 148's DATA was sound and only its
provenance was unverifiable: the peer's test-suite rebuilds moved the
binary without changing its behaviour, because the only source change in
that window was the doctrine-duplication fix and benches never load a
save.

THE WITHDRAWAL WAS STILL RIGHT

That the numbers survived is something I could only learn by re-running.
Before the re-run I had a curve measured across three binaries and no
way to know whether the differences between checkpoints were policy or
build. Keeping it would have been correct by luck. Six hours of bench to
confirm a result I already had is the price of not knowing which kind of
result it was, and the alternative -- assuming a straddle is harmless --
is the assumption that makes every future straddle invisible.

CONFIRMED: THE THRASH IS REAL

Fourfold swings in land held between checkpoints twelve minutes apart,
every point below the parent, no trend. Checkpoint selection is not
sufficient: the best point is ck-07 at 44.17%, thirteen points of the
world below where training started, and its neighbour sits at 17.10%.

The quarter-rate run is at eight snapshots and still going -- longer
than the full-rate run's seven, which is itself a small signal: smaller
steps mean the policy diverges more slowly from the parent's behaviour,
so maps take longer to resolve. Its curve is the test of whether the
step size is the cause.

## 153 — the quarter-rate run, and its league record

Finished: 10 snapshots, 10 distinct hashes, data/ai/model.bin untouched.
The run went longer than the full-rate one (10 snapshots against 7),
which is itself consistent with smaller steps -- the policy diverges
more slowly from the parent's behaviour, so maps take longer to resolve.

Its league record differs sharply from the full-rate run's:

    check   full-rate (frozen / trainee)   quarter-rate
      1            1.0 /  2.3                1.0 /  2.3
      2            4.5 /  9.0                3.0 / 24.3
      3            6.8 /  1.0   first loss   4.0 / 10.3
      4           25.6 / 11.2                6.7 / 16.9
      5           42.3 / 32.0                7.8 / 71.5
      6                                      4.3 /  1.2   first loss

Five wins before the first loss against two, and by much wider margins.

That is consistent with the step-size hypothesis and it is not evidence
for it. The league opponent is the trainee's own frozen ancestor, so the
record measures relative progress inside a run and cannot see a run that
is descending as a whole -- which is exactly the trap I nearly fell into
with the full-rate record at entry 146. A run can win six in a row while
every participant sits below where it started.

SCREENING: qk-01 through qk-05 consecutively, plus qk-10.

Consecutive on purpose. The full-rate curve's defining feature was
ADJACENT points differing fourfold, and sampling every other checkpoint
would destroy the adjacency that defines thrashing. Five neighbours plus
an endpoint answers the shape question; ten arms at 400 turns would cost
eight hours to answer it more precisely than it needs answering.

The single most informative comparison is qk-01 against ck-01: same
parent, same twelve minutes, quarter the step. Full-rate lost more than
half the parent's land in that window, 57.47% to 24.17%.

## 154 — quarter rate: the hypothesis is half right and my prediction was wrong

    point     full rate            quarter rate
              rating   land%       rating   land%
    parent      221    57.47         221    57.47
    1           110    24.17         208    46.33
    2           162    35.47         205    37.60
    3            59    10.23         234    65.57
    4            86    12.00          73    10.23
    5           106    21.77          90    16.23
    end         193    44.17         130    16.37

I predicted in entry 151 that if step size were the cause, the
quarter-rate curve would show a TREND rather than a scatter. It does
not. It scatters just as violently -- 46, 38, 66, 10, 16 percent of the
world -- and by the end it is WORSE than full rate.

What quarter rate actually buys is a DELAY. The first three checkpoints
hold near the parent, one of them (qk-03) exceeds it on both rating and
land, and then it falls off exactly as the full-rate run did. Smaller
steps postpone the instability by roughly half an hour; they do not
prevent it.

That is a real finding and it retires the fix I proposed to the user.
The damage is not a step-size artefact that a gentler optimiser avoids.
It is something the run REACHES regardless, and the learning rate only
changes when.

ON qk-03, WHICH BEATS THE PARENT

234 / 65.57% against 221 / 57.47%. The first trained model this session
to exceed N24 on both columns, and it is not selectable on this
evidence: its immediate neighbour twelve minutes later sits at 10.23%.
A point that good next to a point that bad is a draw from a distribution
with enormous variance, not a policy worth shipping, and one seed set
cannot distinguish those. Six results today died on exactly that
distinction.

If anything deserves the confirmation run it is qk-03, and the honest
framing is that I would be testing whether one lucky sample repeats --
prior low, cost one hour, and the alternative is leaving the only
parent-beating checkpoint of the session unexamined.

WHAT THIS LEAVES

Checkpoint selection is not sufficient at either rate. The reward
horizon (N_STEP = 12 turns, against a 400-turn judgement) is now the
standing suspect and is untested.

## 155 — qk-03 was a lucky sample

    set   parent            qk-03             verdict
    C     221 / 57.47%      234 / 65.57%      qk-03 by 8 points of land
    D     284 / 68.60%      229 / 45.10%      parent by 23

The only checkpoint in two training runs to beat its parent does not
beat it on a second world set. Its neighbour twelve minutes later sat at
10.23%, which was the warning, and the confirmation cost one hour.

Seventh result this session to die on a second seed set. The others: the
+75 threat ranking, the aggression dial, N45 as a better model, the
+10.7% concurrent-wars land gain, the campaigns floor trade, the
district-law reading. Every one looked solid on the set it was found on.

TRAINING, CLOSED FOR NOW

Two runs, seventeen checkpoints, both learning rates, checkpoint
selection at both. Nothing beats N24 on more than one seed set. The
findings that survive:

  * training from a strong parent destabilises rather than degrades --
    the curve scatters, it does not decline
  * quarter rate delays the instability about half an hour and ends
    worse than full rate; the step size changes WHEN, not WHETHER
  * checkpoint selection is insufficient at either rate, because the
    good points sit adjacent to collapses and one seed set cannot tell a
    good policy from a lucky draw
  * the league record cannot see a run descending as a whole, since its
    opponent is the run's own frozen ancestor

WHAT REMAINS UNTESTED, and it is now the only live hypothesis:

N_STEP = 12. Every decision is credited by the state change over the
next twelve turns, and every conclusion that has survived this session
was measured at 400. Campaigns are worth +73 at 400 turns and far less
at 120; the siege research cut +113. The reward cannot see the horizon
on which the things that matter here pay off.

That is a bigger change than a learning rate -- it alters what the
policy is being asked to maximise, and static_assert ties N_STEP to
AI_PLAN_HORIZON, so the economy's planning window moves with it. Worth
doing, worth doing deliberately, and worth the user's say-so rather than
mine.

## 156 — the play half of the horizon, separated from the training half

AI_PLAN_HORIZON = 12 does two jobs: it is what projectIncome is asked
for at PLAY time (the doctrine budget, a warship's berth) and, through
the static_assert, it is N_STEP, the credit window a TRAINING decision
is scored over.

The standing suspicion after two failed training runs is that 12 is too
short for a game whose good moves pay off over hundreds of turns.
Testing the training half costs a run and the user's say-so. Testing the
play half costs two bench arms, and OD_PLAN_HORIZON separates them --
it changes only the projectIncome argument, leaving N_STEP and the
assert untouched.

Two-sided on purpose, and measured in both directions (36 and 6). A
longer window projects more income and therefore permits more spending,
and this AI's bankruptcy history is precisely that failure: hulls bought
against income that had not arrived, 144 turns of a thousand spent
bankrupt. So "plan further ahead" is not obviously good here.

Both of the peer's cautions applied: the control is re-run on the same
binary rather than compared against a stored number, and the horizon is
400 rather than 120 -- an economy's planning window is exactly the kind
of thing that should look worse short and better long, which is the
pattern that has reversed six results today.

Their own break is worth recording beside this: they built the target
they had touched, saw it green, and did not build the target that
CONSUMES it. Same shape as run_all rebuilding my binary -- verifying the
thing you changed rather than the thing your change reaches. Two
instances in one day, in opposite directions, and neither of us saw it
coming in our own work.

## 157 — the plan horizon cannot do anything, and the knob proved it

Three values, identical on every seat:

    OD_PLAN_HORIZON=6    221 / 57.47%
    default 12           221 / 57.47%
    OD_PLAN_HORIZON=36   221 / 57.47%

And OD_ACT_HIST says the mask offers a1 exactly 4,625 times either way,
so the knob does not even change what the economy is allowed to
consider.

WHY. projectIncome's `turns` argument does one job:

    for (const PendingUpgrade& pu : m_pendingUpgrades)
        if (pu.turnsRemaining > turns || !ownedByUs(...)) continue;
    for (const PendingShipBuild& sb : m_pendingShipBuilds)
        if (sb.turnsRemaining > turns || ...) continue;

It filters the PENDING queues by completion time. Industry upgrades take
3 turns and ship builds take 3. So any horizon of 3 or more admits every
queued item, and 6, 12 and 36 are identical by construction. The knob is
correct and inert for a reason that has nothing to do with the knob.

SO "THE ECONOMY PLANS TWELVE TURNS AHEAD" IS FALSE. It plans as far as
its build queue reaches, which is three. AI_PLAN_HORIZON = 12 has been
doing nothing at the play call sites for as long as both numbers have
had their current values, and the static_assert tying it to N_STEP --
"the economy plans exactly as far ahead as it is scored" -- asserts a
correspondence between a training window that matters and a play
constant that does not.

TWO CORRECTIONS TO MYSELF IN ONE EXPERIMENT

First: I said a1 (enact doctrine) is never taken. That was the Norway
seat. On France it is taken 4,423 times, 95.6% of the times it is
offered. A dead action on one seat is not a dead action, and I have been
quoting a single seat's histogram as if it described the AI.

Second: I guessed the knob was reaching dead code. It was reaching live
code that cannot respond. Both guesses were wrong in the same direction
-- assuming the failure was where I had last seen a failure.

WHAT IT MEANS FOR THE REWARD-HORIZON HYPOTHESIS

Nothing, and that is worth being clear about. This tested the PLAY half
and found the play half is a no-op. The training half -- N_STEP = 12
turns of credit for decisions judged at 400 -- is untouched by this
result and remains the standing hypothesis. If anything the finding
sharpens it: the constant is load-bearing in exactly one of its two
jobs.

## 158 — a byte-identical inertness proof, and what it does not cover

The peer added a bias term to decideDiplomacy for their LLM advisor and
asked for the proof rather than the argument. Module off, 400 turns,
three hold-out seeds, one binary each side, model N24:

    seat              before      after
    1914:FRA:rung    18.1667    18.1667
    1914:FRA:rush     7.8333     7.8333
    1914:SWE:rung     2.6667     2.6667
    1939:NOR:hood     0.4667     0.4667
    1939:USA:rung    22.6000    22.6000
    modern:CHN:rung   5.7333     5.7333

Identical to four decimals on every seat. In a floating-point simulation
over 400 turns, one changed decision anywhere would have cascaded, so
this is a strong result rather than a weak one.

WHAT IT DOES NOT COVER, which they insisted on before I ran it: three
other files moved between those builds, so the proof is that the DIFF is
inert, not that the block is. That is the property my baselines need and
the one that matters for shipping, and it is not the narrower claim.

Worth recording that this is the ONLY byte-identical inertness result
either of us produced today. Everything else we called inert was
inferred from reading the code -- my knobs, their gates -- and one of
those inferences was wrong within the hour (the plan horizon was inert
for a reason I had not guessed). Byte-identical seats cost one bench run
and settle the question completely; an argument costs nothing and
settles it not at all.

The general form, since three of today's failures share it:

    "adds zero when off" is a claim about the code
    "byte-identical seats" is a claim about the behaviour
    only the second is a proof, and it is cheap

## 159 — every histogram claim I made came from one seat

Found by accident while debugging the plan horizon: econ a1, "enact a
doctrine", is taken 0.0% of the time on 1939:NOR and 95.6% of the time
on 1914:FRA. Same model, same binary, same turn count.

Every OD_ACT_HIST conclusion in this journal was measured on ONE seat --
mostly Norway, occasionally France -- and reported as a fact about the
policy. That includes:

  * "the econ head is collapsed, 8 of 12 actions at 0.0%"
  * "86% of economic decisions are the research slider"
  * "ports offered 1,914 times and taken zero, ships 6,020 and zero"
  * the whole relational-slice ablation reading, which used one seat's
    numbers to decide which features do anything

Some of those may survive. The naval one probably does -- a landlocked
seat and a coastal one both showed zero ship purchases, and the peer's
independent count agreed. The econ-head one probably does not, since a1
alone moves from dead to dominant between two seats.

The error is the same one the floor magnifier taught and I did not
generalise: an aggregate over six seats hides which seat produced it,
and a measurement on ONE seat is an aggregate with five terms missing.
I spent a day insisting on three seed sets for RATINGS and never once
asked whether a histogram needed more than one seat.

Running all six now. Cheap -- six 120-turn single-seat runs -- and it
either confirms the claims or retires them.

Worth noting what makes this different from the other corrections today:
those were results that failed to replicate. This is a body of claims
that were never tested for replication at all, because the instrument
felt descriptive rather than statistical. A histogram looks like a fact
about the code. It is a fact about a game.

Result -- five seats, and the claims mostly SURVIVE:

    action          FRA   SWE   USA   CHN   NOR
    rsrch up       51.3  48.3  46.8  54.2  52.5
    rsrch down     21.1  25.1  22.5  19.1  20.2
    nothing         9.1   9.3  12.4  14.1  15.6
    doctrine       14.3  13.4  13.6   7.9   5.5
    branch B        4.2   3.8   4.7   4.7   3.5
    fortify         0.0   0.0   0.0   0.0   2.0
    port            0.0   0.0   0.0   0.0   0.0
    specialise      0.0   0.0   0.0   0.0   0.0
    ship D / ship C 0.0   0.0   0.0   0.0   0.0

CONFIRMED on five independent seats: the research slider is about 72% of
every seat's economic decisions, and ports, specialisation and both ship
types are bought NOWHERE. Those were the load-bearing claims and they
are stronger now than when they rested on one seat.

RETIRED: "8 of 12 actions at 0.0%" as a general statement. Four are zero
everywhere (port, specialise, two ships), one is zero on four seats and
2.0% on Norway (fortify), and doctrine ranges 5.5 to 14.3.

AND MY CORRECTION WAS ITSELF WRONG. I told the peer and the journal that
a1 is "0.0% on Norway and 95.6% on France". The 95.6% was
TAKEN-WHEN-OFFERED, a different column from share-of-picks, and I
compared it against a share. By share, doctrine is 5.5% on Norway and
14.3% on France -- a real difference, a third as much rather than the
infinite ratio I claimed, and never zero.

So the single-seat error was real and my statement of it was not. Two
columns in one table, and I read across them while writing a correction
about reading carelessly.

WHAT SURVIVES OF THE ORIGINAL WORRY

The concern was right in general and small in this instance: seats do
differ (doctrine varies 2.6x, nothing varies 1.7x, fortify is
Norway-only), but no conclusion I built on the histogram depended on the
part that varies. The naval and port findings are uniform across every
seat measured, and those are the ones I acted on.

## 160 — re-testing the naval reflex on the corrected instrument

The finding it addresses is now the strongest in the journal: ports,
specialisation and both ship types are bought on ZERO of five seats,
confirmed independently by the peer's count. The AI operates a fleet it
can never replace.

The fix was rejected on the instrument this session spent a day
discrediting: 120 turns, fitted seeds, read on rating and floor. N24
224 -> 184 and N37 206 -> 213. Re-reading three hold-out rejections in
land overturned one of them temporarily, so a rejection taken on the
worse instrument deserves the same re-look.

400 turns, hold-out set C, read in land, control is the standing
221 / 57.47% on this binary.

Stated before the numbers: I do not expect it to reverse. The naval
reflex bought 24 ports and 62 destroyers in a Norway game and the seat
still lost, and the berth costs that bankrupted this AI historically are
a per-turn drain that a longer horizon should punish MORE, not less. If
anything 400 turns should look worse than 120.

The reason to run it anyway is that the same reasoning would have
justified not re-testing concurrent wars, which turned out to be +10.7%
of the world in land before a second set retired it. My predictions
about which rejections deserve a second look have been wrong in both
directions today, and the run costs fifty minutes.

Result -- confirmed, and three times worse at the long horizon:

    control          221 / 57.47%
    naval reflex     106 / 16.83%   -40.6% of the world

At 120 turns it read -40 rating on one model and +7 on another. At 400
turns it costs forty points of the map. The berth upkeep is a per-turn
drain, so a horizon three times longer punishes it roughly three times
harder -- which is what I predicted before the run and the first
prediction I have stated in advance and had confirmed all session.

THE NAVAL QUESTION IS CLOSED PROPERLY NOW

Ports and ships are bought on zero of five seats. That is the strongest
descriptive finding in this journal. Forcing the purchases costs 40% of
the world. So the policy's refusal is CORRECT PLAY rather than a
collapsed head: the capability is not worth having at the price the game
charges for it, and the head has learned that better than I reasoned it.

Which also settles the standing recommendation that the econ head's dead
actions justify a retrain. They do not -- at least not for the naval
ones. A retrain that unlocked ship purchases would be buying a
capability measured at minus forty points of the map.

WHAT THE RE-TEST WAS WORTH

Nothing changed, and it was still right to run. The identical reasoning
-- "I know why this failed, a re-test cannot reverse it" -- would have
justified skipping the concurrent-wars re-read, which showed +10.7% of
the world in land before a second seed set retired it. Two re-tests, one
apparent reversal that died, one confirmation. The cost of the policy is
fifty minutes per rejection; the cost of not having it is that a
rejection taken on a bad instrument stays in the record as a fact.


## 161 -- auditing the rules that are already in the game

Checking one claim before I let it into memory turned up the biggest gap
in this whole session's method.

I had just written "a head's refusal is a priced decision" off the naval
result. Before storing it I checked the obvious counterexample, and it is
sitting in the build: `fort` is also at 0.0 probability, and
`fortifyReflex` (AISystem.cpp:6685) forces it anyway, unconditionally,
every turn a country is at war, for a recorded +4.7. So the rule is
false as stated, and I softened it.

But the check found something worth more than the correction.
`fortifyReflex` runs on every country every turn, and its ONLY evidence
is +4.7 measured against a rusher -- an old number, from the instrument
this session spent thirty hours discrediting: fitted seeds, 120 turns,
held/par floor. Nothing has re-checked it.

And it is not alone. Eleven reflexes run unconditionally every turn:

    garrison  fortify  redeploy  austerity  manpower  siege
    campaign  peace  pacification  withdraw  callToArms

THESE ARE NOT CANDIDATES. THEY ARE THE AI.

Every rule I have measured for two days was something I proposed ADDING.
Twenty-three candidates, twenty-three rejections, one climb. I never once
asked whether what is already there still earns its place -- even though
the journal has the precedent written into the source: OD_WIDTH_MARGIN,
worth "20-50 on the OLD resolver", now OFF by default because a resolver
fix silently made it a 15-point liability.

That is exactly the failure mode I have in memory as "heuristics outlive
the bug", and I applied it to knobs I was adding rather than to the
twelve rules doing the actual playing. The candidate rejection rate is
22 of 23. There is no reason to think the shipped set is cleaner --
it was selected by the SAME instrument, and selected harder, because a
rule only ships if it measured well.

Built `reflexAblated()`: OD_ABLATE="fortify,peace" skips named reflexes
for one arm. Unset it returns false on the first branch, so the play path
is unchanged -- and per the standing rule about proving inertness rather
than trusting a baseline, the first arm run will be a control that has to
reproduce 221 / 57.47% exactly on the new binary before any ablation
number is believed.

Not building yet: nine bench processes are holding
build-loop/OpenDoctrinesServer for the supply-margin run. Rebuilding
under them is the exact straddle that corrupted the checkpoint curve.

## 162 -- the parked positive dies, 23 for 23

Supply margin, set D, 400 turns, both arms fresh on the same binary:

    control        284 / 68.60%
    supply margin  202 / 54.93%     -82 rating, -13.7 points of map

On set C it read -3 rating / +7 survival / +22 floor, which is why I
parked it rather than retiring it. Retired now. Eighth result to die on a
second seed set.

The control arm reproduced 284 / 68.60% EXACTLY against the run on the
pre-LLM-fix binary. That is an independent behavioural confirmation of
the byte-identical inertness proof for the peer's diff -- two different
methods, same conclusion -- and the first clean replication of a number
in a session where the tree moved under a measurement five times in a day.

RUNNING TALLY: 23 candidates measured, 23 rejected. Not 22 of 23. All.

Every rule I proposed adding has failed. That is worth stating plainly
because it is the strongest available evidence for the audit in entry
161: an instrument that generated twenty-three plausible positives, none
of which survived contact with a second seed set or a longer horizon, is
the SAME instrument that approved the eleven reflexes now playing the
game. They were not held to a higher standard. They were held to a lower
one -- they shipped when a single measurement looked good.

The asymmetry has been in my method the whole time. A candidate has to
survive two seed sets and 400 turns to get in. A shipped rule has to
survive nothing at all to stay in.

## 163 -- I nearly blamed the peer for my own missing export

The ablation control came back 236 / 55.87% against a standing control of
221 / 57.47%. Fifteen points, above the resolution limit, so a real move.

I went looking for what changed in the tree, found the peer had committed
8a3f7a4 one minute after my previous binary was built, saw it touched
src/ai/AISystem.cpp (+22) and AISystem.h (+28), and had most of a message
drafted telling them their "inert" LLM disposition term had moved my AI
bench by fifteen points.

It had not. Their commit is inert exactly as they claim, and their
CMakeLists hunk is an EXCLUDE_FROM_ALL test target that cannot reach the
server binary. The provenance in the store:

    naval-400    (the 221 control)   seeds 909091, 20230115, 42424242   set C
    abl-control  (the 236)           seeds 20260801, 4242, 90210        FITTED

My chain exported OD_BENCH_TURNS=400 and did NOT export OD_BENCH_SEEDS, so
it fell back to the fitted defaults. I compared two different world sets
and read the difference as a code change.

Three things worth keeping:

1. THE PROVENANCE FIELDS PAID FOR THEMSELVES. I added binary_mtime, seeds
   and turns to the store this session after the build straddle. They are
   the only reason this took ten minutes instead of becoming a false
   accusation and a wasted afternoon of bisecting a peer's inert commit.

2. I checked the tree before I checked my own command. The peer's commit
   was the interesting explanation and my missing export was the boring
   one, and I went to the interesting one first. The boring one was right.
   Same shape as the naval story: the exciting reading ("collapsed head")
   lost to the dull one ("correctly priced").

3. A control is not a control because I called it one. It is a control
   only if its seeds, turns, binary and model match the arm. Compare the
   stored provenance, never the remembered label.

The four running arms are all on fitted seeds with a matching fitted
control, so they are internally valid -- and fitted seeds are the HOME
GROUND of the shipped reflexes, since that is where they were selected.
A shipped rule that fails on the very seeds it was fitted to is damned
without needing a hold-out. Anything that survives needs set C after.

## 164 -- four of the eleven were never running

Entry 161 said: "Eleven reflexes run unconditionally every turn. THESE
ARE NOT CANDIDATES. THEY ARE THE AI." That is wrong, and the bench caught
it in the cheapest possible way.

    abl-control    236 / 55.87%
    abl-withdraw   236 / 55.87%     identical to the decimal

Six seats, three seeds, four hundred turns, and removing withdrawReflex
changes nothing whatsoever. The reason is four lines into its body:

    static const bool on = std::getenv("OD_WITHDRAW_REFLEX") && ...
    if (!on) return;

OFF by default. The CALL is unconditional; the BODY gates itself. I read
the call site, saw eleven names with no `if` in front of them, and
concluded eleven rules were playing the game. Checking each body:

    ACTIVE (7):  garrison  redeploy  fortify  austerity  campaign
                 siege  manpower
    OFF     (4):  callToArms  withdraw  peace  pacification

And my own memory records WHY withdraw is off -- "two withdraw rules both
cost" -- with the numbers sitting in a comment directly above the gate I
did not read. I had the answer in two places and looked at neither,
because the call site LOOKED like it settled the question.

The pacification arm queued behind withdraw would have burned another
fifty minutes proving the same thing about another disabled rule. Killed
it and requeued the seven that are actually live.

WHAT THE IDENTICAL NUMBER IS WORTH

It also proves reflexAblated() is inert, which is what the control arm was
supposed to establish and could not, since its own provenance was wrong
(entry 163). An ablation that removes a disabled rule and reproduces the
control exactly is a per-call inertness proof of the switch itself. The
wasted arm paid for the thing the wasted control failed to buy.

Standing correction: the audit is of SEVEN rules, not eleven, and the
control for all of them is 236 / 55.87% on the fitted seeds.

## 165 -- the first shipped rule on trial passes, and the instrument proves itself

    control        236 / 55.87%
    -fortify       200 / 49.23%     removing it costs 36 rating, 6.6 of map

fortifyReflex earns its place, and by far more than the +4.7 on record --
that number came from a different measure (ADVANTAGE against a rusher) on
the old instrument. On the seat bench at 400 turns it is worth 36.

The pair of arms is also a two-sided proof of the ablation switch, which
is worth more to me than the fortify number:

    -withdraw (DISABLED rule)   236 / 55.87%   identical -> switch is inert
    -fortify  (LIVE rule)       200 / 49.23%   -36        -> switch bites

Inert when it should be, effective when it should be. Nothing else I have
built in two days has both halves. Every other instrument in this journal
was validated in one direction only, which is exactly how the fitted
seeds, the 120-turn horizon and the held/par floor all got through.

THE ASYMMETRY TO RESPECT

These are the fitted seeds -- the home ground of every rule under test,
since this is where they were selected. That makes the two outcomes mean
different things:

    fails here  ->  damning. It cannot even hold the ground it was fitted
                    to. Retire without a hold-out.
    passes here ->  inconclusive. It works where it was fitted, which is
                    the weakest possible claim. Needs set C.

So fortify is not yet "confirmed good", it is "not yet retired". Six arms
to go; anything that survives fitted seeds gets a hold-out run after.

## 166 -- the audit finds a shipped rule costing 15 points of the world

    control        236 / 55.87%
    -austerity     233 / 70.90%     rating -3, LAND +15.03

Rating says nothing happened. Land says removing a shipped rule gains
fifteen points of the world -- the largest positive movement in two days.
Per seat:

    SWE rung   9.80 -> 19.70   +9.90    nearly DOUBLES without it
    USA rung  10.77 -> 16.30   +5.53
    FRA rung  22.70 -> 27.83   +5.13
    CHN mod    6.70 ->  2.50   -4.20    the bankrupt seat needs it
    FRA rush   5.43 ->  4.17   -1.27
    NOR hood   0.47 ->  0.40   -0.07

Three seats up big, three down, net +15. This is `caution rules trade
growth` exactly: austerity protects the failing country and charges the
healthy ones for it. Note the two aggregates disagree in SIGN-ish ways
again, and the per-seat column is the only one that explains anything.

THE MECHANISM, AND WHY THE COMMENT IS WRONG

The cut order is research, pacification, doctrine, minorities, navy.
Research is cut FIRST, and the source says why:

    "They come down first because they come back up for free the moment
     income recovers."

That assumes the deficit is EPISODIC. It is not. AI treasuries run at
zero -- that is in my memory as a standing fact about this game -- so the
reflex fires turn after turn and the slider ratchets down 0.15 each time
with nothing pushing it back up. It is not a dip, it is a one-way
ratchet, and it is being applied to the compounding growth engine: 86% of
economy decisions are the research slider, and army research is what wins
battles.

So the rule cuts the thing that comes back SLOWEST first, on the belief
that it comes back fastest.

BUILT: OD_AUSTERITY_RESEARCH_LAST moves research from first cut to last,
after the navy. Keeps the solvency floor that CHN needs (the four other
cuts still fire in order) and spares the growth engine on seats that are
merely having a bad decade. Default OFF, so the running audit is
undisturbed. Cannot build until the chain releases the binary.

Prediction, stated before the run: this recovers most of SWE/USA/FRA's
gain and keeps CHN above the -4.2. If it instead lands halfway on both, the
honest read is that the solvency floor and the growth tax are one
decision and cannot be separated -- which is what the ceiling/floor entry
found for campaigns.

## 167 -- manpower looks like a second find and is not

    -manpower   255 / 61.93%    rating +19, land +6.07

Both aggregates up, which I said out loud made it stronger than the
austerity result. Wrong, and the per-seat column says so immediately:

    USA rung  10.77 -> 28.97   +18.20    <- the entire result
    FRA rung  22.70 -> 15.67    -7.03
    CHN mod    6.70 ->  0.43    -6.27    <- nearly dies
    SWE rung   9.80 ->  5.93    -3.87
    FRA rush   5.43 ->  9.10    +3.67
    NOR hood   0.47 ->  1.83    +1.37

One seat gains 18.2 and carries a net of +6.07 while three seats get
materially worse and the bankrupt seat falls to nothing. Compare the
shape of the austerity result: +5.13, +9.90, +5.53 across three
different seats, with one explained loss. That is a rule with a
consistent effect. This is an outlier with company.

TWO AGGREGATES AGREEING IS NOT CORROBORATION

I have written down twice that the aggregates are magnifier-prone and the
per-seat column is what explains things, and I still read "rating and land
both up" as two witnesses. They are not two witnesses. They are two
summaries OF THE SAME SIX NUMBERS, and when one seat moves 18 points both
summaries move with it. Independent confirmation means a different world
set, not a second way of adding up the same one.

manpowerReflex: NOT RETIRED. The +19/+6 is one seat.
austerityReflex: the finding stands, on shape as well as size.

## 168 -- garrison is the load-bearing one

    control      236 / 55.87%
    -garrison    156 / 38.77%    removing it costs 80 rating, 17.1 of map

The largest effect of any rule in the audit, and in the direction that
keeps it. "Holding a threatened border is not a choice the policy should
be gambling on once every eight turns" turns out to be worth seventeen
points of the world.

Running tally of the seven live reflexes:

    garrison    -80 / -17.1 to remove   KEEPS ITS PLACE, decisively
    fortify     -36 /  -6.6 to remove   KEEPS ITS PLACE
    austerity    -3 / +15.0 to remove   COSTS 15 points of map (entry 166)
    manpower    +19 /  +6.1 to remove   one-seat artefact, not retired (167)
    redeploy, campaign, siege           still running

Worth stating: the audit is not turning up a graveyard. Two of the four
resolved so far are strongly load-bearing, one is an artefact, and one is
a real liability. That is roughly what an honest re-examination should
look like, and it is a much better hit rate than my twenty-three
candidate rules, none of which survived.

## 169 -- two seats own the land column

    -redeploy   254 / 63.50%   rating +18, land +7.63
    per seat:   USA +12.57 = 165% of the net; SWE -6.70, FRA rush -5.10

Same artefact shape as manpower. Not retired. But five arms against one
control is enough to ask how much each seat actually moves, and the answer
changes how I read every land number in this journal:

    seat        control     min     max    range
    USA:rung      10.77    0.50   28.97    28.47   <-
    SWE:rung       9.80    3.10   19.70    16.60   <-
    FRA:rung      22.70   15.67   28.40    12.73
    FRA:rush       5.43    0.33    9.10     8.77
    CHN:rung       6.70    0.10    8.07     7.97
    NOR:hood       0.47    0.40    1.83     1.43

USA alone spans 28 points against a whole-board total of about 56. It is
sitting on a tipping point -- it either runs away or collapses -- and it
takes the six-seat land sum with it whichever way it falls.

THE RULE THIS GIVES ME

A net land change is worth reading only if BOTH hold:
  1. no single seat contributes more than about half of it, and
  2. it replicates on a different world set.

Applying it to the audit:
    austerity  +15.03  USA contributes 5.53, three seats up   PASSES (1)
    redeploy    +7.63  USA contributes 12.57 = 165%           FAILS  (1)
    manpower    +6.07  USA contributes 18.20 = 300%           FAILS  (1)

So the shape criterion I used by instinct on manpower now has a number
behind it, and it independently clears austerity, which is the only
candidate I am still carrying. It still owes me (2).

Note what this does NOT say. It does not say USA is a bad seat or should
be dropped -- a tipping-point seat is exactly where a rule change shows
up. It says the SUM is the wrong summary when one term has that much
variance, and I have been quoting the sum all session.

## 170 -- campaigns keep the pressured seats alive

    -campaign   172 / 41.27%   removing it costs 64 rating, 14.6 of map

    CHN mod    6.70 ->  0.03   -6.67   annihilated
    FRA rush   5.43 ->  0.17   -5.27   annihilated
    USA rung  10.77 ->  7.77   -3.00
    SWE rung   9.80 ->  9.03   -0.77
    NOR hood   0.47 ->  0.43   -0.03
    FRA rung  22.70 -> 23.83   +1.13
    5 of 6 seats hurt

The cleanest result in the audit and the one with the widest base. It
also passes in the direction that matters: the two seats under real
pressure -- bankrupt China and France against a rusher -- go to
essentially nothing without it. Commitment is what keeps a country alive
while it is being attacked, which is the same conclusion the foresight
work reached from the opposite end (search, longer horizons and exact
projection all lost to committing).

Six of seven resolved:

    garrison    -80 / -17.1 to remove    KEEP, 3 seats collapse without it
    campaign    -64 / -14.6 to remove    KEEP, 5 of 6 seats hurt
    fortify     -36 /  -6.6 to remove    KEEP
    austerity    -3 / +15.0 to remove    LIABILITY, passes the shape test
    manpower    +19 /  +6.1 to remove    artefact, USA is 300% of the net
    redeploy    +18 /  +7.6 to remove    artefact, USA is 165% of the net
    siege                                still running

Three of the shipped rules are strongly load-bearing, two apparent wins
are one volatile seat, and one is a genuine liability with a mechanism
and a fix already written. That is a far better return than 23 invented
candidates, all rejected -- and it took seven arms rather than two days.

## 171 -- a note for whoever commits my 3,309 lines

The peer hit three extraction failures in sequence today, each invisible
to the one before: a missing include (failed at compile), a missing
method (failed at compile, but only once the include was satisfied), and
a missing definition for --probe-trade (failed at LINK, only once both
were). Each needed a full build to reach the next.

The third one was my code. Their earlier 182-file commit had swept in my
edits to src/server/ServerMain.cpp and src/Game.h -- files they thought
of as theirs -- while correctly excluding Game_AITrain.cpp, which
DEFINES runTradeProbe. So HEAD declared and called a function nothing
implemented. They fixed it by removing the branch from the committed
blobs rather than committing my file, which is the right call.

Verified both trees are now independently valid:
    my working copy   CLI 2, declaration 1, definition 1   compiles+links
    HEAD              0 references anywhere                compiles+links
    data/ai/model.bin OK against its protected hash

WHY THIS IS WRITTEN DOWN HERE

My pending work spans nine files. If any of it is ever committed in
pieces, this is exactly the failure waiting for it: an exclusion has to
be closed under dependency, and dependency spans the preprocessor, the
type system and the linker, so no single check catches all three. Only
building the artefact does -- not the working copy, which in a shared
worktree is a different thing.

Standing instruction to myself: nothing of mine gets committed without
the user's word, and when that comes, it goes in as one build-verified
unit, from a clean checkout, not as hunks.

## 172 -- the audit's biggest number, and both findings share one cause

    control    236 / 55.87%
    -siege     270 / 82.07%    removing it GAINS 26.20 points of the world

    FRA rung  22.70 -> 34.90  +12.20
    USA rung  10.77 -> 19.63   +8.87
    SWE rung   9.80 -> 15.63   +5.83
    FRA rush   5.43 ->  9.00   +3.57
    NOR hood   0.47 ->  0.57   +0.10
    CHN mod    6.70 ->  2.33   -4.37
    5 of 6 seats improved; largest single seat 47% of the net

It passes every test I set for myself: five seats of six, no seat above
half the net, and the one loser is the bankrupt seat that austerity also
protects. This is the largest movement in the audit.

THE TWO FINDINGS ARE ONE FINDING

siegeReflex also cuts research -- 0.15 a turn, same step as austerity.
And its own comment states the mechanism as established fact:

    "and research funding does not go up: the treasury runs at zero
     every turn"

So the ratchet I inferred for austerity in entry 166 is not an inference.
It is written into the source of the OTHER rule that does the same thing,
by whoever built it, as a known property of the game. Two independent
rules, both cutting the research slider, together costing about 41 points
of the world, in a game the source itself says never lets the slider
recover.

There is more. The siege comment records its own per-rung measurements:

    "+35 hard, +8 easy, -32 on NORMAL, where it trades research for forts
     against a threat that is not [real]"

Minus thirty-two on NORMAL, known, recorded, and shipped per-rung anyway.

WHAT THIS CHANGES ABOUT THE FIX

Not "remove these two rules". Both do other things that are load-bearing
-- siege builds forts, austerity keeps a bankrupt country solvent, and
CHN drops on BOTH ablations. The suspect is the research cut they share.

OD_SIEGE_RESEARCH=0 already exists (siege keeps forts, drops the research
step). I wrote OD_AUSTERITY_RESEARCH_LAST for the other. Four arms
queued: control for inertness on the new binary, each rule's research cut
removed alone, and both together.

Prediction, before the numbers: if the ratchet is the cause, "both"
recovers most of the 41 points while CHN holds up better than it does
under either full ablation, because the solvency and fort machinery stays.
If instead each arm only recovers a little, the cuts are doing real work
and the growth cost is the price of the floor -- the ceiling-and-floor
outcome again.

## 173 -- what the siege number does and does not cover

Two precisions on entry 172, both of which narrow the claim.

FIRST: siege has three entry points, and I ablated one.

    underSiege(cid)     predicate, gated
    siegeEarmark(cid)   reserves money for forts, gated
    siegeReflex(cid)    the reflex -- the only one my OD_ABLATE guard skips

So the earmark machinery still ran in the -siege arm. The +26.20 is
attributable to siegeReflex specifically, which is a cleaner claim than
the one I made, not a broader one. Siege's total footprint is larger than
what I measured.

SECOND: the bench runs rung 3, which is INSANE, and siege is rung-gated.

    DIFFICULTY[5]: easy, normal, hard, insane, self-play
    useSiegeReflex is TRUE on all five.
    OD_BENCH_DIFFICULTY defaults to 3 -> insane.

The source's own per-rung record is "+35 hard, +8 easy, -32 on NORMAL".
My -26.20 land is a fourth rung. So the rule is now negative on at least
two rungs and positive on two, by numbers taken on two different
instruments, while the table switches it on for all five.

This does NOT weaken the other results. garrison, fortify, campaign and
austerity have no difficulty gate at all -- they run on every rung, so
those numbers carry. Only siege needs the caveat, and it needs it badly:
a fix validated on insane may be worthless or harmful on the rung people
actually play.

FOLLOW-UP OWED: whatever the research-ratchet arms say, the siege half
has to be re-run at difficulty 1 and 2 before it means anything for a
player. The austerity half does not.

## 174 -- prediction falsified on the siege half

I wrote in 172: "if the ratchet is the cause, both arms recover most of
the 41 points while CHN holds up better." The siege arm says no.

    OD_SIEGE_RESEARCH=0     265 / 65.97%    +10.10 land over control

which is 39% of the full ablation's +26.20 by land and 85% by rating --
and when two aggregates disagree that much about the same six numbers,
the per-seat column is the only thing that can settle it:

    seat        control   -siege all   -research only
    USA:rung      10.77       19.63       28.57   +17.80
    FRA:rung      22.70       34.90       22.37    -0.33
    SWE:rung       9.80       15.63        3.40    -6.40
    FRA:rush       5.43        9.00        2.40    -3.03
    CHN:mod        6.70        2.33        8.97    +2.27
    NOR:hood       0.47        0.57        0.27    -0.20

FOUR OF SIX SEATS GET WORSE. The +10.10 is USA at +17.80 -- 176% of the
net, the same signature that disqualified manpower (300%) and redeploy
(165%). By the rule I wrote in entry 169 before I had any stake in this
particular answer, rc-siege is not a validated improvement.

So the two findings are NOT one finding, and I said they were. Removing
siegeReflex entirely gains 26 points on 5 of 6 seats; removing only its
research cut gains nothing broad and costs four seats. Whatever
siegeReflex is doing wrong, the research step is not the main part of it.

WHAT SURVIVES

CHN goes 6.70 -> 8.97, ABOVE control, and far above the 2.33 it falls to
under the full ablation. So the fort machinery is what the bankrupt seat
needs and the research cut is what it can do without -- a real result,
one seat wide, and not the one I predicted.

THE HONEST SHAPE OF THIS

Entry 172 called the shared research cut "one finding" on the strength of
a mechanism written in a comment plus two rules that both touch the same
slider. That is a story, and it was a good story. The measurement says
the story explains at most a sliver of siege. This is the sixth time this
session a mechanism has been falsified while the numbers that suggested
it held -- the austerity +15.03 and the siege +26.20 are both still
exactly where they were. Only my explanation died.

Austerity's own arm is still running and has to be judged on its own.

## 175 -- austerity: the growth comes back, the floor does not

    control                 236 / 55.87%
    -austerity (all)        233 / 70.90%    land +15.03, rating  -3
    research-last           288 / 69.73%    land +13.86, rating +52

By aggregates that is the best arm of the session: 92% of the land the
full ablation buys, plus 55 rating points the full ablation does not.
Per seat it is not that.

    seat        control  -austerity  research-last
    USA:rung      10.77      16.30      19.83   +9.07   <- 65% of the net
    FRA:rush       5.43       4.17      10.67   +5.23
    NOR:hood       0.47       0.40       3.80   +3.33
    SWE:rung       9.80      19.70      11.73   +1.93
    FRA:rung      22.70      27.83      20.97   -1.73
    CHN:mod        6.70       2.50       2.73   -3.97   <- the floor is gone

TWO FAILURES AGAINST MY OWN CRITERIA

1. Largest seat is 65% of the net. My threshold, set in entry 169 before
   I had any stake in this answer, is 50%. Not as bad as manpower (300%)
   or redeploy (165%), and still a failure.

2. CHN 6.70 -> 2.73, against 2.50 for removing the rule outright. The
   whole premise of research-last was to keep the solvency floor while
   sparing the growth engine. It kept none of the floor.

I predicted it would "recover most of SWE/USA/FRA's gain and keep CHN
above the -4.2". Half right: the growth came back almost entirely, the
floor came back not at all. And I named the alternative in the same
entry -- that the floor and the growth tax might be one decision that
cannot be separated. That is what happened, and it is the sharper result.

THE RESEARCH CUT IS THE FLOOR

CHN dies at 2.5-2.7 whether austerity is removed entirely or merely
reordered, and sits at 6.70 only when research is cut FIRST. So cutting
research early is precisely what keeps a bankrupt country alive -- it is
the fastest money in the list, and a country at zero needs money this
turn, not a better laboratory in forty turns. The same act taxes every
solvent country roughly 14 points of the world.

That is the ceiling-and-floor pattern for a second mechanism, and this
time on a rule that ships. It is not a bug to be fixed. It is a trade
that is currently made one way for everybody, and the right shape of a
fix is to make it CONDITIONAL -- cut research first only when actually
near bankruptcy, cut it last otherwise -- rather than to reorder it for
all countries as I did.

Neither arm is shippable. The combined arm is still running and I expect
it to inherit both problems.

## 176 -- the naval repricing invalidates every baseline in this journal

The peer repriced naval upkeep with the user's approval:

    SHIP_UPKEEP_CARRIER    25 -> 4.0
    SHIP_UPKEEP_DESTROYER  10 -> 1.5
    and maintenanceCostMod now applies to navy as well as army

Their measurement: navy was 9.0% of gross income on average and 47.7%
for the worst country, against 0.43% for army. One carrier cost the
upkeep of twenty-five million soldiers. The two unit costs had never been
compared with each other.

Timing, checked rather than assumed: their files changed at 11:01 and
11:02; my running arm uses the binary built at 10:17. rc-both is
therefore still valid against the 236 / 55.87% control. Everything after
it is not.

EVERY NUMBER ABOVE THIS ENTRY IS NOW BUILD-RELATIVE HISTORY.

    control 236 / 55.87%          stale
    the seven-reflex audit         stale as absolute values
    austerity +15.03, siege +26.20 stale

The audit's ORDERING probably survives -- garrison and campaign being
load-bearing does not depend on what a destroyer costs -- but no
absolute number does, and I will not quote one against a new arm.

THE INTERACTION THAT IS NOT OBVIOUS FROM THEIR SIDE

austerityReflex fires on `inc.net < 0`. Navy upkeep is a term in net
income. They have just cut the largest expense line in the game by
roughly a factor of six, so countries that were insolvent may now be
solvent, and austerity may fire far less often -- or barely at all.

That means my austerity finding is not merely un-rebaselined, it may be
LARGELY DISSOLVED: a 15-point tax that only applies while insolvent is
worth much less in a game where insolvency is rarer. The finding has to
be re-measured, not re-based.

Which is the correct order of operations anyway: their change is a fix to
a defect (two unit costs never compared), mine was a candidate. A rule
tuned around a broken price is exactly the "heuristic outliving the bug"
pattern, and austerity's research cut may be one.

## 177 -- measure the condition before measuring the fix

The peer sharpened the austerity question into a better experiment than
the one I had planned, and it is the difference between a re-baseline and
a real test:

    "If firings collapse, OD_AUSTERITY_RESEARCH_LAST is a fix to a rule
     that has stopped mattering, and the 15 points were never really
     austerity's -- they were the navy's, arriving through austerity."

That is exactly right and I had not seen it. My plan was to rebuild and
re-measure the reordering, which would have produced a small delta that I
would have read as "the reordering is neutral" when the correct reading
is "the condition went away". Same number, opposite conclusion.

So: MEASURE THE FIRING RATE, NOT THE FIX.

--eval-ai already prints the line that settles it:

    "[EVAL]   solvency: X% of country-turns bankrupt, Y austerity cuts per 1k"

Preserved build-loop/OpenDoctrinesServer as OpenDoctrinesServer.prenaval
BEFORE rebuilding, so both sides of the comparison exist. That was the
one irreversible step in this sequence and it came within a rebuild of
being lost -- the pre-repricing game would have been unmeasurable and I
would have had only an argument about what it used to do.

Pre-repricing eval queued under odlock (two slots, ~4 GB peak, per the
tool's own note) so it does not fight the last research arm for memory.

Their prediction, worth recording before the numbers: a meaningful share
of chronic insolvency in this game was countries paying for hulls at
twenty-five million soldiers each. If so, bankrupt country-turns and
austerity cuts should both fall substantially. Mine: I expect a fall but
not a collapse, because the minority bill was measured at 25.58 per
country-turn against a 27.71 standing bill -- navy is one large term
beside another large term, not the only one.

## 178 -- the combination is what neither arm was

    control                            236 / 55.87%
    OD_SIEGE_RESEARCH=0                265 / 65.97%   +10.10 land
    OD_AUSTERITY_RESEARCH_LAST=1       288 / 69.73%   +13.86 land
    BOTH                               359 / 91.87%   +36.00 land, +123 rating

    seat        ctl    siege    aust    BOTH
    USA:rung  10.77    28.57   19.83   28.63   +17.87  (50% of net)
    FRA:rush   5.43     2.40   10.67   15.87   +10.43  nearly triples
    FRA:rung  22.70    22.37   20.97   30.47    +7.77
    CHN:mod    6.70     8.97    2.73   11.97    +5.27  THE FLOOR NEARLY DOUBLES
    NOR:hood   0.47     0.27    3.80    0.50    +0.03
    SWE:rung   9.80     3.40   11.73    4.43    -5.37  the only loser

    5 of 6 seats up; largest seat exactly at the 50% threshold.

SUPERADDITIVE: 10.10 + 13.86 = 23.96 measured separately, 36.00 together.

I WOULD HAVE REJECTED BOTH ARMS

rc-siege failed the shape test at 176% of net on one seat. rc-aust failed
it at 65% AND destroyed the floor (CHN 6.70 -> 2.73). By the criteria I
set in entry 169, each is unshippable on its own. Together they are the
best configuration measured this session, and the seat they each hurt
worst is the seat the combination helps most.

That is the "ablations do not compose" note in my memory, met from the
other side. It is written there as a warning that single-rule ablations
reverse sign in the shipped config. This is the same fact with the sign
that favours you: two changes that each look bad can be good together,
and the only way to know is to measure the combination you intend to ship
rather than the parts.

ENTRY 175 IS WRONG

I concluded there that "the research cut is the floor" and that the
solvency floor and the growth tax are one decision that cannot be
separated. The combination gets the growth AND a better floor than the
control. So they can be separated -- just not by either change alone.
Third mechanism story falsified today while its numbers held.

WHAT THIS IS NOT YET

Pre-repricing binary, fitted seeds. The naval change landed at 11:01 and
makes every number above stale. This has to be re-run on the new binary
and then on a hold-out before it means anything. But the DIRECTION is
worth the re-run, which is more than any of the 23 candidates earned.

Pre-repricing solvency baseline, for the comparison the peer asked for:

    0.1% of country-turns bankrupt, 2.65 austerity cuts per 1k
    (3 maps x 400 turns, seed 20260801, difficulty insane, generated
     worlds -- NOT the bench's scenario seats, so comparable only to an
     identically-run post-repricing eval)

## 179 -- the condition went away, and the ships are still not bought

    identical eval both sides (3 maps x 400t, seed 20260801, insane)
                              pre       post
    bankrupt country-turns    0.1%      0.0%
    austerity cuts per 1k     2.65      0.44      -83%

The peer's hypothesis, confirmed. A large share of chronic insolvency in
this game was countries paying for hulls at twenty-five million soldiers
each. My "austerity costs 15 points of the world" was substantially the
NAVY'S cost arriving through austerity, and the rule was the messenger.

Their sequencing insight is the transferable part and it was not mine:
measure how often the rule FIRES before measuring whether changing it
helps. I would have re-measured the reordering, seen a small delta, and
recorded "the reordering is neutral" -- the same number, the opposite
conclusion, and a wrong one.

SHIPS: STILL ZERO, ON 83,156 OFFERS

    a5 destroyer   0 of 59,311    0.00%
    a6 carrier     0 of 23,845    0.00%

Not reduced -- zero, at a price six times lower. So the head is not
price-sensitive here at all, which means my entry-165 reading ("the
refusal is correct play, the capability is not worth its price") was
right about the conclusion and wrong about the reason. It is not
declining a bad deal. It is a FROZEN POLICY trained against 25/10 with
the refusal baked in; the price it refuses no longer exists and it
refuses anyway.

That distinction only became visible because the price moved. No
histogram of the old build could have separated "correctly declines" from
"never learned to consider it", because both produce 0 of 6,020.
Separating them now needs a model trained after the change -- a retrain,
not a probe.

A BUG THE REPRICING SURFACED, IN MY FILE

    AISystem.cpp:6937   carrier ? 25.0f : destroyer ? 10.0f
    AISystem.cpp:8222   carrier ? 25.0f : destroyer ? 10.0f

AISystem.cpp includes BuildCosts.h on line 7 and then re-derives the
constant twice anyway. scrapSaving feeds austerity's stopping condition,
so the AI now thinks scrapping a carrier closes a 25-point hole that is
actually 4, and stops cutting while still insolvent.

This is "expose the resolver's numbers" exactly: a re-derived constant is
a silent second copy, and it stays silent until someone changes the
original. Reported rather than silently patched -- it is the peer's
constant. Will measure the fix as its own arm.

## 180 -- what 0 of 83,156 does and does not tell you

Fixed both sites to call shipUpkeep(s.type, s.crew) from BuildCosts.h --
the peer added the function rather than hand me the constants, so the
number cannot be re-derived a third time. No hardcoded ship cost remains
anywhere in src/. They verified my diagnosis independently before acting
on it instead of taking my word, which is the right way round.

Their correction on the zero is sharper than my reading and I am adopting
it:

    "0 of 83,156 is not a preference, it is an action the policy has
     never had a reason to represent. A model trained against 25/10
     learned 'never' as a constant, not as a function of price, so
     re-running it at 4/1.5 asks a question it has no machinery to
     answer."

So the honest statement about the repricing is: the price is now
commensurate with the army, and its effect on play is UNMEASURED. Not
good, not bad. The zero is evidence about the training, not the price --
which is this project's own note about masking changes needing a retrain,
arriving from the other direction.

I have now given three different readings of the same zero in one day:

    morning   collapsed head, needs a retrain to unlock       WRONG
    midday    correct play, capability not worth its price    RIGHT ANSWER,
                                                              WRONG REASON
    now       frozen policy, the question is unanswerable
              without a model trained after the change        (I think right)

The first two were confident. The third is the one that says it does not
know, and it only became reachable because someone changed the price out
from under the measurement. No amount of staring at the old build could
have separated "declines a bad deal" from "never represented the action"
-- both emit 0 of 6,020.

THE FINDING WORTH KEEPING FROM TODAY

An AI rule can be deleted by fixing the resolver defect that created the
state it handles. austerityReflex fires 83% less because the navy stopped
bankrupting countries. Nobody improved the rule; the condition it exists
for largely stopped occurring. When an AI heuristic looks expensive,
one of the candidate explanations is that it is correctly handling a
state that should not exist.

## 181 -- the finding survives the repricing at less than half its size

    post-repricing, 11:12 binary, fitted seeds, 400 turns
    np-control   257 / 53.23%
    np-both      284 / 69.10%     +27 rating, +15.87 land

    FRA:rung  20.77 -> 27.37   +6.60
    SWE:rung   6.47 -> 11.87   +5.40
    CHN:mod   11.30 -> 13.67   +2.37   the floor improves again
    FRA:rush   3.47 ->  5.33   +1.87
    USA:rung  10.80 -> 10.47   -0.33
    NOR:hood   0.43 ->  0.40   -0.03
    4 of 6 up; largest seat 42% of net -- PASSES the shape test

Pre-repricing it was +36.00. Now +15.87, less than half, which is what
the austerity dissolution predicted: a chunk of the original number was
the navy's cost arriving through austerity, and that is now gone from
both arms.

What is left is BETTER EVIDENCE than what it replaced, on three counts:
the baseline is honest (257 measured on this binary, not carried over),
the shape test passes at 42% rather than sitting exactly on the 50%
threshold, and USA -- the tipping-point seat that carried the earlier
artefacts -- contributes nothing this time (-0.33). The gain is spread
across FRA, SWE, CHN and FRA:rush.

The floor improves for the second time: CHN 11.30 -> 13.67. Note the
control's CHN also rose from 6.70 to 11.30 with the repricing alone, so
the seat that dies to bankruptcy was substantially a victim of navy
prices. Two independent changes both helped it, which is a mild
convergent check on the whole story.

STILL OWED: a hold-out seed set. That is the one criterion from entry 169
this has not met, and it is the criterion that killed seven earlier
results.

## 182 -- a file-size delta nobody had to notice

The peer reported that the model blob shrank during training and said
plainly that they could not explain it. Not "probably optimizer state" --
they listed what they had ruled out, listed what they could not, and
called it a live risk. That message is the only reason the next six hours
of my bench time were not wasted producing a confident wrong answer.

    parent (N24)   9,020,822 bytes
    after round 1  8,974,598
    delta            -46,224 = 11,556 floats = 36 rows x 321
                                              (321 = TRUNK_OUT 320 + bias)

Whole output rows of a 320-wide layer. Cause, verified in both trees:

    replicateOutputBlocks   my tree: NeuralNet.h + AISystem.cpp (2 calls)
                            HEAD:    ZERO references

My tree runs DIPLO_OUTPUTS = DIPLO_ACTIONS * OFFER_KINDS across both
m_leagueDiplo (12470) and m_diplo (12648). HEAD has only {320, 2}. N24
was written by my binary, so its diplomacy blobs are wide; HEAD's loader
expects narrow, hits a shape mismatch, and Xavier-initialises rather than
failing -- the exact behaviour NeuralNet.h documents and the peer quoted
back to me before either of us knew it applied.

So their run was training a policy whose two diplomacy heads were RANDOM
from step one, at quarter learning rate, which cannot recover them. That
layout is worth 108 -> 160 rating and survival 68 -> 87 in this project's
record -- the largest architectural gain the model has had.

WHAT I WOULD HAVE CONCLUDED

Six checkpoints, all far below N24, and the report writes itself: "the
new naval price does not help; N24 stays." Wrong, with numbers behind it,
and no part of my screening plan would have caught it -- the action
histogram would have shown ships still unbought, which is exactly what a
model with a wrecked diplomacy head and an untouched econ head produces.

I had also already built the trap myself: entry 179 says the honest test
of the new price needs "a model trained after the change". True, and it
silently assumed the training pipeline preserves the architecture the
parent was trained with. Across two trees where only one has the
widening, it does not.

THE GENERAL SHAPE

A model file is not a portable artefact between two trees that disagree
about a layer width, and the disagreement is SILENT by design -- the
migration path exists so that loading an old model never fails. Every
property that makes it robust in production makes it undetectable here.
The only signal was a byte count that nobody was required to look at.

## 183 -- the first thing all session to survive a hold-out

    HOLD-OUT set C (909091, 20230115, 42424242), 400 turns, 11:41 binary
    ho-control   238 / 52.53%
    ho-both      308 / 81.87%     +70 rating, +29.33 land

    FRA:rung  16.93 -> 27.57  +10.63
    USA:rung   4.40 -> 21.77  +17.37
    SWE:rung   9.80 -> 13.03   +3.23
    CHN:mod   16.13 -> 18.03   +1.90
    NOR:hood   0.33 ->  0.43   +0.10
    FRA:rush   4.93 ->  1.03   -3.90

    breadth      5/6 seats                          PASS
    replicates   fitted +15.87, hold-out +29.33     PASS
    no artefact  largest seat 59% of net            FAIL (threshold 50%)

Two of three. I am not going to quietly drop the third.

BUT THE CROSS-SET PATTERN IS STRONGER THAN EITHER NUMBER

    fitted seeds   +15.87   USA contributes  -0.33   gain from FRA/SWE/CHN
    hold-out C     +29.33   USA contributes +17.37   59% of the net

Different seats carry the gain on different world sets. That is the
opposite of the artefact signature: manpower and redeploy were USA at
300% and 165% on ONE set and had no second set to answer to. Here a
USA-specific artefact cannot explain +15.87 on a set where USA moved
essentially nothing, and a FRA/SWE/CHN-specific one cannot explain
+29.33 where they contribute a third.

So the 59% is a real caveat about the hold-out MEASUREMENT and not, on
this evidence, about the finding. The honest statement is that the effect
is large, replicates across two independent seed sets, and its per-seat
distribution is unstable -- which is what a broad effect looks like when
measured on six volatile seats.

WHAT IT IS

    OD_SIEGE_RESEARCH=0            siege keeps forts, drops its research cut
    OD_AUSTERITY_RESEARCH_LAST=1   austerity cuts research last, not first

Both already exist; the second is two lines I added. Neither is a new
rule -- both stop an existing rule from ratcheting the research slider
down in a game whose own source says the slider never comes back up.

STILL TO DO BEFORE THIS IS A RECOMMENDATION

Set D as a tiebreaker on the 59%. Three sets is what this project's own
note demands and seven results died at set two. Queued.

## 184 -- I built the screen and it measured the same model twice

Sanity-checked the checkpoint screen on two different files and got
byte-identical output:

    parent-worktree   a5 0/48,374   a6 0/19,337   a8 0/89,945   solvency 0.63
    head-narrow       a5 0/48,374   a6 0/19,337   a8 0/89,945   solvency 0.63

Two different model files (4,166,584 and 4,137,796 bytes), identical offer
counts to the unit. Impossible unless it was the same model both times.

CAUSE: the binary does not parse `--model` AT ALL.

    ServerMain.cpp:335   parses --vs-model
    (no --model anywhere)
    od_bench.py:409      env["OD_EVAL_MODEL"] = abspath(model)
    Game_AITrain.cpp:814 if (const char* em = getenv("OD_EVAL_MODEL"))

od_bench passes the model through the ENVIRONMENT. I copied the flag from
od_bench's own usage line -- `tools/od_bench.py --model x.bin` -- where
it is the PYTHON script's argument, and passed it to the C++ binary,
which ignored it silently and loaded the data directory's own model. The
run even printed which file it used:

    [EVAL] Model: .../safedata/ai/model.bin (read-only ...)

and my grep filtered that line out. The instrument told me it was
measuring the wrong thing and I had removed the line that said so.

WHAT THIS ALMOST COST

Six checkpoints screened, all reporting a5/a6 at zero, and the conclusion
"no checkpoint learns to buy a ship at the new price" -- which is also
exactly what a WORKING screen would print if the training genuinely did
not help. The failure mode is invisible in the output. Only comparing two
known-different inputs exposes it.

That is the second instrument-validation catch today from the same
technique: prove the tool can tell two things apart before trusting it to
say they are the same. The reflex ablation had it by luck (withdraw was
disabled, so its identical numbers PROVED inertness). Here identical
numbers meant the opposite, and only the file sizes said which.

FIXED: OD_EVAL_MODEL, and the "[EVAL] Model:" line is now kept in the
output rather than filtered, so every screen states which file it read.
Re-verification queued: two known-different models must produce DIFFERENT
numbers before I screen anything of the peer's.

## 185 -- VALIDATED: three sets, same sign, with one honest cost

    set          control    both     land
    fitted         257       284    +15.87
    hold-out C     238       308    +29.33
    set D          256       298    +20.40
                                     mean +21.87, all same sign

    seat            fitted   hold-C   set D
    FRA:rung        +6.60   +10.63  +10.57   positive on all three
    SWE:rung        +5.40    +3.23   +2.33   positive on all three
    CHN:mod         +2.37    +1.90   +2.90   positive on all three (FLOOR)
    USA:rung        -0.33   +17.37  +11.43   positive on two
    FRA:rush        +1.87    -3.90   -5.80   NEGATIVE on two, worsening
    NOR:hood        -0.03    +0.10   -1.03   noise

This is the first thing all session to survive three seed sets, and 23
invented rules did not survive one.

THE CRITERION THAT FAILS, AND WHY I AM NOT DISMISSING IT

Largest single seat: 42%, 59%, 56% -- over my 50% threshold on two sets.
But the threshold was built to catch a result that is ONE seat with noise
around it, and the per-seat table shows the opposite: three seats move
positive on every set with stable magnitudes (FRA +6.6/+10.6/+10.6,
CHN +2.4/+1.9/+2.9). USA is large and volatile, so it dominates the sum
whenever it moves, but removing USA entirely still leaves +16.2/+11.9/+9.0.
The effect does not depend on it.

THE COST IS REAL AND HAS A MECHANISM

FRA:rush goes +1.87, -3.90, -5.80 -- worsening as the seed sets change,
and it is the one seat being actively rushed. That is exactly what
OD_SIEGE_RESEARCH=0 should cost: the siege reflex's research cut is the
emergency funding line for forts when a stronger enemy is at the border,
and turning it off takes that money away from precisely the country that
needs it. The rule is right for the besieged case and wrong for everyone
else, which is why the fix is a gate rather than a deletion.

So the honest recommendation is: this is worth landing, and it trades
about 4 points of a rushed France for about 22 points of everyone else.
The next experiment is whether that trade can be avoided -- keep siege's
research cut when actually under siege by a materially stronger enemy,
drop it otherwise -- rather than accepting it.

WHAT IT IS, in one line each:

    OD_SIEGE_RESEARCH=0           siege builds forts, stops cutting research
    OD_AUSTERITY_RESEARCH_LAST=1  austerity cuts research last, not first

The second is two lines I wrote; the first is a knob that already existed
and shipped defaulted the other way. Neither adds a rule. Both stop an
existing rule ratcheting down a slider that this game's own source says
never comes back up.

## 186 -- the gate already existed too

The validated combination costs FRA:rush about 4-6 points and the reason
is mechanical: OD_SIEGE_RESEARCH=0 removes the siege reflex's emergency
research cut from EVERY country, including the one with a stronger army
across the border. That rule is right for the besieged case. It is only
wrong as a blanket.

The gate is already in the source:

    bool AISystem::besieged(const CountryStat& st) const {       // 7000
        static const float share = getenv("OD_SIEGE_SHARE") ... : 0.25f;
        if (st.worstDeficit <= 0 || st.worstThreatPid < 0) return false;
        return st.worstDeficit >= share * std::max(1LL, st.army);
    }

So "besieged" currently means an adjacent enemy stack exceeding a QUARTER
of our army, which on reflection is not a siege, it is a border. Raising
the share makes the whole reflex -- forts and research cut together --
fire only when genuinely outmatched, which is the shape the per-seat table
argues for: keep it for FRA:rush, remove it from the other five.

Testing 0.75 and 0.50 on set D, where the cost was largest (-5.80),
against sd-both 298 / 72.50%. What would make this the better change:
FRA:rush recovers toward its control value while FRA:rung, SWE and CHN
keep their gains. What would retire it: the gains come from siege being
OFF generally, in which case tightening the gate loses them and the
blanket switch is simply correct.

Prediction before the numbers, so it is on the record: I expect 0.75 to
recover most of FRA:rush and to give up some of USA's gain, because USA's
swing has tracked siege's total footprint in every arm so far. If both
gate values land between the control and sd-both on every seat, the
honest read is that this is one dial and the blanket setting is its best
point -- which is the ceiling-and-floor outcome a third time.

INSTRUMENT NOTE: the checkpoint screen now names the file it loaded and
returns different counts for different models (a5 offers 26,013 for
HEAD's model against 48,374 from the broken version). It discriminates.
The "diplomacy head widened" line does NOT appear on the OD_EVAL_MODEL
path -- that printf is in the --vs-model opponent loader only -- so file
size stays the practical shape signature.

## 187 -- gating does not buy back the cost; the blanket stands

    set D, 400 turns, against sd-control 256 / 52.10%
    gate 0.75   261 / 59.90%   net land  +7.80
    gate 0.50   256 / 68.27%   net land +16.17
    blanket     298 / 72.50%   net land +20.40

    seat            ctl    g.75    g.50  blanket   monotonic
    FRA:rush       8.63    5.17    3.67     2.83   YES, falling
    USA:rung      10.90   13.20   14.83    22.33   YES, rising
    FRA:rung      10.50   15.20   13.50    21.07   no
    SWE:rung       9.50    8.47   18.73    11.83   no
    CHN:mod       11.13   17.50   17.33    14.03   no
    NOR:hood       1.43    0.37    0.20     0.40   no

HYPOTHESIS REJECTED. Gating trades about 12 points of net land to buy
back about 2.3 points on the rushed seat. That is a bad trade and the
blanket setting is the better change.

The two monotonic seats are the ones the mechanism predicts, and they are
the evidence that the mechanism is right even though the fix is not:
FRA:rush degrades smoothly as siege's research cut is withdrawn
(8.63 / 5.17 / 3.67 / 2.83) and USA improves smoothly as it is
(10.90 / 13.20 / 14.83 / 22.33). Dose-dependence in both directions on
the two seats the story is about.

The other four are not monotonic and I am NOT going to read that as four
per-seat optima. USA spans 28 points across arms and SWE 16 on six seats
and three seeds; non-monotonicity in that much variance is what noise
looks like. Claiming SWE "prefers 0.50" (18.73) would be the same
over-reading that manpower and redeploy died of, and I would be doing it
because the number is pleasing rather than because it replicated.

WHAT I PREDICTED AND WHAT HAPPENED

I said 0.75 would "recover most of FRA:rush and give up some of USA's
gain". The halves inverted: it recovered SOME of FRA:rush (40% of the
loss) and gave up MOST of USA's gain (11.43 -> 2.30). I also named the
alternative -- that it is one dial with the blanket at its best point --
and on net land that is what the numbers say.

So the cost is the price, not a defect to engineer around. The
recommendation is unchanged and now has a sweep behind it rather than an
assumption: about 4-6 points of a rushed France for about 22 points of
everyone else, and the intermediate settings are worse than either end.

## 188 -- garrison's keep verdict does not survive the hold-out

    removing garrisonReflex        rating        land
    fitted seeds (entry 168)     236 -> 156     -17.10
    hold-out set C               238 -> 243      -2.43

    seat            fitted    hold-out
    FRA:rung        +3.50       -8.30    direction flips
    USA:rung       -10.27       +5.43    direction flips
    SWE:rung        -6.60       -1.50
    CHN:mod         -4.70       -1.13
    FRA:rush        +0.90       +2.97
    NOR:hood        +0.07       +0.10

I called this "load-bearing, decisively" and "the largest effect of any
rule in the audit". On a second seed set it is worth 2.4 points of land
and its rating effect changes sign. The two seats that carried the fitted
verdict, FRA:rung and USA, both reverse.

This is the same disease that killed twenty-three candidate rules today,
found in a rule that SHIPS -- which was the entire premise of the audit
and I still did not expect it here. Garrison was the safest-looking
result I had: biggest number, cleanest story ("holding a threatened
border is not a choice the policy should gamble on"), three seats
collapsing without it.

WHAT THIS DOES AND DOES NOT SAY

It does NOT say remove garrisonReflex. Removing it still costs land on
the hold-out, just 2.4 rather than 17.1, and one set is not a verdict --
that is the mistake I am correcting, not repeating. What it says is that
its measured value is mostly fitted-seed inflation and its true worth is
unknown and much smaller than the audit claimed.

It also retroactively weakens entry 168's framing, where I wrote that the
audit "is not turning up a graveyard" and had a "much better hit rate
than my twenty-three candidates". That comparison was between three keeps
measured on ONE set and twenty-three rejections measured on two or more.
Not a hit rate -- a difference in evidential standard, and I was the one
applying it unevenly.

Third set queued for garrison specifically. campaign and fortify are
still running on hold-out C and now carry the same suspicion.

## 189 -- the same uneven standard, running the other way

Checking where I STOPPED rather than what standard I applied, prompted by
the peer's point that stopping is silent:

    keeps       garrison, campaign, fortify   fitted only -> now hold-out
    dismissals  manpower, redeploy            fitted only -> NEVER RE-RUN

I dismissed manpower (+19 rating, +6.07 land to remove) and redeploy
(+18, +7.63) as single-seat artefacts because USA carried 300% and 165%
of their nets. On one seed set. Then I questioned only the keeps.

The asymmetry is the same one from entry 188 and I repeated it inside an
hour of writing it down, in the opposite direction and so invisibly: a
dismissal keeps the rule, which is the conservative-looking outcome, so
it never felt like a claim that needed defending. But if either rule
genuinely helps on other worlds, "single-seat artefact" is a missed gain
sitting in the journal as a settled negative.

The USA argument may well hold -- that seat spans 28 points and dominates
whatever it touches. But that is a reason to expect the dismissal to
survive, not a reason to skip measuring it, and it is exactly the shape
of "the result agrees with me so I stopped".

Queued both on hold-out C behind the rest.

WHAT MADE THIS VISIBLE: not a review of my conclusions, which all still
look defensible, but a review of where the questioning ended. Nothing in
the journal marked those two as under-evidenced, because a question not
asked leaves no artefact. The keeps got re-run because a reversal was
imaginable and interesting; the dismissals did not because their outcome
was already the comfortable one.

## 190 -- campaign holds, and the two results teach different lessons

    removing campaignReflex     rating          land
    fitted                    236 -> 172       -14.60
    hold-out set C            238 -> 179        -9.37

Same sign, comparable magnitude, and the rating effect is near-identical
(-64 against -59). campaignReflex is a CONFIRMED keep on two seed sets --
the first audit verdict to earn that.

    rule       fitted net   hold-out net   rating fitted -> hold-out
    campaign      -14.60        -9.37         -64 -> -59   replicates
    garrison      -17.10        -2.43         -80 -> +5    collapses

A METHODOLOGICAL POINT I HAD BACKWARDS

Campaign's PER-SEAT sign agreement is 3 of 6. Individual seats disagree
wildly between the two sets -- FRA:rung +1.13 then -6.63, FRA:rush -5.27
then +3.73 -- while the net and the rating barely move. Garrison has the
same per-seat scatter and its aggregate does NOT survive.

So per-seat agreement across DIFFERENT WORLD SETS is a bad replication
test. The worlds are different; the seats are volatile (USA spans 28
points); scatter is the expected outcome even for a real effect. What
replicates or fails to replicate is the aggregate.

That is not a retreat from reading per-seat. The two uses are distinct
and I had been conflating them:

    WITHIN one set    per-seat catches the single-seat artefact --
                      is one seat carrying the whole net? (manpower 300%)
    ACROSS sets       the aggregate is the replication test; per-seat
                      scatter says nothing either way

Applying that cleanly: campaign replicates (aggregate stable, no single
seat dominating either measurement). Garrison does not (aggregate
collapses 86%, rating flips sign). Both have messy per-seat tables and
that fact distinguishes neither of them.

fortify still running; garrison set D behind it; then the two dismissals.

## 191 -- fortify holds; the audit's hold-out is 2 of 3

    removing fortifyReflex      rating          land
    fitted                    236 -> 200       -6.60
    hold-out set C            238 -> 221       -6.20

Six-tenths of a point apart on land across two independent world sets.
The most stable measurement in this journal, of any kind. Rating is
smaller (-36 against -17) but the same sign.

THE AUDIT AFTER HOLD-OUT

    rule       fitted land   hold-out land   verdict
    fortify        -6.60         -6.20       CONFIRMED keep
    campaign      -14.60         -9.37       CONFIRMED keep
    garrison      -17.10         -2.43       COLLAPSED, set D pending
    manpower       +6.07           ---       dismissal, hold-out queued
    redeploy       +7.63           ---       dismissal, hold-out queued

Two of three keeps survive. The one that failed is the one I called
"decisively load-bearing" and "the largest effect of any rule in the
audit" -- the biggest number and the cleanest story, which between them
bought it exactly no extra reliability.

Worth noting against my own framing in entry 188: I wrote there that the
uneven standard meant "every verdict in the audit rests on unequal
evidence", which is true, and the peer told the user something similar.
The implication both of us let stand was that they would therefore all
be weak. They are not. Two came back stable, one of them to within 0.4
points. Unequal evidence means UNKNOWN, not wrong -- and the correction
to a too-confident claim inherits its confidence unless you make it stop,
which this project has written down and I have now done twice in a day.

So the audit's real yield, stated at the standard it should have used
from the start:

    fortify and campaign are load-bearing on two seed sets.
    garrison is unresolved and was oversold.
    the research ratchet is the change, on three sets.

## 192 -- the mechanism check is weak support, not confirmation

    3 maps x 400 turns, seed 20260801, insane, identical settings
                              control   treatment
    research nodes per 1k      123.13     126.83    +3.0%
    austerity cuts per 1k        1.13       0.28    -75%
    bankrupt country-turns        0.1%       0.0%

The direction survives: research output IS higher under the treatment.
That is the prediction the mechanism makes and it could have come back
negative. It did not.

But +3.0% is weak support for a story that claims to explain +21.9
points of world, and I am not going to write it up as confirmation. Two
reasons to hold it loosely:

1. NODES ARE A PROXY. Research nodes COMPLETED move with income,
   territory and elapsed turns, not only with the slider. A country that
   is winning completes more nodes whatever its allocation, so some of
   the +3.0% is the treatment's own land gain feeding back.
2. WRONG WORLDS. --eval-ai runs generated maps; the bench runs six
   scenario seats. The +21.9 was measured on the seats and this was not.

The downstream numbers moved much harder than the direct one: austerity
firings down 75%, bankruptcy eliminated. That is consistent with a
virtuous cycle -- stop cutting research, better tech, better income,
fewer deficits, less austerity -- but "consistent with" is exactly the
standard the peer's negative control just demolished. Their two timing
assertions were also consistent with their explanation and measuring
something else.

SO: BUILT THE INSTRUMENT THAT READS THE SLIDER ITSELF.

Under OD_ACT_HIST, accumulate m_countryResearchAllocation once per
country per turn and print the mean at exit. That is the quantity the
explanation names -- not a proxy for it, and not downstream of the
outcome. If the mean allocation is materially higher under the treatment
the mechanism stands; if it is flat, the +21.9 is real and comes from
something else those knobs touch, and I will have been telling the user
and the peer a wrong story about a right number.

NOT BUILT YET: garrison set D and the two dismissal arms are running on
the 11:41 binary. Rebuilding under them is the straddle that corrupted
the checkpoint curve. The edit is in the tree, default-off, and builds
when the queue drains.

## 193 -- set D rescues garrison, and my correction was the wrong one

    removing garrisonReflex   land     rating
    fitted                  -17.10       -80
    hold-out C               -2.43        +5
    set D                   -20.90      -103
    mean                    -13.48

    seat            fitted   hold-C    set D
    FRA:rung        +3.50    -8.30    -0.33
    SWE:rung        -6.60    -1.50    -2.13
    USA:rung       -10.27    +5.43    -2.10
    CHN:mod         -4.70    -1.13   -11.10
    FRA:rush        +0.90    +2.97    -4.37
    NOR:hood        +0.07    +0.10    -0.87

garrisonReflex KEEPS ITS PLACE. Two of three sets say strongly
load-bearing; hold-out C is the outlier, not the fitted seeds.

I WAS WRONG, AND I HAD ALREADY BEEN WARNED IN THE EXACT TERMS

Entry 188 called this "a shipped rule whose justification was mostly
fitted-seed inflation" and said the audit's biggest verdict had failed
the standard. The peer replied, before set D ran, that I should not let
the reversal become as load-bearing as the claim it corrected, because
one hold-out set flipping a sign is one set and set D could equally
rescue it. I agreed, wrote the corollary into memory under
corrections-inherit-confidence, told them I had written the entry so this
outcome would not be embarrassing -- and then kept describing garrison as
collapsed in the two entries that followed.

Writing down the caveat is not the same as holding it. I did the first
and reported as though I had done the second.

WHAT SURVIVES FROM 188

The uneven-standard finding stands and is untouched: three keeps measured
on one seed set against twenty-three rejections measured on two-plus was
a void comparison whatever the keeps later turned out to be worth. That
was a fact about my process, not about garrison, and garrison holding up
does not repair it. The instance was wrong; the pattern it exposed was
real. Those are separable and I nearly bundled them.

What does NOT survive is every sentence where I used garrison as the
worked example of a shipped rule failing. It is now the worked example of
a correction failing.

AUDIT, FINAL, ALL ON >=2 SETS:

    garrison   -13.48 mean (3 sets)   KEEP
    campaign   -11.99 mean (2 sets)   KEEP
    fortify     -6.40 mean (2 sets)   KEEP, most stable measurement here
    manpower, redeploy                dismissals, hold-out running

## 194 -- the two dismissals, re-run: one confirmed, one mischaracterised

MANPOWER -- dismissal confirmed, and the sign flips.

    removing manpowerReflex   fitted +6.07 land, +19 rating
                              hold-out -1.13 land, -13 rating

The fitted "gain" was the USA artefact (300% of net) exactly as called.
On a second world set there is no gain to be had and removing it costs a
little. manpowerReflex stays. Conservative call held, for the stated
reason rather than by luck.

REDEPLOY -- dismissal stands, but "artefact" was the wrong word.

    seat          fitted   hold-out C
    USA:rung      +12.57      +15.40   <- replicates closely
    SWE:rung       -6.70       -6.97   <- replicates closely
    FRA:rung       +5.33       -1.63
    CHN:mod        +1.37       -4.83
    FRA:rush       -5.10       +1.43
    NOR:hood       +0.17       +0.07
    net            +7.63       +3.47
    largest seat     165%        444% of net

I dismissed this as a single-seat artefact. The dismissal was right --
there is no broad improvement here, 4/6 then 3/6 seats, and the net is
dwarfed by its largest term twice over. But "artefact" implied noise, and
the two big terms are the OPPOSITE of noise: USA +12.57 then +15.40, SWE
-6.70 then -6.97, across independent world sets. Those replicate better
than most things in this journal.

So the honest characterisation is not "the effect is not real" but "the
effect is real, heterogeneous, and nets to almost nothing". redeployReflex
consistently costs USA about 14 points and consistently buys SWE about 7.
It is a redistribution between seats, not a gain or a loss.

That is a distinction I would have missed entirely by not re-running it,
and it is the third thing today where the aggregate and the seats tell
different stories. Not a recommendation -- a net of +5.55 mean composed of
two opposing double-digit terms is not something to ship on. But worth
knowing that the rule has a specific victim rather than a diffuse cost,
because that is the shape a targeted fix could address later.

AUDIT COMPLETE. Every verdict now on two or more seed sets:

    garrison   -13.48 mean (3 sets)   KEEP
    campaign   -11.99 mean (2 sets)   KEEP
    fortify     -6.40 mean (2 sets)   KEEP, most stable measurement here
    manpower    +2.47 mean (2 sets)   KEEP, fitted gain did not replicate
    redeploy    +5.55 mean (2 sets)   KEEP, heterogeneous, USA vs SWE
    withdraw, peace, pacification, callToArms -- OFF by default, inert

## 195 -- the slider moves the right way and the number is nearly meaningless

    mean research allocation, 3 maps x 400 turns, generated worlds
    control     0.4261 over 44,222 country-turns
    treatment   0.4351 over 45,932 country-turns
                +2.1% relative

Direction confirmed: the slider IS higher under the treatment, which is
the prediction the mechanism makes and it could have come back flat or
negative. It did not.

BUT THE STATISTIC IS DILUTED TO NEAR-USELESSNESS

austerity cuts fire 1.13 per 1,000 country-turns in the control. So on
the generated maps, the rules under test do essentially nothing on
roughly 99.9% of the country-turns that go into the mean. A global
average over 45,932 country-turns is dominated by countries the knobs
never touched, and a 2% shift in that average is consistent with almost
any effect size on the countries that WERE touched.

Which means I built an instrument that reads the right quantity in the
wrong place, having already written down that --eval-ai runs generated
maps while the +21.9 was measured on six scenario seats. I flagged that
as a caveat in entry 192 and then went ahead and treated the answer as
the test anyway. The caveat was correct and I under-weighted it because
the instrument was newly built and I wanted to use it.

WHERE THE RULES ACTUALLY FIRE

The bench seats. modern:CHN dies of bankruptcy there -- that is written
down in this project's own notes -- and FRA:rush is besieged by
construction. Those are the conditions austerityReflex and siegeReflex
exist for, which is precisely why the finding showed up on the seats and
barely registers on generated worlds.

Re-running with OD_ACT_HIST through od_bench itself, one seed, control
against treatment, so the mean is taken over the six seats where the
+21.9 was measured rather than over 42 countries that were never in
deficit.

If the slider is materially higher there, the mechanism holds. If it is
flat there too, then the +21.9 comes from what austerity cuts INSTEAD --
doctrines, pacification, minority programmes, hulls -- and the story I
have been telling is wrong in a specific and interesting way rather than
merely unproven.

## 196 -- the bench eats the instrument's output

The bench arm ran and printed no allocation line at all. Cause, one grep:

    tools/od_bench.py:420   subprocess.run(..., capture_output=True, ...)

od_bench captures the child's stdout and parses the score out of it;
everything else is discarded. So the atexit dump has been going into a
buffer that od_bench throws away, and would have done so silently for as
long as I cared to run it.

Caught only because I knew what line to expect and it was missing. An
instrument that produces NOTHING is the easy case -- the dangerous one is
today's earlier bug, where the screen produced a full, plausible,
identical-looking histogram for two different models.

FIXED: OD_ACT_HIST_FILE appends "<mean> <count>" from inside the benched
process, bypassing the capture entirely. Source edit only -- the treatment
arm is still running on the current binary and rebuilding under it is the
straddle again.

Also worth recording, since it is the third distinct failure of the same
tool today: the action histogram has now been wrong via an ignored --model
flag, right-but-diluted on generated worlds, and discarded by the bench's
output capture. Each failure was in a different layer and none was visible
in the layer above it.

## 197 -- MECHANISM CONFIRMED, measured where the finding was made

    mean research allocation, bench maps, seed 20260801, 400 turns
    control     0.4049 over 232,735 country-turns
    treatment   0.4339 over 214,448
    delta      +0.0290   +7.2% relative
    rose on 6 of 6 runs

    run   control -> treatment    delta
     1    0.3917    0.4367       +0.0450
     2    0.3954    0.4326       +0.0372
     3    0.3989    0.4324       +0.0335
     4    0.4110    0.4374       +0.0264
     5    0.4078    0.4299       +0.0221
     6    0.4131    0.4317       +0.0186

The story I have been telling is TRUE and now measured on the quantity it
names, on the worlds the +21.9 was measured on. Not a proxy (nodes
completed), not downstream (austerity firings), not diluted across
generated maps where the rules never fire: the slider itself, six runs
out of six.

Against +2.1% on generated worlds, +7.2% here. The dilution diagnosis in
entry 195 was right, and it was worth the rebuild to find out.

THE SIGNATURE I DID NOT PREDICT

    control spread   0.3917 - 0.4131 = 0.0214
    treatment spread 0.4299 - 0.4374 = 0.0075   three times tighter

And the per-run deltas are ordered: the runs with the LOWEST control
allocation gain the most (+0.0450 at 0.3917) and the highest gain least
(+0.0186 at 0.4131). That is exactly the shape a removed ratchet
predicts and nothing else obviously does. A rule that drags the slider
down by a variable amount produces variable, depressed allocations; take
it away and every world converges on where the policy actually wants to
sit. I did not anticipate the variance collapse and it is better evidence
than the mean, because a confound that lifts the average has no reason to
also compress the spread and align the deltas with the deficit.

WHAT IS NOW ESTABLISHED, END TO END

    the change    OD_SIEGE_RESEARCH=0 + OD_AUSTERITY_RESEARCH_LAST=1
    the effect    +15.9 / +29.3 / +20.4 land, three independent seed sets
    the cost      about 4-6 points on a rushed France, dose-dependent,
                  and a gating sweep to avoid it made things worse
    the mechanism research allocation +7.2%, 6/6 runs, variance -65%

Four days of this loop produced twenty-three rejected candidates. The one
change that works was already in the game, defaulted the wrong way, with
a comment in its own source stating the fact that makes it wrong.

## 198 -- determinism, answered from data already on disk

The peer reported tests/determinism_check.sh failing intermittently on the
GAME binary -- owners identical, armies and money diverging, bimodal
across runs -- and flagged that if the race were in shared turn code then
every paired arm I ran today sits on a floor that can move.

Answered without running anything. Compared every pair of stored bench
runs sharing seeds, turns, difficulty and model:

    251 pairs identical on all six seats to full precision

The clean ones are literal repeats of identical settings, hours apart,
several ACROSS A REBUILD:

    slider-bench-ctl == sb2-ctl        different binaries
    slider-bench-trt == sb2-trt        different binaries
    abl-control      == rc-control     different binaries
    sd-control       == sda-control    same binary
    parent-setD      == sm-ctl-setD    different binaries

OpenDoctrinesServer under od_bench is deterministic. Every paired arm
today stands, and so do the byte-identical inertness proofs.

WORTH NOTING ABOUT THE METHOD

Four days of measurements had already answered a question nobody had
asked, and the answer was sitting in build/od_bench_results.json the
whole time. The provenance fields I added after the build straddle --
seeds, turns, binary_mtime, model_size -- are what made the query
possible; without them I could not have told which pairs were comparable
and would have had to spend 35 minutes re-running instead.

That is twice today those fields have paid: once catching the fitted-seed
mix-up in entry 163, once here. They cost four lines.

The peer's bug is now localised rather than solved: deterministic under
the server, non-deterministic under the game binary, with owners
identical while armies and money differ. That fits a thread touching
per-country numeric state without touching ownership -- and the game
binary has audio, LLM pumps and a renderer that the server does not.

## 199 -- the ceiling nobody examined, and a correction to my own mechanism

Following the confirmed mechanism upward found a bare constant:

    case 7:  // research funding up
        alloc = std::min(0.5f, alloc + 0.05f);

Research funding is HARD CAPPED AT 50%. No comment, no knob, nothing
measured against it -- while every other number near it in this file
carries a paragraph of justification.

The head takes `fund up` on 96.40% of the turns it is offered and takes
`fund down` on 0 of 114,650. It pushes in one direction, always. With the
research ratchets removed the mean allocation settles at 0.4339, which is
87% of the cap. So the policy is pressing against this ceiling
continuously and cannot get past it, and nobody has asked whether 0.5 is
where it should be.

That is the "sweep the defaults" pattern exactly: the previous largest
gain in this project came from an unexamined constant inside a rule that
already worked, not from a new rule.

A CORRECTION TO MY OWN MECHANISM STORY, found in the same read

OD_SIEGE_RESEARCH gates TWO sites, not one:

    7633   siegeReflex cuts the slider by 0.15
    5121   the HEAD refuses `fund up` while siegeEarmark(cid) > 0

So the treatment does not only stop a reflex ratcheting down; it also
stops the head being BLOCKED from ratcheting up while a fort is owed. I
have been describing it as one thing and it is two, both pointing the
same way ("stop suppressing research"), which is why the slider
measurement came out clean and why I did not notice.

Not a retraction -- the mechanism is confirmed and the direction is
right. But "siege stops cutting research" was an incomplete account of a
knob I have recommended, and the incompleteness was visible in the source
the whole time.

QUEUED: cap 0.60 and 0.75 on hold-out C, with the default-cap arm as an
inertness check that must reproduce 308 / 81.87%.

Prediction, before the numbers: I expect a gain at 0.60 and am genuinely
unsure about 0.75. The head is choosing to sit near the cap rather than
being forced there, which is the distinction that separates this from the
naval and attack experiments -- those forced an action the policy
declined; this removes a limit the policy is pushing against. If both
lose, the honest read is that 0.5 was load-bearing and the person who
wrote it knew something not written down.

## 200 -- the ceiling is not the constraint

    cap-base (0.50)   308 / 81.87%
    cap-60   (0.60)   308 / 81.87%    IDENTICAL on all six seats

Raising the research ceiling changes nothing. Not "a little" -- nothing,
to full precision, across three seeds and six seats. So allocation never
reaches 0.5 even once, min(0.5, x) and min(0.60, x) take the same branch
every time, and the cap is dead code in practice.

PREDICTION WRONG. I wrote "I expect a gain at 0.60 and am genuinely
unsure about 0.75", reasoning that a head taking `fund up` on 96.4% of
offers and `fund down` on 0 of 114,650 must be pressing against the
ceiling. It is not. The 0.4339 mean with a 0.0075 spread was already
telling me that -- an equilibrium comfortably below the cap, not a
distribution piled against it -- and I read one-directional PRESSURE as
evidence of CONTACT.

WHAT ACTUALLY LIMITS IT

Not the ceiling: the remaining down-force. Even in the treatment,
austerity's research cut still exists -- moved to last, not removed --
and still fires. The slider settles where the head's +0.05 per taken
offer balances against that. Remove one down-force and the equilibrium
rises (0.4049 -> 0.4339, entry 197); the cap plays no part in either.

So the +21.9 finding is unaffected and better understood: it moved an
equilibrium, and there is no headroom above it to collect by relaxing a
limit that was never binding.

WORTH KEEPING ABOUT THE SHAPE OF THE ERROR

"Sweep the defaults" found a real gain once, on a constant that WAS
binding. This is the same move on a constant that is not, and the
distinguishing evidence was available before I ran anything: a
distribution pressed against a cap piles up at the cap, and mine sat at
87% of it with a tight spread. I had the number in entry 197 and used it
as motivation instead of as a test.

cap-75 still running and I expect it to be identical too; if it differs
that would mean allocation reaches 0.60 but not 0.50, which is not
possible, and would indicate the knob is broken rather than the finding
interesting.

## 201 -- the cap was unreachable because a different constant gates the offer

    cap 0.50 / 0.60 / 0.75    308 / 81.87% on all three, identical

Three values, one result. That is not "the cap is generous", it is "the
cap cannot be reached", and the reason is one line in the mask:

    validEconomy:  v[7] = alloc < 0.45f;   // fund up

Fund-up is only OFFERED while allocation is under 0.45. Each acceptance
adds 0.05. So allocation tops out at 0.4999 and the min(0.5f, ...) clamp
in case 7 is unreachable BY CONSTRUCTION -- dead code, provably, not
merely in practice.

I had the right shape and the wrong constant. "An unexamined number is
holding research down" was correct; it just was not the one I found
first, and the one I found first was the one with the comment-free
`std::min` that looked like a ceiling. The actual ceiling is a mask bar
0.05 below it, and the two together mean nobody can have noticed either,
because the visible one never fires.

Worth keeping: THREE IDENTICAL RESULTS WERE THE FINDING. A knob that
changes nothing at three settings is not a null result about the
quantity, it is evidence that something else binds first. I nearly filed
"the ceiling is not the constraint" and moved on -- entry 200 says
exactly that and is correct as far as it goes, but it stops one question
short. The follow-up question is not "so research is not limited" but
"so WHAT limits it", and the answer was fifty lines away.

OD_RESEARCH_BAR added, default 0.45, unchanged. Sweep queued behind the
reflex-pair arms.

## 202 -- the collapse happens at two, and the third barely registers

    hold-out C, against control 238 / 52.53%
    garrison alone            -2.43
    campaign alone            -9.37
    fortify alone             -6.20
    garrison+campaign        -19.13    62% superadditive
    +fortify (triple)        -20.56    only 1.43 worse than the pair

Marginal cost by suppression order: the second costs -16.70 and the third
-1.43. Twelve times. The damage is entirely front-loaded.

That is the same mechanism read from the other end. garrison and campaign
are ALTERNATIVES -- hold the border, or take it back -- so removing the
second one removes the fallback for the first and the country stops
defending. Once that has happened there is nothing left for fortify to
protect, so removing it too costs almost nothing. A country with no
defensive behaviour does not miss its forts.

So the cap of 1 is right for a reason stronger than the pair number
alone: the cliff is between one and two, not between two and three. A cap
of 2 would sit exactly on the edge it exists to prevent.

## 203 -- I attributed one model's behaviour to another, and built a hypothesis on it

    ECON a8 = research funding down
      working-tree model.bin       0 of 114,650      0.00%
      N24                    103,619 of 273,252     37.92%

Same game, same binary, same settings. Different model.

I have been saying, to the peer and in entries 199 and 201, that the head
"takes fund up on 96.4% of offers and fund down on 0 of 114,650" and
therefore "pushes in one direction, always" and is "pressing against the
ceiling continuously". That was measured on the working-tree model.bin,
which is NOT the model in a single bench number I have produced today.
Every arm passes N24. And N24 takes fund-down on 38% of offers.

The instrument was correct. The run loaded exactly what it said it
loaded. I described one model's behaviour as if it were a property of the
AI, and then reasoned about N24's benches from it.

WHAT SURVIVES AND WHAT DOES NOT

SURVIVES: the slider measurement (entry 197). That was N24 under both
arms, 0.4049 -> 0.4339, 6/6 runs, variance -65%. The reflexes really do
suppress research and removing them really does lift it. Nothing there
depended on the histogram.

DOES NOT: my account of WHY the equilibrium sits at 0.43. I said a
one-way ratchet pressing against a ceiling. For N24 it is a tug of war --
142,945 ups taken against 103,619 downs, a net upward drift of about
39,000 half-steps across all country-turns. The reflex cuts are one
downward force among two, not the only one, and the head supplies the
other.

The OD_RESEARCH_BAR experiment is still worth running -- if the head
wants up 97% of the time it is offered, raising the bar gives it room --
but the motivation I wrote for it in entry 201 is wrong in its
characterisation, and I would rather the knob's comment say so.

THIRD ATTRIBUTION FAILURE TODAY, ALL DIFFERENT

    --model ignored          measured the wrong model, output looked right
    generated vs bench maps  right quantity, wrong population
    this one                 right measurement, wrong subject

None was an instrument bug. Each was me saying something slightly wider
than what had been measured, and the width is invisible in the number.

## 204 -- I restored a snapshot over the user's own evening of training

data/ai/model.bin failed its hash check at 20:34. I preserved the new file,
restored my snapshot, and reported a mechanism: the game binary's
~AISystem saves unconditionally while only ServerMain and the eval path
set s_readOnlyModel, and determinism_check runs the game.

The peer TESTED it rather than agreeing: hash before and after one
determinism_check run, unchanged. determinism_check.sh:97 uses --eval-ai,
which is read-only. My mechanism was wrong.

What actually wrote it: the user, playing their game. aiLearning is on,
Game_TurnLogic constructs AISystem with the real model path, and the game
trains and saves by design. 4,198,838 bytes is a TRAINED model, which is
why it grew rather than changed shape.

So I reverted an evening of the user's own play to satisfy a guard I had
built. Put back: data/ai/model.bin is 835fee66 again, their version, and
both older files are preserved (model.bin.snapshot-sep7.bak,
model.bin.written-20-34.bak). Guard re-baselined to their current model.

THE GUARD WAS THE WRONG SHAPE FOR THE INSTRUCTION

"Never overwrite model.bin" meant do not clobber it with benches. It did
not mean the bytes are frozen -- the owner's normal use of their own game
rewrites it, and a hash cannot tell whose write it was. Mine reported the
owner's work as a violation, and I acted on that report.

The protection that actually works is the one already in place and which
I never needed to check: od_bench passes OD_EVAL_MODEL which sets
read-only, and my evals ran against a symlinked sandbox with a copied
ai/. My runs could not write that file. That was true all day.

A REAL DEFECT DID COME OUT OF IT, and it is the peer's find: aiLearning
gated LEARNING (AISystem.cpp:9554) and not SAVING. With the setting OFF
the game still rewrote model.bin on every quit -- nothing learned, the
trained model overwritten anyway. A player switching it off to protect
their model achieved the opposite. Fixed on their side, per turn rather
than at construction, because it is a live toggle.

So: my mechanism wrong, my restore wrong, and the report still worth
making. "The file changed and here is a path that could do it" was enough
to find a defect neither of us was looking for.

## 205 -- why the AI does not industrialise: two gates, neither of them the policy

The user asked why the AI does not build industry the way a player does.
Instrumented the econ mask to record WHY each action is absent, which the
per-country counter has tracked since it was written and nothing ever
reported:

    econ a1 (upgrade industry)
      offered            4,640    2.4%
      blocked by cash   11,597    6.0%
      NOT POSSIBLE     178,670   91.7%

The policy is not refusing. Industry is taken on 49.8% of the offers it
gets -- one of the highest rates in the module. It is simply not on the
menu.

TWO GATES IN SERIES, and the ratio matters:

    1. CAPACITY. nextIndustryBuy skips a province when lvl >= industryCap
       or lvl >= provinceIndustryCapacity. industryCap is
       clamp(max(3, researchedIndustryLevel), 1, 10) -- so 3 until building
       research raises it. The AI takes `focus army` on 98.4% of research
       picks and `focus buildings` on 0 of 7,311. So the cap stays at 3,
       every province reaches 3, and industry becomes impossible.

    2. CASH. Of the 16,237 checks where industry IS possible, 11,597 are
       blocked by the treasury -- 71%. So even with the cap raised, money
       would stop it seven times in ten.

The screenshot shows a player at III and IV across the map. The AI stops
at III by construction: it never researches the tech that would allow IV.

WHAT THE SOURCE ALREADY SAYS, and where it is now contradicted

The comment at case 9-11 records that forcing `want` to buildings was
measured and lost, reasoning "building research unlocks HIGHER levels of
things the AI already cannot afford to build". My gate numbers support
that reasoning as far as it goes -- 71% cash-blocked when possible -- but
they also show the cap is the FIRST binding constraint, not the second,
and the old measurement changed the research branch without touching the
cash gate. It tested "unlock more" alone, which the numbers predict fails.

The untested combination is unlock AND afford. Memory has the shape of
the second half already: a lump-sum cost gate on an AI action is a
prohibition rather than a price, because AI treasuries run at zero.

## 206 -- unlock and afford, tested together for the first time

Four arms on hold-out C, all on top of the validated research-ratchet
treatment, because that is the config I would ship:

    ind-base     treatment only                    inertness, must be 308/81.87
    ind-unlock   OD_FOCUS_BRANCH=buildings         raises industryCap above 3
    ind-afford   OD_INDUSTRY_REFLEX=1              takes every possible buy
    ind-both     both

WHY THESE TWO LEVERS. The gate numbers say industry fails at two places
in series: impossible on 91.7% of checks because every province is at the
cap of 3, and cash-blocked on 71% of the remainder. FOCUS is
{buildings, army, navy} and the head takes army on 98.4% of research
picks and buildings on 1.6%, so the cap never rises. That is the unlock
half. The afford half is not "give it money" -- it is that industry is
sampled on 2.4% of econ decisions and taken on half of those, so most
affordable opportunities are never offered to the policy at all. The
reflex takes them on a cadence, the way fortifyReflex does for forts.

PREDICTION, before the numbers:

  unlock alone   LOSES or is flat. This is close to what the source says
                 was already measured ("building research unlocks higher
                 levels of things the AI cannot afford"), and my own
                 numbers agree that 71% of possible buys are cash-blocked.
                 Raising a ceiling nobody can reach should do little, and
                 it costs army research, which is the cheaper branch and
                 the one that wins fights.

  afford alone   SMALL GAIN. It converts existing opportunities into
                 purchases without creating new ones. Bounded by the 8.3%
                 of checks where industry is possible at all.

  both           THE INTERESTING ONE. If the two gates are genuinely in
                 series, removing only one leaves the other binding and
                 the combination should be worth more than the sum -- the
                 same shape as siege+austerity this morning (+10.1 and
                 +13.9 giving +36.0). If both arms are flat and the
                 combination is flat, the honest read is that industry
                 is not what limits this AI and the user's observation,
                 while correct about the behaviour, does not point at a
                 lever worth pulling.

Stated plainly because I have been wrong twice today predicting from
mechanism: the ceiling sweep and the pair cap both went against me.

## 207 -- an inertness check that nearly passed

    ho-both   (11:41 binary)   81.8667   27.57 13.03 21.77 18.03 1.03 0.43
    cap-base  (20:33)          81.8667   identical
    ind-base  (01:45)          81.9667   27.57 13.03 21.77 18.13 1.03 0.43
                                                              ^^^^^

One seat, +0.10 land, five seats byte-identical, rating unchanged at 308.

I cannot attribute it, and I am recording that rather than picking the
most likely culprit:

  - the peer's qbias hook short-circuits on `if (want == 0.0f) continue;`
    so `any` is never set with the module off -- genuinely inert
  - their ten reflex gates call llmSuppressesReflex, which returns false;
    short-circuit order means the reflex still runs
  - OD_RESEARCH_BAR compares against a static const float initialised to
    0.45f, which is the same comparison as the literal it replaced
  - the gate instrumentation only increments counters behind a static
    bool that is false when OD_ACT_HIST is unset

Most plausible remaining explanation is float contraction changing with
code layout -- more code, different inlining, a fused multiply-add where
there was not one -- and a 400-turn chaotic sim amplifying a last-bit
difference into one seat. That is consistent with earlier rebuilds
reproducing EXACTLY (slider-bench-ctl == sb2-ctl across binaries): the
compiler was stable then and the added code was smaller.

WHAT THIS DOES AND DOES NOT COST

Does not: the four industry arms all run on THIS binary, so the
comparison is internally valid and that is what the experiment needs.

Does: I can no longer cross-compare to yesterday's numbers, and my
"byte-identical inertness" proofs are only as strong as compiler
stability across an edit. Two of today's cleanest results -- the
reflexAblated two-sided proof and the shipUpkeep refactor -- rested on
exact reproduction. They are not invalidated (they reproduced at the
time, on their own binaries) but the METHOD has a floor I had not
noticed: exact reproduction proves inertness only when it happens, and
its absence does not prove non-inertness.

## 208 -- unlock and afford: the gates ARE in series, and it still is not a win

    hold-out C, on top of the research-ratchet treatment, same binary
    ind-base     308 / 81.97%
    ind-unlock   285 / 76.33%    -5.64   force buildings research
    ind-afford   230 / 52.57%   -29.40   industry reflex takes every buy
    ind-both     317 / 84.77%    +2.80   both

    sum of parts -35.04, together +2.80 -- a swing of +37.84.

THE SERIES HYPOTHESIS IS CONFIRMED. Two changes that are individually
harmful are together roughly neutral, because each alone leaves the other
gate binding: unlock raises a ceiling the treasury cannot reach, afford
spends the treasury on marginal upgrades under a ceiling of 3. Remove
both and the money buys something worth having. That is the third
composition result today and the largest swing of the three.

BUT IT IS NOT AN IMPROVEMENT, and the shape says so plainly:

    seat        base   unlock   afford    both
    USA:rung   21.77    +0.27    -5.93  +10.47   <- 374% of the net
    FRA:rung   27.57    -5.13   -14.00   -5.97
    CHN:mod    18.13    -3.10    -2.47   -2.40
    SWE:rung   13.03   -10.63   -10.03   -1.50
    FRA:rush    1.03   +13.00    +3.07   +2.27
    NOR:hood    0.43    -0.03    -0.03   -0.07

    2 of 6 seats up. Four get worse. The +2.80 is one seat outweighing
    four losses, which is exactly what retired manpower and redeploy.

So the honest statement is: removing both gates CANCELS the harm of
removing either, landing near neutral. It does not buy anything.

MY PREDICTIONS: 1 of 3. Unlock losing was right. Afford was predicted as
a small gain and cost 29 points -- the largest single miss I have made.
Both was predicted as the interesting one and is, though not in the
direction the prediction implied.

THE CAVEAT THAT MATTERS FOR THE USER'S QUESTION. N24 was trained in a
game where industryCap was 3 and industry was possible on 8.3% of
checks. It has never seen an economy where high industry is reachable, so
it has no policy for exploiting one -- the same frozen-policy argument
that made the naval repricing unmeasurable. "The AI does not industrialise
like a player" is TRUE and now fully explained; "making it industrialise
would help" is not supported, and cannot be tested properly against a
model trained under the old ceiling.

## 209 -- the shipped model and N24 cannot be benched on one binary

    prog-shipped   15 / 4.17%   under MY binary
    N24           238 / 52.53%  under MY binary

That gap is not a measurement of the shipped model. My binary logs, on
loading it:

    [AI] diplomacy head widened to one pair per request kind;
         the trained pair was copied to all 7

The shipped model is narrow ({320,2}); my tree runs
DIPLO_ACTIONS x OFFER_KINDS. The compatibility path replicates its single
trained pair across seven request kinds, so it answers a ceasefire with
weights trained on an undifferentiated average -- precisely the defect the
per-kind split fixed, recorded in this project as 108 -> 160 rating.

AND IT IS SYMMETRIC, which is the part that matters. N24 under HEAD's
binary is the peer's bug from yesterday: the wide blobs fail to parse,
the migration Xavier-initialises, and two trained heads are replaced with
random weights. Each model is crippled by the other's architecture.

SO THERE IS NO SINGLE-BINARY COMPARISON. Any number putting these two
side by side on one build measures the migration, not the models. That
retroactively explains the "N24 beats shipped 226 to 85" figure I have
been carrying: same confound, unverifiable now because it predates the
provenance fields.

WHAT IS FAIR: a PACKAGE comparison -- each model on the binary it was
trained for, same seeds, same turns. Binary and model both differ, which
is a confound, but it is the RIGHT confound: it measures what a player
would actually experience between the two versions. Queued: HEAD built in
a detached worktree running the shipped model, against N24 on mine.

## 210 -- where the AI actually stands, measured as packages

Each model on the binary it was trained for, hold-out set C, 400 turns:

    configuration                          land   worst  seats>10%  wiped
    RELEASED (HEAD binary + its model)    12.27%   0.03%      0        3
    N24 on the current binary             50.80%   0.33%      2        1
    N24 + the research-ratchet change     81.97%   0.43%      4        1

    x4.1 from the model alone; x6.7 with the change.

Per seat, released against best:

    FRA:rung    5.77% -> 27.57%
    USA:rung    2.90% -> 21.77%
    CHN:mod     0.03% -> 18.13%    annihilated -> a major power
    SWE:rung    2.90% -> 13.03%
    FRA:rush    0.23% ->  1.03%    still nearly wiped
    NOR:hood    0.43% ->  0.43%    unchanged

The released AI is wiped out on three of six seats and holds more than a
tenth of the world on NONE. The recommended package holds four seats
above 10% and loses only one.

WHAT THIS COMPARISON IS AND IS NOT. Binary and model both differ, which
is a confound and the RIGHT one: it is the difference a player would
experience between the released version and what I am recommending. It is
NOT a model-versus-model result -- entry 209 established those cannot
share a binary, because each is crippled by the other's diplomacy
architecture.

HONEST LIMITS:
  - one hold-out seed set at 400 turns. The research-ratchet half is
    validated on three sets; this package number is one.
  - FRA under a rusher is still nearly annihilated in both. The AI has
    not learned to survive a determined early attacker, and nothing this
    session moved it.
  - NOR is unchanged at 0.43%, so the tiny-country case is untouched.
  - nothing is committed. Realising this needs the N24 model file AND
    two knob defaults flipped.

## 211 -- estimating against v1.0.8a, three weeks and many rule changes back

A direct comparison is impossible: --bench-seat does not exist in
v1.0.8a, and its model is 6,324,962 bytes against today's 4.1M, a
different architecture that will not cross-load. What both versions DO
share is --eval-ai against the scripted rung, each in its own game.

    3 maps x 400 turns, seed 20260801, difficulty 3, each on its own build

    metric (per 1k country-turns)      v1.0.8a      current (N24)
    countries still alive               97.9%          81.6%
    largest power, share of world        9.1%          13.3%
    war declarations                     2.13           4.22
    ceasefire offers                     0.00           0.91
    rebellions                           5.60           3.09
    research nodes                      81.55         123.13
    pacts proposed                       0.00              ?
    conciliations                        0.00              ?
    calming policies                     0.00              ?

THE QUALITATIVE FINDING IS BIGGER THAN THE QUANTITATIVE ONE. In v1.0.8a
the AI never offered a ceasefire, never proposed a pact, and never took a
single minority action -- conciliate, repress and calm are all exactly
0.00 across three 400-turn games. Those subsystems existed and the policy
did not touch them. Today ceasefires and pacts happen.

WHAT THE NUMBERS SAY, read carefully:
  - it FIGHTS twice as much (2.13 -> 4.22 declarations) and now
    negotiates its way out (0.00 -> 0.91 ceasefires)
  - it CONQUERS: 2.1% of countries eliminated becomes 18.4%
  - the largest empire grows by half (9.1% -> 13.3%)
  - it RESEARCHES half again as much (81.55 -> 123.13 nodes)
  - and it is more stable: rebellions 5.60 -> 3.09

CAVEATS, and they are not small. The scripted rung is code and changed
too, so the yardstick moved. Map generation changed. Naval upkeep was
repriced sixfold yesterday. This is an estimate on the one axis both
builds can express, and I would not defend any single number to better
than its first digit -- but "the AI barely conquered anything and had
three dead subsystems" versus "it fights, negotiates, governs and takes
a fifth of the field" is a difference too large for the caveats to erase.

ONE READING TRAP, noted because I nearly fell into it. With the
research-ratchet change ON, largest power DROPS 13.3% -> 9.2% and
survival RISES 81.6% -> 85.0%. That is not the change failing. In this
eval EVERY country gets it, so it lifts the whole field and flattens
concentration -- the same reason a rule that helps everyone lowers every
seat's score on the seat bench. The aggregate is the right instrument for
comparing VERSIONS and the wrong one for measuring that change.

## 212 -- the version comparison, completed

    metric (per 1k country-turns)     v1.0.8a    current (N24)
    countries still alive               97.9%          81.6%
    largest power                        9.1%          13.3%
    war declarations                     2.13           4.22
    ceasefire offers                     0.00           0.91
    pacts proposed                       0.00          18.74
    hulls scrapped                       0.00           1.16
    conciliations                        0.00          31.20
    repressions                          0.00           0.00
    calming policies                     0.00           7.90
    rebellions                           5.60           3.09
    research nodes                      81.55         123.13

FIVE SUBSYSTEMS WENT FROM EXACTLY ZERO TO ACTIVE: ceasefires, pacts, ship
scrapping, minority conciliation, calming policies. Not "rare" -- zero,
across three 400-turn games. Diplomacy is the largest single change:
0.00 to 18.74 pacts proposed per thousand country-turns.

REPRESSIONS ARE STILL 0.00 IN BOTH. The AI has never once repressed a
minority in either version. That is the one-way ratchet the austerity
comment describes -- conciliate is taken on 78% of offers and repress on
0.2% -- and it is unchanged in three weeks. It is also the reason the
minority bill runs at 25.58 per country-turn against a 27.71 standing
bill: the AI can only ever spend more on minorities, never less.

So the honest summary of three weeks: the AI learned to negotiate, to
govern its minorities, to maintain a fleet and to fight twice as often
while ending wars it cannot win. It did not learn that repression is an
option, and it still cannot survive a determined rusher.

## 213 -- why the AI never represses: the credit horizon, not the mask

    POLITICS a9  conciliate   3,458 of 11,843 offers   29.20%
    POLITICS a10 repress          0 of 52,468 offers    0.00%

NOT A MASKING PROBLEM. Repress is the MORE available action -- 52,468
offers against conciliate's 11,843 -- because validPolitics gates
conciliate on socialRoom (a budget check) and repress on nothing but
"is there room to tighten". Repression saves money, so it never answers
to the budget. It is on the menu nearly every turn and has never once
been taken.

NOT THE OLD BUG EITHER. The "repress: already hardest" no-op, which the
source says was "teaching the politics head that the action is safe and
free", was fixed in c813b49 on 2026-08-01. N24 was trained on 2026-09-04,
after it.

THE MECHANISM IS IN THE SOURCE, one line, describing conciliate and
fund-up together:

    "both commit income PERMANENTLY while costing nothing at the moment
     they are chosen"

Conciliate: free when chosen, bills forever.
Repress:    costs when chosen (alignment down, unrest up, rebellion
            risk within a few turns), saves forever after.

N_STEP is 12. Repression's cost lands INSIDE the credit window;
conciliation's cost is a per-turn drip whose damage accrues over hundreds
of turns, entirely OUTSIDE it. The policy is optimising precisely what it
can see.

WHY I BELIEVE THIS RATHER THAN "REFUSAL IS CORRECT PLAY": it explains TWO
ratchets with one mechanism. The same note pairs conciliate 78.1%/0.2%
with fund-up 81.1%/0.4% -- same shape, same asymmetry, same explanation.
A story that only explained repression would be a story about minorities;
this one predicts the research slider too, and the research slider is
where the only validated gain of this session came from.

IT ALSO GIVES N_STEP A PREDICTION. I flagged the reward horizon in the
first hours of this loop as the last untested training hypothesis and
never ran it, because it needs a retrain rather than a knob. It now has a
falsifiable consequence: a longer horizon should make the AI conciliate
LESS and repress MORE. If it does not, the horizon story is wrong and the
refusal is genuinely about unrest being lethal.

## 214 -- an arm that died without leaving a trace

s1-r1 produced no printed line and no stored row. The chain moved on to
r2 and r3 without stopping, and I only noticed because the results jumped
from the control straight to round 2.

CAUSE: my run() helper pipes stderr into a grep that keeps only
"OD BENCH|land ". Any failure -- crash, kill, traceback -- matches
neither pattern and is discarded. The harness reports success or silence,
and silence looks like a gap rather than a failure.

That is the third time today the same shape has bitten: I filtered out
the "[EVAL] Model:" line that would have revealed the ignored --model
flag, I filtered the gate dump into a buffer od_bench discards, and now
this. Each time the fix was obvious afterwards and each time the filter
was written to make the output readable.

Round 1 itself is fine: run by hand at 40 turns it scores 125 / 30.90%,
loads without a migration warning, and has a distinct hash. So the arm
died of something transient -- most likely resource contention, since I
was running the v1.0.8a build and two evals against the same two odlock
slots at that moment.

Re-queued with the filter removed, to run after stage 1 finishes.

WHAT I WOULD DO DIFFERENTLY: the store is the record, not the log. This
project already has "verify the stored row, not the printed score"
written down from a case where the printing failed AFTER a good
measurement. This is the same rule from the other side -- absence of a
row means the measurement did not happen, and a chain should check for
that rather than trusting that every iteration produced one.

## 215 -- the retrain with both gates open: eight rounds, all worse

    one seed (909091), 400 turns, benched under the config they trained for
    N24 (parent)   270 / 62.10%
    r7             151 / 28.10%   best checkpoint
    r3             132 / 26.90%
    r6              77 / 14.80%
    r4              71 / 13.20%
    r5              90 / 10.60%
    r8              20 /  6.70%
    r2              45 /  3.00%
    r1                            arm died silently, re-running

The best of eight rounds holds 45% of what the parent holds. Not one came
close. This is not erosion at the margin, it is the model being taken
apart: r2 at 3.00% and r8 at 6.70% are near-annihilation.

CONSISTENT WITH THE RECORD. This project has self-play from a strong
parent losing 44-60% of the map on both hold-out sets, and 12 of 13 steps
eroding at full rate. I ran at quarter rate, which the record says delays
instability rather than preventing it, and eight rounds was enough.

THE LEAGUE SIGNAL WAS WORSE THAN USELESS. r7 posted the worst league
result of the run -- opponent at 39.7 provinces per country against our
4.0 -- and benches BEST of the eight. r3, which lost 0 of 4, benches
second. Selecting on the trainer's own fitness number would have discarded
the leading candidate. That is the recorded PBT failure reproduced exactly.

WHAT IT DOES NOT YET SAY. "Training made it worse" is established.
"Industry is useless to a model that learned it" is NOT -- the training
may have taught industry use while wrecking everything else. Queued a
histogram comparing N24 and r7 on econ a1 picks and on the capacity/cash
gates, which separates those two claims.

## 216 -- the trained model DOES industrialise, and plays far worse

    both models under the gates-open config, 2 maps x 400 turns
                        N24        r7 (best checkpoint)
    a1 share of picks   1.29%      4.93%      nearly 4x
    taken when offered  60.1%      73.6%
    industry possible   16.5%      24.7%
    bench land         62.10%     28.10%

So the training did what it was asked to do. r7 wants industry more, is
offered it more, and takes it more often. It also holds less than half
the world N24 holds.

WHICH ANSWERS THE ORIGINAL QUESTION AS FAR AS IT CAN BE ANSWERED. Two
independent routes now agree that making this AI industrialise does not
make it win:

  1. FORCE it on a trained model (unlock+afford on N24): +2.80 land, 2 of
     6 seats, one seat at 374% of the net -- an artefact, not a gain.
  2. TRAIN a model to want it: it wants it, builds 4x more, and plays at
     45% of the parent.

WHAT I CANNOT SEPARATE, and will not pretend to. All eight checkpoints
degraded, including ones that industrialise less, so r7's poor play
cannot be attributed to industry specifically -- self-play from a strong
parent wrecks this model whatever it is taught. The honest statement is
"training taught industry use AND training degraded the model", with the
causal link between industry and the degradation unmeasured.

Separating them properly would need a control arm: the same eight rounds
with the gates CLOSED, same seeds, same rate. If those checkpoints
degrade just as far, industry is exonerated and self-play is the culprit.
That is 80 minutes of training plus nine bench arms, and it is the
experiment I would run next if the question matters enough.

## 217 -- the control that decides whether training is a dead end

The gates-open retrain degraded every one of eight checkpoints, best at
45% of the parent. I cannot say whether that was INDUSTRY or SELF-PLAY,
because I changed both at once and this project's record says self-play
from a strong parent erodes on its own.

Running the arm that separates them: identical parent (N24), identical
seeds (4243..4250), identical rate (OD_LR_SCALE=0.25), identical eight
rounds, identical research-ratchet config -- and the two industry gates
CLOSED. Benched under the config it trained for, as before.

    if the control degrades JUST AS FAR
        industry is exonerated, and the finding is bigger than industry:
        self-play from this parent destroys the model whatever it is
        taught, which means training is a dead end and every future gain
        has to come from hand-written rules.
    if the control degrades LESS
        teaching industry specifically is what hurt, and the gates are a
        bad idea rather than training being a bad tool.

I have written down the second outcome as the one I expect less. Every
training run in this project's history has eroded -- two of mine
yesterday, the peer's today, twelve of thirteen steps in the record --
across completely different objectives. A mechanism that indifferent to
what is being taught is more likely to be the method than the lesson.

Note this also re-uses the failure from entry 214: the bench chain now
pipes tail -4 rather than grepping for success patterns, so an arm that
dies leaves its error in the log instead of a silent gap.

## 218 -- VERDICT: the objective is irrelevant, self-play is the cause

    round      gates OPEN    gates CLOSED
    r1               22%            19%
    r2                5%            17%
    r3               43%             7%
    r4               21%            20%
    r5               17%             3%
    r6               24%            15%
    r7               45%            24%
    r8               11%            16%
    mean             24%            15%
    best             45%            24%

    (each arm as a share of N24 measured under its OWN config:
     62.10% gates open, 81.90% gates closed)

THE CONTROL DEGRADED MORE. Sixteen checkpoints, two unrelated
objectives, not one above 45% of its parent. Whatever destroys this model
does not care what it is being taught.

SO INDUSTRY IS EXONERATED, and the finding is the bigger one I hoped for
rather than the smaller one: the collapse in entry 215 was not caused by
teaching industry. It was caused by training.

WHAT THIS MEANS FOR THE LOOP. Self-play from a strong parent has now
failed in this project: twice by me yesterday, once by the peer, twelve
of thirteen steps in the older record, and sixteen checkpoints across two
objectives today. It has NEVER produced a checkpoint that beats its
parent. That is not a tool that sometimes fails; on this evidence it is
not a tool for improving N24 at all.

Which retrospectively explains the shape of the whole session. Twenty
three invented rules failed. One found rule succeeded (+21.9 land, three
seed sets). Every training run failed. The only thing that has ever
worked here is reading the game's own code for a rule that is wrong, and
the audit found one by asking which shipped rules had never been
re-measured.

WHAT A RETRAIN IS STILL GOOD FOR: answering questions the frozen policy
cannot. "Does the head represent this action at all" needed r7, and r7
answered it -- 4x the industry picks. Run training to learn something,
expect a worse model, and never to ship the result.

ONE CAVEAT KEPT DELIBERATELY: one seed, one rate, one parent. A different
learning rate or a fresh-initialised model might behave differently. What
is established is narrower and still decisive for the loop: N24 cannot be
improved by more of this training.

## 219 -- the research bar: 0.45 beats more room, and more room stops mattering

    hold-out C, 400 turns, on top of the research-ratchet treatment
    bar 0.45 (default)   308 / 81.97%
    bar 0.55             290 / 73.37%   -8.60 land
    bar 0.65             290 / 73.37%   IDENTICAL to 0.55

Two findings in three arms.

FIRST: raising the bar COSTS 8.6 points of the world. The hypothesis in
entry 201 -- that an unexamined constant was holding research down -- is
rejected. Giving the head more room makes it worse.

SECOND: 0.55 and 0.65 are byte-identical, so allocation never reaches
0.55 and the bar stops binding somewhere between. That is the same
signature the 0.5 cap gave, one level up: the equilibrium re-forms below
whatever ceiling you provide, and only the ceilings BELOW the equilibrium
have any effect at all.

WHICH MAKES THE RESEARCH-RATCHET FINDING SHARPER, not weaker. That change
was worth +21.9 land and I described it as "letting research rise". It is
not. Both results together say: the head picks a level, the reflexes were
dragging it BELOW that level, and removing them let it return to where it
wanted to be. Pushing it ABOVE where it wants to be costs 8.6 points.

So the value was never in more research. It was in not overriding the
policy's own choice -- which is the same lesson the naval, attack and
industry forcing experiments all produced from the other direction, and
the fourth independent time this session that overriding a trained
policy's preference has measured worse than leaving it alone.

Running 0.35 and 0.40 to see whether 0.45 is a genuine optimum or merely
the top of a plateau.

## 220 -- the research bar is a sharp optimum, and dangerously sensitive

    hold-out C, 400 turns, on the research-ratchet treatment
    bar 0.35    232 / 53.67%
    bar 0.40    242 / 50.07%
    bar 0.45    308 / 81.97%   <- the shipped default
    bar 0.55    290 / 73.37%
    bar 0.65    290 / 73.37%   identical to 0.55

0.45 IS THE BEST OF FIVE, and by a wide margin: +28.3 over 0.40 and
+8.6 over 0.55. The constant I went looking at because it had no comment
and no measurement turns out to be well chosen.

A 0.05 STEP COSTS 32 POINTS OF THE WORLD. That is the number anyone
editing this line needs: 0.40 and 0.45 differ by one increment of the
head's own step size and by nearly a third of the map. If this were
tuned by hand it was tuned carefully; if it was picked by eye it was
lucky.

WHAT I CANNOT EXPLAIN, and am not going to dress up. 0.40 (50.07%) is
WORSE than 0.35 (53.67%). The curve is not monotone below the optimum. I
can think of reasons -- a tech tier that becomes reachable at a
particular allocation, or simply that three seeds on six volatile seats
cannot resolve 3.6 points -- and I have no evidence for either. The 0.45
figure is the one I trust most because it has been measured three times
on this binary and reproduced exactly each time; the other four are
single measurements.

TAKEN WITH ENTRY 219, THE PICTURE IS CONSISTENT. Above the head's
preferred level, more room is worth -8.6. Below it, less room is worth
-28.3. The default sits at the top. Every direction away from what the
policy would choose for itself is a loss, which is now the fifth
independent time this session that overriding the trained policy has
measured worse than leaving it alone.

Recorded as: the bar is NOT a lever. It is a correctly set constant, and
the +21.9 research-ratchet gain came entirely from stopping OTHER rules
overriding the policy, not from moving the policy's own ceiling.

## 221 -- 215,000 refused executions, and two wrong diagnoses

Instrumented every didNothing site. Two maps, 400 turns:

    215,515 refused executions
    108,650   50.4%  recruit: too poor/small
     48,422   22.5%  bombard: nothing in range
     23,841   11.1%  bombard: no ammo
     16,045    7.4%  reinforce: nothing to move

Half of every refusal in the AI is one line. Each is a turn where the
policy chose recruit, the rule declined, and the decision still fed a
gradient -- the exact shape of the repress no-op the source describes as
"teaching the politics head that the action is safe and free".

DIAGNOSIS 1, WRONG: the mask asks country POPULATION while the executor
asks the chosen province's availableManpower. Tightened the mask to
require some province able to yield 1000 men.
    108,650 -> 104,895.  Barely moved.

DIAGNOSIS 2, WRONG: the fallback picks the most POPULOUS province, which
is the first to be conscripted out. Made it pick by availableManpower.
    108,650 -> 106,984.  Barely moved.

Why 2 failed is visible in the code I had already read: the fallback only
runs when there are NO frontiers. A country at war picks its province by
frontier score, and most countries here are at war, so the branch I fixed
is not the branch being taken.

STOPPING THE GUESSING. Two mechanisms proposed from reading, two
refuted by measurement, and the second was refuted by a fact present in
the same twenty lines I had quoted. The next step is not a third
mechanism; it is splitting the counter so the code says which branch
chose the province and which of the two conditions failed -- manpower or
budget. That is one build and one eval, and it ends the guessing.

Worth noting the instrument keeps earning its cost: it caught both wrong
fixes within minutes, and the bench numbers alone would not have. rm-on
even LOOKED plausible (+3.73 land) while removing almost none of the
waste it was built to remove.

## 222 -- refusal count does not track outcome, so the whole thread was built on sand

    rm-on  (mask tightened)     108,650 -> 104,895 refusals   land +3.73
    rp-on  (province choice)    108,650 -> 106,984 refusals   land -41.64

Two changes, near-identical effect on the thing I was optimising, and
forty-five points of world between them. The refusal count and the
outcome are DECOUPLED.

Which means the premise was wrong, not just the fixes. I found 215,515
refused executions, reasoned that each was a wasted turn feeding a false
gradient, and treated reducing them as obviously valuable. The evidence
says a change can cut refusals and gain 4 points, or leave refusals alone
and lose 42. Refusals are not a lever and were never shown to be a cost.

WHERE THE ARGUMENT WENT WRONG. The source's repress no-op WAS a real
defect, and I generalised from it without checking the difference: repress
returned "already hardest" EVERY turn for those countries -- an action
that could never succeed. Recruit succeeds much of the time. A head that
learns "recruit works about half the time" has learned something TRUE. A
50% refusal rate is not evidence of a bug; it is what a contested action
looks like.

AND THE PROVINCE CHOICE WAS LOAD-BEARING. Picking the most POPULOUS
province rather than the one with the most conscriptable men looked like
an obvious defect. Changing it cost 41.64 points of the world. That is
the sixth time this session an apparent defect has turned out to be doing
work -- ships, attacks, industry, the research bar in both directions,
and now this.

WHAT I SHOULD HAVE DONE FIRST: asked whether the metric I was about to
optimise had ever been shown to correlate with the outcome. It had not. I
built an instrument, got a striking number, and let the number's size
stand in for its relevance -- 215,515 is impressive and means nothing on
its own.

Both knobs stay OFF. The refusal histogram stays, because it is a good
diagnostic for "can this action ever succeed" even though it is a bad
optimisation target.

## 223 -- the disabled reflexes are correctly disabled, all four

    hold-out C, 400 turns, against dr-base 308 / 81.97%
    enable pacification   193 / 40.33%   -41.64
    enable callToArms     234 / 53.10%   -28.87
    enable withdraw       296 / 73.87%    -8.10
    enable peace          338 / 85.83%    +3.86   3/6 seats, 137% one seat

No wrong-way knob. Three are decisively right to be off and the fourth
gains only in aggregate, on a redistribution that fails the shape test
(CHN -5.30 paid for by USA +4.97 and FRA:rush +4.57).

TAKEN WITH THE EARLIER AUDIT, THE WHOLE REFLEX SET IS NOW MEASURED IN
BOTH DIRECTIONS on the current instrument:

    7 enabled     all earn their place (garrison, campaign, fortify on
                  two-plus seed sets; manpower and redeploy neutral-to-
                  positive to keep; siege's research half is the
                  exception and is the shipped finding)
    4 disabled    all correctly disabled

Eleven rules, eleven correct defaults, with one exception that is already
the session's recommendation. That is a better-tuned rule set than I
expected to find, and it retires "look for another wrong-way knob" as a
strategy -- there are no more to find in the reflex layer.

ONE THING WORTH FOLLOWING, though. peaceReflex's stated verdict was a
LOSS on 4 of 4 models (265 -> 199). On the current build it GAINS 30
rating. The verdict reversed completely, which says the old measurement
does not transfer even when it looked strong -- and it is the second
reversal tonight after withdraw's 7-point margin turned out to be right
for the wrong reason.

AND A LEAD: FRA:rush moved +4.57 here and +4.50 under the recruit-mask
fix. Two unrelated changes both lift the rusher seat by about the same
amount and both fail everywhere else. That seat is RESPONSIVE and its
gains do not generalise, which is the signature of something that wants a
conditional rule -- fire only when badly outmatched -- rather than a
global one. It is also the seat that has been near-annihilated in every
configuration all session, so it is where the remaining upside is.

## 224 -- gating peace on weakness destroys the thing it was built to keep

    seat            off   ungated   bar1.5   bar2.5
    FRA:rung      27.57     29.07    22.50    31.93
    SWE:rung      13.03     11.17     2.40     3.10
    USA:rung      21.77     26.73    19.93    12.87
    CHN:mod       18.13     12.83    19.27    13.67
    FRA:rush       1.03      5.60     0.97     1.73
    NOR:hood       0.43      0.43     0.33     0.40
    TOTAL         81.97     85.83    65.40    63.70

PREDICTION WRONG, AND INVERTED. I reasoned that ending side wars is
survival when overrun and a forfeited conquest when winning, so gating on
"enemies outnumber us" should keep FRA:rush's +4.57 and drop CHN's -5.30.
The gate took FRA:rush from 5.60 to 0.97 -- it destroyed precisely the
gain it was built to isolate -- and cost 16-18 points overall, worse than
both the ungated rule AND no rule at all.

WHAT THE DATA SAYS INSTEAD. The rusher seat's benefit comes from ending
side wars BEFORE it is outmatched, not after. My gate only fires once the
enemy army already exceeds ours by 1.5x, which in this game is past the
point where a ceasefire helps. Peace is worth having PREVENTIVELY and
worth little REMEDIALLY, which is the opposite of the intuition that a
losing country wants out.

That also explains SWE collapsing 13.03 -> 2.40 under the gate: a country
that would have tidied up its side wars early now holds them until it is
losing badly, and by then the offer buys nothing.

STATUS OF THE THREAD. Ungated peaceReflex remains +3.86 land and still
fails the shape test (3 of 6 seats, one seat 137% of the net), so it is
not shippable. Gating it is decisively worse. The knob stays at its
default of 0 and OD_PEACE_REFLEX stays off.

WHAT I WOULD TRY NEXT IF I CHASE THIS. The insight is preventive-vs-
remedial, so the conditional to test is not strength but TIME or COUNT --
fire while the country still has few wars, or early in one. That is a
different shape of rule from anything measured tonight, and the FRA:rush
seat is still the only place with visible headroom.

## 225 -- five narrowings of peaceReflex, five losses

    ungated            85.83%   +3.86   best form measured
    off                81.97%    ----
    wars >= 4          75.30%   -6.67
    strength 1.5:1     65.40%  -16.57
    strength 2.5:1     63.70%  -18.27
    wars >= 3          51.83%  -30.14

Every conditional form is worse than the ungated rule, and four of five
are worse than not having the rule at all. Both the strength gate and the
war-count gate were motivated, measured, and wrong.

THE PATTERN IS NOW GENERAL FOR THIS SESSION. Narrowing a rule's firing
condition has not helped once:

    siege gating (share 0.75 / 0.50)   worse than either end
    peace strength bar (2 values)      worse than both ends
    peace war count (2 values)         worse than both ends
    austerity research-last ALONE      lost the floor; only the pair worked

Nine attempts to make a rule fire more selectively; nine losses. Against
that, the two changes that HELPED both removed a condition rather than
adding one -- stop siege cutting research, stop austerity cutting it
first.

I do not have a mechanism for this and I am not going to invent one. As a
working heuristic it is strong enough to change what I try next: in this
AI, conditions are expensive. A rule that fires broadly and is sometimes
wrong beats a rule that fires narrowly and is usually right, at least
everywhere I have measured.

PEACE THREAD CLOSED. Ungated peaceReflex is +3.86 land and still fails
the breadth test (3 of 6 seats, one seat 137% of the net), so it is not
shippable. Every gated form is worse. OD_PEACE_REFLEX stays off,
OD_PEACE_BAR and OD_PEACE_WARS stay at their inert defaults.

## 226 -- the shippable half, measured on HEAD, is worth nothing

    clean HEAD worktree + the shipped 30 Aug model
    3 hold-out seeds, 400 turns, difficulty 3, same binary both arms

    research cut FIRST  (HEAD as-is)   113 / 14.53%
    research cut LAST   (the change)    94 / 14.27%
                                       -19 rating, -0.26 land

No gain. The one piece of the +21.9 that HEAD can physically accept is
neutral-to-slightly-negative in HEAD's world.

WHICH IS THE PREDICTION, NOT A SURPRISE. I said three hours ago that a
number measured on a fifteen-reflex build is not a prediction about a
six-reflex one, and refused to ship the 21.9 on that basis. This is that
argument coming back with a number attached: the austerity reordering is
worth +13.86 when siege, campaign and the rest are present and worth
-0.26 when they are not.

It also explains WHY the pair was superadditive. Cutting research last
only helps if something else is also failing to cut it -- with siege
still ratcheting the slider down every besieged turn, reordering austerity
alone moves an equilibrium that siege immediately re-lowers. In HEAD there
is no siege reflex at all, so there is no ratchet to relieve, and the
reordering is just a different order of the same cuts.

CONSEQUENCES:

  1. There is no small honest claim for 1.2.0a. The release note is "no
     measurable AI change", which is what I told the peer before running
     this and now have the number for.
  2. The +21.9 is available ONLY as part of the branch. It is not a knob
     that can be lifted out, and I now have direct evidence rather than
     an argument from composition.
  3. Shipping the research knob alone into 1.2.0a would have been a
     change that does nothing, sold as an improvement. The clean-worktree
     bench is the only reason I know that.

Four hours of extraction and verification to arrive at "do not ship it",
and it was worth every arm: the alternative was a release note claiming
+21.9 for a change measured at -0.26 in the build it was going into.

## 227 -- the code and the model cannot ship apart

    pre-branch code + 30 Aug model              113 / 14.53%
    my code + 30 Aug model, NEW defaults         31 /  9.03%
    my code + 30 Aug model, OLD defaults         15 /  4.17%

The architecture is the cause, not the defaults -- and the research
defaults MITIGATE the damage rather than causing it. So the obvious
partial revert (drop c06b3cc, keep the instruments) would have shipped
4.17%, worse than leaving both commits in and far worse than reverting
both.

I recommended "revert both" from caution and was right by luck. The
smaller revert was the one that looked safer and it is the worst of the
three states.

WHY. My code runs DIPLO_ACTIONS x OFFER_KINDS. The 30 August model has a
single trained {320,2} pair, so the loader replicates it across all seven
request kinds and the AI answers every request with weights trained on
their undifferentiated average. That is exactly the defect the per-kind
split was built to fix, reintroduced by shipping the split without a model
trained for it.

THE GENERAL CONSTRAINT, which is bigger than this release: in this
project the AI CODE AND THE MODEL ARE ONE ARTEFACT. Any change to the
network's shape -- and the diplomacy widening is one -- cannot ship ahead
of a model trained under it, and the migration path that makes old models
loadable is what hides the damage. It loads, it runs, it plays worse, and
nothing warns you.

That also retires a framing I used all day: "the AI code is better, the
model is unchanged, so play is much the same". For shape-changing code
that sentence is never available. Better code plus an old model is a
regression, not a neutral.

DECISION HANDED BACK: revert both commits, or ship N24 with them. There
is no third option and the partial revert is a trap.

## 228 -- the regression is real, reproducible, and I cannot explain it

    pre-branch code + 30 Aug model                 113 / 14.53%
    my code + old model, as committed               31 /  9.03%
    my code + old model, campaign+siege ablated     18 /  5.17%
    my code + old model, old research defaults      15 /  4.17%

TWO MECHANISMS PROPOSED, TWO REFUTED.

  1. "The diplomacy widening degrades an old model." Argued from the
     migration. But replicating one trained {320,2} pair across seven
     request kinds is behaviourally IDENTICAL to HEAD's single head
     answering all kinds -- there is no training at play time to make the
     copies diverge. I asserted this mechanism to the peer and to the user
     before checking that reasoning.
  2. "The new default-on reflexes (campaign, siege) misfire with a model
     they were not tuned against." Measured: ablating them costs a further
     3.86 points. They are HELPING.

Both components I suspected are load-bearing in the wrong direction: the
research defaults are worth +4.86 on this pairing and the two reflexes
+3.86. Strip either and it gets worse. The regression sits underneath
both and I do not know what it is.

WHAT IS SOLID: my branch with the 30 August model holds 9.03% of the
world where the pre-branch code holds 14.53%, on three hold-out seeds at
400 turns, reproduced twice. The practical conclusion is unchanged and
does not depend on the mechanism -- do not ship this code with that model.

WHAT I MUST WITHDRAW: entry 227's general rule, that "the AI code and the
model are one artefact" because a shape-changing change cannot ship ahead
of its model. That is a good-sounding principle built on mechanism (1),
which I have now refuted myself. The observation that prompted it stands;
the explanation does not, and I put it in the journal as a general
constraint on one measurement and one plausible story.

This is the seventh mechanism this session to be falsified while its
number held, and the second time today I have generalised a rule from a
mechanism before testing the mechanism.

## 229 -- the behavioural diff: not diplomacy, and the AI is doing MORE

Same 30 August model through both builds, 3 maps x 400 turns:

    metric (per 1k country-turns)   pre-branch    my build
    pacts proposed                       2.58        2.59
    war declarations                     4.55        4.71
    ceasefire offers                     0.93        0.66
    research nodes                     103.94      109.92
    austerity cuts                       4.08        2.06
    bankrupt country-turns               0.2%        0.1%
    conciliations                       82.81      103.37
    largest power                       14.6%       23.2%

DIPLOMACY IS UNCHANGED. 2.58 against 2.59 pacts. Whatever my branch does
to an old model, it does not do it through the diplomacy head -- which
independently confirms that withdrawing the widening hypothesis was
right, by a route that did not depend on my reasoning about it.

THE CHANGES ARE ECONOMIC. Research output up 5.8%, austerity cuts HALVED,
bankruptcy down, conciliations up 25%. That is the research change doing
exactly what it was built to do: countries keep their research, stay
solvent, and therefore austerity fires half as often -- so it trims the
minority programmes half as often, and the conciliation bill grows.

A CANDIDATE, OFFERED AS A CANDIDATE. Fewer austerity cuts means less
trimming of minority spending, and the minority bill is the largest
standing expense in this game. A model trained under the OLD economics
may be mispricing a world where that bill is allowed to grow. I am not
asserting this -- I have proposed two mechanisms tonight and refuted
both, and the honest status of this one is "consistent with the numbers
and untested".

WHAT IS SOLID AND NEW: the regression is not in diplomacy, and my branch
makes the AI MORE active on every axis rather than less. Largest power
14.6% -> 23.2% means the strongest AI country snowballs harder. The seat
bench scores ONE seat against a scripted world, so a change that helps
every model country compete can lower the measured seat while raising
the cohort -- which the bench itself hinted at with "model-cohort share
0.47 vs seat's own 0.37".

That is a measurement-shape problem I have written down before (a rule
that helps everyone lowers every seat's score) and it may be the whole
story here. It does NOT change the recommendation: the seat bench is what
a player experiences as their own country, and 9.03% against 14.53% is
still the number that matters for shipping.

## 230 -- not a metric artefact: the seat is annihilated

    1914:FRA, old model, 400 turns, one seed
    pre-branch build   seat 12.5%   model 12.5% / script 87.5%
    my build           seat  0.0%   model  0.1% / script 99.9%
                       cohort share 0.11 vs seat 0.00

Both the seat AND the model cohort collapse. So the "a change that helps
every model country lowers the measured seat" explanation -- which I had
started to believe, and which my own notes support as a real failure mode
-- does not apply here. France is simply destroyed.

THIRD HYPOTHESIS REFUTED TONIGHT. The regression is not:
    the diplomacy widening   (pacts 2.58 vs 2.59, unchanged)
    the new default-on reflexes (ablating them costs a further 3.9)
    the research defaults    (reverting them costs a further 4.9)
    a seat-versus-cohort measurement artefact (cohort collapses too)

WHAT IT LEAVES. My branch changes the rules enough that a policy not
trained under them is annihilated rather than merely worse. 12.5% to zero
is not degradation, it is a different game that the old model cannot
play. With N24 -- trained under these rules -- the same branch scores
81.97%.

SO THE CONCLUSION I WITHDREW IN 227 IS BACK, ON DIFFERENT EVIDENCE. I
said "the AI code and the model are one artefact" and withdrew it because
I had built it on the diplomacy mechanism, which was wrong. The claim now
rests on measurement instead: four candidate causes eliminated, the seat
annihilated with an untrained policy, and the same code scoring 81.97%
with a trained one. That is a much better footing than the story I
originally gave it, and the practical rule is unchanged -- these ship
together or not at all.

Worth noting the difference between the two versions of this entry. In
227 I had one measurement and a plausible mechanism, and I wrote a
general rule. Here I have five measurements, no mechanism, and the same
rule. The second is worth more.

## 231 -- isolated: the campaign system and the siege reflex, together

    1914:FRA, 30 August model, 400 turns, one seed
    pre-branch build                        12.5%
    my build                                 0.0%
    OD_CAMPAIGNS=0                           0.0%   campaigns alone: nothing
    OD_CAMPAIGNS=0 + siege ablated           8.0%   the pair: most of it back
    all six new reflexes ablated             8.0%   no more than the pair

The campaign system and the siege reflex TOGETHER account for 8.0 of the
12.5-point gap. Neither alone moves it at all, and my other four new
reflexes add nothing beyond the pair.

That is the fifth superadditive interaction measured in this session and
the clearest. Both are large rule additions -- campaigns commit a country
to a chosen offensive, siege diverts money to forts and research away
from the laboratory -- and a policy trained without either is destroyed
by having both.

WHAT MADE IT FINDABLE: switching to a SINGLE SEAT. Every earlier attempt
cost fifty minutes because I was benching six seats and three seeds, and
I twice called this investigation too expensive to pursue. France alone
shows the effect at its most extreme (12.5% to zero), so each arm took
three minutes -- a sixteenfold speedup on exactly the question I had been
deferring. The instrument was the obstacle, not the question.

THE REMAINING 4.5 POINTS are in code I have not isolated, and I am
stopping here rather than chasing them. The practical answer is complete:
the branch reshapes the game enough that an untrained policy cannot play
it, the two subsystems responsible are named, and the fix is the one
already established -- ship the branch with a model trained under it.

FOR 1.2.1: this is not a defect to repair before shipping. Campaigns and
the siege reflex are worth having; N24 is trained with both and scores
81.97% where the pre-branch pairing scores 14.53%. It is a coupling to
respect, not a bug to fix.

## 232 -- the rusher seat wants a different policy, and now there is proof

    threat-aware recruit, hold-out C, 400 turns, against 308 / 81.97%
    FRA:rush    1.03 ->  7.93   +6.90   ABOVE its par of 6.7
    SWE:rung   13.03 ->  2.27  -10.77
    USA:rung   21.77 -> 12.87   -8.90
    FRA:rung   27.57 -> 23.93   -3.63
    CHN:mod    18.13 -> 14.70   -3.43
    net -19.97

Third change to lift the rushed seat and lose overall:

    peace reflex     FRA:rush +4.57   net  +3.87
    recruit mask     FRA:rush +4.50   net  +3.73
    threat recruit   FRA:rush +6.90   net -19.97

Three unrelated mechanisms, all lifting the same seat, all costing the
others. That is no longer a coincidence: THE RUSHED SEAT WANTS A
DIFFERENT POLICY FROM THE REST OF THE BOARD, and no global knob can serve
both. It is also the only seat with real headroom left -- everything else
is at or above par.

The source already rejected this knob, and correctly: "41 rating, 9
survival and 33 floor worse than applying it to reinforcement alone... it
is not how OFTEN the correction fires, it is WHICH decision it governs."
My -19.97 agrees. What that measurement could not see is that the loss is
paid by five comfortable seats and the gain lands entirely on the one
being overrun.

WHY THIS IS THE ONE CONDITIONAL WORTH TRYING, after nine failed ones.
Every narrowing that failed this session narrowed a rule that was
GLOBALLY POSITIVE -- siege gating, the peace strength bar, the peace war
count. Restricting a rule that already helps everywhere can only lose.
This rule is globally NEGATIVE and locally worth seven points on a seat
at 15% of par. A condition is not an optimisation here; it is the only
way the rule could ever be used at all.

Next: gate it on being outmatched, and measure. If nine-for-nine holds
and this fails too, the honest conclusion is that the rusher problem
needs a different KIND of change than a knob.

## 233 -- ten narrowings, ten losses, and the rusher problem is not a knob

    threat-aware recruit, gated on being outmatched
    control                     81.97%
    ungated                     62.00%   -19.97
    bar 0.25 (besieged default) 61.83%   -20.14
    bar 0.50                    57.10%   -24.87

Tightening the gate makes it WORSE, monotonically. And this was the case
where the argument for a condition was strongest: a rule that is globally
negative and locally worth +6.90 on a seat at 15% of par. If a conditional
was ever going to work, it was this one.

TEN NARROWINGS THIS SESSION, TEN LOSSES:

    siege share 0.75 / 0.50            worse than either end
    peace strength bar 1.5 / 2.5       worse than both ends
    peace war count 3 / 4              worse than both ends
    austerity research-last alone      lost the floor
    threat-recruit bar 0.25 / 0.50     worse than ungated

Against that, both changes that helped REMOVED a condition rather than
adding one. I do not have a mechanism and I am not going to invent one,
but as an empirical rule for this AI it is now strong enough to act on:
conditions are expensive here, and a rule that fires broadly and is
sometimes wrong beats a rule that fires narrowly and is usually right.

THE RUSHER THREAD CLOSES. Three unrelated changes lift that seat and all
three cost the board; gating the best of them makes it worse. The seat is
not reachable by any knob in the game, conditionally or otherwise. If it
is to be fixed it needs a different KIND of change -- a policy that knows
it is losing, which is a training-side property, and training from this
parent has failed sixteen checkpoints out of sixteen.

So the honest closing position on the rusher seat: understood, measured,
and out of reach of the tools available. That is a better place than it
was twelve hours ago, when it was simply "the seat that always dies".

## 234 -- the credit horizon causes the conciliation ratchet, and nothing else

Two models from one parent, same rate, same rounds, same rule config,
differing only in OD_N_STEP (verified in the log: "N_STEP overridden: 40").

                              N_STEP=12    N_STEP=40
    conciliate taken-when-offered  73.23%      2.30%
    conciliations per 1k          140.34      35.24
    repress taken                   0.00%      0.00%
    fund up                        98.29%     98.52%
    fund down                      41.41%     41.20%

CONFIRMED: the horizon causes the conciliation ratchet. Widening the
credit window from 12 to 40 turns collapses conciliation by a factor of
THIRTY. Conciliation is free at the moment it is chosen and bills for
ever; at 12 turns the bill is invisible and at 40 turns it is not. That
is the first mechanism this session to survive its test rather than be
falsified by it, and it was predicted in writing before the run.

REFUTED, and it was half my case: repression stays at EXACTLY 0.00% of
44,961 offers. The horizon is not what stops the AI repressing. A longer
window makes it stop PAYING, not start coercing -- it simply does less
of both. So whatever holds repression at zero is something else, and I no
longer have a candidate.

ALSO REFUTED: "one horizon explains both ratchets". Fund-up and fund-down
are unchanged to within a tenth of a percent (98.29/41.41 against
98.52/41.20). The research slider is completely insensitive to the credit
window. I had presented the pairing of the two ratchets as the strongest
evidence for the horizon story; the pairing is not real.

Note also that fund-down runs at 41% in BOTH trained models, so the
"fund up 81% / fund down 0.4%" ratchet in the source comment is a
property of some particular model, not of the architecture. That is the
same attribution error I made about ships and about fund-down earlier --
a rate read off one model and described as the AI's.

WHAT IS ACTIONABLE. If training is ever made to work here, N_STEP is a
real lever on the minority bill, which is the largest standing expense in
the game and which three separate hand-written gates failed to control.
That is worth knowing and it is not reachable by any rule.

## 235 — the last open question in memory was already closed, against the memory

`econ-head-is-collapsed` carried an explicit untested item: whether the four
non-naval dead actions (`fort`, `specialize`, `fund down`, `focus bldg`) were
the same priced-refusal story as the naval ones. Checked before spending a
bench run on it. Three of the four are not dead at all — on N24, `fund down`
runs at 29.5% (and ~41% in both horizon-test models), `focus bldg` is the
research-branch action at 90.4%, `fortify` at 0.7% is forced by a shipped
reflex. Only `specialize` (0.3%) is near-dead.

The "eight actions at exactly 0.0" list was marginal probability on ONE model,
2026-08-29. Written down as a property of the head, it survived nine days and
would have sent this session to price actions that are alive.

That is the third instance today of the same error: a number true of one model
quoted as a property of the architecture. The other two were mine (the fund
up/down attribution) and the codebase's own comment. Both memories corrected,
index hook included -- an index line that still sells a retracted claim is
worse than no memory.

Nothing untested remains in the dead-action thread.

## 236 — the austerity step, and a metric that changes identity under you

Swept a constant nobody had looked at: austerity drags the research slider
by 0.15 a pass, while the head's own research action moves it by 0.05. So
austerity cuts three times faster than the policy can restore it, and the
policy is visibly trying -- fund up on 95.5% of offers against fund down on
29.5%. Hold-out set C, N24, 400 turns, difficulty 3, same binary both arms,
knob proven inert per call (1560 of 1561 lines identical unset vs 0.15):

    step 0.15 (shipped)   OD BENCH 311   land 82.73%   survival 77   worst 17
    step 0.05             OD BENCH 336   land 92.50%   survival 92   worst 51

    seat              par    0.15 land  score   0.05 land  score   d.surv
    1914:FRA:rung     6.7        27.70    413       27.70    413     +0.0
    1914:SWE:rung     1.0        13.17    500       13.17    500     +0.0
    1939:USA:rung     5.6        21.90    391       23.23    415     +0.0
    modern:CHN:rung   2.5        18.20    500       18.73    500     +0.0
    1914:FRA:rush     6.7         1.17     17        9.00    134    +82.6
    1939:NOR:hood     1.3         0.60     46        0.67     51     +5.1

FAILS the shape test: 80% of net land is one seat. The peer argued survival
and worst-seat do not decompose like land, so the test was rejecting a broad
improvement on a narrow metric. Checked instead of assuming: survival is 94%
one seat -- MORE concentrated than land, not less. The four comfortable seats
sit at 391-500% of par and cap at 100, so they cannot contribute, which makes
survival more dominated by the single uncapped seat.

WORST SEAT 17 -> 51 IS A SEAT SWAP, NOT A FLOOR LIFT. At 0.15 the worst seat
is FRA:rush at 17. At 0.05 FRA:rush reaches 134 and stops being worst;
NOR:hood at 51 becomes worst, having itself moved only 46 -> 51. The metric
changed identity between arms. It will do that every time a single bad seat
is fixed, and it reads exactly like a broad improvement.

WHAT IT IS: a country being actively overrun ends ABOVE its starting share
(17% -> 134% of par) where it was previously near annihilation. Nothing else
moves and nothing gets worse. Narrow, large, and at no cost -- which is new:
every previous rusher-seat fix paid for it on the board, and that is why I
concluded in entry ~230 that the seat was unreachable by any knob. That
conclusion was wrong. It was wrong because I never looked inside a rule that
was already working.

Not a magnifier artefact: FRA:rush par is 6.7, the largest in the set. The
NOR:hood 46 -> 51 IS in magnifier territory and I do not claim it.

FOR THE RELEASE RECORD: 82.73% is the 0.15 arm. 0.05 measured better on one
hold-out set BEFORE the tag, and 1.2.0a ships 0.15 deliberately -- one seed
set is not enough for a default, and four numbers inverted on contact with a
different configuration today. Set D is running. Recording it now so it is
legible as a decision rather than discovered later as an oversight.

## 237 — the user's model, measured before it was replaced

The user's play-trained model had never been benched, and the release
proposed to overwrite it with N24. Hold-out set C, N24's exact conditions,
6/6 seats both runs, zero dropped seeds:

    seat              par    USER    N24   N24@0.05
    1914:FRA:rung     6.7      79    413    413
    1914:SWE:rung     1.0       7    500    500
    1939:USA:rung     5.6     151    391    415
    modern:CHN:rung   2.5      36    500    500
    1914:FRA:rush     6.7      95     17    134
    1939:NOR:hood     1.3      41     46     51

    USER play-trained    OD BENCH  68   survival 59   worst  7
    N24 @0.15 (ships)    OD BENCH 311   survival 77   worst 17

N24 ships: 4.6x better overall, four seats won outright.

BUT THE MODEL WE WERE ABOUT TO DISCARD SCORES 95 ON THE RUSHER SEAT WHERE
N24 SCORES 17. Predicted in advance from [[selfplay-erodes-rush-defence]] --
human play teaches something self-play destroys -- and it is now a measured
fact rather than an inference from that memory. Replacing it unmeasured
would have thrown away the only artefact in the project that survives being
invaded, and nothing would have recorded that it existed.

Caveats that keep it honest: trained at whatever difficulty the user plays,
benched at 3, so 68 is a FLOOR on it and not a fair overall measure. And it
has its own catastrophe -- SWE at 0.1 land, score 7. Not a hidden gem.

WHAT I WROTE AND THE PEER CORRECTLY REFUSED: that the 0.05 constant
"recovers by rule what human play had learned". 134 and 95 are compatible
with that and nothing has tested it. They could reach those numbers by
entirely different routes. Two measurements pointing the same way are not
one story -- which is exactly [[measurements-replicate-explanations-dont]],
cited by me in the same message where I broke it. Recorded as two facts
that happen to agree, and a hypothesis for a later run.

## 238 — the austerity step does not replicate; it inverts. RETRACTED

Set D, same binary, same model, same 400 turns and difficulty, 6/6 seats
both arms, zero dropped seeds:

    seat              par |  setC .15  .05      d |  setD .15  .05      d
    1914:FRA:rung     6.7 |      413  413     +0 |      297  273    -24
    1914:SWE:rung     1.0 |      500  500     +0 |      500  190   -310
    1939:USA:rung     5.6 |      391  415    +24 |      402  402     +0
    modern:CHN:rung   2.5 |      500  500     +0 |      500  500     +0
    1914:FRA:rush     6.7 |       17  134   +117 |       44    4    -40
    1939:NOR:hood     1.3 |       46   51     +5 |       38   38     +0

    setC  OD BENCH 311 -> 336  (+24)
    setD  OD BENCH 297 -> 235  (-62)

FRA:rush -- the seat that WAS the finding -- moves +117 on one hold-out set
and -40 on the other. Sweden loses 310 points on set D having been untouched
on set C. Entry 236 is retracted in full.

WHAT I SAID AND WHAT IT WAS WORTH: I called this "the largest single
improvement I have measured all session" and told the peer that if it held it
would change the release. It was noise on one seed set. The mechanism was
articulable, the direction was predicted in advance, the effect was large, it
cost nothing anywhere, and it was still noise.

WHAT ACTUALLY WORKED, and it is the only reason this is a retraction rather
than a shipped regression:

  1. The shape test flagged it -- 80% of net land on one seat -- and I nearly
     argued past it because the number was attractive and a peer offered a
     plausible reason to. The correct response to "one seat carries this" is
     lower confidence, not a better story about why the seat matters.
  2. Both of us refused to put it in a tag that was minutes away, on the
     grounds that one hold-out set is not enough for a default. That judgement
     is worth more than the finding would have been.

Note the control arms differ too: FRA:rush scores 17 on set C and 44 on set D
under identical code. The seat's difficulty varies 2.6x between hold-out sets,
which is the whole reason a single set cannot settle anything about it.

Standing conclusion restored: the rusher seat is not reachable by a knob. I
retracted that conclusion in entry 236 on this evidence and now retract the
retraction. [[corrections-inherit-confidence]] -- the retraction got less
scrutiny than the original claim and failed the same way.

## 239 — what the shipped change actually does: it buys growth with the floor

The research change (OD_SIEGE_RESEARCH off + austerity research last) shipped
in 2f100d1. Measured against its own absence in the SHIPPED binary with the
SHIPPED model, both hold-out sets, 6/6 seats, no dropped seeds:

    seat              par |   C off   C on      d |   D off   D on      d
    1914:FRA:rung     6.7 |   17.07  27.70 +10.63 |   10.20  19.90  +9.70
    1914:SWE:rung     1.0 |    9.93  13.17  +3.23 |    9.67  11.97  +2.30
    1939:USA:rung     5.6 |    4.33  21.90 +17.57 |   11.03  22.53 +11.50
    modern:CHN:rung   2.5 |   14.73  18.20  +3.47 |   11.10  14.13  +3.03
    1914:FRA:rush     6.7 |    5.03   1.17  -3.87 |    8.73   2.93  -5.80
    1939:NOR:hood     1.3 |    0.47   0.60  +0.13 |    1.37   0.50  -0.87

    rating      241 -> 311  (+71)   |   255 -> 297  (+42)
    survival     81 ->  77  ( -4)   |   100 ->  80  (-20)
    worst seat   36 ->  17  (-18)   |   105 ->  38  (-67)

IT HOLDS. +71 and +42 rating, +31.2 and +19.9 land, on two hold-out sets it
was not chosen against. That is the confirmation the austerity constant failed
and it is why one of these shipped and the other is retracted.

BUT IT IS A TRADE, AND THE TRADE ALSO REPLICATES. The four comfortable seats
gain 3 to 17 points of the world each. The seat under invasion LOSES 3.87 and
5.80 points of the world -- raw land share, par 6.7, so not a small-par
magnifier artefact. On set D the pre-change AI held EVERY seat above par
(worst 105, survival 100); after the change the worst seat is 38.

So the release note figure is real and one-sided. The AI we shipped expands
much better and collapses much harder. Both halves replicate.

Note what this says about the aggregate: OD BENCH rose 71 and 42 while
survival fell 4 and 20. A single number cannot carry this change, and the one
we quote is the half that flatters it. [[aggregate-vs-seat-measurement]] and
[[floor-not-rating]] both warned exactly this and I still read the headline
first.

It also makes WORSE the specific failure the user's play-trained model was
best at -- 95 on the invaded seat against N24's 17 (entry 237). The shipped
change pushes that seat down further. Two independent lines now point at
collapse-under-invasion as the AI's real weakness, and the release moved it in
the wrong direction while improving everything else.

NOT a reason to unship: +71/+42 aggregate on two hold-out sets is the best
evidence any change has had this session. It is a reason for the note to say
what was bought and what it cost.

## 240 — the obvious fix for the trade makes the trade worse. 0 for 11

Entry 239 showed the shipped research change costs the invaded seat 3.87 and
5.80 points of the world. The obvious fix: when a country is being overrun,
revert it to the old behaviour -- fund defence, not laboratories. Implemented
as OD_RESEARCH_GUARD_OVERRUN (worstDeficit >= bar x army), gating both the
austerity ordering and the siege research cut. Default 0.0 = shipped.

One seat, 1914:FRA:rush, N24, 400 turns, difficulty 3, three hold-out seeds:

    control (shipped)   3.1  0.2  0.2   mean 1.17
    guard 0.25          0.2  0.3  0.3   mean 0.27

It does not merely fail to help the seat it was built for. It makes that seat
THREE TIMES WORSE. Narrowings are now 0 for 11 this session, and this one lost
on the very seat it targeted -- so the failure is not "the gate costs more
elsewhere than it recovers here", it is that the premise was wrong. A country
being overrun does not do better by cutting research; it does worse.

That kills the reading I had of entry 239. I described the trade as "funding
laboratories instead of defence", which sounds like an error the AI is making.
It is not: taking the research away from the losing country makes it lose
harder. Whatever the shipped change costs that seat, redirecting the money is
not the recovery.

METHOD NOTE, and it is why this cost six minutes rather than three hours:
diagnosed on ONE seat before benching six. The full A/B would have been ~3.3
hours to reach the same kill.

The control arm doubled as the inertness proof -- it reproduced the stored
setC mean of 1.17 EXACTLY on a freshly built binary, which simultaneously
validates the harness and proves the knob inert when unset. Worth building
that check into every diagnostic: a control that must reproduce a known stored
value catches a broken harness before the treatment arm is read.

I needed that check. The first run of this diagnostic passed --vs-exploit 1
where the rush world is 3, and returned a clean control mean of 27.6 -- which
is the RUNG seat's stored value (27.70), not the rush seat's. Accurate numbers
about a world I had not asked for. Seventh instance today of
[[true-about-the-adjacent-thing]] and the second I produced myself.

Code reverted; patch kept at scratchpad/research-guard-overrun-FAILED.patch.

## 241 — "the model that survives invasion" was a mean over two observations

Tried to diagnose WHY the play-trained model beats N24 on the invaded seat,
by diffing action histograms. The diagnostic run scored the user model 0.2 on
seed 909091 where N24 scored 3.1 -- the opposite of the advantage I have been
citing all session. So I went back to the spread I never read:

    1914:FRA:rush, 400 turns, difficulty 3, three hold-out seeds

        N24          3.1   0.2   0.2     mean 1.17   score 17
        play-trained 0.2   8.7  10.1     mean 6.30   score 95

It is BIMODAL. The play-trained model holds 8.7 and 10.1 on two worlds and is
annihilated at 0.2 on the third -- which is the one world where N24 does
better. So "the only artefact that survives being overrun" is wrong. The
defensible claim is that it holds an invaded country on MOST worlds where N24
holds none, and with n=3 and that variance the difference in means rests on
two observations.

I stated the stronger version repeatedly today: to the peer, to the user, in
journal 237, and in a README that is committed and pushed. Corrected in
6bd704c. The numbers were right every time -- 6.3, 1.17, 95, 17 are all
accurate -- and the sentence built on them was not.

This is [[rates-need-counts]] in its plainest form: a mean whose spread I
never printed, quoted as a property. od_bench PRINTS the spread on every seat
line, in brackets, and I read the mean column for two hours.

SECOND FAILURE, same run: OD_ACT_HIST_FILE produced 13 bytes -- "0.4287 36277",
the research-allocation accumulator, not the action table. The action
histogram goes to stdout, which I had filtered to `grep "^\[BENCH\] seat"`.
So the diagnostic I built this run for produced nothing at all, and I only
noticed because the score line contradicted a claim I believed. Had the scores
agreed with my expectation I would have reported an empty histogram as
"no behavioural difference found".

## 242 — the invaded seat is BISTABLE, and three seeds cannot measure it

Ran the invaded seat on 8 fresh seeds for both models, to test whether the
advantage I have been citing all session exists at all. Paired, 11 seeds:

    seed          N24   USER    diff
    909091        3.1    0.2    -2.9
    20230115      0.2    8.7    +8.5
    42424242      0.2   10.1    +9.9
    111333        9.5    7.5    -2.0
    24680         0.3   10.8   +10.5
    5150          7.7    9.1    +1.4
    909           0.3    8.6    +8.3
    77777         0.3    1.3    +1.0
    31415        11.3    0.5   -10.8
    8675309       2.9    5.7    +2.8
    4040404      10.8    8.2    -2.6

    N24  4.24 (sd 4.63)   USER 6.43 (sd 3.93)
    paired diff +2.19  se 2.01  t = 1.09  95% CI [-2.29, +6.67]

THE ADVANTAGE IS NOT SIGNIFICANT. The interval spans zero and includes N24
being better. What produced the apparent 5.6x gap was the SEED SET:

    N24   set C 1.17   ->  all 11  4.24     3.6x understated
    USER  set C 6.33   ->  all 11  6.43     representative

Set C happened to hold three of N24's collapse worlds. The play-trained
model's number was fine; N24's was not, and I compared them anyway.

THE REAL STRUCTURE, and it is worth more than the retraction: the seat is
BISTABLE. Every one of the 22 runs either holds 5.7-11.3 land or collapses
below 1.5. There is essentially nothing between 1.5 and 5.7. Collapse rate
is 5/11 for N24 and 3/11 for the play-trained model.

So measuring that seat with three seeds is estimating a Bernoulli parameter
from three coin flips. Every three-seed reading of FRA:rush this session --
including the ones that drove the austerity finding (17 -> 134) and its
retraction (44 -> 4) -- was doing that. The austerity result inverting between
set C and set D is exactly what a bistable outcome does under n=3, and I
explained it at the time as a real effect that failed to replicate. The truer
statement is that neither reading measured anything.

WHAT SURVIVES: the shipped change's cost on this seat (-3.87 setC, -5.80 setD)
is a PAIRED within-seed comparison, control and treatment on identical worlds,
and it replicated across two independent seed sets in the same direction. That
is much more robust than a between-model comparison. But its MAGNITUDE rests
on 3+3 seeds of a bistable quantity and should not be quoted precisely.

WHAT DOES NOT: "the play-trained model survives invasion where N24 does not",
which I said to the user, to the peer, in entry 237, in entry 241's partial
correction, and in a committed README. Third correction of the same claim
today, each one smaller than the last and each one still overstated.
[[corrections-inherit-confidence]].

## 243 — the bistability detector, and why it cannot be a detector

Added a bistable-seat warning to od_bench. First attempt keyed it off the
SAMPLE: flag a seat whose seeds straddle both regimes. Tested it against the
data that fooled me -- set C, [3.1, 0.2, 0.2] -- and it did not fire.

It could not. Nothing in those three values reaches the holding band, so
there is no straddle to detect. Inferring bistability from three samples runs
into the identical small-n problem the warning exists to announce. The
detector failed on the exact case it was written for, which is the cleanest
possible demonstration of the thing being flagged.

Rewritten to key off the SEAT -- 1914:FRA:rush is known bistable from 22 runs
-- with sample-straddle kept as a secondary trigger for seats not yet on the
list. Verified three ways: fires on the 3-seed case that fooled me, fires on
a rung seat given an artificial straddle, and at 11 seeds switches to
"read collapse rate 5/11, not the mean", which independently reproduces the
rate I computed by hand.

A test expectation of mine was also wrong: I asserted the warning should go
SILENT at 11 seeds. It should not. A two-regime seat has no meaningful mean at
any n; what changes with n is the ADVICE -- below ten seeds read nothing, above
it read the collapse rate. The code was right and my expectation was wrong,
which is worth recording because I nearly "fixed" working code to match it.

The tool now carries the most expensive lesson of the session at the point
where the number gets read, which is the only place a warning survives.

## 244 — the bench's noise floor, measured at last, and a claim of mine it breaks

Mined every per-seat spread od_bench has printed into logs still on disk.
Three complete 6-seat runs recovered, per-seed ratings reconstructed:

    53   68   83     mean  68   sd 15   se(n=3)  9
    288  330  309    mean 309   sd 21   se(n=3) 12
    318  333  350    mean 334   sd 16   se(n=3)  9

A 3-SEED OD BENCH HAS SE 9-12. An unpaired difference under ~28 points is
noise. This number was available all session -- the spreads were printed on
every seat line -- and I never computed it.

Against it: the austerity constant's +24 on set C was inside the floor before
anything else was wrong with it. The shipped change's +71 and +42 are outside
it, on two independent sets, which is why that one is real.

Some seats are far noisier than I assumed. 1914:SWE:rung ran [24.2, 9.0, 6.3]
-- a 4x spread on a par-1.0 seat, so its score is pinned at CAP regardless and
its variance is invisible in the rating. 1914:FRA:rung ran [19.1, 36.0, 28.0].
Only 1939:NOR:hood was genuinely tight, and only because it collapses every
time.

od_bench now prints `+/- N se (unpaired diffs under ~M are noise)` beside the
rating, computed from that run's own spreads. Verified against the hand
calculation: se 12, per-seed [288, 330, 309], identical.

A CLAIM OF MINE THAT THIS BREAKS. I told the peer, and put in entry 242, that
"pairing is the defence against bistability, because both arms draw the same
world". The first half is right and the conclusion is wrong.

Pairing controls WORLD DIFFICULTY. It does not control regime-flipping, and
the data says the world does not determine the regime: on seed 909091 N24
scored 3.1 and the play-trained model 0.2; on seed 31415, 11.3 and 0.5. Same
world, opposite regimes, decided by the policy. So a bistable seat sits near a
tipping point and ANY small code change can flip it, giving +/-10 land of pure
amplification that pairing does nothing about.

What actually carries the shipped change's cost finding is REPLICATION across
two independent seed sets in the same direction -- not pairing. The peer's
note says both, so it stands; my reasoning for it did not.

## 245 — a third of the rating could not move, and my first version of that claim was wrong

The peer's line -- "a seat whose variance is structurally invisible in the
rating is not contributing information, it is contributing the appearance of
information" -- is testable, so I tested it.

MY FIRST HYPOTHESIS, from the recent N24 spreads: SWE (par 1.0) and CHN (par
2.5) always pin at CAP, so a third of every rating is a constant. Checked
against all 829 stored runs:

    1914:SWE:rung   pinned in 101/829 runs   12%
    modern:CHN:rung  pinned in  55/829       7%
    every other seat under 7%, none above 12%

WRONG as stated. Across the corpus -- which is mostly weaker models -- almost
nothing pins. I had generalised from today's runs to the instrument.

BUT RIGHT WHERE IT MATTERED. Restricted to the A/Bs I actually ran today, all
on N24, counting seats pinned in BOTH arms:

    austerity  0.15 vs 0.05   set C   2 of 6 dead   set D   1 of 6
    shipped change on/off     set C   2 of 6 dead   set D   1 of 6

So on set C a third of the rating was an identical constant in both arms and
could not express a preference. The bench is blind to any improvement on a
pinned seat: SWE ran [24.2, 9.0, 6.3] behind a score of 500 in every case.

It also means the noise floor from entry 244 is FLATTERING. Constant terms
have zero variance, so pinning damps the measured spread of the rating; the
se of 9-12 is real for the rating as computed, but the discriminating part of
it is proportionally noisier than that.

od_bench now names pinned seats and says the rating cannot see improvement
there. Third instrument change today, all the same shape: the summary line
now states its own denominator, its own resolution, and which of its inputs
are inert.

Worth noting the pattern in my own error: I formed the hypothesis from six
recent runs and stated it as a property of the bench. The corpus said 12%.
The discipline that saved it was checking before claiming -- which is the
same discipline that failed on the play-trained model, where I checked three
seeds and claimed a capability.

## 246 — the research bar: mechanism live, effect absent, and the instrument earned itself

OD_RESEARCH_BAR (0.45) is the real ceiling on research spending -- it stops
OFFERING fund-up at 0.45, which makes the 0.50 clamp unreachable. Flagged
twice in this journal as "still worth running" and never run.

MECHANISM CONFIRMED FIRST, for six minutes rather than assuming:

    bar 0.45   mean allocation 0.4353   21,634 decisions
    bar 0.65   mean allocation 0.4929   17,038 decisions

+5.8 points of national output into research, sustained over 400 turns. Note
it settles at 0.49 and not 0.65 -- the head balances rather than pins, which
is the "tug of war, not a ratchet" correction already in the source comment.
A 20-point ceiling lift buys a 6-point equilibrium shift.

BENCHED, hold-out C, N24, both arms one binary:

    bar 0.45   OD BENCH 311 +/- 12   land 82.73%   survival 77   worst 17
    bar 0.65   OD BENCH 292 +/- 21   land 73.90%   survival 85   worst 13

    seat              par    0.45    0.65       d
    1914:FRA:rung     6.7   27.70   19.97   -7.73
    1914:SWE:rung     1.0   13.17    7.33   -5.83
    1939:USA:rung     5.6   21.90   17.80   -4.10
    modern:CHN:rung   2.5   18.20   20.37   +2.17
    1914:FRA:rush     6.7    1.17    8.27   +7.10   <- BISTABLE
    1939:NOR:hood     1.3    0.60    0.17   -0.43

-19 rating against a floor the tool printed as ~34. NOT RESOLVABLE, and the
direction is negative on every seat that can measure. The lone gain is the
bistable seat flipping regime, and it is what lifts survival 77 -> 85.

NOT following it to set D. A result inside the noise floor does not earn
another 1.8 hours; that is what measuring the floor was for.

THIS IS THE AUSTERITY EPISODE IN MINIATURE and the difference is the tooling.
Same shape -- aggregate moved, driven by the bistable seat, survival up,
growth seats quietly down. In the morning I called that shape the largest
improvement of the session and spent hours before set D killed it. This time
od_bench printed "1 seat(s) bistable" and "diffs under ~34 are noise" above
the number, and the reading took one pass.

ALSO A CLEAN INERTNESS CHECK, unplanned: the 0.45 control reproduced OD BENCH
311 and land 82.73% EXACTLY, matching austerity-step-0.15-setC from a binary
two rebuilds ago. So removing the failed overrun guard and fixing the
OD_SIEGE_RESEARCH comment were behaviourally inert, as claimed rather than
assumed.

The research subsystem is now well mapped: the clamp is unreachable, the bar
is negative, and the order-plus-siege change is the one that works and shipped.

## 247 — austerity has nothing cheap left to cut; the ordering is forced

The shipped win was a REORDER of the austerity list (research first -> last,
+71/+42). Asked whether any sibling reorder is available. Instrumented which
branch fires instead of guessing -- 400 turns, one world, N24:

    research-first    0     <- the shipped change, confirmed doing its job
    pacification     20
    doctrine         28
    minority         46
    scrap-ship        5
    research-last    10

FIRST HYPOTHESIS, killed by the counts: scrap worthless warships before
repealing doctrines. The AI buys 0 ships in 6,020 offers and forcing it to
cost 40% of the map, so a hull is near-free to give up while a doctrine is an
ongoing benefit. But scrap fires only 5 times in 109 cuts -- the ships are
SCARCE, so the reorder can convert at most 5 events. Not within a mile of a
~34-point floor.

SECOND HYPOTHESIS, killed by the structure: minority trimming fires most (46),
and alignment loss compounds into unrest and rebellion, so it looks like the
research case. But the loop takes the FIRST AVAILABLE branch, so minority
firing 46 times means pacification and doctrine were exhausted or unavailable
on those turns. Move minority later and what fires instead is scrap-ship (5
available) or RESEARCH-LAST -- which is the thing the shipped change exists to
protect. Demoting minority re-loads the cut onto research and undoes the win.

SO THE ORDERING IS NEARLY FORCED. Austerity reaches for the cheapest thing it
has; the win came from the one item that was catastrophically mispriced, and
after removing it there is no cheap resource left to substitute. Scarcity, not
ordering, is now the binding constraint.

Cost: ~20 minutes of instrumentation instead of a ~2 hour A/B that could not
have cleared the floor either way. This is the same instrument-first move that
killed the overrun guard on one seat in six minutes.

Branch counters kept -- they are three lines behind OD_ACT_HIST and the next
person to have this idea gets the answer without a build.

## 248 — an inverted reading caught before it became a finding

Audited for the pattern that produced the one shipped win: a place where the
AI prices something with its own number instead of the resolver's. Two
BuildCosts functions are never called by the AI -- artyMoneyCost and
maintenanceCostMod -- and austerity's "repeal the costliest doctrine" (28
firings a world) ranks by `costPerTurn` alone.

policies.json shows 23 doctrines with `maintenanceCostPct` / `industryCostPct`
levers, and the biggest STICKER prices carry the biggest levers:

    war_economy_total      cost 18   maintenanceCostPct -18
    war_on_several_fronts  cost 16   maintenanceCostPct -20
    mass_mobilisation      cost 11   maintenanceCostPct -25

I read those as savings and had a serious-sounding bug: austerity
systematically repealing the doctrines that pay for themselves, worst-first,
in a branch that fires 28 times a world.

IT IS INVERTED. `buildCostMod(pct) = max(0, 1 - pct/100)`, so POSITIVE is a
discount and NEGATIVE is a surcharge. `mass_mobilisation`'s generated UI text
lists "Army upkeep +25%" under COSTS. Those doctrines cost money TWICE, and
repealing the highest sticker first is roughly right.

WHAT SAVED IT: the struct comment said "a COST reduction is stored POSITIVE"
and I checked it against buildCostMod and then against the generated UI string
rather than trusting my reading of the JSON. Two independent confirmations of
a sign, for a finding I wanted to be true.

Also nearly lost to a bad instrument first: my initial parser looked for
`costPerTurn` where the schema is `cost_per_turn`, and returned "0 policies
with a cost and a cost effect" -- a clean, wrong null. Caught by asking the
parser to prove it could see ANY costs at all before believing it saw none.

RESIDUAL, too small to chase: ranking by sticker ignores surcharge levers,
which mis-orders two doctrines only when armyUpkeep exceeds ~100. Upkeep is
0.01 per 10k men, so that is a hundred million men. Stickers (10-18) dominate
the percentage term (1.5-2.5) for any real army.

## 249 — the research bar is a growth/survival dial, and 0.45 is its land optimum

I swept this knob upward, found -19, and wrote the subsystem up as "mapped".
That was one direction of a two-directional constant. Completing it:

    bar    OD BENCH        land     survival   worst
    0.25   195 +/- 11     43.97%       77        18
    0.35   237 +/- 28     54.63%       89        59
    0.45   311 +/- 12     82.73%       77        17    <- shipped
    0.65   292 +/- 21     73.90%       85        13

0.45 IS THE PEAK. Lowering is decisively worse -- -116 at 0.25 against that
arm's own ~30-point threshold. Raising is -19, inside the floor. The default
was well chosen and is now measured rather than assumed.

WORTH MORE THAN THE OPTIMUM, THOUGH: this knob is a GROWTH/SURVIVAL DIAL, and
the two do not peak together.

    0.35   survival 89   worst seat 59   land 54.63%
    0.45   survival 77   worst seat 17   land 82.73%

At 0.35 the AI holds far less of the world and is dramatically harder to kill
-- worst seat 59 against 17, the best floor any configuration has produced all
session, on the seat nothing else has moved. OD BENCH is a land-weighted mean,
so it picks 0.45 by construction. A different objective picks 0.35.

That is a design question rather than a bug, and it is the user's to answer:
should AI countries snowball, or should they be hard to eliminate? The bench
answers the first and has been silently deciding the second all session.

Note this is the same trade the shipped change makes (journal 239: +71 rating,
survival 100 -> 80 on set D). Two independent knobs in one subsystem, both
buying growth with the floor. That is now a characterised property of the
research allocation, not a coincidence of one change.

METHOD: I called the subsystem mapped after sweeping one direction. Second
time today I generalised from a partial sample -- the first was three seeds
reading as a measurement. Sweep both ways before writing the conclusion.

## 250 — RETRACTING 249's design claim: the survival half was two unmeasurable seats

Entry 249 said the research bar is a growth/survival dial, and put to the user
that 0.35 makes the AI "dramatically harder to kill -- worst seat 59 against
17, the best floor any configuration has produced". Checked it properly:

    bar     RELIABLE seats        rush    hood
            (FRA/USA/CHN rung)
    0.25    202                     18      44
    0.35    262                     77      59
    0.45    435                     17      46
    0.65    372                    123      13

WHAT SURVIVES: 0.45 is the optimum. On the three seats that can actually
measure -- large par, never pinned, not bistable -- the response is monotone
up to 0.45 and down after, with 60-230 point gaps. Solid, and far outside any
floor.

WHAT DOES NOT: everything I said about survival. The four non-noisy seats sit
at exactly 100 in BOTH arms, so survival 77 -> 89 is entirely rush (17 -> 77)
and hood (46 -> 59). rush is the bistable seat and hood has par 1.3, deep in
magnifier territory. Neither is monotone in the bar: 18/77/17/123 and
44/59/46/13. That is regime-flipping, not a dose-response.

And the "worst seat 59" was a DIFFERENT SEAT -- hood, not rush -- which is the
seat-swap I wrote a memory about this morning and then walked into.

FOURTH TIME TODAY a claim of mine has rested on these two seats: the austerity
constant, the play-trained model's "capability", the shipped change's survival
cost, and now this. The pattern is not random. I reach for survival and floor
numbers because they tell a better story than land share, and they are computed
from the two seats least able to support one.

NOT retracting journal 239's survival cost for the shipped change: that also
lives on these seats, but its LAND cost replicated across two independent seed
sets in the same direction (-3.87, -5.80). Replication is what this claim
lacks -- one seed set, non-monotone.

TO THE USER: I put a design question to them -- should AI countries snowball or
be hard to kill -- on the strength of this. The question is legitimate; the
evidence I offered for it is not. Corrected in the same message.

## 251 — fixing the reader, not the resolve

Four claims today rested on the same two seats. I added flags for those seats
this afternoon and then read past them, twice. Flags in the margin do not work
when the number in the middle of the line still reads as a six-seat statistic.

So the line itself now carries it:

    survival 77   worst seat 17 (1914:FRA rush)   [!! survival varies over only
                                                   2 of 6 seats -- the rest are
                                                   at or above par and constant]
    survival 89   worst seat 59 (1939:NOR hood)   [!! ... 2 of 6 seats ...]

Two failure modes closed at the point of reading:

  1. Survival is a mean of min(seat,100). Any seat at or above par is a
     constant, so survival varies ONLY over seats below par. In the arms that
     fooled me, four of six were pinned at exactly 100 in BOTH arms and the
     whole 77 -> 89 was rush and hood. It now says so.
  2. The worst seat NAMES itself. "17" and "59" above are different seats, and
     printed side by side that is obvious. Printed as bare numbers it reads as
     a floor lift, which is what I reported to the user yesterday and retracted
     today.

Verified by discrimination rather than by inspection: fires on both arms that
fooled me, silent on the play-trained model's run where survival genuinely
varies over five seats.

This is the fifth instrument change today and the first aimed at MY reading
rather than at the tool's correctness. The tool was never wrong. It reported a
mean of six numbers, four of which could not move, and I supplied the rest.

## 252 — the AI's budget, measured for the first time

Instrumented CountryIncomeSnapshot at the econ entry. 217,538 country-turns,
N24, 400 turns, share of GROSS income:

    research         41.1%   (78.92)      pacification    10.8%   (20.77)
    minority         18.0%   (34.61)      policy           4.2%    (8.11)
    industry upkeep  17.9%   (34.43)      navy             0.5%    (0.94)
                                          army             0.3%    (0.57)

    gross 191.93   expenses 178.35   net ~13.6, a 7% margin

THREE THINGS FALL OUT.

1. RESEARCH DOMINATES at 41%, more than twice the minority bill. Journal and
   memory have repeatedly called the minority bill "the largest standing
   expense in the game". It is not, and never was measured. Corrected.

2. THE ENTIRE MILITARY IS 0.8% OF GROSS. Army 0.3%, navy 0.5%. Money is not
   what limits the AI's army, so there is no improvement to find in military
   spending. That is consistent rather than surprising: above the combat
   frontage both sides cap, so a bigger army buys nothing (journal 39a) and
   the AI is correct not to fund one. The width rule, not the budget, is the
   binding constraint on army size.

3. THE 7% MARGIN EXPLAINS THE INDUSTRY BLOCK. The econ head withholds industry
   for want of cash on 90.4% of the turns it wants it. After research,
   minorities, upkeep and pacification there is almost nothing left.

AND IT CLOSES THE OBVIOUS FOLLOW-UP BEFORE IT COSTS ANYTHING. "Free money from
research to fund compounding industry" is the natural read of point 3, and it
has already been measured: OD_RESEARCH_BAR at 0.35 and 0.25 scored 262 and 202
on the reliable seats against 435 for the shipped 0.45. Research compounds
harder than industry at this horizon. The allocation is approximately right.

So the budget is characterised and it is not where the next improvement lives.
That is a negative result, but it is the first time the question has been
answerable at all -- and it retires a wrong claim that had been repeated in
three places.

## 253 — specialisation is not the missing income: there is not enough of it to buy

AI_SOCIAL_BUDGET_SHARE's note says the AI's poverty "cannot be fixed by
redirecting" spending, and names where the income must come from instead:
"trade, specialisation, resource development". Specialisation is also the last
near-dead econ action never priced -- 9 taken of 3,473 offered.

The structural case for trying it was real and different from the naval case
that cost 40% of the map: a ship is permanent UPKEEP and compounds against you
over 400 turns, while a specialisation is a ONE-OFF price with permanent
income and no upkeep. Opposite shapes over the horizon, so that precedent does
not transfer. And nextSpecBuy already ranks by return per gold, so a reflex
would buy the four-turn province rather than the hundred-turn median.

Built specializeReflex (OD_SPEC_REFLEX = payback ceiling in turns, 0 = off).
Verified inert unset: fired 0, seat score 3.1, matching the known control
exactly. Then counted firings instead of benching:

    payback ceiling      fired (400 turns, ALL countries)
    10 turns                 5
    30 turns                33
    100 turns               84

Even at a hundred-turn ceiling -- which buys HALF of all candidates, including
deals that take a quarter of the game to repay -- it fires 84 times across
about twenty countries and four hundred turns. At the ten-turn ceiling that
represents a genuinely good deal, five.

So the head declining specialisation 99.7% of the time is not a collapsed
head refusing a good action. It is a correct response to a menu that is
overwhelmingly bad deals, and the good ones are nearly singular: 13,076 of
20,740 opportunities had NO candidate at all.

CONCLUSION: specialisation is not where the missing income is, and the note's
suggestion is half-refuted -- not because specialising is bad, but because
there is not enough of it on the map to matter. Trade and resource
development remain untested.

Reflex reverted; the firing counts are the artefact and they are here. Cost
about twenty minutes against a ~2 hour A/B that would have measured 5 events.

## 254 — half the politics head is dead, and the documented trade fix did not work

OD_ACT_HIST on the politics module, 400 turns, N24, 30,471 decisions. Mapping
from the source comment: hold policy pac+ pac- cancel ally nap guarantee calm
concil repress TRADE.

    a0  hold          offered  30471   taken 56.99%
    a1  enact policy  offered   7318   taken  0.00%
    a2  pacify UP     offered  10976   taken 21.35%
    a3  pacify DOWN   offered  18251   taken  0.00%
    a4  cancel policy offered  22210   taken  0.00%
    a5  alliance      offered   6897   taken  0.72%
    a6  NAP           offered   5560   taken  6.64%
    a7  guarantee     offered   6858   taken  0.00%
    a8  calm          offered  11022   taken 51.94%
    a9  conciliate    offered  11572   taken 39.92%
    a10 REPRESS       offered  30471   taken  0.00%
    a11 TRADE         offered   2287   taken  0.00%

SIX OF TWELVE ARE EXACTLY ZERO, on tens of thousands of offers each.

1. REPRESS IS OFFERED ON EVERY SINGLE DECISION -- 30,471 of 30,471 -- and
   taken never. That answers the standing question "why does the AI never
   repress" as definitively as it can be answered: not a mask problem, not
   availability. The head is offered it every time and refuses every time.

2. THE TRADE FIX DID NOT WORK. The source note records that a flat 400
   treasury floor was "the single reason AI-to-AI trade had never once
   happened", and derives a lower floor so it "cannot silently become a
   prohibition again". The floor was the diagnosis and it was fixed. Trade is
   now offered on 7.5% of politics decisions against the note's 3.1% -- and is
   still taken ZERO times. The fix raised the offer rate and changed no
   behaviour, which is [[mask-changes-need-a-retrain]] exactly: a frozen policy
   does not take a newly-available action. The note anticipated that wall for
   specialisation and not for trade.

3. PACIFICATION IS A ONE-WAY RATCHET. Up is taken 21.35%, down is taken 0.00%
   on 18,251 offers. So pacification only ever rises by the head's choice, and
   falls only when austerity cuts it -- i.e. only once the country is already
   insolvent. It is 10.8% of gross (journal 252). Same SHAPE as the research
   ratchet whose fix shipped for +71/+42, though the research one had both
   directions alive (95.5% up, 29.5% down) where this has one direction dead.

4. DOCTRINE MANAGEMENT IS ENTIRELY REFLEX-DRIVEN. enact 0.00%, cancel 0.00%,
   yet austerity repeals doctrines 28 times a world. The head does not manage
   doctrines at all; the reflex does.

Nothing acted on yet. Recording because this is the first time the politics
head has been measured action-by-action, and three of these were open
questions.

## 255 — 94% of pacification buys nothing, and removing it costs 90 rating

The politics head raises pacification on 21.35% of offers and lowers it on
0.00% of 18,251 (journal 254). Instrumented what that costs: the rebellion sum
is clamped at zero on return, so suppression that overshoots is invisible while
the money is still spent.

    applied 19,471,985   needed 1,029,786   WASTED 94.7%   over 1.63M province-turns

Pacification is 10.8% of gross (journal 252), so that reads as ~10% of all AI
income spent on nothing, against a net margin of 7%.

Built pacificationTrimReflex: ask the resolver for the smallest allocation that
still zeroes every province (Game::pacificationNeeded, exposing the resolver's
own arithmetic rather than copying it), step down 0.125 a turn, never below
need + slack. Verified inert unset -- control seat score 3.1 exactly. At slack
0.10 it cut applied suppression 19.5M -> 7.5M, a 61% reduction.

    reliable-seat rating       hold-out C   435 -> 338   (-97)
                               hold-out D   400 -> 314   (-85)

REPLICATED AND DECISIVELY NEGATIVE. Every reliable seat loses land on both
sets; France halves on C.

WHY, AND IT IS THE USEFUL PART: the overshoot is a BUFFER. Unrest fluctuates;
suppression above current unrest is what stops it ever crossing the threshold.
Because it works, the clamp records it as having cancelled nothing. My metric
compared suppression against CURRENT unrest and could not see the spikes that
never happened -- the better the buffer, the more wasteful it looks.

So the head's 0.00% pacify-down rate is correct play and the "ratchet" is not a
defect. Fourth unrest lever to lose.

TWO METHOD NOTES:
  - The aggregate would have called set D inconclusive: OD BENCH 297 -> 274,
    inside that arm's own +/-42. The reliable-seat rating said -85. Two pinned
    seats dilute the headline, which is why the per-seat read is not optional.
  - I told the user this was "the strongest lead since the shipped change" on
    the strength of the mechanism being LARGE. It was large in the wrong
    direction. Mechanism size has not predicted sign once today.

Reflex and the Game helper reverted; the waste counter stays behind OD_ACT_HIST
because the next person will ask the same question. Patch kept at
scratchpad/pactrim-FAILED.patch.

## 256 — the knob space is exhausted, and now there is a map rather than an assertion

Checked the last two unexamined heads against what is already recorded, before
building anything. Both were closed: raising pacification is OD_PACIFY_REFLEX
(N37 255 -> 171, journal 40), and every gated form of suing for peace is worse
(journal 14128). So the day ends with a subsystem map instead of a list of
attempts.

WHAT IS MEASURED AND CLOSED, with the direction that was tested:

    research allocation   optimum at 0.45; BOTH directions measured
                          (0.25/0.35 -> 202/262, 0.65 -> 372, vs 435)
    pacification          local optimum; BOTH directions measured
                          (trim -97/-85 today, raise -84 on N37)
    peace / ceasefire     every gated form worse; head takes it 0.27%
    austerity ordering    forced -- no cheap resource left to substitute
    social budget share   0.33 never binds; bill sits at 28.8% of gross
    specialisation        5 good candidates per 400 turns; not a lever
    trade                 offered 7.5%, taken 0.00%; the floor fix raised
                          availability and changed no behaviour
    doctrine repeal       sticker ranking is right; lever signs are inverted
                          from how they read
    military spending     0.8% of gross; width caps power, not money
    minority spend        three gates, all lost
    reflex layer          11/11 audited, defaults correct
    narrowings            0 for 11

WHAT THE BUDGET LOOKS LIKE (journal 252, first ever measured): research 41.1%,
minority 18.0%, industry upkeep 17.9%, pacification 10.8%, policy 4.2%, navy
0.5%, army 0.3%. Net margin ~7%.

THE CONCLUSION, stated plainly: the AI's ALLOCATIONS are approximately optimal
and its refusals are approximately correct. Every place a rule could move money
or force an action has been measured, and the head was right in almost every
case. The three big refusals -- repress 0.00% of 30,471, trade 0.00% of 2,287,
pacify-down 0.00% of 18,251 -- are priced decisions, not collapse. Journal 255
is the clearest case: 94% of pacification "buys nothing" and removing it costs
90 rating, because the overshoot is a buffer against spikes that then never
happen.

SO FURTHER IMPROVEMENT NEEDS A DIFFERENT POLICY, not a different rule. That is
training, which has failed 16 checkpoints out of 16 from this parent
(memory: training-degrades-the-model), and the one lever on it that is known to
work -- N_STEP, which collapses the conciliation bill thirtyfold -- only
matters if training works at all.

The honest summary of the day: one real improvement found and shipped
(+71/+42, replicated), one release, six instrument changes, and a systematic
map of why the remaining knob space is empty. The map is worth more than
another attempt would have been.

## 257 — training never meets the opponent it is scored against, and that is not why it fails

WHY TRAINING FAILS, part one: a real defect. setRandomCountries() is called
from runAIEvaluation and NOWHERE ELSE. So during every --train-ai run
m_randomCids is empty, isRandomCountry() is false for every country, and
m_scriptedThisCountry can never be set. Counted rather than argued:

    training     1500 country-turns   inCohort 0     scripted 0
    evaluation   first decision       inCohort 1     scripted 1

Two consequences. The policy is optimised against copies of itself and scored
against a world of scripted opponents it has never played. And the variant mix
in AISystem.cpp -- SCRIPT_BLITZ/TECH/DIPLO/NAVY/TURTLE, labelled TRAINING ONLY
and written because "across an entire training run the policy faced exactly one
strategy" -- is gated on isRandomCountry() and HAS NEVER EXECUTED in the path
it was written for. The diagnosis was right, the fix was built, and it was
unreachable.

Wired it in behind OD_TRAIN_SCRIPTED_SHARE (0 = off = unchanged). At 0.33,
29% of training country-turns are played by scripts, and the mix fires on every
map (18 of 53 on 1914, 25 of 76 on crowded).

THE EXPERIMENT: one parent (N24, md5 verified identical in both sandboxes),
8 maps x 300 turns, OD_LR_SCALE=0.25, benched on hold-out C.

    arm              reliable seats     land
    parent                    433      81.97%
    self-play only             11      10.27%
    scripted 0.33              53      16.80%

THE FIX HELPS AND IT DOES NOT MATTER. +42 reliable against pure self-play, and
both arms are catastrophically below a parent they started from. 433 -> 11 is
not erosion, it is destruction, and 2,400 turns did it at QUARTER learning
rate (OD_LR_SCALE verified read at AISystem.h:3109, not merely passed).

NOT A LOAD BUG. Trained 1 map x 5 turns from the same parent and benched it:
28.6 on the seat against the parent's 18.9. The parent deserializes and plays
fine; the damage accumulates over the run.

SO THE REAL PROBLEM IS NOT THE OPPONENT DISTRIBUTION. It is whatever makes
2,400 turns of gradient destroy 97% of a model's rating. The self-play/scripted
mismatch is a genuine defect and worth having fixed, but it is a second-order
term next to this. Sixteen checkpoints failing was never going to be explained
by the opponent mix.

Next question, unanswered: where in the run does the collapse happen? A cliff
and a slope want different fixes, and benching at 1, 2, 4 and 8 maps would say
which this is.

## 258 — the collapse is a cliff inside the FIRST map, and it is selective

Benched independent trainings from the same parent at 1, 2, 4 and 8 maps
(same seed, so map N is the same map in every run), on the three seats that
can measure. Raw land, 3 seeds each:

    maps      1914:FRA   1939:USA   modern:CHN
    parent      27.57      21.77       18.13
    1            1.27      26.57        0.00
    2            1.27      16.40        2.17
    4            3.40      27.57        1.67
    8            0.30       1.63        0.00

A CLIFF, NOT A SLOPE, and it lands inside the first 300 turns. France goes
27.57 -> 1.27 and China 18.13 -> 0.00 after ONE map. Combined with journal
257's five-turn check, which came back healthy at 28.6, the whole collapse
happens between turn 5 and turn 300 of the first map.

AND IT IS SELECTIVE, which is the informative part. USA survives four maps and
at four maps BEATS the parent (27.57 against 21.77) while the other two seats
are already destroyed. Training is not uniformly damaging the network; it is
moving the policy toward whatever the training map rewards and away from
everything else. That is catastrophic forgetting, not erosion.

It also retires my own framing from journal 257. I described 433 -> 11 over
2,400 turns as if the run accumulated damage. It does not: the damage is done
in map one, and maps two through eight mostly hold the wreckage in place.

WHY THIS MATTERS FOR THE FIX: an opponent-distribution change (journal 257's
OD_TRAIN_SCRIPTED_SHARE) cannot address a policy that forgets two seats inside
one map. Nor can a longer run, more maps, or a better objective. The candidates
are step size, and the absence of anything holding the policy near a parent
that already plays well -- replay across maps, a trust region, a KL penalty.

Step size is the cheapest to falsify and is running: one map at OD_LR_SCALE
0.25, 0.05 and 0.01. If France survives at 0.01 this is a step-size problem
with a boring fix. If it does not, no learning rate saves it and the update
rule needs something that remembers.

## 259 — training goes from 45% of parent to 89%: step size plus the opponent fix

Journal 258 located the collapse as a cliff inside map one. Falsified step size
as the cause, one map from the same parent, France seat, 3 seeds:

    parent   27.57
    LR 0.25   1.27      <- destroyed; this is what every run used
    LR 0.05  18.47      <- survives
    LR 0.01  16.73      <- no better than 0.05

So 0.25 is catastrophic and 0.05 is not, and lowering further buys nothing.
NOTE this contradicts memory ladder-quarter-rate, which records 0.25 as having
HELD a strong parent and beaten full rate by 36 paired. Either that was
conditions-specific or it is stale; here 0.25 destroys France in 300 turns.

Then eight maps at the safe rate, with and without the journal 257 fix:

    arm                        FRA     USA     CHN    reliable
    parent                   27.57   21.77   18.13       433
    self-play, LR 0.05        7.40   18.77   11.77       305   (-128)
    scripted 0.33, LR 0.05   19.37   20.87   20.43       387    (-46)

THE OPPONENT FIX IS WORTH +82 once the step size is not drowning it -- it was
worth +42 at LR 0.25. China at 20.43 BEATS the parent's 18.13.

387/433 is 89% of parent. Memory training-degrades-the-model records all
sixteen historical checkpoints as "none above 45% of parent". Two changes --
a step size that does not destroy, and an opponent the policy is actually
scored against -- move training from catastrophic to nearly break-even.

STILL NEGATIVE, and France carries almost all of it: 19.37 against 27.57 while
USA and CHN are at or above parent. So there is a France-specific failure left
that the other two seats do not share.

OPEN: is 387 a waypoint upward or a stop on the way down? Sixteen and
twenty-four maps at these settings are running. If the curve climbs, training
works and the remaining question is how long to run it. If it bleeds, the
missing piece is an anchor -- replay across maps, a trust region, a KL penalty
-- because nothing currently holds the policy near a parent that already plays
well.
