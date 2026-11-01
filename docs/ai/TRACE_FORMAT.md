# The AI decision trace (`OD_AI_TRACE`)

What the 1.3.3b visualiser reads, and what anything else that wants to look
inside ParrotZero's decisions can read too. Groundwork item 6 of
[REWORK_PLAN.md](REWORK_PLAN.md).

## Turning it on

```
OD_AI_TRACE=/path/to/trace.jsonl OpenDoctrines --eval-ai 1 20 7
```

Any mode that runs the AI writes it: play, `--eval-ai`, `--train-ai`, the
server. The file is truncated when the first AI is created and kept open for
the whole process, across map rotations. If it cannot be opened, the game says
so on stderr and runs untraced.

Unset, it costs one null-pointer check per decision. Set, it does not change
what the AI decides: the record is built by re-running the nets through
`NeuralNet::forwardInto` into scratch buffers of its own, which writes nothing
the AI reads and draws no random numbers. Check it the usual way:

```
OD_DECISION_HASH=1                     OpenDoctrines --eval-ai 2 30 7 | grep DECHASH
OD_DECISION_HASH=1 OD_AI_TRACE=t.jsonl OpenDoctrines --eval-ai 2 30 7 | grep DECHASH
```

The two hashes must match.

It is big: about 9 KB a decision, so a two-map, thirty-turn eval writes
around 300 MB. Trace short runs.

## The format

[JSON Lines](https://jsonlines.org): one JSON object per line, one line per
module decision. A module that acts several times in a turn writes one line
per pick. Lines are in decision order, which is the order `OD_DECISION_HASH`
hashes in.

| key | type | meaning |
|---|---|---|
| `v` | int | format version, `1`. A reader should refuse a version it does not know. |
| `turn` | int | the AI's turn counter |
| `cid` | int | the deciding country's id |
| `module` | int | 0 economy, 1 politics, 2 war, 3 navy (`AISystem::Module`) |
| `pick` | int | 0 for the module's first pick this turn, then 1, 2, ... |
| `action` | int | the chosen action, after every override, before the executor runs |
| `source` | string | who chose it, below |
| `value` | float | the module's value net on `features` |
| `valid` | int[] | the legality mask, 1 = offered |
| `logits` | float[] | the policy head's raw output, before bias, mask or mixing |
| `probs` | float[] | the distribution the policy sampled from (after mask, critic and search bias, temperature and mixing). Empty when no net drove the pick. |
| `features` | float[] | the trunk's input, the 143 features |
| `trunk` | float[][] | the trunk's activations per layer: the input copy first, the output (the embedding) last |
| `policy` | float[][] | the module's policy head's activations per layer: its input (the embedding) first, the logits last |

`source` is one of:

- `policy`: the policy head sampled it.
- `book`: the opening book (`AI_OPENING_TURNS`) played it. `logits` are still
  the policy's opinion, which is what the book trains it toward.
- `script`: the scripted rung.
- `random`: the random cohort.
- `league`: a frozen league opponent's policy. `trunk` and `policy` are that
  opponent's nets.
- `search`: the MCTS visit distribution chose it (`mctsSims() > 0`).
- `mod`: a mod's `Neural.Decide` overrode the AI's choice.

Floats are written with `%.6g`. A non-finite value is written as `null`, so a
line with a NaN still parses.

Layer arrays may be empty when the model has no such net (a model file without
that head).

## What it does not carry yet

- The orders an action expanded into. Those go through `sim::OrderSink`
  (`src/sim/Order.h`); a recording sink can add them to the trace once the
  visualiser wants the map link.
- The stance head, the diplomacy net, the target and attack heads. They are
  separate decisions; they get records of their own when the visualiser needs
  them.
