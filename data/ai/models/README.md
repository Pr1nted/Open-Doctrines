# Kept models

Models worth keeping that are not the shipping `data/ai/model.bin`.

## play-trained-2026-09-09.bin

Trained on the repo owner's own play rather than by self-play. Never
committed at the time — it existed only as an uncommitted change to
`data/ai/model.bin` and was replaced by N24 in 8be6b40, so this is the
only copy in version control.

Benched 2026-09-10 on hold-out seeds 909091 / 20230115 / 42424242,
400 turns, difficulty 3, against the same scripted world as N24:

| seat | par | this model | N24 (shipping) |
|---|---|---|---|
| 1914:FRA rung | 6.7 | 79 | 413 |
| 1914:SWE rung | 1.0 | 7 | 500 |
| 1939:USA rung | 5.6 | 151 | 391 |
| modern:CHN rung | 2.5 | 36 | 500 |
| **1914:FRA rush** | 6.7 | **95** | **17** |
| 1939:NOR hood | 1.3 | 41 | 46 |
| | | **OD BENCH 68** | **OD BENCH 311** |

N24 is far stronger overall, which is why it ships.

**The reason this model was originally kept did not survive testing.** It was
kept because it scored 95 on the invaded seat against N24's 17 — a 5.6x gap.
Re-measured on 11 paired seeds (the original 3 plus 8 fresh), that gap is
mostly an artefact of which seeds were used:

| | original 3 seeds | all 11 seeds |
|---|---|---|
| N24 | 1.17 | **4.24** |
| this model | 6.33 | 6.43 |

    paired difference  +2.19 land   se 2.01   t = 1.09
    95% CI             [-2.29, +6.67]   — spans zero
    wins               this model 7 of 11

This model's score was representative. N24's was not: the original three
seeds happened to contain three of N24's collapse worlds, understating it
3.6-fold. On 11 seeds the difference is not statistically distinguishable
from zero.

**What IS real, and is the more useful observation:** the invaded seat is
BISTABLE for both models. Every run either holds 5.7-11.3 land or collapses
below 1.5 — almost nothing in between. Collapse rate is 5/11 for N24 and
3/11 for this model. So a 3-seed measurement of that seat is estimating a
Bernoulli parameter from three coin flips, and will swing wildly.

Kept anyway: it is the owner's own play data, it is not reproducible, and
nothing else in the project was trained on human play.

Caveats: trained at whatever difficulty the owner plays and benched at 3,
so 68 is a floor, not a fair overall measure. It has its own catastrophe
at 1914:SWE, score 7.

Load it without replacing the shipping model:

    OD_EVAL_MODEL=data/ai/models/play-trained-2026-09-09.bin ...

Same architecture as the shipping model (magic ODAZ, version 01,
uncompressed size 9,872,777 bytes), so it loads with no migration.
