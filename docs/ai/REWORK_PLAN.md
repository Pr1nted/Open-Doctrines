# Parrot Zero rework plan

The plan for turning Parrot Zero from a set of per-module policy nets into a
search-based engine, in the manner of a chess engine: generate candidate moves,
look ahead, and score positions with an evaluator. It also covers the network
visualiser planned for 1.3.3b.

This document is the plan. The groundwork it lists is meant to land before the
rework starts, and it must not change how the current AI plays:
`OD_DECISION_HASH` has to stay identical through every groundwork step.

## Where Parrot Zero is today

A summary. `wiki/AI-Architecture.md` is the full reference.

- **Decisions.** `AISystem::takeTurn` (`src/ai/AISystem.cpp`) runs once per
  country per turn. Four modules (economy, politics, war, navy) each pick one
  action from a small fixed menu: 12, 12, 8 and 7 actions. Legality masks gate
  the menus. A stance head picks a posture, and learned heads score war
  targets and attacks.
- **Execution.** Each chosen action expands into real orders through about
  10,000 lines of hand-written executors and reflexes (`execEconomy`,
  `execWar`, garrison, redeploy, peace, campaign...). Those write straight
  into `Game::m_pending*`.
- **Networks.** Small MLPs (`src/ai/NeuralNet.*`). A shared trunk
  (143→512→320) feeds policy, Q and value heads per module. There is also a
  neighbour-attention encoder and a MuZero-style latent dynamics net. Weights
  are in `data/ai/model.bin` (ODAZ/ODAI).
- **Search.** There is some, but it never touches the real game:
  - `mctsPolicy` is PUCT over the *latent* dynamics model. It is off by
    default.
  - `searchScores` is a 1-ply plus beam-3 2-ply search, also in latent space.
  - Both search one module of one country, and never model opponents.
- **Training.** In-repo self-play: PPO, behaviour cloning from the scripted
  book, and a league of frozen checkpoints. Parallelism is separate processes
  whose models are averaged (`tools/train_parallel.py`).

## Why it plays badly

These are the structural reasons, not the tuning ones.

1. **It never looks ahead in the real game.** Each decision is one forward
   pass on 143 summary features. It cannot see that an attack leaves its
   capital open, or that a war will draw in a guarantor.
2. **What it learns is not what happens.** The nets choose a macro action,
   and hand-written executors decide what that means in orders. The reward
   therefore credits the net for the executors' choices.
3. **Opponents are invisible.** Forty countries move at once, and every one
   of them decides as though the others will stand still.
4. **The value signal is weak.** Learned value heads over coarse features
   take the place of an evaluation of the position.

A chess engine avoids all four: it generates legal moves, plays them forward
on a copy of the position, and scores the results. Every structural blocker
below exists because Open Doctrines cannot do those three things yet.

## The target engine

```
              ┌──────────────────────────────────────────────┐
  WorldState ─┤ MoveGen: candidate order sets, per country    │
  (copyable)  │  (macro plans compiled to orders, pruned by   │
              │   a policy prior)                             │
              ├──────────────────────────────────────────────┤
              │ Search: for the acting country, over its      │
              │  candidates; opponents answer with a fast     │
              │  policy (sampled), N turns deep               │
              ├──────────────────────────────────────────────┤
              │ step(WorldState, Orders[]) → WorldState       │
              │  (the real turn resolution, deterministic)    │
              ├──────────────────────────────────────────────┤
              │ Evaluator: hand-written terms first, then a   │
              │  value net trained on search results          │
              └──────────────────────────────────────────────┘
```

- **Search algorithm.** Simultaneous moves and around 40 players rule out
  plain alpha-beta. Two options fit:
  - Sampled-opponent MCTS: the acting country is searched, and the other
    countries play their policy net's moves.
  - Best-response search at a shallow depth, 1 to 3 turns, with a small
    number of candidate order sets per country.

  Start with the second. It is cheaper, easier to debug, and it is what the
  evaluator needs first.
- **Move generation.** Candidates are *plans*, not single orders: "attack
  X with the southern front", "build 3 divisions and hold". Plans compile to
  order sets. The existing executors are the first plan compilers, so the
  10,000 lines are reused rather than thrown away.
- **Evaluator.** Start hand-written and readable: land, manpower, industry,
  frontline balance, threat from neighbours, alliance coverage. Once search
  works, replace it with a value net trained on search outcomes. That is the
  AlphaZero loop: search improves on the net, and the net learns the
  search's results.
