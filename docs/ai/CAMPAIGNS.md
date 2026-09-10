# Campaigns: giving the AI something to plan WITH

Status: DESIGN, approved in principle by the user 2026-09-05. Nothing built.
Companion to docs/ai/LOOP.md and the entries around journal 52.

## The problem, stated as three measurements

1. **The credit horizon is 12 turns.** `N_STEP = AI_PLAN_HORIZON = 12`, plus
   one `BOOTSTRAP_DISCOUNT = 0.9` on the value of the window's end state. The
   seat bench measures 120 turns. A decision that pays at turn 40 barely
   reaches the weights.
2. **There is no search at decision time.** A latent MCTS exists
   (`mctsPolicy`, depth 6) and is off unless `OD_MCTS_SIMS` is set. Every
   shipped decision is one forward pass.
3. **Nothing the AI does persists.** Every action resolves inside its turn.
   A plan cannot be *expressed* in the action space, so it cannot be rewarded,
   so no amount of training produces one.

The third is the architectural one, and the evidence that it matters is the
peer's standing battles: the first change that made a decision carry across
turns, and the frozen models' floor rose on it (worst seat 15-21 -> 26-33).

## The change

A **campaign** is a commitment with a target, a budget and a deadline, chosen
once and executed by a resolver over many turns.

    struct Campaign {
        int    countryId;
        int    targetCountry;      // WHO it is against -- see the note below
        int    targetProvince;     // the current objective inside that country
        int    stagingProvince;    // where force is gathered
        long long committedMen;    // budget: men earmarked, not yet spent
        int    startedTurn;
        int    deadlineTurns;      // abandon if not achieved by then
        enum Kind { CONQUER, DEFEND, LAND } kind;
        // measured, so the reward can see whether it worked
        int    provincesTaken, provincesLost, roundsFought;
    };

**Phase 1 opens campaigns from a REFLEX, not a head action.** The war policy
head is `NeuralNet({TRUNK_OUT, WAR_ACTIONS})` with WAR_ACTIONS = 8, so a ninth
action changes the net shape: an ARCH bump, and every model file in
build/loop/reference refused. That is a real cost and it should be paid only
once the mechanism is known to work. A reflex needs no retrain to show a
signal, and reflexes are what produced every gain on 2026-09-05 (the siege
reflex, the guarantor bar). Phase 2 gives the head the choice, bumps ARCH to
9, and widens the loaded 8-row head to 9 with the new row fresh so today's
models survive the bump.

Everything else is resolver in both phases:

- Recruitment and reinforcement prefer the staging province while a campaign
  is open, so the commitment shapes the ordinary turn-by-turn actions instead
  of competing with them.
- Attacks from the staging province are issued by the campaign, not chosen
  per turn, so a repulse does not re-open the decision every turn.
- The campaign closes on success (target held), on deadline, or when the
  committed men fall below a floor. Closing is a resolver rule, not a head
  decision, because the withdraw experiments (journal 46) showed the head
  cannot judge "this is going badly" from one turn's evidence.

## Why this should work where reward shaping did not

The reward does not need a new term. A campaign that takes its target
produces land, and land is already the largest term (`PHI_PROV = 2.4` plus
2.0 in the war line). What changes is that ONE decision now owns twelve
turns of consequences, so the existing credit horizon covers the whole
decision instead of a twelfth of it. That is the same shape as the standing
battle: the peer did not add a reward for persistence, they made persistence
the default and the existing reward found it.

## How it will be measured

- **Invariant first** (the peer's technique, and its known limit): with no
  campaign open, every code path is byte-identical to today. Assert it per
  call, not by comparing runs.
- **Then the isolated bench**: one binary, `OD_CAMPAIGNS=0/1`, five frozen
  models plus the shipped one. A mask change needs both instruments
  (journal 52): the invariant cannot see how a learned policy redistributes.
- **Then a retrain**, because this adds an action, and journal's
  mask-changes-need-a-retrain applies in the opposite direction: a frozen
  policy has never chosen `open campaign` and its prior on it is arbitrary.
  This is the first retrain in this project with a reason to expect a gain
  (the action space genuinely grows) rather than the world moving under a
  fixed one.

## Risks, in the order I expect them

1. **The head never picks it.** The mask must offer it only when it would
   fire (`enactablePolicy`'s lesson), and the Dirichlet root noise in the
   latent MCTS is the only mechanism in this codebase that revives an action
   the policy scores near zero - which is an argument for running the search
   experiment first.
2. **It commits and starves the rest.** Budget is a share of the army, and
   the earmark must not starve the frontier the way `siegeEarmark` starved
   the industry head (journal 39g).
3. **It reads as a regression on frozen models.** Expected; the retrain is
   the measurement, and the frozen five are the control for the world change.


## What the first implementation got wrong (2026-09-05)

A campaign was written against a PROVINCE and it was pointless: 84 of 89
closed on the turn they opened. An adjacent province the AI can beat falls to
the ordinary attack in one turn -- 97% of assaults in this game are walk-ins
into empty ground -- so there was nothing for a multi-turn commitment to be
about. Re-targeting a campaign at a COUNTRY fixed it: 58 opened, lifetimes 1
to 12 turns, 46 ending in BEATEN (nothing of theirs left within reach).

The general lesson, which applies to the next attempt as much as this one:
the grain of a plan has to match the grain of the game's slowest real
process. Here that is finishing an enemy, not taking a tile.
