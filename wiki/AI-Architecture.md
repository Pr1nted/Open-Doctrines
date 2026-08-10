# The country AI

Every country in OpenDoctrines that is not the player is driven by the same
neural network. This page describes what that network is, what it can see, what
it is allowed to do, how it is trained, and where each piece lives in the source.

It is written to be read start to finish. Nothing here assumes prior knowledge of
reinforcement learning; the terms are introduced where they are first needed.

Source: [`src/ai/AISystem.h`](https://github.com/Pr1nted/Open-Doctrines/blob/main/src/ai/AISystem.h),
[`src/ai/AISystem.cpp`](https://github.com/Pr1nted/Open-Doctrines/blob/main/src/ai/AISystem.cpp),
[`src/ai/NeuralNet.cpp`](https://github.com/Pr1nted/Open-Doctrines/blob/main/src/ai/NeuralNet.cpp),
[`src/Game_AITrain.cpp`](https://github.com/Pr1nted/Open-Doctrines/blob/main/src/Game_AITrain.cpp).

---

## 1. The short version

- One shared model, stored in `data/ai/model.bin`, about 995,000 parameters
  across nine small networks, roughly 12 MB on disk.
- Every country reads the same weights but thinks for itself: its own view of the
  world goes in, its own decision comes out.
- Each country makes four decisions a turn, one per module: economy, politics,
  war, navy. A fifth network answers diplomacy aimed at it.
- Impossible actions are removed before the model chooses, so it never picks a
  move the game would have to reject.
- It learns by playing itself, headless, at roughly thirty turns a second, and
  the model file it produces is the one a normal game loads.

---

## 2. The networks

Twenty plain feed-forward networks. No convolutions, no recurrence; the one
attention is a pooling step over neighbours, not a transformer. Input layer, one
or two hidden layers with `tanh`, a linear output layer.

Most of them are **heads on one shared trunk**. The trunk is the encoder: it
reads the 143 features once per country per turn and every head below reads its
320-wide output, so the representation is learned from all four modules'
gradients at once instead of each module learning the same job alone.

| Network | Shape | Output means |
|---|---|---|
| Trunk | 143 - 512 - 320 | the shared embedding, `tanh` |
| Economy policy | 320 - 12 | one score per economic action |
| Politics policy | 320 - 11 | one score per political action |
| War policy | 320 - 8 | one score per military action |
| Navy policy | 320 - 6 | one score per naval action |
| Stance | 320 - 4 | expand / consolidate / defend / develop |
| Diplomacy | 320 - 2 | reject, accept |
| Q heads (four) | 320 - actions | expected return per action, blended into the policy |
| Value heads (four) | 143 - 160 - 1 | how well this country is expected to do |
| Diplomacy value | 143 - 160 - 1 | the diplomacy head's own baseline |
| War target | 152 - 256 - 128 - 1 | one score per country we could declare on |
| Attack target | 152 - 256 - 128 - 1 | one score per province we could assault |
| Neighbour encoder | 8 - 24 | one neighbour, embedded |
| Neighbour scorer | 24 - 1 | how much that neighbour matters, for attention pooling |

The value heads keep their own narrow pathway from the raw features rather than
reading the trunk: a critic should be free to disagree with the actor's
representation. The two target heads read raw features **plus** the candidate
being scored, which is a different input space again — there is one forward pass
per candidate rather than a fixed output layer, because the number of candidates
changes every turn.

Neither the value heads nor the Q heads are used to play in the ordinary sense.
They exist for training: see [section 8](#8-how-it-learns).

The network code is vendored and self-contained
([`NeuralNet.cpp`](https://github.com/Pr1nted/Open-Doctrines/blob/main/src/ai/NeuralNet.cpp)),
in the same spirit as the project's other third-party pieces. It builds anywhere
the game builds, including WebAssembly, and pulls in no dependencies.

### One model, many countries

There is one set of weights for the whole world. A forty-country map produces
around 160 decisions per turn, and every one of them trains the same model. That
is the reason self-play converges at a usable speed: experience is pooled, not
divided.

It also means countries are not individually characterised. Two countries in
identical situations will reach for the same move. What makes them behave
differently is that they are never in identical situations, and that action
selection is stochastic.

---

## 3. What the model sees

Each decision starts from a vector of 143 floating-point numbers, built by
`buildFeatures`. Everything in it is something a player could read off the user
interface. Nothing in it is hidden state, and nothing in it is a map coordinate:
values are ratios, shares and normalised logarithms, so a model trained on one
world transfers to another.

| Slots | Contents |
|---|---|
| 0-7 | treasury, net and gross income, expense shares by category, income trend |
| 8-15 | provinces, share of the world, population, army size and density, ship counts |
| 16-23 | wars, alliances, pacts, guarantees, frontier count, strongest and weakest neighbour relative to us |
| 24-31 | unrest sample, pacification spending, political compass, research progress, industry and fort density, best port |
| 32-42 | port tiers, affordability flags, rebellions this turn, turn number, active policies, army relative to the world average, loaded transports |
| 43-51 | research allocation, points, active node and its progress, build caps, key unlocks |
| 52-59 | total enemy and allied strength across all wars, outgunned flag, claims held against us and by us, overseas invasion opportunity |
| 60-66 | defensive posture: ground lost, share of borders under threat, enemy troops on those borders against our own, worst single deficit |
| 67-74 | coalition: allied troops nearby, allies fighting with us, allies sitting it out, staging routes, troops abroad, war weariness |
| 75-76 | fleet upkeep as a share of income, and whether the fleet has anything to do |
| 77-79, 85-86 | minorities: mean and worst alignment, whether current policy is winning them over or driving them out, what it costs, how many groups |
| 80-84, 87-94 | spare, plus request context written only when answering diplomacy (see [section 7](#7-answering-diplomacy)) |
| 95 | constant 1, the bias input |
| 96-103 | trends: how provinces, army, industry, population, treasury, threat, minority alignment and weariness have moved against a baseline up to `TREND_WINDOW` turns old |
| 104-111 | the world rather than us: concentration, the largest power's share, our share and rank, how much of the map is at war, how crowded it is |
| 112-115 | the posture currently in force, one-hot (see [section 5](#5-choosing-an-action)) |
| 116-139 | the neighbours, attention-pooled: each is embedded and scored, and the weighted sum lands here |

Two properties of this vector matter more than its contents.

**It is bounded.** Nearly every slot is passed through `tanh` or clamped to a
range. Treasuries and populations in a long game grow until they overflow a
float; unbounded inputs would take the whole model with them.

**It is finite by construction.** The last thing `buildFeatures` does is replace
any non-finite value with zero. One NaN input makes every output NaN, and a NaN
gradient corrupts the weights permanently, including the copy written to disk.

---

## 4. What the model can do

Four action menus. Each is a fixed list; the model outputs one score per entry
and one entry is chosen.

**Economy (12).** Save; build industry; build fortification; build or upgrade a
port; specialise a province; build a destroyer; build a carrier; raise or lower
research funding; direct research at buildings, army or navy.

**Politics (11).** Hold; enact the policy that best fits this country's
politics; raise or lower pacification spending; cancel the costliest active
policy; propose an alliance, a non-aggression pact, or a guarantee; enact a
policy aimed at calming the country; conciliate a minority; repress a minority.

The last three are the domestic half of government, and are described in
[section 9](#9-governing-at-home).

**War (8).** Hold; recruit; reinforce threatened borders; attack; declare war;
fire artillery; offer a ceasefire; stage troops on allied ground.

**Navy (7).** Hold; move the fleet; bombard; embark troops; land them (or bring
them home if there is no hostile shore); scrap a warship the country is paying
for and not using; engage an enemy hull.

Engage was added last, and its absence had been invisible: naval combat existed
and was resolved every turn, but only the player and the network could ever
queue an order, so an AI fleet sailed past an enemy fleet without attacking and
could only ever *be* attacked. It targets the way a person does -- a loaded
transport first, because sinking one kills the invasion it carries, then the
most damaged hull, ties to the nearest since damage falls off with distance.

Moving is routed rather than aimed. The fleet used to steer straight at the
nearest enemy port by straight-line distance with no test that a sea route
existed, which put 93% of all ship moves against a coastline and left them
there. It now follows waypoints over a coarse water-connectivity grid, and only
targets ports it can actually reach.

Every action is issued through the same pending-order queues the player's buttons
fill, and pays the same cost at the same moment. There is no separate AI code
path through the turn resolver, and no way for the AI to build something for
free.

That sentence was half true for a long time, and the half that was false is worth
recording. The queues were shared and every build *was* paid for -- nine
deduction sites, each refusing when the treasury could not cover it. But the
price was not the same one. `Game_Render.cpp` multiplied every build by the
research cost modifier and `AISystem.cpp`, working from its own duplicate copy of
the cost tables, did not. `industryCostPct` reaches 50 and `conscriptionCostPct`
reaches 50, so a country that had finished those trees built and recruited at
half price when a person ran it and full price when the AI did.

Nothing about that looked wrong from either side. The AI was not cheating; it was
being overcharged by its own research, and the economy module learned from the
overcharged world every training run it ever did. The tables now live in one
place (`src/BuildCosts.h`) with the modifier beside them, because a shared table
alone would not have caught this -- what diverged was not the numbers but what
was done with them.

### Choosing a verb is not choosing a move

The policy picks a *kind* of action. What that action then does was, for most of
these, a fixed rule — and since the rule decides where the army actually goes, it
rather than the policy was the ceiling on how well this AI could play. A player
who felt "the AI attacked my weakest province again" was experiencing forty lines
of C++, not a trained policy.

Two of those choices are now learned, both on the same pattern:

| Decision | Head | Warms up over |
|---|---|---|
| Whom to declare war on | `m_target` | 300k updates |
| Which province to assault | `m_attack` | 300k updates |

Each scores candidates one forward pass at a time and samples across the scores.
The arrangement that makes them trainable is the warmup: **below the threshold
the old rule still chooses, and the head merely watches and is told which
candidate the rule took**. Without that, nothing is recorded until the head is
good, and it is never good because nothing was recorded. It also means an
upgraded model plays exactly as it did until the head can do better.

**One decision covers every front.** For its whole life an "attack" produced
exactly one move order, so a country with fifteen active fronts pushed on one of
them while a player pushed on all fifteen — a cap on competence no amount of
training reaches. Worse, training *adapts* to it: pressing an attack you cannot
follow up really is worth less when you only get one, so hours of self-play
would have tuned a policy for a game that was about to change.

The fix is in the executor, not the decision. The policy still decides once that
this is a turn for attacking; the orders then go out on up to
`ATTACK_ORDERS_PER_TURN` fronts, ranked by the same head, one per launching
province. That is how a player plays — you resolve to go on the offensive and
then issue all your orders, you do not re-litigate it province by province. A
front whose launch province already has an order queued is now skipped rather
than abandoning the whole action, which used to turn one busy province into a
turn where the country did nothing at all.

What stays a rule, deliberately, is which candidates exist: a province is only
offered as a target if the assault is winnable on the same arithmetic as before.
That is a mask in the same sense the validity masks are, and it keeps the head
choosing between sane options rather than free to throw armies at fortresses.
The eval reports how many attacks the head actually aimed, because a head that
is steering badly and one that has not been let out yet look identical in every
other number.

### Validity masks

Before the model chooses, a mask marks each action possible or impossible, and
impossible ones are set to negative infinity. The model therefore never spends
probability on a move that cannot happen, and execution never has to reject a
choice.

This is load-bearing, not tidiness. Measured over a 400-turn run before the masks
were tightened, the war module answered "nothing to reinforce" 3,181 times and
"no researched ammunition" 3,271 times. Those were not decisions, they were turns
thrown away, and they taught the model that the war module mostly does nothing.

The masks encode real preconditions: reinforcing needs a neighbouring garrison
big enough to split; artillery needs a researched shell the treasury can afford;
embarking needs somewhere to invade; scrapping needs a warship that is genuinely
idle or unaffordable.

### Reflexes

Three behaviours are not sampled at all. They run every turn, for every country,
before the war action is chosen.

- **Garrison.** Move troops toward any province an enemy stack is standing next
  to. Holding a line is doctrine, not a bet, and a country invaded across six
  borders needs six answers rather than a one-in-eight chance of one.
- **Redeploy.** In peacetime, walk interior garrisons toward the frontier.
- **Manpower.** When income is negative or the army is eating a third of gross
  income, stand down a tenth of it, from the deepest provinces first, never from
  a border.
- **Austerity.** When the treasury has fewer than eight turns of runway left,
  make one cut, in the order the bankruptcy cascade uses: research funding,
  pacification, the costliest doctrine, a minority programme, a warship.
- **Amphibious.** Sail loaded transports at the nearest hostile port and land
  them the moment they are in range.

The rule for what belongs here: if no competent player would ever decide it
differently, it is a reflex. Everything with a real trade-off stays with the
policy.

Two of these are recent and both replaced a failure the policy could not have
been expected to solve. Mounting an invasion is a decision, but finishing one
needed the module to sample "move" several turns running and then "disembark" at
exactly the right moment against five competing actions — measured at 1,372
embarkations for 121 landings, with the rest of the army carried around at sea
and brought home again. And running out of money is not a strategy, it is an
accounting failure whose bill is spread across four modules that each see only
their own share of it.

---

## 5. Choosing an action

Scores from the network are turned into a choice with two knobs, after two
things have leaned on them.

**The posture.** The stance chosen for this country (see section 2) adds a small
bias to the module's action scores — an expanding country is pushed toward
attacking and declaring, a developing one toward industry and research and away
from starting wars. A *bias*, not a mask, and in both directions: it moves the
odds by roughly two and a half at the hard-difficulty temperature, enough to make
a country behave like one that has decided something, nowhere near enough to stop
it defending itself because it declared a building phase ten turns ago.

This is what makes the stance a decision at all. For most of its life `stanceOf`
was read in exactly one place — to set the one-hot in features 112–115 — and
gated nothing, biased nothing and changed no executor. In principle the trunk
could learn stance-conditional behaviour from that one input; in practice one
channel among a hundred and forty, with nothing forcing the association, is a
note the country leaves itself rather than a plan.

**The critic.** Where a Q head has trained past its warmup, its centred scores
are blended in the same way.

That warmup is currently set past any reachable update count, which is to say
the critic is off. Bisected by forcing each warmup gate shut in turn and
measuring land share against the scripted rung over 400 turns: with every head
learned the model held 62.7%, and with the critic alone held back it held 76.7%
-- fourteen points given away by a critic that had crossed a threshold of two
million updates without having learned enough to be worth listening to. The
paragraph the constant carried had predicted exactly that failure; only the
threshold was wrong. Q is still trained below the gate, so re-enabling it is a
one-line change the moment there is evidence it helps.

Then the two knobs.

**Temperature** flattens or sharpens the distribution. High temperature makes
strong and weak actions closer to equally likely; temperature near zero collapses
to always taking the highest-scoring one.

**Epsilon** is the chance of ignoring the model entirely and picking uniformly at
random from the valid actions.

A difficulty setting changes both knobs **and which faculties the AI is allowed
to use**.

| Difficulty | Temperature | Random | Critic | Learned aim | Posture |
|---|---|---|---|---|---|
| Easy | 1.6 | 8% | — | — | — |
| Normal | 0.9 | 5% | yes | — | yes |
| Hard | 0.35 | 2% | yes | yes | yes |
| Insane | 0.05 (effectively always the best move) | 0% | yes | yes | yes |

*Critic* is the Q head's opinion blended into the choice. *Learned aim* is the
target and attack heads — whom to declare on and which province to take; without
them the old margin rule aims, which is exactly how this AI played before those
heads existed and makes a perfectly reasonable weaker opponent. *Posture* is
whether the country has a plan at all.

This used to be the two knobs alone, and Easy was the best policy the project
has, told to ignore itself **35% of the time**. That is not a gentler opponent,
it is an erratic one: the country that fortified its border last turn declares
war on a great power this turn because a coin came up heads, and a player reads
that as the game being broken rather than as themselves winning. A ladder should
take faculties away, not add noise. Epsilon survives, much smaller, so a human
cannot read the AI off a table.

Self-play always trains at the top tier regardless of the setting: an opponent
that aims with the old rule teaches the aiming heads nothing, and a policy
trained against a handicapped copy of itself learns to beat the handicap.

Difficulty is applied here and nowhere else. The model is never weakened; only
the way its output is sampled changes. This matters because a confident network
keeps large gaps between its scores, and temperature alone cannot make it play
badly. The random component is what makes easy genuinely easy.

Two refinements sit on top.

**Grave actions.** Declaring war is excluded from random exploration during
normal play. Exploration is meant to make one country play worse, not to make the
world incoherent, and a coin flip landing on "declare war" reads to a player as
derangement rather than weakness. During self-play the restriction is lifted, because
a model that never tries a war cannot learn what one is worth.

**Sampling bias.** A caller can add a standing offset to specific scores before
sampling. It is used to make answering a call to arms harder and accepting a
non-aggression pact easier. The learning step does not see the offset, so it is
an immediate lever rather than a permanent one: what has to hold in the long run
is the reward.

---

## 6. The turn

```
Game::processTurn
  AISystem::beginTurn        world aggregates, once, for every country
  for each AI country:
      AISystem::takeTurn     four decisions, executed as pending orders
  ... the turn resolves ...
  AISystem::endTurn          rewards, gradients, occasional checkpoint
```

`beginTurn` does a single pass over provinces, armies, ships, relations and
claims and builds a per-country summary: frontiers and who is on the other side
of them, threatened provinces and the strength standing opposite, staging routes
through allied territory, troops abroad, overseas invasion targets, standing
agreements. Without it, feature extraction would rescan the map once per country.

`takeTurn` builds the feature vector once and reuses it for all four modules,
snapshotting each network's internal activations so the learning step later does
not have to recompute them.

---

## 7. Answering diplomacy

When another country proposes something to an AI country, the diplomacy network
decides. The country's own feature vector is used, with request-specific context
written into slots that are otherwise zero: which kind of request it is, how
strong the proposer is relative to us, and, for a ceasefire, what the terms are
actually worth to the recipient in provinces, claims and money. That vector then
goes **through the shared trunk**, and the diplomacy head reads the embedding —
the same path every policy head takes.

> **It did not, for a long time.** The head was handed the raw feature vector
> instead of the embedding. `NeuralNet::forward` returns an empty vector on a
> width mismatch and `pickAction` answers action 0, which is *reject* — so every
> ceasefire, alliance, non-aggression pact, guarantee and call to arms was
> declined unconditionally, by every country, in shipped games as well as in
> training, and the network was never consulted at all. Nothing downstream could
> report it: with no alliance ever formed nobody could issue a call to arms, so
> the coalition counter read "0 of 0 answered" — an empty denominator, not a
> policy. Training made it worse rather than better, because every sample it
> stored carried action 0 and a behaviour log-probability of 0, so the head was
> fitted to always-reject through a meaningless PPO ratio. Repairing the
> plumbing is not enough on its own; the head has to be reset:
>
> ```
> OpenDoctrines --reset-ai-head data/ai/model.bin diplo
> ```
>
> The counter that would have caught it is now reported and gated: **"said
> yes/was asked"** per cohort, and `agreements are possible` in the blunder
> checklist.

A call to arms is judged separately, because it is the most expensive thing an AI
can agree to: an immediate war it did not choose, plus a large jump in war
weariness at home. Refusing costs the alliance, which is a real price but a
one-off one.

Four conditions refuse it outright, before the network is consulted:

- already fighting a war of its own
- war weariness already at or above the block threshold
- enemy troops on its own borders, or ground lost since last turn
- the aggressor outguns the calling ally and us combined by more than half again

If none of those hold, the network decides, with a standing bias against
accepting.

### Saying why

A refusal used to be a bare `false`. The reason existed — the gates above
compute a perfectly good one — and was thrown away, so a player was told only
that their offer had been declined. An opponent whose every refusal is
unexplained reads as arbitrary, and arbitrary reads as stupid even when the
decision was sound.

Refusals now carry a **stated reason**, and three rules shape it.

**It gates nothing.** No request is blocked for want of a reason and no reason
has to be given. Delete the mechanism and every action in the game is exactly
where it was. That is deliberate: a casus belli you are *required* to have is a
different game, and you can still declare war on anyone you like for no stated
reason at all.

**Silence is a move.** Because a reason is optional, "declined and said nothing"
exists and means something. A country that always has an answer ready is as
readable as one that always tells the truth, so the AI stays quiet
`REFUSAL_SILENCE_CHANCE` of the time.

**Either side may lie, and neither side has an ability the other lacks.** What
is *stated* is chosen separately from what is *true* — by
`AISystem::chooseStatedRefusal` for the AI, and by the player from the same
unfiltered list on the request popup. A country with something to hide reaches
for the excuse that gives nothing away.

Believability is not enforced by hiding options. It is a consequence, and the
game already decides what is knowable: wars and borders are on the map, war
weariness and intent are not. `Game::refusalIsContradicted` asks whether the
*listener* could check a claim against what it can already see —

| Stated reason | Checkable? |
|---|---|
| already fighting wars of their own | yes — wars are public |
| losing ground on their own borders | yes — the map shows the stacks |
| the other side is too strong | roughly — garrisons are drawn |
| their people will not stand another war | no — private |
| it is not in their interest | no — a preference |
| they do not trust you | no — a preference |

— so no table of plausible excuses has to be maintained; the filter is a
question asked of state the observer has anyway. The player may still state
something the map disproves. So may the AI, in principle. It chooses not to,
and `refusals: caught out` in the eval is an **invariant** that must read zero:
an opponent keeping something back is devious, and one whose excuse falls apart
the moment you look at the map has simply not noticed what you can see.

### Saying why you declared

Wars carry the same arrangement, with one difference that matters: there are
**two** goals, and only one of them is public.

| | Who sees it | What it does |
|---|---|---|
| **Stated** | everyone — it is on the relation | announced with the declaration; may be false |
| **True** | nobody | derived from what `findWarTarget` knew; steers what actually gets taken |

The true goal is not shown, not serialised into the relation and not exposed to
the UI. It is also not inert, and this is where it bites: **the peace terms are
built around it.** A country demands the land it claims before anything else,
and one losing a war of recovery will pay, cede ground elsewhere and still
refuse to renounce the claim it went to war for.

That is what makes the goal observable — not a label on a panel, but a
settlement that keeps bending around the same provinces. Before the terms
consulted it, "demand provinces" meant walking the defender's territory in map
order and taking the first that touched us, so a war fought for Danzig was
settled for somewhere else entirely and the goal never reached the negotiation
at all. The eval reports the share of demanded provinces that were claimed land;
it now reads 100%.

Declaring is unchanged and unrestricted. Anyone may declare on anyone, at any
time, for nothing — the goal is announced, never required, and the cycler on the
diplomacy panel defaults to "State no reason" so a player who ignores the system
declares in silence exactly as before.

The pretext logic has a pleasing shape. `warGoalIsContradicted` checks a claim
against public state — claims, borders, relative size, who is allied to whom —
and **`WAR_GOAL_CONQUEST` is the one nobody can disprove.** So the honest goal is
always safe to state, and a country that wants a pretext has to find one that
happens to be true: a claim it really holds, a border it really shares, a rival
that really is larger. `AISystem::chooseStatedWarGoal` looks for the most
respectable true thing available and otherwise admits to conquest or says
nothing. `wars: caught out` is the invariant, and reads zero.

### What your word is worth

Statements would be free without this, and a free lie is not a decision.

**Credibility is per pair** — what one country thinks another's word is worth,
from 0 to 1 — and the asymmetry is the reason. The *evidence* that breaks a
claim is public, but the *claim* is not: only the country a thing was said to
knows it was said, so only that country can put the two halves together. It also
gives lying a shape worth having, since you can mislead an enemy and stay
straight with an ally, and the cost lands where you told the story.

Two things spend it:

**Caught at the time.** The statement was already disprovable when it was made.
The AI never does this to itself — `chooseStatedRefusal` and
`chooseStatedWarGoal` filter for it — but a player may, and pays on the spot.

**Caught by conduct.** The more interesting half, because it reaches the lies
nothing could check. "Our people will not stand another war" is unfalsifiable
when you say it — and then you declare a war of your own. "We fight only to
recover what is ours" is true by construction when announced, a claim exists —
and then the war takes provinces you never claimed. Nobody could have known at
the time; everybody can see it afterwards. Those claims are written down when
made (`SpokenClaim`), watched for `CRED_CLAIM_WINDOW` turns, and dropped.

The effect is a **logit bias on accept**, scaled by the shortfall, through the
same channel `AI_NAP_WILLINGNESS` and `AI_CALL_RELUCTANCE` already use. A
country that has never been caught pays nothing, which keeps this a cost of
lying rather than a tax on asking. Nothing is ever blocked, hidden or greyed
out: a country nobody believes can still ask, and can still be told yes, because
sometimes the deal is worth it anyway. `predictAcceptance` applies the same bias,
so a serial liar stops spending its overture budget on partners who have stopped
believing it.

Forgiveness runs every turn and is deliberately far slower than a lie costs, or
the cheapest strategy would be to lie constantly and wait it out.

That slow recovery is also why the eval reports **events** rather than the
current state: a run that caught two liars on turn forty reports a serene 1.000
three hundred turns later, and would look exactly like a run where the checks
never fired. The line reads `2 caught out, lowest word ever 0.750`.

Both directions show on the diplomacy panel — *Their word / Yours* — and only
once one of them has slipped, because a row reading "trusted / trusted" on every
panel from turn one teaches nothing.

Overtures are rate limited in two independent ways: a long cooldown on the
unordered pair, so two countries cannot alternate proposals every turn, and a
per-country budget, so a country with a dozen neighbours cannot fire one overture
per turn for a dozen turns. A refusal cools the pair down for much longer than an
acceptance.

---

## 8. How it learns

The method is REINFORCE with a learned baseline. Stated without jargon: take the
action, wait to see what happens, then make that action more likely if the result
beat expectations and less likely if it did not.

### The reward window

A decision is judged on what changes over the **next twelve turns**, not the same
turn. Single-turn deltas taught passivity: spending money was punished
immediately while the payoff, whether industrial income, conquered ground or a
suppressed rebellion, arrived many turns later and was credited to nothing.

Each country keeps a sliding window of decisions waiting for their verdict. A
decision settles when it is twelve turns old, or immediately if the country is
eliminated.

### Rewards per module

A weak shared term covers survival: ground gained, treasury, income, rebellions
suffered. It is deliberately small. It used to dominate, which meant conquering a
province rewarded the economy, politics and navy heads as well, even when all
three had chosen to do nothing. With four modules acting at once, each one's
learning signal was three parts noise.

The shared term also carries the only two things in the dense reward that
mention anybody else:

- **Standing** — did the country move up or down the land table over the window.
- **Lead** — did the gap between it and the strongest *other* country narrow or
  widen. This one moves when the leader moves, which is the point: standing
  still while somebody runs away with the game is now a loss.

Everything else here is a quantity of the country's own — land, income, unrest,
research — and a policy optimising only those is optimising a dashboard. There
*was* a competitive signal, but only at the terminal: one number per map, after
hundreds of windows of shaping, reached through a value function fitted mostly on
the dashboard. That is why the AI would not coalition against a runaway, which is
the first thing anyone who has played a grand strategy game expects.

On top of the shared term, each module is judged on what it actually controls.

- **Economy**: income growth, industry built, research completed, treasury, and
  a penalty for every turn spent with an empty treasury.
- **Politics**: rebellions, heavily; allies who actually fight; standing
  agreements held; how the country's minorities came to feel about it; war
  weariness, both its level and any increase over the window.
- **War**: ground taken and ground lost, both explicitly; army growth, but only
  when there is a war to fight or a border under pressure; a small standing cost
  for being at war and achieving nothing; a penalty for starting a war against a
  country holding no land this one claims.
- **Navy**: ground taken, and hulls, but hulls count as an asset only when there
  is a war or a crossing to make and as a liability otherwise.
- **Diplomacy**: what saying yes did to this country, namely the war weariness it
  took on and the ground it lost, against the allies it kept.

Two of these deserve their history. Army growth was once rewarded
unconditionally, and over 400 turns the war module chose "recruit" 14,849 times
and "attack" 214: recruiting is riskless and pays every turn, attacking risks the
stack and only pays if it takes ground. No amount of exploration digs a policy out
of an incentive like that. Ship count had exactly the same shape and was
corrected the same way.

### Terminal outcomes

Three endings are scored directly rather than through deltas.

- **Eliminated**: every decision in the final window scores -4.
- **Won the map**: +4, the mirror of it.
- **Map ended undecided**: each surviving country is scored on its final share of
  the world against an equal split, on half the range. Finishing large is
  evidence; winning is proof.

The third case exists because most maps end this way. Without it, the last twelve
turns of every rotation trained nothing, and a run of maps that nobody won
produced no statement at all about who finished ahead.

### Normalisation and the baseline

Raw rewards are normalised by a running mean and variance per module, so the
scale of an advantage is comparable across a twelve-province map and a
four-hundred-province one. Those statistics are saved with the model, because the
AI is destroyed and rebuilt on every map rotation and the yardstick should not
reset with it.

The value head predicts the normalised reward. The difference between what
actually happened and what the value head expected is the **advantage**, and that
is what scales the weight update. Without it, every action taken in a good
position looks good.

### Batching and the optimiser

A turn produces hundreds of experiences that all settle together. Their gradients
are averaged and applied as one Adam step per network per turn. A batch of one is
the noisiest estimator there is; averaging first cuts the noise by roughly the
square root of the batch size and costs nothing, because the work was already
being done.

The consequence is easy to miss: at a fixed learning rate, batching fifty samples
into one step moves the weights about fifty times less per unit of experience.
The learning rates are set with that in mind. The diplomacy network is the
exception and keeps a lower rate of its own, because it sees roughly one sample
per turn rather than fifty and therefore gets none of the noise reduction that
justifies a larger step.

Gradient work is spread across up to four threads, each with a private copy of
the activations and gradient accumulators. The weights themselves are shared and
only read during accumulation; the sum happens once, serially, at the end. This
is also the shape a GPU port would need.

---

## 9. Governing at home

Three of the politics module's actions are about the country itself rather than
its neighbours. They were added last, and two of them were impossible before a
data-model change.

### Policy, or "doctrines"

The Politics screen is titled **Doctrines** in the user interface; in the source
it is `m_allPolicies`. A policy has a cost per turn, an implementation delay, a
compass requirement, a compass shift, incompatibilities with other policies, and
effects on unrest, public opinion, immigration and minority growth.

The AI has two ways to reach for one, because there are two different questions
a government asks:

- **Enact a policy that fits our politics.** Scored on distance from the
  country's own compass, with a penalty proportional to what the policy costs
  against income. Fit still dominates — a government does not enact things it
  disagrees with — but a cheap policy now wins ties.
- **Enact a policy that calms the country.** Scored on unrest reduction, public
  opinion shift and minority growth instead. Offered only when there is
  something to calm: low minority alignment, a rebellion this turn, or war
  weariness on the rise.

Both go through `canCountryEnactPolicy`, the same gate the player's buttons use,
which checks compass requirements, duplicates, incompatibilities and whether the
country can actually afford it.

A third action cancels the costliest active policy, which is the budget escape
hatch.

### Paying for it

An empty treasury adds **twenty percentage points** to every province's
rebellion chance, for as long as it lasts — flat, immediate, and gone the turn
the country is solvent again. Against a loyalty floor and a pacification budget
that tops out at fifty, that is the difference between a quiet country and one
coming apart.

For that to be a punishment rather than a death sentence, the game's bankruptcy
cascade has to be able to reach whatever the country is actually paying for. It
cuts in order of what it costs to undo: discretionary budgets, then doctrines
(re-enactable, at the price of an implementation delay), then minority
settlements (a slider, but the goodwill takes many turns to win back), then
ships, then troops. The two middle steps were missing until recently, and their
absence was a trap — a country whose expenses were political could sell its
fleet and disband its army and still be bankrupt, because the cascade could not
touch the thing draining it.

The AI does not wait for that. The austerity reflex makes one cut per turn, in
the same order, once the treasury has fewer than eight turns of runway — which
is the difference between trimming and a fire sale.

### Minority policy

Six categories — deportation, economic incentives, cultural autonomy, political
representation, language, integration — each with three options running from
conciliatory to repressive. Every option has an alignment effect per turn, a
population growth effect, a cost, and sometimes a compass shift.

Alignment matters because it feeds `getProvinceRebellionChance` directly, and
rebellions are the largest single term in the politics reward. This is the
module's most direct lever on its own score.

It is also the piece that could not exist until recently. Both the policy table
and the accumulated alignment were keyed on the minority's **name alone**, once
for the whole world — so one government's treatment of a group was every
government's treatment of it, and only the player could edit it. Every AI
country's rebellion risk was being driven by a screen the AI could not reach.
Both are now keyed by country as well, which is what lets a group be loyal in one
country and in revolt across the border.

The AI gets one step per turn, in one of two directions:

- **Conciliate** the least reconciled group: find the single category change
  that buys the most alignment per unit of extra cost, and that the country can
  actually pay for.
- **Repress** the most expensive group: find the change that saves the most
  money for the least alignment lost.

Steps are taken in `alignmentPerTurn`, never by option index. The option lists
are not ordered consistently — "Harsh, Medium, Light" runs one way and "Full
Autonomy, Partial Autonomy, Suppression" the other — so stepping an index would
liberalise one category and tighten another in the same breath.

Repression is not punished as such. It is free, and a government that can absorb
the resentment keeps the money. What makes it a real decision rather than a free
win is that the resentment is real: alignment falls, rebellion chance rises, and
the rebellion term collects the bill some turns later. The reward puts the trade
to the module rather than deciding it in advance.

---

## 10. Restraint

Some behaviour is not learned. Superiority bars, war limits and refusal
conditions are ordinary constants at the top of
[`AISystem.h`](https://github.com/Pr1nted/Open-Doctrines/blob/main/src/ai/AISystem.h).

That is a deliberate choice, for two reasons. The model ships trained, so "be
less aggressive" cannot wait for a retrain. And a gate the policy cannot talk its
way past is the only kind that holds, whereas anything expressed as a reward is
something the policy is free to trade away.

The bars separate **claimed** from **unclaimed** land. Retaking territory a
country claims is its war goal and stays cheap. Attacking a neighbour it has no
argument with is what is throttled.

| Constant | Value | Meaning |
|---|---|---|
| `AI_WAR_BAR_CLAIMED` | 0.85 | reconquest: may attack at a slight disadvantage |
| `AI_WAR_BAR_UNCLAIMED` | 2.50 | a war of choice needs a decisive edge |
| `AI_WAR_BAR_UNCLAIMED_NAVAL` | 2.75 | amphibious assault, higher again |
| `AI_WAR_BAR_SECOND_FRONT` | +0.50 | added when already fighting |
| `AI_MAX_CONCURRENT_WARS` | 1 | finish one before starting another |
| `AI_WAR_WEARINESS_BLOCK` | 6.0 | not while the home front is this unhappy |
| `AI_CALL_MAX_OWN_WARS` | 1 | refuse a call while fighting our own war |
| `AI_CALL_WEARINESS_BLOCK` | 5.0 | refuse when unrest is already this high |
| `AI_CALL_MAX_ENEMY_ODDS` | 1.50 | refuse when the aggressor outguns our side |
| `AI_CALL_RELUCTANCE` | 1.20 | standing bias against answering a call |
| `AI_NAP_WILLINGNESS` | 0.80 | standing bias toward accepting a pact |

None of these stop a country defending itself or finishing a war already under
way. They restrain only wars it chooses to start and commitments it chooses to
take on. Being attacked, and honouring a guarantee, still happen regardless, so
coalitions still form.

---

## 11. Persistence

`data/ai/model.bin` is a flat binary: a four-byte tag, a format version, a
network count, then each network's weights, biases and Adam optimiser state, then
the reward statistics. Every network records its own architecture, and loading
refuses a file that does not match rather than half-reading one.

One exception. A policy head that has **gained** actions loads successfully:
existing outputs keep their trained weights and new ones start from their
initialisation, which is the correct prior for an action nothing has been learned
about yet. Adding an order to a module therefore does not throw away the training
in the other eight networks.

Saving writes to a temporary file and renames it into place, which is atomic. An
in-place write leaves a window in which the file is truncated, and a crash or a
second process starting in that window destroys the model.

Checkpoints happen on a wall clock, once a minute, not on a turn count. At thirty
turns a second a turn-based interval would rewrite a 12 MB file several times a
second to protect work that is never more than a moment old.

Two switches guard the file. `config.aiLearning` is off by default in normal
play, so a game never quietly rewrites the model. `--ai-readonly` loads and plays
the model but never saves, which is how a normal game runs alongside a training
session without the two fighting over the same file.

---

## 12. Self-play training

```
OpenDoctrines --train-ai [maps] [turnsPerMap] [countries] [seed]
```

With no arguments it trains until the window is closed. Each round:

1. Pick a scenario archetype and jitter its parameters. There are eight:
   pangaea, continents, islands, archipelago, crowded, sparse, duel, cold war.
   They differ in land coverage, continent count, coastline complexity, province
   density and country count.
2. Generate a fresh procedural map and load it through the same pipeline the menu
   uses, so training sees real game state rather than a mock.
3. Play every country against every other, with the player slot empty.
4. Rotate when one country is left, when nothing strategic has moved for 1,500
   turns, or at the turn cap.

Rotating both maps and scenario shapes is what stops the model memorising one
geography. Because the features are ratios rather than coordinates, what
transfers between worlds is strategy.

The dashboard shows the live map, per-module reward trends, a rolling decision
log, model size and hyperparameters, and behavioural counters: wars declared,
ceasefires offered, pacts proposed, embarkations against landings, calls to arms
issued against answered and refused, troops staged onto allied ground, ships
scrapped. Those counters are the honest read on whether a mechanism is being
used, in a way an average reward never is.

Measured throughput on a ten-core laptop, on a map of roughly fifty countries:

| Quantity | Rate |
|---|---|
| Turns | about 30 per second |
| Experiences per policy network | about 1,700 per second |
| Optimiser steps per network | one per turn, about 2.6 million per day |
| Diplomacy experiences | about 20 per second |

The last row is the one to watch. The diplomacy network sees roughly one sample
per two turns against a policy head's fifty, because it only learns when somebody
actually proposes something. It is the slowest-training part of the system by two
orders of magnitude.

### Sharing the machine

```
OpenDoctrines --resource-limit 90 --train-ai 0 10000
```

`--resource-limit` takes a percentage and applies for that run only. It is not
written back to `config.json`, because a limit typed on a command line describes
one invocation rather than a preference, and inheriting an overnight trainer's
cap into the next ordinary game would be a mystery to debug. The same control is
available live from the F10 or Ctrl+L panel, and as a settings slider.

Below 100% the limiter measures the process's actual CPU time against wall clock
and sleeps at the end of each turn until the ratio comes back under budget. It
counts every thread, including the learning workers and the render loop, which a
naive "work for 90% of the time, idle for the rest" model does not.

### Several worlds at once

```
tools/train_parallel.py --workers 3 --limit 90
```

One world was measured at about 3 GB resident, so on a 16 GB machine the ceiling
is three workers — memory, not the ten cores. That number is what the launcher
defaults to, and it warns rather than obeys if asked for more.

Worlds live in separate processes, not threads: raylib allows one window per
process and map loading touches the GL context. Each worker owns a model file
(`data/ai/model.wN.bin`), plays its own maps, and every two minutes saves its
copy and pulls a third of the way toward the mean of its peers. Nobody blocks on
anybody, and a worker that dies costs only its own progress. On exit the launcher
merges the survivors into `data/ai/model.bin`, which is the file the game loads.
`--merge-ai <out> <in...>` does that merge on its own if you need it.

Both the periodic peer pull and the final merge go through one list of nets
(`blendAllToward`). There used to be two, written out by hand, and they had
drifted: both covered the trunk, the policy heads, the value heads and the
diplomacy net, and both silently skipped the stance head, the war-target head,
all four Q heads, the relational encoder and scorer, and the diplomacy value
head — eight of fifteen. Every worker's learning on those eight was discarded at
the merge and replaced by whatever the first input file happened to hold. Adding
a net and updating one of two lists is how that happened, so there is now one.

The honest caveat: averaging periodically-diverged copies approximates a summed
gradient rather than computing one. A third of the way rather than all of it,
every two minutes rather than every ten, is what keeps the copies close enough
for the approximation to hold — pulling fully to the mean would erase whatever a
worker had just learned, which is the only thing it contributes.

Note also what this does and does not buy. Learning rate work established that
optimiser *steps* are the scarce resource, not samples; N workers give N times
the steps as well as N times the experience, which is why this is worth doing
and why simply batching more samples into the same one step per turn would not
have been.

### Related modes

`--simulate <map.odmap> <turns>` plays a shipped scenario unattended and keeps
the save. It is deliberately not a variant of training: the save is the output,
and it is what `--export-timelapse` needs. It is also the smallest honest
end-to-end check of a build.

---

## 13. Measuring the model

```
OpenDoctrines --eval-ai [maps] [turnsPerMap] [seed] [difficulty]
```

Defaults: eight maps, one per scenario archetype; 3,000 turns each; a constant
seed; difficulty hard.

Training tells you the reward went up. It cannot tell you the AI got better,
because the reward function is one of the things that keeps changing: add a term
or reweight one and the sparkline is measuring a different quantity, so
yesterday's curve and today's are answers to different questions. The evaluation
harness plays the model instead and counts what it did.

Three properties make two runs comparable.

- **Fixed seeds.** The default seed is a constant rather than the clock, so map
  N is the same world every time. The turn resolver and the AI's own generator
  are both deterministic, so the whole run is.
- **No learning.** The model is loaded read-only and never updated, so what is
  measured is the file on disk rather than a moving target. A training session
  can keep running in another process throughout.
- **No training-mode sampling.** Self-play deliberately injects exploration
  noise; a measurement that inherited it would be measuring dice. Sampling comes
  from the difficulty setting, the way a real game samples it.

### What it reports

Per map: the scenario and seed, how it ended (decided, frozen, or hit the turn
cap), how many countries survived, the largest country's share of the owned
world, and the concentration of the map, which is the sum of squared shares and
reaches 1.0 when one country owns everything.

Aggregated across maps, every behavioural counter is reported per thousand
country-turns rather than as a raw total, because all of them scale with how many
countries are alive. A raw total says more about the scenario's country count
than about the model.

| Line | What it answers |
|---|---|
| outcome | do wars ever resolve, or does the map freeze |
| survival, largest power, concentration | does the world consolidate or stalemate |
| war | how much of the map's activity is fighting |
| diplomacy | how much of it is agreement |
| agreements | what share of all requests — treaties and calls alike — anyone said yes to. Read this before the coalition line: it is upstream of it, and a zero here explains a zero there |
| coalition | do alliances mean anything, and are calls to arms answered |
| thinking | milliseconds per country-turn — the playability number, not a quality one. 185 countries on the present-day map is where it stops being free |
| amphibious | what share of embarked troops reach a hostile shore |
| fleet | are unusable hulls being paid off |
| unrest | rebellions and research per country-turn |
| solvency | share of country-turns spent bankrupt, and austerity cuts made to avoid it |
| minorities | mean alignment, share of groups below the 40% mark where minority unrest starts feeding rebellion chance |
| governing | conciliations, repressions and calming policies per country-turn |

The last two lines are a header and a row of comma-separated values, in a stable
field order, so two runs against two model files can be diffed directly or
appended to a spreadsheet.

### Against a random bot

```
OpenDoctrines --eval-ai --vs-random
```

Every other number here is relative — to the previous run, to a reward function
that keeps changing. This one is absolute. Half of each map's countries are
driven by uniform-random choice over the same validity masks, with the same
reflexes, the same executors and the same restraint constants; the only
difference is where the choice comes from. So the comparison measures the trained
policy's contribution and nothing else.

Cohorts are matched rather than assigned by country id: countries are ranked by
starting size and alternated down the list, so both sides get the same spread of
strong and weak starts and a difference at the end is a difference in play rather
than in dealt hands. Random countries never contribute training samples — a coin
flip has nothing to teach — and the grave-action guard is lifted for them, since
for the control group the random pool *is* the policy and removing an action from
it would quietly handicap the baseline.

The report ends with an advantage figure: the ratio of land held by the model
cohort to land held by the random cohort. Below 1.0 the trained policy is losing
to random selection, which no reward curve will tell you and which has exactly
one honest interpretation.

### Against a named opponent

```
OpenDoctrines --eval-ai --vs-model data/ai/rung1.bin
tools/ai_bench.py --vs-model data/ai/rung1.bin        # with seeds and intervals
```

Random is a **floor, not a level**. It never improves, so once a model clears it
the ratio keeps climbing without saying anything about how well the AI actually
plays — 2.5x against a coin flip could be a competent player or a snowballer
that eats a passive map, and the number reads the same either way.

`--vs-model` hands the control cohort a model file instead. Everything else is
the split above verbatim: the same matched cohorts, the same counters, the same
report, with `RANDOM` replaced by `OPPONENT` throughout so a saved log can never
be mistaken for the other kind of run. The opponent is frozen — its trunk, its
four policy heads and its diplomacy net are loaded read-only from the file, it
consults no critic and contributes no training samples, exactly like a league
checkpoint. A file that will not load aborts the run rather than falling back to
dice, because a report labelled `OPPONENT` over numbers measured against random
would be indistinguishable from a real one.

This is what makes a target like *"as good as an intermediate player"* testable
rather than a matter of opinion: pin the file that represents the level, and
1.00x becomes parity with it. When a model beats that rung, pin a harder one.
Note that the blunder gates in `tools/ai_bench.py` are mostly phrased relative
to the control, so against a model opponent they become comparisons with that
player rather than floors — the tool prints a reminder saying so.

### Against a written-down player

```
OpenDoctrines --eval-ai 2 400 --vs-script
tools/ai_bench.py --vs-script
```

Random is a floor that never rises. A named model is a rung — but only once you
have a model worth pinning, and until this project has one, *"as good as an
intermediate player"* has nothing to be measured against at all.

So rung one is a player written down. It attacks what it can beat, keeps its
books, researches continuously, sues for peace when it is losing, answers its
allies and calms its own unrest. Nothing in it is clever and nothing in it is
learned; the rules are the ones a tutorial would give you, applied in the order
a person would apply them. It shares every reflex, mask and restraint constant
with the model cohort, exactly as the random control does — the only difference
is where the choice comes from.

The point of it is the gap it exposes. Measured over three seeds at 250 turns,
the current model reads roughly **1.2–1.5x against dice and 0.52x against this**
— it holds a third of the map and loses every game. That difference is the whole
distance between "beats a coin flip" and the thing actually being aimed at, and
before this control existed there was no number for it.

### Which worlds

```
OpenDoctrines --eval-ai 6 400 --scenarios
tools/ai_bench.py --scenarios --maps 6
```

For most of this project's life, training and measurement both saw **only**
procedurally generated maps — while every player opens one of the six in
`data/STDmaps`. A generated archetype has no historical alliance network, no
real claims, no minority map anyone has heard of, and an even spread of country
sizes where a real scenario has five great powers among forty small states; the
present-day world has 185 countries and nothing generated comes close. Every one
of those differences is something `buildFeatures` reads, so the policy met all
of it for the first time in the one run that cannot be re-rolled.

Training now plays a shipped map every `SHIPPED_TRAIN_EVERY` rounds, cycling the
list — interleaved rather than in a block, and not more often than that, because
six fixed worlds are something a policy can learn *instead of* learning to play.
Measurement takes `--scenarios` as an explicit opt-in and never mixes the two
kinds in one run: a mean over "three generated and two historical" describes
neither, and every previously stored result was taken on generated worlds.

Whenever a run covers more than one world, the report ends with a per-world
ADVANTAGE table, and `tools/ai_bench.py` gives each entry its own interval
across seeds. A model that is fine on pangaea and hopeless on 1939 cannot hide
in the mean, which is exactly what it had been doing.

### One gate per reward term

`tools/ai_bench.py` prints a second checklist under the blunder one. The reward
is about twenty hand-set constants, and the comments beside them record a cycle:
a term is added, the policy collapses onto whatever it overpays for, the term is
reshaped, and the reshaping breaks an earlier one. Army growth rewarded
unconditionally gave recruit 14,849 choices against attack's 214. A gate on it
was true every turn for a defender, so recruit went to 98.5% of offers. A flat
charge for declaring war gave zero declarations out of 1,827 opportunities.

Every one of those was an action preference pinned to an extreme, every one was
found by hand weeks later, and none was visible in ADVANTAGE at the time. So
each constant now owns a counter and each counter has a band. Bands rather than
minimums, because a policy that *always* picks an action has stopped choosing
just as surely as one that never does. They are deliberately wide: this asks
whether a module is still making a decision, not whether it is making a good
one, and a number out of band means a collapsed distribution that no amount of
further training walks back without a `--reset-ai-head`.

**What is gated is the policy shape, not the take rate.** A take rate — chosen
over offered — is what came out of the dice *after* temperature and epsilon, and
measurement runs at difficulty 2, where temperature is 0.35. That crushes the
bottom of the range. Measured on one model:

| action | policy's own P | take rate |
|---|---|---|
| reinforce | 74.3% | 79.4% |
| recruit | 15.9% | 10.8% |
| ceasefire | 0.3% | 0.4% |
| **declare war** | **2.7%** | **0.1%** |

The middle tracks closely; the low end does not. An action the net gave 2.7% was
taken one time in a thousand, so every "collapsed low" verdict read off a take
rate was overstated by more than an order of magnitude — including one that led
to a war head being reset. `pickAction` therefore also reports its distribution
over the masked logits at a neutral temperature of 1.0, accumulated only over
the turns each action was *offered* so the denominator matches the take rate's
and the two sit side by side. Both are printed; only the shape is gated.

The measurement is purely observational: it draws no randomness and nothing
reads it back, so it cannot change what was chosen. It is recorded only for
countries a policy net is actually driving — not the random cohort, whose
choices are dice, and not the scripted rung, which never consults a net.

The checklist is worth reading against the difficulty ladder. Measured over four
seeds before the fix below: at Hard the war module chose recruit on 98.6% of
offers, and at Insane — argmax, no exploration at all — on **100.000%**, with a
zero-width interval. It never did anything else, which is why the hardest
setting played worse than the one below it.

**What that turned out to be.** Two defects in ten lines, pulling the same way.
`armyTerm` paid `0.3 × tanh(dArmy)` every window its gate was open — an
*annuity*: riskless, repeating, and open almost permanently for a country that
is at war a lot, which this one was because it never made peace. And the
idleness charge still carried the `dArmy` escape clause that the comment
directly above it says was removed, so raising five hundred men also bought
exemption from the penalty for doing nothing. Recruiting both paid and dodged.

`armyTerm` now pays for **progress toward sufficiency**, clamped at 1: closing
the gap is worth `ARMY_PROGRESS_WEIGHT` once, and an army already big enough for
its borders earns nothing more. The total available over a game is bounded, so
there is no trough to settle in. The bar is frozen at the window's start, so a
neighbour's mobilisation cannot charge this country for a decision it did not
make.

A reward correction is only half of it. A converged softmax puts almost no mass
on the actions it has learned to avoid, so the corrected reward is never sampled
often enough to pay — the head has to be told, not persuaded. Reset the war head
and the same model at argmax drops from 100.0% recruit to 11.5%.

Note what a gate cannot see on its own: a *freshly reset* head also has
arbitrary logits and also concentrates, so it reads as collapsed too. The eval
therefore reports each head's update count, and the gates report "no data"
rather than a failure below the point where a take rate means anything.

### Reading it

There is no single score, on purpose. The numbers are diagnostic and several of
them are only meaningful against the previous run:

- Landings well below embarkations means troops are being loaded onto ships that
  never reach anywhere. That was once around ten per cent, and the army was
  effectively being deleted.
- Calls to arms answered near zero means alliances have become decorative;
  answered near one hundred per cent means the AI is being dragged into
  everybody's wars.
- Concentration near the reciprocal of the country count with no maps decided
  means nothing is happening at all.
- Rising war counts with falling decided-map counts means fighting without
  resolution, which is the failure mode the phoney-war term and the ceasefire
  threshold exist to prevent.
- Repressions far outnumbering conciliations with minority alignment falling is
  a model that has learned to take the free option and has not yet felt the
  rebellions it is buying. That gap is roughly the reward window, so it shows up
  as a divergence between the two lines before it shows up in the rebellion
  count.

---

## 14. Debugging

- `config.aiDebug` prints every decision as `[AI] t<turn> <country> [module]
  <label> (score)` and attaches the advantage once the reward settles.
- The in-game overlay reads the same ring buffer of the last 400 decisions.
- The random number generator is seeded to a fixed value, so identical state
  produces identical choices and a run can be replayed.
- Every action executor returns a human-readable label. A label like
  "reinforce: nothing to move" or "bombard: nothing in range" is a validity mask
  that is too loose, and it is visible in the log rather than silent.
- `OD_AI_THREADS` overrides the learning thread count, mainly so the parallel and
  serial paths can be compared on one binary and one map.

---

## 15. File map

| Path | Contents |
|---|---|
| `src/ai/AISystem.h` | architecture, action menus, restraint constants, the experience record |
| `src/ai/AISystem.cpp` | world summary, features, validity, execution, rewards, persistence |
| `src/ai/NeuralNet.h/.cpp` | the network: forward pass, backward pass, Adam, batching, serialisation |
| `src/Game_AITrain.cpp` | the self-play loop and the trainer dashboard |
| `src/Game_TurnLogic.cpp` | where the turn calls into the AI, and where diplomacy is resolved |
| `src/Game_Policies.cpp` | policies, minority policy categories, alignment and unrest |
| `src/Game_Research.cpp` | the research tree and per-country effect queries |
| `src/Game_Mods.cpp` | the read-only view of the AI exposed to mods |
| `data/ai/model.bin` | the trained model |