- **Budget.** Search runs on the host only, as now. It runs inside the
  multiplayer turn timer, with a hard node and time budget. Low-end builds
  (DOS, Wii, 3DS, web) keep the depth-0 policy, which is today's behaviour.

## Groundwork, in order

Each item is behaviour-neutral. It ships with a test that the decision hash
and the save round trip are unchanged.

1. **One `Order` type.** About 15 `Pending*` structs (`src/GameStructs.h`)
   become alternatives of a single `Order` variant, with
   `apply(Game&, const Order&)` and a byte encoding. The multiplayer wire's
   order encoding is the starting point. Everything that issues orders,
   including the AI executors, goes through it.
   - *Unlocks:* move generation can emit orders, the visualiser can display
     them, and the AI's real action space becomes visible.
2. **An `AIPlayer` interface.** `decide(const WorldView&, cid) →
   std::vector<Order>` replaces direct writes into `m_pending*`. Parrot Zero
   becomes the first implementation behind it.
   - *Unlocks:* a second engine can be developed beside Parrot Zero, chosen
     per country or per difficulty, and A/B-tested with `tools/ai_ab.py`.
3. **Deterministic randomness everywhere.** The four leftover `rand()` calls
   in turn resolution and the `srand(time)` seeding move onto `simRand()`.
   `g_simRng` becomes a member of the state instead of a file static.
   Iteration over unordered maps in resolution becomes sorted.
   - This does change the dice. It is the one item that moves the decision
     hash, so it lands alone, with a new determinism baseline.
4. **Extract `WorldState`.** Gameplay state moves out of `Game` into a plain,
   copyable struct: provinces' gameplay fields (not the raster), countries,
   armies, ships, relations, pending orders and RNG. `Game` keeps rendering,
   UI and the raster, and holds a `WorldState`.
   - This is the largest item. It can be done one field group at a time,
     behind accessors, with the save round trip test guarding each step.
   - The target size is a few MB, so a search can afford copies.
5. **A pure `step()`.** Turn resolution is split from its side effects:
   saving, loading frames, achievements and multiplayer sends.
   `step(WorldState&, const std::vector<Order>&)` must give byte-identical
   results to `processTurn` (`[DET]` hashes). The headless server build
   already proves resolution can run without drawing.
6. **Visualiser hooks.** `NeuralNet::forwardInto(Scratch&)` already keeps
   per-layer activations. Add an opt-in trace sink: when a visualiser (or a
   test) is attached, `takeTurn` records each decision's input features,
   per-layer activations, masked logits and chosen action next to the
   existing `Decision` log. When nothing is attached it costs nothing.
   - This is small and can land first. The 1.3.3b visualiser needs nothing
     else from the AI.

Items 1, 2 and 6 are independent and small. Items 4 and 5 are most of the
work, and are what the rework is waiting on.

## The visualiser, 1.3.3b

Built on the trace sink (groundwork 6). Proposed views:

- **Network view.** Each net's layers drawn as columns. Neurons are coloured
  by activation for the selected decision, and edges by weight × activation
  (the strongest contributions only).
- **Feature attribution.** For the chosen action, which of the 143 inputs
  pushed it there: gradient × input, which the backprop code can already
  compute.
- **Decision timeline.** Per country and per turn: each module's masked
  distribution, what was picked, and the value estimate. This is the
  existing debug overlay, made browsable.
- **Map link.** Selecting a decision highlights the provinces its orders
  touched, using `m_turnOrderLog`.

It can be an in-game panel, since raylib is already there. It can also be an
HTML page that reads a trace file, which works for training runs on servers.
The trace file format should come first, so both are possible.

## Training, after the rework

- Generate self-play with search on, at a low budget. Train the policy on
  the search's visit distribution and the value net on outcomes, as
  AlphaZero does. The current PPO path stays until the new loop beats it.
- One world takes about 3 GB per process, which caps training parallelism.
  `WorldState` fixes this: many states can share one loaded map.
- Every candidate engine is measured with `--eval-ai` and `tools/ai_ab.py`
  against the current model, on fixed seeds. It ships only when it wins.

## Milestones

| Version | Content |
|---|---|
| groundwork | items 1, 2, 6 (small, behaviour-neutral), then 3 alone |
| 1.3.3b | visualiser; `WorldState` extraction under way |
| next | pure `step()`; hand-written evaluator; shallow best-response search behind `AIPlayer`, A/B against Parrot Zero |
| after | value net trained on search; MCTS with sampled opponents; retire executors that plans replace |
