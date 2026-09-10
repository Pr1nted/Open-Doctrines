# ParrotZero: the AI's name and version

The AI is **ParrotZero**. The name is a joke at its own expense: it has
AlphaZero's shape — a trunk, policy and value heads, a latent forward model,
even an MCTS — and does not use most of it, so what it mostly does is do an
impression of a thinking machine. On 2026-09-05 that impression got good
enough to beat the shipped model by better than two to one on the seat bench,
which is the other half of the joke.

## The number

    ParrotZero <ARCH>.<RULES>.<PATCH>          e.g. ParrotZero 8.1.0

Independent of the game's version (1.1.2a today). The game's number moves for
UI fixes and translations; the AI's moves when the AI moves.

| part | bump it when | consequence |
|---|---|---|
| ARCH | the net shape or the action space changes | old model files are refused; it IS the model file's format byte, asserted equal in `AISystem::saveModel` |
| RULES | behaviour a bench can see changes: a resolver rule, a reflex, a default | every measurement taken before it stops describing the code |
| PATCH | traces, counters, comments, knobs defaulting to off | nothing; if unsure, it is RULES |

Definition of record: `src/ai/AIVersion.h`.

## Model files are tagged separately

A weights file has its own identity, `parrotzero-<ARCH>.<RULES>-<lineage>`,
e.g. `parrotzero-8.1-N24`. "Which model" and "which rules" are different
questions, and this project spent a day mistaking one for the other: three
models measured "up" against baselines taken on three different binaries, and
the change responsible was a loss when it was finally isolated.

## Where it shows up

- Every eval prints `[EVAL] ai   ParrotZero 8.1.0`.
- `tools/od_bench.py` records it per label in `build/od_bench_results.json`,
  prints it beside the rating, and prints **MIXED VERSIONS** instead if the
  binary changed while the bench was running — which happened twice on
  2026-09-05 and invalidated both tables silently.

## The rule this exists to enforce

A seat rating is only comparable with another taken by the same ParrotZero
version. Comparing against a historical baseline in a shared tree is unsound
by default; use one binary with its own control.
