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

N24 is far stronger overall, which is why it ships. **This model is kept
for one reason: on the invaded seat it means 6.3 land against N24's 1.17
(scores 95 and 17).**

Read the spread before drawing a mechanism from that, because those are
means of three seeds and they are bimodal:

| seed | N24 | this model |
|---|---|---|
| 909091 | **3.1** | 0.2 |
| 20230115 | 0.2 | **8.7** |
| 42424242 | 0.2 | **10.1** |

So it wins two worlds decisively, and on the third it is annihilated just
as N24 usually is — that world is the one where N24 does better. The
honest claim is that it holds an invaded country on most worlds where N24
holds none, NOT that it reliably survives invasion. With n=3 and that
variance, the difference in means rests on two observations.

No rule change has reproduced even that much: the one that appeared to
(austerity step 0.15 -> 0.05) inverted on a second seed set and was
retracted, and gating the shipped research change on being overrun made
the seat three times worse.

Caveats: trained at whatever difficulty the owner plays and benched at 3,
so 68 is a floor, not a fair overall measure. It has its own catastrophe
at 1914:SWE, score 7.

Load it without replacing the shipping model:

    OD_EVAL_MODEL=data/ai/models/play-trained-2026-09-09.bin ...

Same architecture as the shipping model (magic ODAZ, version 01,
uncompressed size 9,872,777 bytes), so it loads with no migration.
