# Community roadmap — the production economy, and what it drags with it

**Status: PLAN. Nothing in this document is built.** Written 2026-09-04 from the
Discord discussion in which Kaiserin Neptune, Matt and Pr1nted argued about
where the game should go next. Every phase below is a proposal with a cost, an
order, and a list of the things that break when it lands. Read it as a menu,
not as a changelog.

*(This file exists because `docs/review-response-2026-08.md` has already been
mistaken once for a description of code that is in the tree. It is not.)*

---

## 1. What was actually asked for

| # | Idea | Who |
|---|---|---|
| A | Factories produce **materials**, not money. The population needs those materials; more and cheaper materials = better living standards. | Kaiserin Neptune |
| B | You **assign** each factory what to produce, from the resources available — or let the AI do it. | Kaiserin Neptune |
| C | That choice is the **economic system**: planned (you assign all), free market (capitalists assign, you tax), or mixed (you direct some). | Kaiserin Neptune |
| D | **Money stays.** Planned economies get it from trade and selling surplus, and pay for factory upgrades in *materials*. Free markets get it from taxes, and lend it to factories. Capitalists hold their own money and reinvest it. | Kaiserin Neptune |
| E | A province with **no people, no land and no resources should not reach industry level 10.** (Gilmour Island, Canada: pop 0, 84 km².) | Matt |
| F | Combat is **too shallow**; the game reads as Vic3, not HOI4, and provinces are too big for tactical manoeuvre without rewriting the rules. | Matt |
| G | **Releasing nations.** | Pr1nted |
| H | Doctrines, artillery cost and army cost need reworking if (A) lands. | Pr1nted |
| I | Expansion of internal politics and of ethnic policies. | Pr1nted |

And the framing both Kaiserin and Matt converged on, which decides the order of
everything below: **this is a Victoria-shaped game, and its best version is the
better GD, not a turn-based HOI4.** Depth belongs in the economy and in
politics. Combat should stop being embarrassing; it should not become the point.

---

## 2. What exists today, honestly

Worth stating precisely, because three of the ideas above are *already half
present* and the plan is cheaper than it looks.

**Industry.** `ProvinceIndustry{level 0-10, income, specialization,
resourceIncome, popIncome, popModifier, fortification}`. Prices are
`IND_COST[]` in `src/BuildCosts.h`, capped at level 10 **everywhere on the
map**. Income is set in one line — `src/Game_TurnLogic.cpp:5984`:

```cpp
ind.income = it->targetLevel * 2.0f; // Simplified income
```

Flat. It does not read population, area, or resources. That single line is
idea (E): Gilmour Island earns exactly what the Ruhr earns. `industryUpkeep()`
already brakes the snowball nationally, but nothing brakes it *locally*.

**Resources.** Five, per province, `0-100` with a separate `boost`: oil, gold,
metal, rubber, gemstones (`ProvinceResources`). They are already a *quantity*
and already carry a per-province yield. They are consumed by nothing — they are
converted straight to money via `provinceResourceIncome()`, and a province can
"specialise" into one for a boost. **This is the input half of idea (A), built
and shipped.** What is missing is the output half.

**Population.** `m_provincePopulations` (per province), minorities partitioning
each province to 100%, a political compass per province and per country.
`getProvinceRebellionChance()` sums: ideological extremeness, government
distance, minority alignment, foreign claims, war weariness, **bankruptcy**.
That last term — a national condition felt in every province the turn it is
true — is exactly the shape a living-standards term needs. **The hook for idea
(A)'s consequence already exists.**

**Economic ideology.** The compass economic axis is live, 45 policies in
`data/policies.json` including `state_industry`, `privatization`,
`deregulation`, `autarky`, `free_trade`, `total_war_economy`. Today they shift
the compass and cost money per turn. They do not touch how industry works.
**Idea (C) has its political vocabulary already; it has no mechanics under it.**

**Combat.** `resolveAssault()` (`src/Game_TurnLogic.cpp:5519`). One shot,
deterministic:

```
attack  = troops × (1 + armyAtkPct/100)
defence = Σ over hostile stacks of troops × (1 + fort×10/100) × (1 + armyDefPct/100)
carries if attack > defence
```

No terrain, no width, no supply, no attrition, no multi-turn battle, no front.
The whole garrison fights at once and the loser is annihilated. Matt is right.

**Rebel countries.** `createRebelCountry(rebelCid, parentCid, provinceIds)`
already builds a functioning new country from a set of provinces, with its own
compass averaged over them, its own ISO, its own relations. **Idea (G) is
mostly a UI and a rules gate on machinery that already works.**

**AI.** Four policy modules — economy (12 actions), politics (12), war (8),
navy (7) — plus a diplomacy head, over a shared trunk taking a **143-float**
feature vector. `MAX_MODULE_ACTIONS = 12`, and economy is already at 12.

---

## 3. The order, and why

Cheap and load-bearing first. Each phase is separately shippable and separately
revertible; each one moves the AI benchmark baseline (see §11).

```
  0. instruments & guards        (no gameplay change)
  1. industry capacity           (E)          ← unblocks everything in 2
  2. goods                       (A)
  3. economic systems            (B) (C) (D) (I)
  4. repricing                   (H)
  5. releasing nations           (G) (I)
  6. combat depth                (F)          ← independent; can run in parallel
```

Phase 6 touches a different resolver from 1-5 and can be done at any point.
Phases 1-5 are strictly sequential: goods are meaningless without a capacity
that makes a factory a choice, and an economic *system* is meaningless without
goods to allocate.

---

## Phase 0 — Instruments and guards

**No gameplay change. Do this first or measure nothing afterwards.**

1. **A production-economy bench axis.** `tools/od_bench.py` measures seat score
   and ADVANTAGE. Neither can see living standards, goods shortfall, or whether
   a country's factories are producing something anyone wants. Per-seat readouts
   (produced/consumed per good, living-standard index, share of factories idle)
   are needed before anything in Phase 2 can be judged.

   **OWNED BY THE AI LOOP SESSION, NOT BY THIS PLAN.** It writes these itself,
   at Phase 2, so the bench's scoring semantics stay in one hand. **Do not edit
   `tools/od_bench.py`.** What this plan owes it instead is the field names the
   game will expose, early enough to reserve columns — see §12, "Bench
   interface".
2. **~~Freeze a reference model and a reference build.~~ ALREADY DONE**
   (2026-09-04) — `build/loop/reference/`, see §11b. Seat scores are
   build-relative and a game-rule change moves ADVANTAGE on its own, so this had
   to exist before any rule change; it does. **Do not redo it, and do not commit
   or tag it** — the user's rule is that nothing is committed or tagged without
   their word, which is why the freeze is a directory and not a git tag.
3. **A determinism test.** The game is deterministic and players rely on it
   ("that's the exact same flag generated for the AI Yugoslavia in my world").
   A goods economy adds a per-turn allocation pass over every province of every
   country — the highest-risk place in this plan for an iteration-order bug to
   introduce divergence. Add a test that runs N turns twice and diffs the state
   hash, and run it in CI, *before* Phase 2.
4. **A save-format version gate.** Phases 1-5 all add persisted fields.
   `ProvinceDelta` in `src/SaveManager.h` is a bitmask of changed fields and it
   is nearly full. Decide now whether to widen the mask or add a second one, and
   make old saves load with defaults rather than fail.

---

## Phase 1 — Industry capacity *(idea E)* — **BUILT 2026-09-04**

**Status: in the tree, building, tested, verified end-to-end. Not committed.**

What landed:

- `industryCapacity()` in `src/BuildCosts.h` — the rule, four terms, pinned
  constants, calibrated against all four shipped maps.
- `m_provinceAreaArray` in `Game`, filled with cos(latitude)-weighted area
  during the load pass that already walks every pixel.
- `Game::provinceIndustryCapacity` / `provinceIndustryIncome` /
  `countryIndustryCapacity` in `Game_Economy.cpp`.
- Every caller routed through them: the province panel's build gate and its
  button, `upgradeQuote` (and therefore the bulk brush and the multiplayer
  host), `processUpgrades` re-checking at apply time, `projectIncome`, and the
  AI's `nextIndustryBuy`.
- `tests/industry_capacity_test.cpp` — 378 checks, registered in CMake and
  `tests/run_all.sh`.
- `[CAPACITY] cid= used= total= overcap=` per country at end of eval, for the
  bench.
- Tutorial page in `data/dialog/en/tut_economy.oddlg`; `--tutorial-walk` passes
  108 pages, 0 problems.
- README, CHANGELOG, `docs/modding.md`.

Measured on a 40-turn, 65-country eval: world utilisation 25.4% of available
capacity, the heaviest industrialiser at **85.8%** of its own ceiling, `+865`
industry levels still built over the run, and **3** grandfathered provinces —
exactly the number the offline calibration predicted for that map.

**One correction worth recording.** The first fit gave density and area equal
ceilings (1.5 each) and they cancelled almost exactly: Belgium and a province
with the same population over sixty times the land came out at the *same*
capacity, so the density term contributed nothing while appearing to work. It
still scored well against the maps, for unrelated reasons. Constants were re-fit
with density outweighing area; map agreement is unchanged (11 disagreements
across all four maps, same provinces), and the test now asserts the comparison
directly so it cannot silently regress.

### The original plan for this phase



**The change.** A province's industry level is capped by what the province
physically is, and its income scales with the same thing.

**Capacity.** `Game::provinceIndustryCapacity(pid) -> int`, from:

- **population** — the dominant term, and the one the tutorial already claims is
  the dominant term (see §9: `tut_economy.oddlg` says *"a factory in an empty
  province is an expensive shed — the population is the input, not the
  building"*, which is currently false);
- **land area** — a province's pixel count. Not currently stored:
  `m_provincePixels` is built lazily and costs one int per map pixel (~128 MB at
  8192×4096). Add a cheap `m_provinceArea` (one `int` per province, ~6k ints)
  filled during the same load pass that fills `m_provinceCountryLookup`.
- **resource endowment** — the five `ProvinceResources` amounts, which is what
  makes an empty but oil-rich province still worth a refinery.

Floor at 1 so nowhere is permanently barren; ceiling at the research-gated level
so the tech tree still means something.

**Income.** Replace `ind.income = targetLevel * 2.0f` with a function of level
*and* capacity, so building the tenth level in a province that can barely
support three yields accordingly. Keep the level→income relationship monotonic:
a level built must never *lose* money, or players hit a wall and stop.

**Where the rule lives.** In `Game`, not in `Game_Render`. A rule written in
the render layer binds the local player and nothing else — not the AI, not the
network, not a mod. Callers to update, all of them:

- `src/Game_Render.cpp:4455` — the province panel's build gate
- `src/ai/AISystem.cpp:1123` — `AISystem::industryCap()`, today research-only
- `src/ai/AISystem.cpp:111` — `nextIndustryBuy()`, which already ranks by
  population × resources and will now agree with the rule instead of guessing at it
- `src/Game_TurnLogic.cpp` `processUpgrades` — re-check at *apply* time, since a
  province can be conquered or depopulated between order and completion
- `src/Game_Multiplayer.cpp:2713` — the host's order whitelist
- `src/MapEditor.cpp` — show the cap so map authors can see what they are authoring
- `src/mods/` `Economy.Write` — decide whether a scenario tool may exceed the cap
  (recommendation: yes, with the cap re-imposed on the first player-built level)

**Risk.** This is a global economy rebalance. Every existing save gets poorer
overnight in its overbuilt provinces. Either grandfather existing levels (cap
new builds only) or accept the reset and say so in the changelog. **Recommend
grandfathering**: a player's cities should not shrink because they updated.

**Cost.** Small. Two days of work, a week of balance.

---

## Phase 2 — Goods *(idea A)* — **BUILT 2026-09-04, behind the flag**

**Status: the economy exists, runs, persists, displays, is tested and is
measured. Not committed. What remains is listed below and is deliberate — the
parts held back belong to Phases 3 and 4, or wait on the rate sweep.**

Verified: both binaries build; `--tutorial-walk` passes 108 pages with 0
problems **with goods on and off**; `tests/determinism_check.sh` passes six runs
of seed 4242 **in both economies**; `IndustryCapacityTest` 378 checks,
`GoodsRecipeTest` 47 checks, `SaveDeltaTest` 28 checks all pass; and the
province and economy panels render in a loaded 1914 save.

Built: the four goods and their recipes, national stockpiles, per-province
output assignment, extraction → production → consumption → auto-sale in
`processProduction`, the `autoAssignOutputs` allocator, living standards feeding
`getProvinceRebellionChance`, the `OD_GOODS` / `OD_AUTOSELL_PCT` environment
hooks, and `[LIVING]` / `[GOODS]` bench lines.

**Flag off is exactly neutral.** A 40-turn eval with `OD_GOODS` unset is
byte-identical to the Phase-1-only build on every reported figure — survival
67.9%, unrest 71.13, +865 industry levels, solvency 2.7% — and emits zero
`[LIVING]` lines. The old economy is untouched, which is what makes the new one
benchable beside it.

**Flag on, 40 turns, 1 map, seed 4242:**

| | flag off | flag on |
|---|---|---|
| survival | 67.9% | 66.0% |
| rebellions / 1k country-turns | 71.1 | 120.1 |
| median living standards | — | 1.00 |
| factories idle | — | 14% |

Determinism holds with the goods economy on: `tests/determinism_check.sh`, six
runs of seed 4242, agree over 25 turns.

### Two design errors found by measuring, not by reasoning

**1. The consumer good was unmakeable for 27 of 64 countries.** The first recipe
made consumer goods from rubber and gemstones. Gemstones are on 123 of the 1298
provinces of the 1939 map, and 27 countries have *neither* rubber nor gemstones
— so their populations could never be fed, whatever they built or however well
they played. Not a hard game, an unwinnable one. It measured as **93% of the
world's factories idle** and rebellions tripling.

Fixed by giving recipes a substitutable `any` component: the good the population
eats is the one that is not fussy about inputs, because what limits light
industry is people and factories — which is what the tutorial has said all
along. The war goods stay strategically constrained (no oil, no fuel), because
that is the interesting kind of scarcity and a country can trade for it.

**2. The allocator chose by need alone.** It assigned every factory to whatever
the country was shortest of, without asking whether the country held the
materials — so countries with full ore piles starved while their factories sat
assigned to a good they could not make. It now scores by need *and*
feasibility, and leaves a factory undirected rather than parking it on an
impossible good, so a trade or a conquest puts it to work the next turn.

Neither was visible by reading the code. Both were obvious within one eval.

### Unrest coefficient is deliberately low for now

`SHORTAGE_UNREST_PCT` is 6, half of bankruptcy's 20. **The AI cannot yet act on
a shortage** — it has no action that directs an economy, so the allocator is the
only thing responding on its behalf. Punishing a policy at full strength for a
problem it cannot see would measure the harness, not the player. The coefficient
should rise when Phase 3 gives the AI the lever.

### What is NOT built yet

- **~~Persistence.~~ DONE.** `ProvinceIndustry::output` rides the province delta
  (bit 8 of the mask, which was free; stored as an `int8` because -1
  "undirected" is a real value and the commonest one). The national stockpiles,
  the flag and `autoSellPct` go into `state.json`, keyed by the stable lowercase
  names rather than by enum index, so inserting a good later cannot make an old
  save read its own numbers as another good's. **The save now wins over the
  environment**: a campaign keeps the economy it was started under, so a player
  with `OD_GOODS` exported in their shell does not silently convert every old
  save they open, and a bench sweeping the variable cannot rewrite the worlds it
  is measuring. `SaveDeltaTest` passes 28 checks including "a save that needs no
  wide table is byte-identical to before".
- **~~Read-only UI.~~ DONE.** The economy panel shows living standards, the
  shortfall in the same units as the production beside it, per-good output and
  stock, idle factories and the surplus sale; the province panel says what that
  province is making. Present only in a goods world. This is not decoration —
  living standards feed unrest, and a government losing provinces to a number it
  cannot see would be exactly the failure this codebase already names for
  industry upkeep.
- **Directing a factory yourself.** Still not possible, and still deliberate:
  *who* directs an economy is the planned-versus-market choice, and that is
  Phase 3. Phase 2 is the economy existing and allocating itself.
- **No tutorial page, on purpose.** The tutorial is played in a flag-off world,
  so a page about goods would teach a system the player does not have — which is
  the same fault as Phase 1's "expensive shed" line, in reverse. It lands when
  the flag defaults on.
- **The AI action space.** Not widened, and `m_dynamics` not reset. Allocation
  turned out to be a resolver heuristic rather than a policy decision — exactly
  as the plan argued for Phase 3's capitalist — so **Phase 2 needs neither**.
  Both move to Phase 3, where the AI decides whether to direct the economy.
- **Rates are unswept.** `GOOD_PER_LEVEL`, `EXTRACT_PER_AMOUNT`,
  `CONSUMER_PER_CAPITA` and `RAW_FLOOR_PRICE` were tuned by hand over four
  evals to land living standards near 1.0 and survival near baseline. They have
  not been through a proper sweep, which is why the flag is off.
- **Military costs** still money-only. That is Phase 4.

### The design, as settled before it was built

**The decision that needs your eye is §2.2: what happens to resource income.**
Everything else follows from it, and it is the one part of this that changes how
the economy *reads* rather than just what it computes.

### 2.1 The chain

```
  province deposits  ──extract──▶  raw materials  ──factories──▶  goods  ──▶  population
   (oil, metal,                    (national pool)                 (4)         living standards
    rubber, gemstones)                                                              │
                                                                                    ▼
   gold ──────────────────────────────────────────────▶ money            unrest, growth, migration
                                    surplus ◀──────sell/buy──────▶ money
```

Four goods, four inputs, one of the five existing resources left as money:

| Good | Made from | Consumed by |
|---|---|---|
| `consumer` | rubber, gemstones | population — **this is the living-standards good** |
| `machinery` | metal | industry upgrades, forts, ports |
| `fuel` | oil | navy, army movement, artillery |
| `munitions` | metal | recruitment, artillery |

Gold stays money. Historically apt, and it saves inventing a conversion rule.

**Inputs come from a national pool, not the province's own deposits.** Most
provinces hold zero of any given resource — 416 of 1298 on the 1939 map hold
*none of the five* — so a factory that could only eat what was under it would be
idle almost everywhere. A pool is also what makes Kaiserin's "trade deals for
the resources you lack" mean something.

### 2.2 THE DECISION: resource income stops being money

Today `provinceResourceIncome()` turns deposits straight into cash, and it is a
major line on every economy. Under the chain above, deposits produce *materials*
instead, and money comes from taxes and from selling what you do not need.

That is the honest version of what was asked for, and it is a real change to how
the game reads: a resource-rich, industry-poor country goes from "wealthy" to
"holding things other people want".

**DECIDED 2026-09-04 (user): it is a dial, not a mode.** The question was posed
as full-vs-split-vs-parallel and the answer was better than the question —
*"can it be toggleable between split, full, and what percentage do we sell?"*

So there is one number, `autoSellPct`, the share of each turn's surplus raw
materials sold automatically at the administered floor price:

```
  autoSellPct = 100   every surplus material is sold      ~ today's economy
  autoSellPct =  60   a working split                     the intended default
  autoSellPct =   0   nothing sells itself                the "full" version
```

"Full" and "split" stop being two implementations to choose between and become
two settings of one, which removes the migration this section was worried about:
a save loads at whatever its world was created with, and the balance question
becomes tuning rather than a fork. The rejected third option — leaving resource
income as money and giving factories a *separate* abstract input — stays
rejected: it double-counts the same ore and players will spot it.

**Where the dial lives.** Three readers, one value:

- a **per-world setting**, chosen at world creation and saved with it, so an
  existing save keeps the economy it was started under;
- an **environment override** (`OD_AUTOSELL_PCT`) so the bench can sweep it
  without authoring a world per value — the AI's economy has to be measured at
  more than one setting before a default is defensible;
- and, **from Phase 3, the economic system moves it.** This is the part that
  makes the dial more than a convenience: a planned economy directs materials
  rather than selling them (low `autoSellPct`), a free market sells its surplus
  (high). Kaiserin's "in a planned economy money comes from trade deals and
  selling extra resources" and "in a free market you tax" are then the same
  mechanism at two settings, and the compass is what moves between them.

**Default: 100 at first**, so Phase 2 lands economy-neutral and every existing
save stays solvent on load, then tuned down toward 60 once the bench can see
what it does. A default that quietly bankrupts a player's campaign on update is
not a balance change, it is a bug report.

### 2.3 Living standards

`supply / demand` for `consumer`, per country, 0..1+. Demand scales with
population and with the compass — a left or libertarian population expects more.
It feeds exactly two things:

- `getProvinceRebellionChance()`, shaped like the existing
  `BANKRUPTCY_UNREST_PCT` term: on while short, off when met, no accumulation.
  That term already exists and already works, so this is a sibling rather than a
  new mechanism.
- population growth and migration, which already have research-effect plumbing
  (`popGrowthPct`, `migrationRate`) to share.

**Two consequences, both visible, and nothing else.** A hidden third multiplier
is how an economy becomes unexplainable.

### 2.4 What this does NOT do

- No per-province stockpiles. One national pool per good. Per-province inventory
  is a logistics game, and this is not one.
- No price simulation. Buy and sell at administered prices that move slowly with
  world supply. A market sim is a project of its own and would swamp everything
  else in this document.
- No supply chains deeper than one step. Raw → good → consumed. Not raw → part →
  assembly → good.

### 2.5 The feature flag

Per §13, Phase 2 ships behind a per-world toggle, default off until benched.
`OD_GOODS=1` and a world setting; the money economy stays playable and benchable
throughout, and the flag comes out at Phase 4. This is the rollback that is not
a revert, and for a batch this size it is not optional.

### The original outline for this phase



**The change.** Factories stop emitting money and start emitting **goods**,
which the population consumes, and whose sufficiency drives unrest and growth.

**Keep the list short.** Every good is an AI action, a UI row, a save field, a
tutorial page and 21 translations. Four:

| Good | Made from | Consumed by |
|---|---|---|
| **Consumer goods** | rubber, gemstones | population — this is the living-standards good |
| **Machinery** | metal | industry upgrades, forts, ports |
| **Fuel** | oil | navy, army movement, artillery |
| **Munitions** | metal | recruitment, artillery |

Gold stays money, which is both historically apt and saves a conversion rule.

**The per-turn pass**, per country, in `processEconomy()`:

1. each province with industry produces `throughput(level, capacity)` of its
   assigned good, limited by input resources available;
2. the country's population generates demand for consumer goods
   (∝ population, scaled by the compass — a libertarian/left population expects
   more);
3. `supply / demand` → a **living-standards index**, 0..1+;
4. that index feeds:
   - `getProvinceRebellionChance()` as a term shaped like the existing
     `BANKRUPTCY_UNREST_PCT` one — on while short, off when met, no accumulation;
   - population growth and migration (`popGrowthPct`, `migrationRate` already
     exist as research effects and can share the plumbing);
   - **not** a hidden multiplier on anything else. One index, two visible
     consequences, both explainable in one tutorial page.
5. surplus goods are sellable for money; deficits can be bought, if trade
   exists with someone who has them.

**Money survives, and stays legible.** Kaiserin was explicit that money should
stay. It does: taxes and resource sales in, army/navy/policy upkeep out, and
now goods trade on both sides. What changes is that *industry income* is no
longer the largest line on the books — production is.

**Where.** `Game_Economy.cpp` grows a production pass; `CountryIncomeSnapshot`
grows goods columns; `ProvinceIndustry` grows an `output` assignment;
`Game_Render.cpp`'s economy panel grows a production tab.

**Risk. This is the phase that can break the game.** It changes what money is
for, and every existing cost was tuned against the old answer. Ship it behind a
flag and bench both worlds before switching the default.

**Cost.** Large. This is the centre of the plan.

---

## Interlude — "army and people", built 2026-09-04

Not a numbered phase: a user request (mobilisation doctrines should drive what
an army costs) that turned up two live bugs on the way in. All three landed
together as one benchmark change.

**1. Mobilisation doctrines drive army cost.** 17 doctrines now carry
`conscriptionCostPct` and `maintenanceCostPct` on *opposed* signs, so a
mobilisation doctrine is a trade rather than a discount — Mass Mobilisation
recruits -30% / upkeep +25%, Demobilisation +15% / -25%, Professional Army worse
at both and much better in the field. Parked at
`docs/design/patches/mobilisation-levers.patch` while the loop session gated, then
re-applied.

**2. `maintenanceCostPct` was summed and never spent.** Four research nodes and
four doctrines advertised an army-upkeep reduction; army upkeep was a flat rate
per man with no modifier anywhere in it. Eight promises, none ever applied.

**And the fix for it did nothing, which the eval caught.** Wiring the modifier
into `computeCountryIncome` produced *byte-identical* output. A probe found it
reaching zero countries: `refreshIncomeCache` holds a **second copy** of the
army upkeep line and that is the copy the game reads, because
`computeCountryIncome` returns from `m_countryIncomeCache` for almost every
caller. **Third time in that one file that a formula has had two homes** — the
build-cost tables, the income projection, now army upkeep. If a fourth appears,
they should share a function rather than a comment.

**3. The population runaway.** 2.1 billion at load became ~110 billion by turn
120 — 52× — and nothing had noticed because every other reader of population is
logarithmic or fractional. Two causes:

- `growCountryPopulation` was written to be the correct per-province rule, and
  its own comment records *why* — growth "applied per minority per province" made
  "a three-minority province grow three times as fast as a one-minority
  neighbour". That fix landed and **the old block was left standing** in
  `applyEthnicPolicyEffects`, whose `processed` set is country-scoped, so the
  first province holding each new minority name got growth applied once per name
  — a mean of 9.5× over — while its neighbours got nothing. Effective compounding
  ~3.35%/turn, and (1.0335)^120 = 52.4.
- The logistic taper aimed at a flat `MAX_PROVINCE_POP` of 1e10 per province. The
  largest real province holds 74.7M, so headroom read 0.9925 and **the taper did
  nothing anywhere a real game goes.**

Now: growth once per province, ethnic policy share-weighted across a province's
minorities (never summed), and the ceiling is the province's own
cos(latitude)-weighted area — the Phase 1 measure, reused, no new state — at
`POP_PER_AREA` 1400, putting the 1939 world near 8.5 billion.

| implied world population | before | after |
|---|---|---|
| turn 40 | 5.03 bn | 1.67 bn |
| turn 80 | 81.1 bn | 1.85 bn |
| turn 120 | 141.6 bn | 1.82 bn |

Goods off, same map and seed, before → after the batch: turn 120 survival
58.5% → 66.0%, unrest 61.70 → 62.64, readiness 106,935 → 99,427.

---

## Phase 3 — Economic systems *(ideas B, C, D, I)* — **BUILT 2026-09-04**

**Status: in the tree, building, verified. Not committed.** Inert with goods off.

- `plannedShare(cid)` straight off the economic compass — -100 → 1.0, centre
  0.5, +100 → 0.0. **The compass is the dial**, not a separate setting, which is
  what makes moving it cost something.
- `directableFactories(cid)` = that share of a country's factories. Planned
  directs everything, free market nothing, the middle is Kaiserin's "mix system
  (where you control certain factories, not all of them)" — continuous, not
  three buttons. Enforced in `setProvinceOutput`, so player, AI and multiplayer
  host hit one rule.
- `ProvinceIndustry::directed` separates "a government chose this" from "the
  economy allocated it"; the allocator skips directed factories.
- `autoSellPctFor(cid)` — a planned economy auto-sells a fifth of what a market
  does and must find money in trade. This is the Phase 2 dial, now moved by the
  compass exactly as §2.2 promised.
- A planned economy pays for factories partly in **machinery** rather than cash
  (`PLANNED_MATERIAL_SHARE` 0.7), money half scaled down to match, charged
  all-or-nothing in `queueUpgrade`. That is idea (D).
- A province-panel button to direct a factory: cycles through the goods and back
  to "let the economy decide", greyed with the reason when the country has no
  capacity to direct.

**And it needs no AI action-space widening either.** A government's economic
system is a consequence of the doctrines it runs, and the politics head already
enacts doctrines — so an AI that nationalises is one that chose Left doctrines,
which it can already do. `ECON_ACTIONS` stays 12, `FEATURE_COUNT` 143,
`m_dynamics` keeps its training. **The migration reserved since Phase 2 has now
been avoided twice.**

Verified: both binaries build; determinism 6/6 at seed 4242 with goods on;
IndustryCapacityTest 378, GoodsRecipeTest 52, PolicyRulesTest, SaveDeltaTest 28;
`--tutorial-walk` 108 pages, 0 problems.

### The original plan for this phase



**The change.** *Who* assigns a factory's output becomes a governing choice, and
that choice is the existing economic compass axis rather than a new orthogonal
menu.

- **Planned** (compass economic ≤ −X): you assign every factory's output.
  Upgrades cost **machinery**, not money. Money comes from trade and surplus
  sales. Maximum control, minimum liquidity — exactly Kaiserin's description.
- **Free market** (≥ +X): capitalists assign output, chasing profit. You raise
  money by **taxing people and business**, and you can **lend** it to a factory
  to make it upgrade sooner. Capitalists accumulate their own treasury and
  reinvest it. Minimum control, maximum liquidity.
- **Mixed** (the middle): a *fraction* of factories are state-directed —
  the fraction being a function of how far left the compass sits, so the
  spectrum is continuous rather than three buttons. Nationalised industry
  produces to plan; the rest produces to profit.

**Why on the compass and not as a separate setting.** Because it makes the
compass *cost* something, which answers idea (I) — "expansion of internal
politics" — without inventing a second political system. Moving left buys
control and costs money; moving right buys money and costs control. And the
existing policies (`state_industry`, `privatization`, `nationalisation`,
`deregulation`, `autarky`, `free_trade`) stop being flavour text with a compass
shift attached and become the levers that move you along it.

**The capitalist actor.** Free-market factory assignment needs an agent. Do
**not** put it in the neural AI — it is a per-province greedy choice with a
clear objective (profit given prices), it must be deterministic, and it runs for
every province of every country every turn. Write it as a resolver heuristic in
`Game_Economy.cpp`. The neural AI decides *policy* (tax rates, loans,
nationalisation); the capitalist heuristic decides *allocation*.

**Cost.** Medium, and mostly UI. The mechanics ride on Phase 2's pass.

---

## Phase 4 — Repricing *(idea H)* — **BUILT 2026-09-04**

**Status: in the tree, building, verified. Not committed.** Goods-off is
*byte-identical* on every reported figure, so the AI ruler does not move.

- **Recruitment** costs munitions as well as money.
- **Artillery and naval bombardment** cost fuel and munitions; aircraft types
  (napalm, carpet) are fuel-heavy, warhead types (chemical, nuclear,
  biological) munitions-heavy.
- **A standing army burns fuel every turn.** A shortfall is *bought in* at
  `FUEL_SHORTFALL_PRICE`, not paid in combat power — deliberately, because
  degrading combat would move `resolveAssault`, the function the amphibious
  work is being measured on. One variable at a time.
- **Machinery, fuel and munitions finally have real demand**, which closes the
  gap the loop session flagged on the very first goods bench ("three of four
  goods are pure factory-idling right now").
- **Doctrines are the exchange rate**, reusing existing levers rather than
  inventing effect names: `conscriptionCostPct` scales what it costs to *arm*
  (recruits' munitions, shells), `maintenanceCostPct` what it costs to *run*
  (an army's fuel). All 17 mobilisation doctrines gained a second dimension
  with no new plumbing.

### Seven copies of the artillery price list

Money costs for ammunition lived in **seven** places: `ALL_ARTY` in the province
panel, `ARTY_COST` beside the firing code, a second beside naval bombardment,
`CANCEL_ARTY_COST` and `ARTY_CANCEL_COST` for refunds, two `Ammo` tables in the
AI's executors, and an eighth-in-effect `SHELLS` table in the AI's *mask*. All
agreed on the day — adding two more currencies to seven copies is not a risk of
divergence, it is a plan for one.

Two were especially dangerous. **A refund that read a different table from the
charge would have been a money printer.** And **a mask that prices a shell
differently from the executor offers an action the executor then refuses**, so
the policy head learns from a choice that never happened. All seven now read
`ARTY_COSTS` in `BuildCosts.h`; only the research-node mapping stayed the AI's.

### The allocator had to be rewritten, twice

"Feed people first, then fill the emptiest shelf" was **all-or-nothing**, and it
starved the war economy completely: consumer demand is chronically a little
short almost everywhere, so at turn 40 consumer goods were the *only* thing
produced in the world — machinery, fuel and munitions all at 0.0 against real
demand, every army buying its fuel at the penalty price.

Ranking all four goods by the **fraction** of their need unmet fixes it: a small
need entirely unmet outranks a large one nearly covered. Proportional rather
than absolute is essential — consumer demand is in the hundreds and fuel in
single digits, so an absolute gap would rank consumer first for ever.

Then, **not clamping the fraction at zero**: with met needs all scoring 0 the
tie fell to one good and consumer piled to seven times what anyone wanted
(stock 8,121 vs demand 1,147) while machinery sat at twice its reserve. Letting
it go negative spreads surplus across the shelves.

### One rate is unswept, and says so

`ARMY_FUEL_PER_10K` was tried at 0.004, 0.01 and 0.02 on one map and one seed.
The results were **not monotonic** — living standards 0.76 / 0.56 / 0.61,
survival 54.7% / 45.3% / 52.8%. A single run cannot separate them, and reading
that ordering as signal would have been fitting noise.

So it is chosen on the one thing a single run *can* show: whether the mechanic
exists. At 0.004 world fuel demand was 7 against a stock of 677 — provably
inert. At 0.02 production tracks demand turn by turn (29.8 vs 28.8) — provably
live. **0.02 is the smallest value tested at which the phase does what it is
for**, and it wants a multi-seed sweep before anyone calls it balanced.

Verified: goods-off byte-identical; determinism 6/6 at seed 4242 in **both**
economies; IndustryCapacityTest 378, GoodsRecipeTest 52, PolicyRulesTest,
SaveDeltaTest 28; `--tutorial-walk` 108 pages, 0 problems.

### The original plan for this phase



Pr1nted's own condition on (A), and it is correct: once goods exist, a purely
monetary army is an anachronism.

- **Recruitment** costs money *and* munitions. Today: `count/10000 × costMod`
  (`Game_Render.cpp:1629`).
- **Artillery** costs fuel and munitions. Today: a flat money cost per type.
- **Army upkeep** eats fuel per turn alongside its `0.01 per 10k men` payroll,
  so a large standing army is a *production* burden — which is the entire
  reason a Victoria-shaped game has a production economy.
- **Doctrines** become the exchange rate between production and combat power:
  a doctrine that trades munitions for attack, or fuel for movement, or manpower
  for both. The game is called *Open Doctrines*; doctrines should be the most
  consequential screen in it, and currently they are a per-turn bill with a
  compass shift.

**Note for the AI.** Costs are how the economy module's mask is built. Changing
them changes which actions are *offered*, not just which are chosen. See §11.

**Cost.** Medium. Mostly numbers, but numbers that need benching.

---

## Phase 5 — Releasing nations *(ideas G, I)* — **BUILT 2026-09-05**

**Status: in the tree, building, verified. Not committed.** All six phases done.

- `src/ReleaseRules.h` — the rule, pure and testable without a game;
  `tests/release_rules_test.cpp`, 23 checks.
- `Game::releasableRegions` feeds it province minorities and alignment;
  `Game::releaseNation` acts, re-checking ownership and the size cap because
  the panel's list can be a frame old and the host takes orders from clients.
- `createRebelCountry` gained a `peaceful` flag — a release is a revolt with the
  opposite ending, so it shares the 1,100 lines of naming, flags, compass and
  pixel surgery instead of copying them. Peaceful means no war, a **guarantee
  from the releaser**, no "Breakaway State!" popup, and **no `noteRevolt`** —
  teaching the AI it *lost* ground would train it away from a lever it is meant
  to use.
- A release button in the province panel, naming the people and the size.
- Release as the **last rung of the bankruptcy cascade**.

### The rule changed twice, and real map data drove both

**Excluding "the country's own people" cannot be computed here.** By population
Britain's largest group is Hindustani and France's is Kinh; by province count
Britain's is Indigenous Canadian and the Netherlands' is Javanese either way,
and there is no capital in the map data. Every definition picks a *colonial*
people for a colonial empire — so the rule would have **forbidden Britain from
releasing India while offering to release Britain.** Decolonisation is the
feature's best use and the rule banned it.

Dropping it exposed the opposite: on the 1939 map the size cap alone let the
Soviet Union release the Russians (34 provinces, 94 M) and the USA release White
Americans (36 provinces).

**Alignment is the signal that was already there.** Release is offered only for
*disaffected* peoples. Your own are reconciled and never appear; a repressed
minority does. It also puts release where the roadmap wanted it — the last rung
of the ethnic-policy ladder, since repression makes a region releasable and
conciliation removes the need.

What it finds on the shipped maps (pre-alignment, which is runtime-only): 93
regions across 29 of 64 countries on 1939 — Austria-Hungary → Czech, Slovak,
Hungarian; the Ottomans → Turkish, Arab, Jordanian Arab; Britain → Sudanese
Arab, Shona, Ovambo; France → Tunisian Arab, Algerian Arab, Berber. Historically
legible with no hand-authored data.

### The cascade trigger was wrong, and measurement caught it

First version fired when the country was *still short after the cascade*. That
happened **zero times in a 120-turn run with 46 bankruptcies** — disbanding
troops clears almost any single shortfall, because men are cheap to shed and
there are millions of them. It would have been dead code, exactly like Phase 6's
attrition.

The failure it targets is not one bad year, it is chronic insolvency: cut
everything, the cuts raise unrest, the unrest costs provinces, the smaller
country is broker still. So the trigger is now a **five-turn bankruptcy streak**,
reset on solvency and on a release, so a country sheds at most one region per
five broke turns rather than dissolving in a single cascade.

It fires: one release, two provinces, in a 120-turn run. Deliberately rare — and
the seat bench then showed it is **causal on the seat it was built for**.

Champion model, v9 → v10, same seeds, deterministic:

| seat | v9 | v10 |
|---|---|---|
| 1939 FRA | 245 | 245 |
| 1914 SWE | 240 | 240 |
| 1939 USA | 324 | 324 |
| FRA rush | 71 | 71 |
| 1939 NOR | 13 | 13 |
| **modern CHN** | **96** | **171** |

Five seats identical to the digit and one seat moved: the rung is the only thing
that touched it. Overall rating 165 → 177.

That seat is the project's long-standing failure — it disintegrates through
bankruptcy → minority cuts → unrest rather than being conquered, and two earlier
attempts to fix it by changing *which resource the cascade cuts* were measured
and reverted. The spiral was never about the order of cuts; it was that the
country kept the provinces that were bankrupting it. A trained China now sheds
the region it cannot govern and survives smaller.

**The boundary is worth keeping too:** the *shipped* model's China moved only
3 → 7, because it disintegrates faster than a five-turn streak can accumulate.
The rung helps a country still fighting to stay solvent, not one already gone.

Verified: determinism 6/6 at seed 4242 in both economies; ReleaseRulesTest 23,
IndustryCapacityTest 378, GoodsRecipeTest 52, PolicyRulesTest, SaveDeltaTest 28;
`--tutorial-walk` 108 pages, 0 problems.

### The design, as settled before it was built

**Reuse `createRebelCountry`, do not write a second one.** It is ~1,100 lines
and already does every hard part: a plausible country name that does not
collide (Japanese → Japan, not Japana), an ISO, a flag, a compass averaged over
the released provinces, the province ownership transfer, `reindexProvinceOwner`,
and the pixel-list surgery that a naive implementation gets quadratically wrong.

What it hardcodes is **war**: `m_relations[a][b].war = true` both ways, plus a
"Breakaway State!" popup and `noteRevolt`. A release is the same event with the
opposite ending, so it takes a `peaceful` flag rather than a parallel function.
A second copy of that machinery would be the eighth two-homed thing in this
codebase.

**What can be released.** A run of contiguous provinces the player owns where
one minority dominates — `m_provinceMinorities` already partitions every
province to 100%, so "dominates" is a share threshold plus a BFS over
`m_provinceNeighbors`. A foreign claim (`m_claimsByProvince`) is the second
signal and can come later.

**What it costs and buys.** Land, population and industry, against unrest gone
and a friend: the new state starts at peace with a guarantee, so releasing is a
way to shrink deliberately rather than be shrunk. That trade is the point —
it makes overextension a decision instead of a slow loss.

**The AI, and the migration this does NOT force.** The roadmap specced a new
politics action, `POL_ACTIONS` 12 → 13 — which, since politics already sits at
`MAX_MODULE_ACTIONS`, is the widening that re-indexes `DYN_ACTION_ONEHOT` and
forces the `m_dynamics` reset. That is a real migration and it should be timed
by the loop session, between gates, with a retrain budgeted.

So it splits:

- **Now, no action-space change:** the rules, the player's ability to release,
  and release as the **last rung of the bankruptcy cascade** in
  `applyBankruptcyPenalties`. That cascade already runs budgets → policies →
  minority spending → ships → troops, ordered by what is cheapest to undo.
  "Give up the province you cannot govern" is a coherent final rung and aims
  straight at the known `CHN dies to rebellion` failure: a country that
  bankrupts itself placating minorities and then disintegrates would instead
  shed the ungovernable region and survive smaller.
- **Later, when a migration window exists:** the strategic action, so the head
  can release because it judges the trade worth making rather than only when it
  is already broke.

### The original outline for this phase



`createRebelCountry()` already does the hard part. What is missing is a
*peaceful, chosen* path into it and a relationship on the far side.

- **Who can be released.** A set of provinces with a coherent identity: a
  minority that dominates them, a foreign claim over them, or a historical tag
  in the map data. `m_provinceMinorities` and `m_claimsByProvince` already carry
  both signals.
- **What you get.** A new country, at peace, with a starting relation — a
  guarantee, an alliance, or (if a subject system is added later) a dependency.
  Its compass is averaged over the released provinces, which
  `createRebelCountry` already does.
- **What it costs.** Land, population, and industry. What it buys: unrest gone,
  and a friend. That trade is the point — it makes overextension a decision
  rather than a slow loss.
- **Ties to ethnic policies (idea I).** Release should be the *last* rung of the
  ethnic policy ladder, under repression and conciliation, so the ethnic system
  gains an exit instead of only a dial.
- **AI.** A politics-module action (`POL_ACTIONS` 12 → 13). The AI holding
  ungovernable minority provinces to the point of bankruptcy is a known failure
  — `CHN dies to rebellion` — and this is a lever that addresses it directly.

**Cost.** Small-to-medium. Reuses existing machinery; the work is UI, the
selection rule, and the diplomatic aftermath.

---

## Phase 6 — Combat depth *(idea F)* — **BUILT 2026-09-05**

**Status: in the tree, building, verified. Not committed.** Combat width only —
attrition was built, measured, and removed. Not flag-gated: these are changes to
rules that already exist, so there is no byte-identical arm and the AI ruler
moves.

**Combat width.** Only so many men engage at once, set by the province's
cos(latitude)-weighted area (the Phase 1 measure, reused) and narrowed by
fortification — a fort now stops an attacker bringing his numbers, not merely
makes defenders tougher. Both sides are capped by the same frontage; defenders
share it in proportion.

**A repulsed assault no longer annihilates the reserve.** `resolveAssault` takes
an optional fallback province — the one the attack marched from — and the men
who never got into the fight retreat to it. **The amphibious path passes
nothing, so a failed landing still drowns**, which is the rule the amphibious
doctrine was measured against. Without this, width would have turned a
fortified pass into a machine for evaporating ten-million-man armies.

### Attrition was built, measured, and removed

The plan paired width with a slow bleed on armies standing on foreign ground —
the "enemy troops that never took the province" complaint, and the thing meant
to stop width producing stalemates.

**It never fired once.** Built at 1.5%/turn and measured against itself at 0%, a
120-turn eval produced **byte-identical output on every figure**. In a
deterministic simulation that is proof of absence: one man lost anywhere would
have moved the trajectory.

The problem had already been solved by earlier work. An assault that carries
leaves the attacker holding the province — his men are on their own soil the
moment the fight ends — and an assault that is repulsed is destroyed or falls
back. The eval's own trespass line reads `0 of 2`. There was nothing to bleed.

Removed rather than shipped inert, with the reasoning left in `Game.h` for
whoever adds supply or occupation-without-annexation later.

### Width fires, and its effect is not separable

| | binds |
|---|---|
| assaults over a 120-turn run | 36,306 |
| where the frontage bound | 7,364 (**20.3%**) |

So the rule is real. But a three-seed A/B, same tree, same session:

| seed | survival off → on | largest power off → on |
|---|---|---|
| 4242 | 41.5 → 39.6 | 20.6 → 20.1 |
| 777 | 34.0 → **35.8** | 18.7 → **21.3** |
| 31337 | 43.4 → 37.7 | 25.9 → 21.4 |

**Mixed in sign on both metrics.** Not separable at three seeds.

Shipped on correctness: numbers ceasing to be decisive past a frontage is right
whatever the aggregate does, and the bind rate is the evidence the rule exists.

**And then the AI bench showed it is a large effect after all.** On the v9 gate
the model of record posted **229 — the highest number this project has
produced** — with the defensive champion gaining 49 and the attacking model
losing 21. Width is what did it.

The first measurement asked the wrong question, not a badly-designed one:
survival and concentration describe a whole world grinding along, while the seat
bench asks how well a *particular country* is played. A change can be invisible
in the aggregate and decisive in the decisions. Measuring only the aggregate
would have retired this rule as pointless.

A first single-seed reading looked like a large drop (49.1% → 39.6%) — that was
the shared tree moving under the measurement between the baseline and the test,
not the change. The A/B above was run in one session on one binary pair for
exactly that reason.

Verified: determinism 6/6 at seed 4242 in both economies; IndustryCapacityTest
378, GoodsRecipeTest 52, PolicyRulesTest, SaveDeltaTest 28; `--tutorial-walk`
108 pages, 0 problems.

### The original plan for this phase



**Matt's constraint is right and should be accepted, not fought:** provinces are
too big for tactical manoeuvre, and rewriting the map to fix that is not on the
table. So do not add tactics. Add *duration, cost and constraint* — the things
that make a Victoria-shaped war interesting — and let doctrines drive them.

Four changes to `resolveAssault()`, in increasing order of risk:

1. **Combat width.** Only so many troops can engage at once, set by the
   province (area, terrain, fort level). A ten-million-man stack stops
   automatically beating a well-sited one-million-man stack. This alone removes
   the game's dominant strategy and is a ~30-line change.
2. **Multi-turn battles.** An assault that neither carries nor is annihilated
   becomes a **standing battle** in the province, which both sides can reinforce
   or withdraw from, resolving a round per turn. This is what turns a war from a
   sequence of coin-flips into something a player makes decisions inside.
   It is also the single largest change to the AI's world model in this
   document (see §11).
3. **Supply.** A stack's effectiveness falls with distance from friendly
   territory and rises with a port or a road. Fuel (Phase 2) is the natural
   currency. This makes deep pushes cost something and gives defence a reason
   to exist that is not just forts.
4. **Attrition.** Foreign stacks lose men slowly. Removes the "enemy troops
   parked forever on my territory" class of complaint permanently.

Recommendation: **do 1 and 4 first**, alone. They are cheap, they are safe, and
together they change how a war feels more than their size suggests. Hold 2 and 3
until the economy phases have settled, because 2 rewrites the war head's
environment and 3 depends on fuel existing.

**Do not** attempt sub-province manoeuvre, fronts, or a tactical layer. Matt's
read — that this would mean "completely rewriting the rules" — is accurate.

---

## 9. Tutorial — what has to change, and one thing that is already wrong

Tutorial content is `data/dialog/<lang>/*.oddlg` across **21 languages**, walked
by `--tutorial-walk` (`src/Game_TutorialWalk.cpp`), which checks every route,
every page: that pointers resolve, that conditions are reachable, that choices
name real scripts. It exits non-zero on failure and fails the build. **Every
edit below must be made in `en/` first, walked, then translated, then walked
again with `OD_WALK_LANG` per language.**

Routes: `intro`, `tutorial`, `specifics`, `tut_ships`, `tut_research`,
`tut_economy`, `tut_unrest`, `tut_diplomacy`, `outro`.

**Already wrong, today, before any of this lands.** `tut_economy.oddlg` says:

> *"**Industry** grows where people already are. A factory in an empty province
> is an expensive shed — the population is the input, not the building."*

That is a description of Phase 1. It is not a description of the game as
shipped, where `income = level × 2` regardless of population. **The tutorial is
currently teaching a rule the game does not have** — which is worth fixing on
its own merits, and is a second argument for doing Phase 1 first: it makes the
tutorial true instead of requiring the tutorial to be rewritten.

Per phase:

| Phase | Tutorial work |
|---|---|
| 1 | `tut_economy` — the shed line becomes true; add the capacity readout to the province page. **Text may not need to change at all.** |
| 2 | `tut_economy` gains a production page: what factories make, who eats it, what happens when they run short. New pointer targets (`panel.production`) must be registered or the walk fails. |
| 3 | New route `tut_economy_systems`, or a branch off `tut_economy` — planned vs market vs mixed, and that the compass is the dial. Add to `ROUTES[]` in `Game_TutorialWalk.cpp`. |
| 4 | `tutorial` (main) — recruitment now costs munitions. One page. |
| 5 | `tut_unrest` — release as the way out of an ungovernable province. One page, and a `until=` condition the walk can drive. |
| 6 | `tutorial` (main) — combat width, and that a battle can last. This is the page most likely to need a diagram. |

**Ordering note.** Do not translate until the mechanic is benched and final.
A retuned number is free; a retranslated page across 21 languages is not.

---

## 10. Documentation — the full list

| File | What changes |
|---|---|
| `README.md` | The feature list (lines ~27-46) describes provinces, economy and the AI's 12 economy actions. All three move. Screenshots in `docs/img/` (`economy.png`) get stale at Phase 2. |
| `CHANGELOG.md` | Per phase. Phase 1 and 2 need explicit "your existing saves will look different, here is how" notes. |
| `docs/modding.md` | `Economy.Read` / `Economy.Write` capability tables (lines ~255-257). Goods, output assignment and economic system are all new surface. **`Economy.Write` currently sets industry level and does not charge — decide whether it may exceed the Phase 1 cap.** |
| `sdk/abi.json` + `sdk/gearbox.h` + `sdk/gearbox_generated.h` | New host calls for goods/production. **ABI additions only, never reorder** — every shipped mod binds to the existing table. Regenerate the 11 language bindings (`assemblyscript`, `cpp`, `go`, `java`, `js`, `lua`, `python`, `rust`, `wat`, `zig`, `compat`). |
| `docs/gearbox-sdk.md`, `docs/gearbox-abi.md` | New capabilities and calls documented. |
| `wiki/Getting-Started.md`, `wiki/Home.md` | Player-facing description of the economy. |
| `wiki/AI-Architecture.md`, `wiki/Capability-Modules.md` | Action counts and feature count both move. |
| `docs/multiplayer.md` | New order types on the wire (see below). |
| `docs/greater-diplomacy.md` | If `.odmap` gains per-province fields, the GD5 mapping is affected. |
| **Dragoman** (`~/CLionProjects/dragoman`) | **Not in this repo.** The OD ↔ Greater Diplomacy 5 map translator reads `.odmap`. Phase 1 adds province area (derivable, may need nothing) and Phase 2/3 add per-province output assignment (new field). **Re-run its conformance check after any `.odmap` change.** |

---

## 11. The AI — this is the expensive coupling

An AI improvement loop is running in another session against this tree
(`docs/ai/LOOP.md`, `BACKLOG.md`, `LOOP_JOURNAL.md`). Everything below is why
that session needs to be told before any of this starts, not after.

**Every phase invalidates its benchmark baseline.** Seat scores and ADVANTAGE
are build-relative: a game-rule change moves the number without touching a
weight. Phase 1 alone re-prices every industry decision in the game.

**Phase-by-phase impact:**

| Phase | Mask | Action space | Features | Retrain? |
|---|---|---|---|---|
| 1 | `validEconomy` action 1 gate changes; `industryCap` becomes per-province | unchanged | +1 (capacity headroom) | **Yes** — a mask change on a frozen policy measures worse than it is |
| 2 | every econ action's price changes | +goods assignment action(s) | +4-8 (goods, living standards) | **Yes, from a checkpoint** |
| 3 | new gates on system | +1-2 (nationalise, tax, lend) | +2 | **Yes** |
| 4 | every military cost | unchanged | +2 (stockpiles) | **Yes** |
| 5 | politics | `POL_ACTIONS` 12 → 13 | +1 | Yes, cheap |
| 6.1/6.4 | war | unchanged | +1 | Yes |
| 6.2 | war | +reinforce/withdraw | +3 | **Yes — full retrain**; standing battles change what an assault *is* |

**Two hard constraints:**

1. **`MAX_MODULE_ACTIONS = 12` and `ECON_ACTIONS = 12` — economy is full.**
   Phase 2's output assignment cannot be added without widening
   `MAX_MODULE_ACTIONS`, which widens every `[module][action]` array in
   `TrainStats`, the training buffers, `GUARD_HEADS`-indexed arrays
   (`m_marginalChosen[GUARD_HEADS][MAX_MODULE_ACTIONS]`) and `Experience::visits`.
   Do it once, at Phase 2, with headroom (16), rather than by one each time.

   **And it is not a free append — it silently corrupts the dynamics head.**
   `DYN_ACTION_ONEHOT = MOD_COUNT * MAX_MODULE_ACTIONS` is part of the dynamics
   net's *input layer*: `m_dynamics = NeuralNet({TRUNK_OUT + DYN_ACTION_ONEHOT,
   320, TRUNK_OUT}, 800)`, and `dynamicsInput()` writes the one-hot at

   ```cpp
   in[emb.size() + module * MAX_MODULE_ACTIONS + action] = 1.0f;
   ```

   Widening 12 → 16 **re-indexes the interior of that one-hot**: politics action
   0 moves from slot 12 to slot 16, war action 0 from 24 to 32, navy from 36 to
   48. `NeuralNet::deserialize` sees only that the first layer grew
   (`sameExceptFirst`), keeps the existing weights *where they are*, and
   zero-fills the tail — so every module except economy silently reads its
   actions off the wrong weights. This is the one migration in the plan that
   does **not** reproduce the old behaviour.

   **DECIDED 2026-09-04 by the AI loop session: (a) — reset `m_dynamics` at the
   widening** and let `DYN_WARMUP_UPDATES` (200,000) re-earn it. Play is
   bit-identical to a build without search until warmup clears, so it is safe by
   construction; search at play is separately closed in the loop journal (4/8
   sims measured 32 below the sampled policy — the latent model misguides at
   play), and train-with-search is the only consumer, so the warm-up gap costs
   nothing shipped. The rejected alternative was remapping the one-hot on load.
   Recorded in `docs/ai/BACKLOG.md`, with `GUARD_HEADS`,
   `m_marginalChosen`/`m_marginalOffered` and `Experience::visits` following in
   the same migration.
2. **`FEATURE_COUNT = 143` will grow to roughly 155-160 across the plan.**
   `NeuralNet::deserialize` supports both migrations already — new *inputs*
   start at **zero** so the widened net reproduces the narrow one exactly, new
   *actions* start at Xavier so they start neutral. So the existing model
   survives every phase mechanically. **It will not be good at any of them
   until it is retrained**, which is the whole point of the notification.

**What the AI session should do about it:**

- Do not start a long training run whose value depends on the current economy
  action space — Phase 1 alone re-prices it.
- Prefer work that survives a rule change: instrumentation, the bench harness,
  the entropy guard, head-to-head measurement, the robustness rule.
- Freeze and tag a reference model + build now, so post-change regressions can
  be attributed to the change rather than to the weights.
- Batch the feature-vector and action-space widening into as few migrations as
  possible.

---

### 11b. The tree has a concurrent editor — collision surface

The AI loop session (`opendoctrines-dd`) confirmed on 2026-09-04 that it holds
**large uncommitted, PENDING-COMMIT edits** in exactly the files phases 1-6
touch. Nothing of its work is committed, and the user's standing rule is that
nothing is committed or tagged without their word.

**Overlapping paths — do not `git add -A`, do not revert by path, stage
explicitly:**

```
  src/ai/AISystem.h        src/ai/AISystem.cpp      ← phases 1-6 (masks, actions, features)
  src/Game_TurnLogic.cpp   ← phases 1, 2, 4, 6 (income line, economy pass, resolveAssault)
  src/Game.h               ← phases 1-5 (new resolvers and state)
  src/Game_AITrain.cpp     src/server/ServerMain.cpp
  tools/od_bench.py        ← phase 0 instrumentation lands here
```

**What is in its uncommitted work that this plan must not tread on:**

- `TrainStats` gained per-kind diplomacy counters and a per-kind entropy guard;
  guard arrays are sized `MOD_COUNT + OFFER_KINDS` (`GUARD_HEADS == 11`, with a
  `static_assert`), **not** `MOD_COUNT + 1`.
- Trade and ceasefire **rules** now sit in `decideDiplomacy` ahead of the head —
  consistent with the established finding that the fixed ends of diplomacy are
  resolver rules and the head keeps the middle.
- Pact-breaking costs credibility in `declareWar` (pacts have teeth now).
- The scripted opening uses a bloc-strength test.
- A `--probe-trade` server flag exists.

**The frozen reference** (`build/loop/reference/`, 2026-09-04, HEAD `138fabe`):
champion `i10-final` and `N12` with sha256s, the shipped loop-base, the v7
server binary, the bench JSON, and `v7-source.diff` (2,694 lines) with a README
of the numbers. On the v7 ruler, 3 seeds:

```
    shipped (loop-base)      74 / 64 /  3
    champion (i10-final)    163 / 89 / 36
    N12 (2 maps vs-script)  169 / 83 / 44
```

**This is the baseline any post-change regression is measured against**, and it
is a directory rather than a tag precisely because nothing may be tagged without
the user's word. Phase 0's "freeze a reference" item is therefore **already
done** — do not redo it, and do not commit or tag it.

**Re-baselining:** the loop session will produce a v8 ruler **once**, after the
first phase lands — not per phase. So phases should land in batches that are
worth a re-baseline, and the loop must be told which phase lands first.

---

## 12. Cross-cutting engineering

**Saves.** Phases 1-5 add persisted state: province output assignment, national
goods stockpiles, capitalist treasuries, economic system position. `SaveManager`
uses a per-province changed-field **bitmask** that is nearly full — widen it or
add a second word, once, at Phase 2. Old saves must load with defaults.

**Multiplayer.** New player orders (assign output, set tax, lend, nationalise,
release) travel as JSON and are **whitelisted host-side** —
`Game_Multiplayer.cpp:2708` validates `type` against
`{industry, fortification, port}` and drops anything else. Every new order needs
its own validation there, and the validation must be a *rule*, not a UI
assumption: a modified client will send `targetLevel: 999`. That check already
exists for upgrades and is the pattern to copy. When in doubt, fix the cause
rather than widening the window.

**Determinism.** Non-negotiable — players notice and rely on it. The Phase 2
allocation pass iterates provinces per country per turn; any use of an unordered
container's iteration order in that pass is a divergence bug. Use the existing
ordered index (`provincesOf`) and add the Phase 0 determinism test before
writing the pass, not after.

**Performance.** `processRebellions` was once 59 ms of a 133 ms turn. The goods
pass runs over every province of every country every turn and must not repeat
that. Budget it, and profile a 30-country self-play map before it ships.

**Bench interface.** The AI loop session writes the `od_bench.py` readouts and
asked for field names to reserve columns against. **The goods count is not
decided (§13.3, two vs four), so the schema must not encode it** — these are
keyed by good id, so the 2-good and 4-good worlds differ in rows, never in
columns, and the bench needs no edit if the count changes:

```
  per country, per turn, emitted by the Phase 2 production pass
    goods[<id>].produced        float   made this turn
    goods[<id>].consumed        float   eaten this turn (demand actually met)
    goods[<id>].demand          float   demand before supply was applied
    goods[<id>].stockpile       float   held at end of turn
    livingStandards             float   0..1+, supply/demand for the consumer good
    factoriesIdle  / factoriesTotal      int, provinces with industry producing nothing
```

Good ids are lowercase and stable: **4-good variant** `consumer`, `machinery`,
`fuel`, `munitions`; **2-good variant** `consumer`, `war_material`.

Two readouts arrive **at Phase 1, before any of the above**, and are worth
wiring early because they measure whether the AI's industry decisions survived
the capacity cap:

```
    industryCapacityUsed / industryCapacityTotal    int, summed over owned provinces
    industryOvercapProvinces                        int, grandfathered above cap
```

**Map editor.** Every new per-province field needs an editor control, or map
authors cannot author it.

**i18n.** New UI strings go through `T()` and into `data/lang/*.json` — 21
languages. Same rule as the tutorial: **do not translate until benched.**

---

## 13. Decisions — SETTLED 2026-09-04

The three questions that gated the work have been answered by the user. They are
recorded here rather than struck out, because the rest of the document is
written against them.

| # | Question | Decision |
|---|---|---|
| 1 | First batch | **Phases 1 + 2 together.** Capacity *and* the goods economy, in one batch, one AI re-baseline. |
| 2 | Goods count | **Four** — `consumer`, `machinery`, `fuel`, `munitions`. |
| 3 | Grandfathering | **Keep existing levels.** The cap binds new builds only; nobody's cities shrink because they updated. |

**What decision 1 means for the risk profile.** Phase 2 was flagged above as
"the phase that can break the game", and it is now in the first batch. Two
consequences, and both are now requirements rather than options:

- **The Phase 0 determinism test is no longer optional and no longer deferrable.**
  It must exist and pass before the Phase 2 production pass is written, not
  after. Players rely on determinism and notice when it breaks.
- **The feature flag (old question 5) is now the recommended default, not a
  suggestion.** Phase 2 ships behind a per-world toggle so the money economy
  stays playable and benchable through the transition, with the flag removed at
  Phase 4. A batch this size needs a rollback that is not a revert.

**What decision 3 means.** A grandfathered province is a state the rules cannot
themselves produce — built above its own cap. Every consumer of the cap must
therefore handle `level > capacity` without asserting, clamping, or "correcting"
it: the build gate refuses the *next* level, income is computed from the level
actually built, and the AI's `industryCap` must not read a grandfathered
province as a bug. `industryOvercapProvinces` in the bench schema exists to make
this population visible rather than silent.

---

## 14. Superseded — the questions as originally posed

1. **Scope.** Phases 1-6 is a multi-month programme. Phase 1 alone is a week and
   fixes Matt's actual complaint. **Which slice do you want?** The plan is built
   so that stopping after 1, after 2, or after 3 each leaves a coherent game.
2. **Grandfathering (Phase 1).** Do existing saves keep overbuilt provinces, or
   get corrected? Recommendation: keep. A player's cities should not shrink
   because they updated.
3. **How many goods.** Four is proposed. Two (consumer goods + war material) is
   defensible and much cheaper in UI, tutorial and translation. More than four
   is not recommended at any price.
4. **Combat.** Take the cheap pair (width + attrition) now and defer standing
   battles? Or commit to the full rewrite? The cheap pair does not block the
   full one later.
5. **Feature flag.** Should Phase 2 ship behind a per-world toggle, so the old
   money economy stays playable and benchable during the transition? This costs
   real complexity in every code path it touches, and buys a safe rollback and
   an honest A/B. Recommendation: yes, through Phase 2 and 3, removed at Phase 4.
6. **Should Kaiserin and Matt see this?** They asked for a direction and gave
   the best-argued version of it. This document is publishable to the Discord
   more or less as it stands, if you want to check the reading of their ideas
   before any of it is built.

---

## Phase 7 — A world of your own *(replayability)* — **BUILT 2026-09-05**

Asked for: *"the player will not get deterministic outcome from start ... for
testing, it would still not break it, because the AI still has to be
deterministic, but starting a new save should be non-deterministic, for game
replayability."*

### What was wrong

`src/Game_TurnLogic.cpp` held `static std::mt19937 g_simRng{1337};`, and
`seedSimRng()` was called from **exactly one file** — `Game_AITrain.cpp`, at
four sites. Nothing on the player's path ever reseeded it. So every new game
anybody started anywhere replayed the identical random stream: the same
rebellions in the same provinces on the same turns, forever.

The reason this was survivable for so long is the reason it was easy to fix:
the AI harnesses *already* pin their own seed on the way in. They never
depended on the 1337 default — they overwrote it. Which means the default was
load-bearing for nobody, and changing it touches no benchmark.

### The rule

`Game::chooseWorldSeed()`, called from both `startNewGame` and
`startNewGameWithName` (they are parallel copies, not wrappers — both needed
the call):

1. `OD_WORLD_SEED` if set and non-zero — a tester pins a world.
2. Otherwise a caller-set `m_worldSeed` if non-zero — harnesses pin themselves.
3. Otherwise `std::random_device` — a real new game.

`random_device` rather than the clock, because two worlds started in the same
second are two different worlds and `time()` has one-second resolution on some
builds. Never seeds on 0, so "unset" stays distinguishable from "seed zero".
The chosen seed is written to the load log, so any world a player reports can
be reproduced from the number in their log.

`runHeadlessSimulation` sets `m_worldSeed = 1337` before starting, so
`--simulate` timing runs stay comparable across builds.

### Saves carry the stream, not the seed

`saveStateJson`/`loadStateJson` persist `worldSeed` **and** `simRng` — the
serialised mt19937 state, not just the seed. Saving the seed alone would rewind
the world's luck to turn one on every reload. `setSimRngState` parses into a
spare generator and adopts it only if the parse succeeded: reading a truncated
state straight into `g_simRng` half-consumes it and leaves the world's
randomness at whatever position the failure stopped, which is neither the saved
stream nor a clean one. Older saves have no key and keep the seed they were
given.

The state costs ~6.8 KB of text. Against a 23 KB `state.json` that reads
alarming, but `state.json` is one entry in an 831 KB archive and the
multiplayer snapshot that also carries it is built **once per join**, not per
turn. Under 1% of what is actually written. Measured before deciding, because
the per-turn save cost is the thing that caused the 1.1.2a freeze and is not a
budget to spend blind.

### Measured

| Check | Result |
|---|---|
| `--eval-ai 1 40 4242 2 --scenarios`, three runs | **byte-identical** |
| `--check --map 1939`, three runs | seeds 726247859 / 1363116722 / 4065677663 |
| `OD_WORLD_SEED=777`, two runs | 777, 777 |

Both halves of the ask, separately: same seed reproduces exactly, fresh worlds
differ, and a tester can still nail one down.

### A note on measuring during a shared tree

The 60-turn eval read 66.0% survival before this work and 69.8% after — and
**none of that is this change**. `chooseWorldSeed` is not called on the eval
path at all. The shift is the AI session's v15 (`losingGround`, `AISystem.cpp`,
timestamped 04:47) landing in the shared tree and being picked up by the next
rebuild. Attribution was checked by file timestamp before reporting, not
assumed; the determinism evidence above is the claim that actually stands.

---

## Phase 8 — Land war, further *(idea F, second pass)* — **PLAN, 2026-09-05**

Asked for: *"We also need to modify land war even more, put a plan for it."*

Phase 6 shipped combat width and stopped there deliberately. This is the plan
for what comes after it, written against the resolver as it stands today rather
than against the sketch in Phase 6's "original plan" section, which was written
before any of it existed.

### What is actually shallow, in the code

Four facts, all checkable in `Game::resolveAssault` and `GameStructs.h`:

1. **An army is a single integer.** `struct ArmyUnit { int countryId; int count; }`
   — that is the whole type. There is no infantry, no armour, no artillery *in
   the army*; artillery is a separate order system fired province-to-province.
   This one fact is most of why the game reads as Victoria rather than Hearts of
   Iron, and it is the most expensive thing on this page to change. It gets its
   own stage and its own decision.
2. **A battle takes no time.** `resolveAssault` compares `atkPower > defPower`
   once and returns. A province is never contested at the end of a turn, so
   there is nothing to reinforce, nothing to withdraw from, and no decision to
   make inside a war — only a sequence of decisions before one.
3. **There is no middle outcome.** Win and the *entire* garrison is set to zero.
   Lose and every engaged man dies. A fight decided by one soldier looks exactly
   like a fight decided by ten million.
4. **Depth is free.** Nothing in the resolver knows how far the attacker is from
   home. A stack twenty provinces deep fights exactly as well as one defending
   its own capital.

There is also **no terrain in the map data at all** — `Province` has no terrain,
elevation or river field, and neither does the `.odmap` format. Fortification
and area are the only ground the resolver knows about.

### The constraint, still accepted

Matt's read stands: provinces are too big for tactical manoeuvre, and rewriting
the map to fix that is not on the table. Nothing below adds a sub-province
layer, fronts, or unit facing. The depth comes from **duration, supply and
uncertainty** — things that make a province-scale war interesting without
pretending it is a tactical one.

### One thing changed underneath this, and it matters

Phase 7 gave new games a real seed. Until yesterday, adding *any* randomness to
combat would have been pointless: every campaign drew the same stream, so the
same battles would have gone the same way in every game anybody played. Combat
variance only became a meaningful design tool this week. Stage 3 depends on it.

---

### Stage 1 — Battles that last more than a turn

The single largest improvement available, and the one the community complaint is
really about.

An assault that neither carries nor is annihilated becomes a **standing battle**
held in the province: `m_battles`, keyed by province id, holding the attacker,
the men each side has committed, a round count, and a morale value per side.
Each turn, each battle resolves one round at the Phase 6 frontage; both sides may
**reinforce** it with an ordinary move order or **withdraw**. It ends when one
side's morale breaks, when a side withdraws, or when one is destroyed.

Why this is the right first move:

- It is the only change here that creates *decisions inside a war*. Everything
  else adjusts the price of decisions already being made.
- It makes forts and width mean something over time instead of once.
- It gives Phase 9's middle-state view (below) its most interesting content: a
  battle in progress is, precisely, the middle of the game.

Costs and risks, honestly:

- **It rewrites the war head's world model.** The AI currently sees "assault or
  do not"; it would now see "assault, reinforce, withdraw, hold". That is a new
  action space, and per the mask/executor parity rule a head cannot be judged on
  actions it was never trained to have. **This stage requires a retrain, not a
  re-bench**, and must be planned with the AI session rather than dropped on it.
- Battles are new state: they go in `state.json`, they must survive a reload,
  and they must be deterministic given the seed.
- A battle nobody reinforces must be guaranteed to terminate. Round casualties
  are floored so a battle cannot stall at one man a turn.

### Stage 2 — Supply, and what encirclement can mean without tactics

Give each province a supply distance: a breadth-first walk over
`m_provinceNeighbors` through land the country or its allies hold, from the
nearest **supply source** (a port of level ≥ 1, or the country's highest-industry
province as a stand-in for a capital, since the map data has no capital field).

- Beyond a doctrine-set range, effective power falls with distance.
- Fuel consumption scales with it — `ARMY_FUEL_PER_10K` already exists and
  Phase 2 already makes fuel a real commodity with a shortfall price.
- A province with **no land route at all** to a supply source is cut off, and
  suffers badly.

That last line is the whole point. It is what makes manoeuvre matter at province
scale: cutting a corridor becomes a real operation with a real payoff, without a
single sub-province mechanic. It also gives defence in depth a reason to exist
that is not merely stacking forts.

Depends on Stage 1 only loosely; could be done first if Stage 1 slips.

### Stage 3 — A middle outcome, and honest uncertainty

Replace the binary with a round result: both sides take casualties proportional
to the opposing power at the frontage, and a **morale check** decides who holds
the ground. A close fight then ends with two damaged armies and a province that
changed hands narrowly, which is what a close fight should look like.

Then add a bounded variance draw from `g_simRng` — small enough that a decisive
advantage stays decisive, large enough that a marginal attack is a gamble rather
than a solved calculation. Today combat is a *deterministic threshold*, which
means a player who does the arithmetic never loses a battle they chose to fight.

Sequenced third because it is only meaningful once battles have rounds (Stage 1)
and once the world seed makes variance non-repeating (Phase 7).

### Stage 4 — Army composition — **DECISION REQUIRED, not scheduled**

Making `ArmyUnit` more than `{countryId, count}` is the change that would most
directly answer "it reads as Victoria". It is also the most expensive change in
this entire document, and it should not be started casually:

- the save format's `ProvinceDelta` army encoding,
- the AI feature vector (`FEATURE_COUNT`) and every trained model,
- the army panel, the move UI, the split arithmetic,
- the multiplayer delta and every host/client version check,
- and a retrain from scratch, since army state is an input to every head.

**Recommendation: do not schedule this until Stages 1–3 are shipped and
measured.** They may well deliver the depth being asked for at a fraction of the
cost, and if they do not, this becomes a much better-informed decision. Raise it
with the community first — it is the kind of change worth asking about before
building.

### Terrain — a separate decision, flagged not planned

There is no terrain field anywhere. Adding one is not a resolver change, it is a
**map format change**: `.odmap` gains a field, every shipped map needs values,
and the Dragoman translator's conformance check has to be re-run against the new
format. Deriving a proxy from area and coastline instead would be cheap and
mostly fictional. Recommendation: leave it out of this phase, and decide it as a
map-format question if Stages 1–3 leave the ground feeling too uniform.

### Order, and the recommendation

**Stage 2, then 1, then 3.** Not the order of value — Stage 1 is the most
valuable — but the order of risk. Stage 2 touches no AI action space and can be
built, measured and shipped while the AI session is mid-run; Stage 1 cannot, and
should be started only when a retrain can be scheduled alongside it. Stage 3 is
cheap once 1 exists.

### How this gets measured, and what will not measure it

Phase 6 is the cautionary tale here and its lesson applies directly: combat
width looked like a wash on three-seed survival and turned out to be the largest
single gain the project has recorded, because survival describes a world
grinding along while the seat bench asks how well one country is *played*.

So, for every stage:

- **Seat bench, three seeds minimum, and prefer the change that wins on 5 of 5
  worlds** over the one with the better average on a favourable seed.
- **Both arms under one binary, in one session.** A game rule moves the
  baseline; a number from before the change is not a comparison.
- **Aggregate survival/concentration reported but not used to decide**, for the
  reason above.
- Determinism check must pass in both economies, and the new state must survive
  a save/reload round trip.
- Stage 1's retrain means its verdict comes from the AI session's gate, not from
  a bench against a frozen model that never had the actions.

**What cannot be measured this way:** anything whose effect is on the *human*
experience of a war — duration, tension, whether a battle feels like a decision.
The bench cannot see it and will not reward it. Those parts get judged by playing
them and by asking the players who raised the complaint.

### Explicitly not doing

Sub-province manoeuvre. Fronts. Unit facing or flanking. A tactical layer.
Rewriting the map to make provinces smaller. All of these were correctly
identified by Matt as "completely rewriting the rules", and that judgment has
not changed.

---

## Phase 9 — Seeing the turn *(ship routes, middle state)* — **BUILT 2026-09-05**

Asked for: *"we should make it so we would have a visualization of how our ships
move"* and *"a feature that would show a middle state of the game ... what orders
were given by countries, like opponent artillery, where the ship is going this
turn, the side buttons apart from search settings and claims should not exist
there ... a toggle ... also integrated in a client in multiplayer."*

### The overlay was drawing a path no ship ever sails

Pending ship moves were drawn as a **straight line from the hull to the
destination**. Ships do not sail straight lines — `navRoute` takes them around
land — so the one thing the overlay depicted was the one thing that never
happened. A player watching a boat leave that line had no way to distinguish a
working voyage from a broken one, which is part of why the pathfinding was
reported as bugged.

`Game::drawShipRoutePath` now draws the route the resolver will actually walk:

- the part **this turn's range** reaches, solid and thick;
- the rest of the voyage, faint;
- a dot where the hull will actually stop this turn, and a ring at the
  destination — so "arrives next turn" and "arrives in six" stop looking
  identical;
- an **X, in red, for a destination the router cannot reach at all**. "There is
  no sea route to here" is a different answer from a long voyage, not a lesser
  one, and hiding it was most of the confusion.

Two things that had to be got right. A freshly-issued order has an **empty
route** — the resolver plans it during the turn — so between issuing an order
and pressing the turn button there was nothing to draw, which is exactly when a
player most wants to see it; `shipDisplayRoute` previews it with `navRoute` and
caches it on what it was computed for. And the polyline is built in **continuous
world pixels** with `lonDelta`, taking the camera's wrap offset once from the
hull, because `worldToScreen` picks the nearest map copy *per point* — correct
for a province marker, and for a line it would tear a voyage across the
antimeridian into a stripe across the world.

### The middle state is a record, not a peek

The obvious reading — overlay everyone's pending orders on the live map — **cannot
be built**, and finding out why settled the design:

- an AI country decides inside `processCountryTurn`, not before it;
- `processArtilleryOrders` erases each shot as it fires it.

So while a player is looking at the map between turns, the order queues hold
that player's own orders and **nothing else**. There are no enemy orders in
existence to draw.

Orders are therefore recorded as they are about to resolve — `recordTurnOrders`,
called after a country has decided and before anything it decided is consumed,
the only moment a country's whole turn is on the table — and the view shows the
turn that just happened. That is the only implementable version, and it is also
the only one that is **safe in multiplayer**: it reveals only what the turn's
results already reveal. A live overlay of enemy intentions would decide games.

The view draws artillery (barbed arrows), army movements (plain arrows) and ship
voyages (the recorded route), each in a lifted version of the owning country's
colour. Politics, Economy and Research leave the sidebar entirely — they are
places you go to *change* something and this view is for reading — while
**Claims, Find country and Settings stay**, as asked. The survivors are packed
from the top so the column has no holes, and Settings follows them up.

**Claims was got wrong first time and had to be fixed.** The request named three
things to keep — search, settings and claims — and Claims is one of the four
SIDEBAR buttons, not something beside the search; the first implementation hid
all four. Caught by re-reading the original request against the code rather than
against my own summary of it, which is the only reason it was caught at all.

### Measured

| | |
|---|---|
| orders captured per turn (12-turn eval) | 300–418 across all countries |
| of which army moves / voyages / artillery | ~306 / ~78 / 0–3 |
| voyages recorded **with** a route, before the fix | 45 of 78 (**42% had none**) |
| after planning unrouted voyages at capture | **78 of 78** |
| wire round-trip, 596 samples | 0 parse failures, 0 mismatches |
| hostile payloads (truncated, trailing byte, absurd count), 473 samples | all refused, log never clobbered |
| largest payload | 31 KB for 362 marks |
| eval, 60 turns, seed 4242 | 71.7% / 49.34 — unchanged to the digit |
| `tests/ship_route_test.cpp` | 16 checks, 0 failed |

That 42% mattered: capture happens *before* `processNavyMovement` plans routes,
so without filling them in, nearly half of all voyages would have fallen back to
drawing a straight line to the destination — reintroducing, for every other
country, the exact bug just removed for the player's own ships.

### Multiplayer

A client never runs another country's turn, so it cannot record anything; the
host sends it. `NetMsg::TurnOrders` (id 80) carries an opaque payload written by
`Game::mpSerializeTurnOrders`, exactly as `NetOrdersMsg` carries a player's
orders, broadcast straight after the turn's Delta.

**Additive by construction.** Session's message switch ends in
`default: // Unknown, or a server->client message this build predates. return;`
— so a client older than this ignores it and simply has no overlay. That is why
it is a new id rather than a wider `Delta`: widening `Delta` would have changed
a payload older clients already parse. Coordinates go as fixed-point hundredths
of a degree rather than doubles, since routes are most of the payload and no
overlay needs a hull placed to a millionth of a degree.

It is deliberately **not** in `saveStateJson`. The log is a few hundred entries a
turn with a route on every voyage, and `state.json` is rewritten into the archive
every single turn — that write is what caused the 1.1.2a freeze, and this would
have been several times the size of the thing that caused it, for an overlay.

### The one part of the drawing that IS pinned

`tests/ship_route_test.cpp` covers the antimeridian polyline arithmetic — the
piece most likely to be wrong and least likely to be noticed, since it only
misbehaves on voyages across the dateline and shows up as a stripe across the
world rather than as a crash. It asserts that no segment of a crossing voyage
jumps more than half the map, that continuous x is allowed to run off the map
(clamping it is what tears the line), that eastward and westward crossings both
work, and that four eastward legs total 359 degrees rather than −1. Wired into
`tests/run_all.sh`.

### Not yet verified

**The rest of the drawing is unverified.** The windowed binary stalls
inside raylib initialisation when the machine's display is asleep — it never
reaches the window — so no screenshot, tutorial walk or interactive check could
be run. The data underneath it is verified (the table above); the drawing is
not. It needs one pass with the screen awake: issue a ship order and check the
route, toggle the view and check the sidebar.

---

## The mobilisation doctrines were never in the game — **FOUND AND FIXED 2026-09-05**

Worth its own section, because it invalidated a day of AI measurement and
because the shape of the mistake will recur.

The mobilisation levers built earlier today — army upkeep and recruitment cost
on 14 doctrines, so that how a country raises its army changes what the army
costs — were written into `data/policies.json`. **That is not what the game
reads.** Every `.odmap` embeds its own copy of `policies.json`, and the embedded
copy wins. So the feature was in the repository, in the changelog, and in no
game anybody could play.

It was hiding behind two things at once:

1. **`data/policies.json` is generated** by `tools/gen_policies.py`, and the
   levers had been added by hand-editing its output. So the file had drifted
   from its generator, and the next person to run the generator would have
   silently deleted the feature — which is exactly what happened the first time
   I ran it while investigating.
2. `tools/check_policies.py` catches all of this, reports "N map(s) in sync",
   and is wired into `tests/run_all.sh`. **It had been failing the whole time**,
   and I had not run the suite because the suite calls the determinism check,
   which needs a window, which hangs on a sleeping display. One broken check
   hid another.

Fixed at the source: the levers now live in `gen_policies.py`, `policies.json`
is regenerated from it, and all seven shipped maps are re-embedded. The
generator also now prints `maintenanceCostPct` as "Army upkeep" everywhere — the
same lever had been printing as "Upkeep" on 4 doctrines and "Army upkeep" on 12,
which is the same drift in miniature.

### What it cost, and the measurement lesson

Re-embedding the maps, on one binary, seed 4242, 60 turns:

| | before | after |
|---|---|---|
| survival | 71.7% | **62.3%** |
| rebellions / 1k | 49.34 | 48.19 |

A **9.4-point survival swing from map data alone** — larger than the bankruptcy
ramp, and in the opposite direction. Which is what should happen: 14 doctrines
began charging an army upkeep they had always advertised, so armies cost what
they were meant to and more governments fall over. Proof it was the maps and not
code: the embedded `policies.json` pulled out of `git show HEAD:...1939.odmap`
has the same 45 doctrine ids, none added, none removed, and exactly the 14
differing in levers.

**The rule this leaves behind.** After any doctrine change, run all three:

```
python3 tools/gen_policies.py
python3 tools/update_odmap_member.py policies.json data/policies.json data/STDmaps/*.odmap
python3 tools/check_policies.py
```

And a rule about the rules: a check that is failing tells you nothing once you
have stopped reading it. The determinism check cannot run without a display, so
the whole suite stopped being run, so a second failure sat unnoticed for a day.
A check that cannot run in the environment it is used in should be skippable by
name, not by abandoning the suite.

---

## Multi-country war declarations *(a doctrine)* — **CODE BUILT, DOCTRINE HELD 2026-09-05**

Asked for: *"a doctrine that allows the player to declare war to multiple
countries, like for example, 3 countries."*

### The limit was broader than its own reasoning

`queueDiplomaticAction` refused a second declaration through
`hasPendingDeclaration(sourceIso)` — scoped to the SOURCE, so it blocked a
declaration against a *different* country. But the long note written beside it
justifies something narrower: a country queueing **the same war three times**,
and an alliance answered in the same pass as the declaration that follows it,
leaving a pair "both allied and at war". Every one of those is about ONE PAIR,
and every one is already prevented by `hasPendingDiplomacy(source, target)` on
the line immediately above. The per-turn cap was doing work nobody had argued
for.

So the pair rule stays exactly as it is, and the count becomes a doctrine's to
change: `warDeclarationLimit(cid)` is `WAR_DECLARATIONS_BASE (1)` plus the
`warDeclarations` lever, floored at 1 — a doctrine that removed the last
declaration would leave a country unable to go to war at all, which is a broken
state rather than a trade-off.

**A lever, not a special case**, so the AI, the multiplayer host and the panel
all reach it through `getTotalEffect` like every other doctrine effect. And the
panel now *asks* that function rather than restating the limit — restating it is
the original bug, which is how a cap written in the renderer came to bind the
player while the AI queued whatever it liked.

Verified behaviour-neutral with no doctrine granting the lever: 62.3% / 48.19 /
194.40, identical to the digit.

### Landed 2026-09-05

`war_on_several_fronts` — "War on Several Fronts", authoritarian, 16/turn over 6
implementation turns. `warDeclarations: +2` (three a turn against a base of
one), paid for with `maintenanceCostPct: -20` (a fifth more army upkeep),
`armyDefPct: -8` and +2% unrest; incompatible with demobilisation and
consumer_economy. It buys the ability to open several wars at once and makes the
army that has to fight them more expensive and less able to hold ground, which
is the trade the ability should carry.

Priced as **per-turn upkeep, not a lump sum**, deliberately: a lump-sum gate on
an expensive action is a prohibition rather than a price for an AI whose treasury
runs near zero. At 16/turn it sits with `war_economy_total` (18) and
`universal_healthcare` (16) at the top of the table — costly, not unreachable.
No AI enacted it in a 60-turn eval, which is why seed 4242 reads identical to the
digit before and after.

### The old note on why it was held

Adding it means regenerating `policies.json` and re-embedding seven maps — the
change that just invalidated a full AI gate. It lands once, after the AI
session's current table, so there is one re-gate and not two.

Also unresolved, and a question for the player rather than for me: **the
one-war-a-turn rule may not deserve to exist at all.** The rationale never
supported it. Making it a doctrine is the conservative answer — it keeps the
default and sells the exception — but "declare war on as many countries as you
like, and suffer the diplomatic consequences" is a defensible default too.

---

## Phase 8, corrected — what the battle trace actually shows *(2026-09-05)*

The diagnosis above ("there is no middle outcome") was written from reading
`resolveAssault`. A real trace of a real war says something sharper, and it
changes the recommendation. Source: the AI session's `[BATTLE]` trace,
`build/loop/nor-v17.log`, Sweden invading Norway, province 824 fought four times:

| attackers | engaged | defenders | width | atkPower | defPower | result |
|---:|---:|---:|---:|---:|---:|---|
| 174,800 | 35,631 | 93,048 | 35,631 | 35,631 | 37,769 | repulsed |
| 119,537 | 35,631 | 84,909 | 35,631 | 35,631 | 37,769 | repulsed |
| 87,293 | 35,631 | 66,067 | 35,631 | 35,631 | 37,769 | repulsed |
| 90,513 | 35,631 | 32,453 | 35,631 | 35,631 | 34,400 | **carried** |

### The finding

`atkPower` is **35,631 in all four rows**, against attacking stacks of 174,800
down to 87,293. `defPower` is **37,769 in three of them**, against defending
stacks of 93,048 down to 66,067. Both sides are capped at the frontage, so once
both exceed it **the outcome stops depending on how many men either side has at
all**. The comparison reduces to the modifiers — here the defender's +6% — and
therefore gives the identical answer every turn, for ever, until one side's
stack falls below the frontage. Row four is exactly that: 32,453 defenders,
below the 35,631 frontage, `defPower` finally drops, and the province falls.

**This is a Phase 6 consequence and it overshoots what Phase 6 intended.** The
goal was that a ten-million-man stack should stop automatically beating a
well-sited small one. What was actually built makes army size irrelevant in
*both* directions above the frontage: 174,800 men accomplish precisely what
40,000 would, and the defender's 93,048 defend exactly as well as 35,632 would.

### What this corrects

**"There is no middle outcome" was the wrong complaint.** There is attrition,
and it works: each repulse kills defenders (the last exchange, 66,067 → 32,453,
is 33,614 lost against the attacker's 35,631), and four assaults across three
turns grind a province down and take it. That is a multi-turn battle, arrived at
by accident through repeated single-turn assaults — which is most of what Stage
1 was proposed to build.

So **the missing verb is reinforce-and-withdraw, not partial casualties.** The
grinding already happens; what a player cannot do is feed a battle deliberately,
pull out of one going badly, or hold a line rather than re-issuing an assault
every turn. The AI session reached the same conclusion from the same numbers
independently.

### The revised recommendation

1. **Fix the width cap so numbers matter again.** Troops beyond the frontage
   should not be inert — the natural reading is that they are the reserve that
   replaces losses *within* a fight, so a deeper stack sustains the frontage
   longer rather than fighting wider. This is a small change to `resolveAssault`
   and it restores the thing the trace shows is missing, without undoing what
   width was for: the frontage still caps how much force lands at once.
2. **Then Stage 1 (reinforce / withdraw),** which turns the accidental
   four-turn grind into something a player makes decisions inside.
3. **Stage 2 (supply)** unchanged.
4. **Stage 3 (partial outcomes and variance) is demoted** and may not be needed.
   The attrition is real; the determinism is a smaller problem than the size
   irrelevance above it. Revisit after 1 and 2.

### And a note on the AI's blindness to this

The AI's attack gate scores an assault as attackers-over-defenders **without the
width cap**, so it reads 174,800 against 93,048 as a comfortable attack. Under
the resolver it is a guaranteed loss of exactly `width` men — and, because the
comparison is deterministic, a guaranteed loss *repeated identically every turn*
until the arithmetic changes. That is being fixed AI-side (a width-aware gate,
mirroring the resolver's own comparison). Worth recording that **the rule and
the AI's model of the rule disagreed silently**, which is the same failure as a
rule with two homes — just with the second home in a different codebase.


---

## Pacts are dead paper once a war starts — **BUILT 2026-09-05**

Built. See the section below for what was found on opening it up; the original
note is kept underneath because the reasoning for holding it was sound and the
resolution is more interesting than the plan.

### What was actually missing

`requestAllyJoinWar()` already existed and already let a **player** call an ally
into a war in progress. Two things it did not do:

**A guarantee signed after the war started was dead paper.** Guarantees chain
inside `declareWar` and nowhere else, so a country that wins a guarantee on turn
three of a war it is losing gets nothing from it, for ever. A guarantor can now
be called exactly like an ally.

**Only the player could ask.** The function was hard-wired to
`m_playerCountryId`, so wartime diplomacy existed for precisely one country in
the game. It takes a caller now.

### Asked, not compelled — and why that is the right weaker form

A guarantee ordinarily *compels*: at the moment war is declared, guarantors join
automatically. The obvious symmetry would be to compel on signature too — sign a
guarantee for a country already at war and you are at war with its enemies.

That was rejected. Compelling on signature makes signing a guarantee a **hidden
declaration of war on everyone already fighting them**, and the scripted AI
accepts pact requests from anyone holding fewer than four pacts. Half the map
would have been dragged into wars it never chose, by a mechanism no player
could see. The ask is refusable, which is what makes a pact a decision rather
than a formality — the same reasoning that already separates alliances from
guarantees at declaration time.

### The measurement that justifies it

`callableFriends(cid)` enumerates who a country could usefully call — allies and
guarantors, not already in the war, not on cooldown, sorted so an AI picking
from it replays identically. It exists so a reflex picks from a list **the rule
produced** rather than re-deriving eligibility from `m_relations`, which would be
a second copy that drifts.

It also measures the gap, now printed as `[PACTS]`:

| seed | country-turns with a callable friend | calls actually issued |
|---|---|---|
| 4242 | **508** | 3 |
| 777 | **441** | 7 |
| 31337 | **513** | — |

**Roughly five hundred opportunities a world, taken three to seven times.** Not
because countries refused — because only the player could ask, and only through
a button. That is the same shape as every other finding today: a rule that
existed, fired almost never, and could not be told apart from a rule that was
not there without counting.

The AI half is the AI session's, and it now has a list to pick from and a call
to make.

### Still missing, and out of scope here

**Multiplayer carries no diplomacy at all.** `mpSerializeOrders` has no
diplomatic actions in it, so a client's declarations, pacts and calls do not
reach the host. That is a much larger pre-existing gap than this task, and
widening this change to cover it would have meant designing the whole diplomatic
wire format on the way past. Recorded, not attempted.

---

## The original note *(kept for the reasoning)*

## Pacts are dead paper once a war starts *(found 2026-09-05)*

Found by the AI session while tracing why a besieged Norway never gets help.

`issueCallsToArms` is called **only inside `declareWar`**, and guarantees chain
only at the moment of declaration. So a country attacked on turn 1 that wins an
alliance or a guarantee on turn 3 gets **nobody**: the pact is dead paper for
that war. `m_callToArmsCooldown` already exists for repeat calls, which is the
tell that this was half-built.

The consequence is that wartime diplomacy is pointless at exactly the moment
diplomacy matters most. A small country being overrun has no diplomatic move at
all — and the AI's pact targets are its land neighbours only, so on the traced
map Norway's only possible ask was Sweden, the country invading it.

**Not scheduled, and not a small change.** If an ally can be called to an ongoing
war, the value of every pact changes at once, and this project's own measurements
say pact mechanics bite back: forcing pact signing cost 54 rating points, and
refusing a NAP turned out to be *correct play* because breaking one is free and
prevents nothing. Before making calls to arms reachable mid-war it is worth
settling what a defensive pact actually obliges, since today the answer is
"nothing enforceable". See [[pacts-have-no-teeth]] in the project notes.

The AI half — giving the politics head non-adjacent great powers as guarantor
targets while besieged — is a few lines once the rule exists, and is held.

---

## The AI's defences, and the ruler that argued against them *(2026-09-05)*

Not this document's work — it is the AI session's — but recorded here because the
world-level measurement came from this side and because the *reasoning* is the
part worth reusing.

**v19** is a width-aware attack gate plus a siege reflex: while an enemy stack
sits on a frontier, a country stops funding research (while a fort is owed) and
buys a fort on its worst under-defended frontier. Verified on the shipped world
eval, seed 4242, 60 turns, difficulty 2, scenarios:

| | survival | rebellions/1k | research/1k | largest |
|---|---|---|---|---|
| no reflex | 62.3% | 48.19 | 194.40 | — |
| research cut only | 66.0% | 48.01 | 186.70 | — |
| **both (v19)** | **69.8%** | **50.16** | **177.99** | 19.3% |

The research figure is the useful one: it is direct evidence the reflex fires and
does the specific thing it was built to do, rather than a number that merely
moved in a welcome direction. After a day in which two separate features turned
out never to have run at all, that distinction earns its keep.

### The ruler argued for removing it — twice

The fort half was nearly dropped. On the seat bench it costs 35 points on the
modern-China seat, and a first reading of the arms concluded it was "worse on
average". It was not: the deciding comparison was *both* (mean 211) against
*research-cut-only* (mean 206), and the 192 figure that looked damning was the
fort **without** the research cut — a different question, answering "is the fort
sufficient alone", not "is it harmful alongside".

The trade actually on the table: fort on buys **+5 mean seat rating and +3.8
world survival**, and costs **+2.2 rebellions per 1k and 35 points on China**.
Taken, deliberately, as that.

**This is the third time today the seat ruler has argued against a good change,
and always the same way.** A seat's score is its *share of the world*, so
anything that helps every country defend itself raises world survival and lowers
every seat's score, because the scripted countries keep their provinces too. It
happened to combat width (which looked like a wash and turned out to be the
largest single gain the project has recorded), to the bankruptcy ramp (survival
+3.8 to +5.7, ruler down across the board), and now to this.

The rule to carry forward: **when a change is meant to help everyone, the seat
ruler is the wrong instrument to retire it with.** Read the aggregate too, and
compare models to each other on the new binary rather than to their own old
numbers.

### And a note on labels

The first restored build did not reproduce the figures above — survival 67.9
rather than 69.8 — and since the simulation is deterministic on a fixed seed,
that meant different code, not drift. It was a one-line change made while
splitting the arms (research fund-up refused while *besieged at all* rather than
while *a fort is owed*), found in minutes once someone looked. The label was
withheld until the numbers reproduced exactly.

That is the same discipline the v16 gate lacked, and the reason it lacked it is
in this document too: a name attached to numbers from different code is how a
whole day of measurement ended up describing a game nobody would play.

---

## The guarantor bar — better play, a harsher world *(a decision, 2026-09-05)*

The AI session's v20 refuses an *unclaimed* war declaration when a guarantor of
the target holds more provinces than the declarer. It is `src/ai` only — checked,
it sits at the AI's war-choosing site and touches no resolver, so a human player
is not bound by it.

It is the largest single AI gain of the day: mean seat rating 211 → 228, and the
**shipped 1.1.2a model 131 → 206**. It comes from a real blunder — France
declares on Belgium at turn 1, Britain honours its guarantee, France loses 172
provinces — that the learned war chooser could not see, because it counts
guarantors without weighing them.

Measured on the world, one binary, knob on and off, three seeds:

| seed | survival on → off | concentration on → off | wars/1k on → off |
|---|---|---|---|
| 4242 | 67.9 → 69.8 | 0.085 → 0.082 | 49.70 → 59.47 |
| 777 | 69.8 → 71.7 | 0.082 → 0.079 | 54.18 → 46.23 |
| 31337 | 64.2 → 69.8 | 0.092 → 0.080 | 54.76 → 45.85 |

**"Fewer declarations" was the predicted effect and it is wrong on two seeds of
three** — wars per 1k go *up* with the bar on 777 and 31337. The bar does not
reduce war; it reduces **suicide**. An AI that does not destroy itself in a
turn-1 war is alive to declare more wars later.

What is consistent, 3 of 3 and in the same direction each time: **survival is
lower with the bar and concentration is higher.** The blunder being removed was
doing something for the world — it destroyed the aggressor. Without it,
aggressive powers survive their own aggression, keep expanding against
neighbours who have no guarantor, and consolidate.

### Why this one is different from the other three disagreements

Three times today the seat ruler argued against a change that was good (combat
width, the bankruptcy ramp, the siege fort), and each time the ruler was the
wrong instrument: a rule that helps every country lowers every seat's *share*.

**This is not that.** Both instruments are measuring true things and they point
opposite ways, because the change makes the AI better at winning — and better at
winning is not the same as a better world to play in. So it is a **design
decision, not a tuning decision**: does OpenDoctrines want a competent AI that
consolidates the map, or a blundering one that leaves more countries standing?

That belongs to the person whose game it is, alongside the model choice. Recorded
here rather than settled.

Caveats: one map (1914 scenarios), 60 turns, three seeds, difficulty 2.
Concentration moves are small in absolute terms; survival and the direction of
effect are what to rely on.

---

## Phase 8, built — depth *(2026-09-05)*

The first half of the land-war work, approved and landed.

### The rule

`Game::depthFactor(troops, width)` — men beyond the frontage are the **reserve**.
They cannot widen the fight, which is the whole point of a frontage, but they
can be rotated into it as the men in front fall, so a deeper stack fights at
greater effect. `1 + 0.20 per doubling above the frontage, capped at 1.5`,
applied to **both** sides.

It is an abstraction of a multi-round fight into the single comparison this
resolver makes, and it is stated as one rather than dressed up. The multi-turn
version that would not need it is Stage 1, still unbuilt.

The reported invasion, province 824, all four assaults:

| attackers | defenders | before | after |
|---:|---:|---|---|
| 174,800 | 93,048 | repulsed | **carried** |
| 119,537 | 84,909 | repulsed | **carried** |
| 87,293 | 66,067 | repulsed | **carried** |
| 90,513 | 32,453 | carried | carried |

And the margins are narrow now — 44,843 against 44,498 on the third — where they
used to be a fixed 35,631 against 37,769 that repeated identically every turn.

### The cap is the load-bearing parameter, and it was measured

Four settings, three seeds each, one binary, survival at 4242 / 777 / 31337:

| | | | |
|---|---|---|---|
| depth off | 67.9 | 69.8 | 64.2 |
| 0.25 per doubling, cap 2.0 | 67.9 | 67.9 | 66.0 |
| 0.20, cap 3.0 | 60.4 | 67.9 | 62.3 |
| 0.20, cap 2.0 | 60.4 | 67.9 | 62.3 |
| **0.20, cap 1.5** | **77.4** | **69.8** | **69.8** |

Caps of 2 and 3 are **worse than no depth at all on every seed**, and behave
identically to each other — which says the band between them almost never binds.
What they do is let a stack many times the frontage multiply its power and
steamroll narrow ground, the exact thing width exists to prevent. 1.5 is the only
setting that beats the baseline, and it beats it on rebellions 3 of 3 as well.

### Two things that bit

`atkPower` is now `engaged × atkMod × atkDepth`, so the carried branch has to
divide the survivor count by `atkDepth` as well — otherwise a deep stack **walks
out of a province with more men than the order sent in**. Divided by both, then
clamped to `attackers`, because two guards on creating soldiers out of arithmetic
is the right number.

And the test caught the author. `tests/combat_depth_test.cpp` was written
asserting "on open ground ten to one carries" — which is false at cap 1.5,
because both sides are far past the cap and a draw goes to the defender. That is
the original bug surviving in a corner. It is now asserted as fact, with the
reason, rather than quietly not tested: raising the cap to fix it costs 9–17
survival points, and real fights on the shipped maps run at two to six times the
frontage, which is the range where the rule works.

## Phase 8, built — supply and cut-off *(2026-09-05)*

Built, measured and in the tree. (It was briefly held in the scratchpad while
the AI session bisected a crash — which turned out to be a binary they had built
from my files mid-edit: it compiled and was incoherent. Worth recording as a
shared-tree rule: rebuild from a settled tree before bisecting your own diff.)

Supply is measured in **hops** over the province adjacency graph from the nearest
source, walking only ground the country or its allies hold. Sources are its ports
plus its largest industrial province — the `.odmap` has no capital field, and the
biggest factory town is the closest honest stand-in. Within 4 hops costs nothing;
beyond that, power falls 6% a hop to a floor of 0.55; and a province with **no
land route at all** is cut off at 0.45. Applied to the attacker once and to each
defending stack separately, because two allied garrisons on the same ground can
be supplied completely differently and one may be cut off while the other is at
home.

The cut-off case is the point. It makes **operational** manoeuvre matter at
province scale — severing a corridor becomes a real operation with a real payoff
— without a single sub-province mechanic. Matt's constraint was that provinces
are too big for *tactical* manoeuvre; they are exactly the right size for this.

### The amphibious problem, and a better answer than the one built

A landing has no land route home by definition, so the hop walk reads **every
amphibious assault as an encirclement**. That would have silently repriced the
amphibious doctrine, which was measured against a resolver with no supply term at
all.

The built version uses a flat `SUPPLY_BEACHHEAD = 0.85`: a landing is supplied
from the sea, harder than fighting at home and far better than being cut off,
detected by `fallbackPid < 0` — which *is* the amphibious path by construction,
the same fact that makes a failed landing drown.

**The AI session has suggested a better signal and it should be used instead**:
the embarkation resolver already knows which hulls carried the men and where they
sailed from, so "supplied from the sea *while a friendly hull is in the
destination's sea cell*" is available without the AI having to model anything. A
landing whose fleet has been sunk or has sailed away would then lose its supply,
which is right, and a flat constant cannot express. Unsettled until that is
built.


### The constant was chosen by firing rate, not by outcome

Built at four free hops, where it **fired but did not bite**: 3–4.5% of the sides
weighed were supplied below full and 1.4–1.9% were cut off. Survival moved
+1.9 / 0 / −1.9 across three seeds.

That spread is exactly what "the rule never fires" looks like **and** exactly
what "the rule fires and roughly cancels out" looks like. The aggregate cannot
separate them — which is why `[SUPPLY] checks / penalised / cutOff` now prints
beside `[WIDTH]`, and why it exists at all. Attrition was built, shipped inert,
and only caught because an eval came back byte-identical with it on and off; the
mobilisation doctrines were correct in the repo and absent from every map for a
day. Counting is the only thing that tells a rule from the absence of one.

At **two** free hops it penalises 20% and cuts off 2–3%, with survival roughly
flat (0 / −3.7 / +1.9). Shipped at two, and the reasoning is deliberately *not*
the aggregate: 20% is the same order as the frontage's 25% bind rate, and the
frontage is the rule this project has already learned is invisible in world
aggregates and decisive in the seat bench. **The firing rate chose the constant;
the AI seat gate settles whether the rule is good.**

| | four free hops | two free hops |
|---|---|---|
| sides penalised | 3–4.5% | **20%** |
| cut off | 1.4–1.9% | 2–3% |
| survival, 3 seeds | +1.9 / 0 / −1.9 | 0 / −3.7 / +1.9 |

### What supply changes about the amphibious doctrine

A landing now faces a defender whose supply may be 0.45 rather than 1.0, so
landing against a cut-off garrison is a considerably better bet than it was.
That is a real change to what the doctrine is worth, and it was not designed —
it falls out of the rule. Worth watching rather than assuming it is fine.


---

## The depth rule retired the AI's workaround *(2026-09-05)*

The AI session's width-aware attack gate refused assaults the resolver would
repulse. It was built for a real defect: an above-frontage repulse deleted every
engaged man, and numbers past the frontage bought nothing, so an attack at parity
was a coin flip that cost exactly `width` men. Refusing those attacks was correct
play.

`depthFactor` removed the defect. Five models, one binary, gate on against gate
off: **199/85/27 against 210/87/29** — better *without* the gate on four of five
models and on the shipped one (128 → 133, floor 1 → 18). It is now off by
default.

**A heuristic tuned against a broken rule encodes the brokenness, and it does not
fail loudly when the rule is fixed.** It goes on refusing choices that have become
good, and it still looks like caution. Nothing in the seat bench flags that; only
re-benching the knob does. This is the argument for AI-side reflexes being knobs
rather than code — the question "what was this compensating for, and is it still
true?" has to be answerable, and answerable by measurement.

The world numbers on the shipped configuration (v22 — depth, supply, gate off),
three seeds, 60 turns, difficulty 2:

| seed | survival | concentration | rebellions/1k | supply penalised / cut off |
|---|---|---|---|---|
| 4242 | 62.3% | 0.091 | 54.16 | 19.1% / 0.8% |
| 777 | 71.7% | 0.085 | 55.47 | 18.5% / 3.0% |
| 31337 | 67.9% | 0.086 | 88.04 | 19.6% / 1.8% |

---

## Phase 8, built — standing battles *(2026-09-05)*

The verb the game was missing. An assault that neither carries nor annihilates
no longer ends: the reserve stands in the province as a **battle**, fights a
round a turn, and the way out is a **withdrawal somebody has to order**. Retreat
used to be free and automatic; it is a decision now, and that is the feature.

### The shape

`struct Battle` holds the province, the attacker, the men committed, where they
came from, how many rounds it has run, and — for the panel and for the AI — the
two sides as the resolver last compared them plus each side's losses in the last
round.

**The attackers live in the battle record, not in `m_provinceArmies`.** A
province holds its owner's garrison and nobody else's; the eval asserts it and a
great deal of code walks province armies assuming it. Keeping committed men off
the map preserves that invariant exactly, and it held: `occupation 0 stack(s)
sharing a province` on every run including 120 turns.

Reinforcing needs no new order — an ordinary move into a province you are
already fighting for joins the battle rather than starting a second assault,
which would have weighed a second force against the frontage separately and been
a way of bringing more men to bear than the ground holds.

A **failed landing still drowns**: `fromProvince < 0` is the amphibious path by
construction, there is nowhere to stand and nowhere to withdraw to, and that
keeps the rule the amphibious doctrine was measured against.

`weighAssault()` was extracted so a fresh assault and a battle round make
**literally the same comparison** — frontage, fort, depth, supply, both sides'
research. One rule, one reader.

### The bug this created, and why it matters more than the feature

Men in a battle are off the map, so **every existing thing that counts a
country's soldiers stopped seeing them**. All four of them: fuel demand, the
munitions reserve, per-country upkeep, and the one-pass upkeep table. An army
parked in a standing battle ate no fuel, drew no munitions and cost no upkeep.

That is not a subtle bug. It is a strategy, and a trained model would have found
it. `Game::countryTroops()` now exists as the single reader and all four call it.
This is the same lesson as the build-cost tables, the income projection, army
upkeep and the seven copies of the artillery price list — except that here the
new hiding place was created by the feature itself, which is the version that is
hardest to notice.

### How often it fires, and the honest limit

| | 4242 | 777 | 31337 |
|---|---|---|---|
| assaults | 17,870 | 18,021 | 18,931 |
| **contested** | 483 | 586 | 562 |
| repulsed | 188 | 212 | 200 |
| battles started | 41 | 36 | 74 |
| rounds fought | 113 | 96 | 147 |
| won / lost | 13/15 | 9/17 | 14/42 |
| reinforced | 8 | 15 | 23 |

**97% of "assaults" are walk-ins into empty provinces** — a striking fact about
the game in its own right, and the reason the raw assault count is the wrong
denominator for everything.

A battle forms only where the attacker committed **more than the frontage**: a
sub-width force is annihilated entirely on a repulse, as it always was, so there
is no reserve left to stand. That is 18.6% of repulses. Extending battles to
every repulse means softening the "all engaged men die" rule — which is Stage 3,
demoted and not approved. So Stage 1 and Stage 3 turn out to be coupled after
all, and that is worth knowing before anyone assumes the demotion was free.

Frozen models lose more battles than they win (15/17/42 against 13/9/14) because
nothing in them knew to withdraw — the behaviour automatic fallback was hiding.
The AI session has since built a withdraw reflex on the record's own numbers
("losing, and not winning the exchange, for two rounds"), which took withdrawals
from 0 to 14 in a 20-turn smoke test.

`BATTLE_MAX_ROUNDS = 12` is a safety rail, not a rule anyone should meet; it
fired 0–2 times per world, because battles usually end by exhaustion first.

### Verified

Determinism identical 3/3 at seed 4242; 120 turns clean (86 battles, 281 rounds,
8 withdrawn); occupancy invariant 0 throughout; `tests/battle_rules_test.cpp`
17 checks; full suite green. Battles and pending withdrawals are saved in
`state.json` — a save that forgot them would **delete an army** — and withdrawals
travel to a multiplayer host as objects (the host's parser skips array entries
that are not objects, so bare integers would have been dropped in silence), where
they are authorised not by province ownership — a battle is fought over somebody
else's ground — but by the host confirming a battle of that country's actually
stands there.


---

## A finding that dissolved when both sides looked *(2026-09-05)*

Recorded because the process is the reusable part.

Supply's seat table showed a rush seat — the bench world where the seat is being
invaded — falling from 187 to 39. The obvious reading was that supply punishes
the invaded, and two mechanisms were proposed for it from the world side:

1. **A death spiral**: a country being carved up has its ground fragmented, its
   provinces severed from its own ports, and its garrisons fight cut off at 0.45
   on their own soil. **Disproved** — only 8–17% of cut-offs are defenders;
   83–92% are attackers, which is the rule working.
2. **An asymmetry by construction**: an invader striking just over its border is
   inside its two free hops, while a defender in its own hinterland is far from
   its ports and is docked at home. **Disproved** — defenders are penalised at
   12.8–19.1% against 18.7–20.9% for all sides together. Slightly *below*
   average.

The AI session then traced it from the seat side and found the same: France as
defender was at full supply in 147 of 186 weighed fights, and of the 123 assaults
that carried against her only 34 had her below 1.00 — where 0.92 does not lose a
province. No handful of large penalised stacks carrying it either.

**And then the finding itself dissolved.** The 187 → 39 figure was a three-seed
mean; the trace was one seed on a later build where the same seat reads healthy.
Part of the drop was seed spread, and part of it was very likely a frozen model
with **no way out of a losing battle** — which standing battles and a withdraw
reflex have since given it.

Nothing was changed on the strength of it, and the fix that was nearly built —
exempting a garrison at home from the distance penalty — turned out not to be
needed. Two disproved hypotheses and a retracted attribution is a better day's
work than a plausible fix to a problem that was not there.


---

## What a battle actually looks like from inside *(measured, 2026-09-05)*

Two withdraw reflexes were built on the battle record and both measured **worse
than never withdrawing** (one of five models up, then one of three). The
instinct was to blame the rules. The resolver is the better place to look, and
356 battle rounds across three seeds say something neither of us expected:

| | |
|---|---|
| `atkPower / defPower` | p10 **0.26** · p25 0.40 · **median 0.73** · p75 0.87 · p90 1.00 |
| within 20% of parity | 37% |
| badly losing (< 0.6) | **40%** |
| defenders killed per attacker lost | **0.74** |

**Battles are not close fights, and they are not even exchanges.** The median
round has the attacker at 73% of the defender's power, killing three men for
every four it loses. Two rounds in five are worse than 0.6. And grinding still
wins.

### Why, and what it means

Not the exchange rate — the **surplus**. A country that commits to a battle
usually brought more men than the ground holds, and an unfavourable rate still
wins the race given enough spare: 100k against 40k loses `width` a round and
kills 0.74 × `width`, and the defender runs out first regardless.

So **winning looks exactly like losing from inside, every round**. Both withdraw
rules were correctly detecting a losing position and quitting a fight that was
being won. There is no threshold on the round-by-round comparison that separates
them, which is why a third attempt was not made.

It also settles where the real decision is: **how many men to commit before the
first round.** On these numbers that is not merely a decision the AI can learn,
it is the *only* one that determines the outcome — everything after it is
arithmetic. Withdrawal remains a genuine tool for a **player**, who can see a
hopeless commitment and cut it; its value to an AI on this bench is a floor
effect at best.

### The open design question

The battle system rewards raw numbers **twice**: depth in the comparison, and
surplus in the attrition. If forts, supply and terrain are meant to let a
smaller defender hold ground, they currently delay the arithmetic rather than
change it.

That is a real question about what this system is for, and it is a design
decision rather than a defect — recorded here with the rest of the land-war
decisions rather than settled.

**And it has a seat that would answer it.** The AI session's table has modern
China as the one seat where a large defender with forts still collapses,
scoring anywhere from 32 to 305 across models — the widest spread on the board.
China is a big country with fortified ground and a lot of men, which is exactly
the position forts, supply and terrain are supposed to make defensible. If those
rules ever start changing the arithmetic rather than delaying it, China is the
seat where it will show first, and its spread narrowing is the measurement to
look for.


---

## The sea-cell fix — a landing is not an encirclement *(built, 2026-09-05)*

The last piece of supply, and it replaces an assertion with a question.

A landing has no land route home **by definition**, so the hop walk reads every
amphibious assault as encircled. The first version papered over that with a flat
`SUPPLY_BEACHHEAD` applied to anything arriving from the sea — which *asserted*
that a landing is supplied instead of asking. A constant cannot tell a fleet
riding offshore from a fleet that has been sunk, and being stranded on a hostile
shore is precisely what it should be able to say.

### The rule

`Game::seaSupplied(cid, pid)` — is a friendly or allied hull close enough to
supply this province? **The radius is `shipMaxRangePx`, the same distance a hull
may put men ashore over.** One rule, not two: a fleet that could land here can
supply here, and there is no second constant to tune or to drift.

So a province with no land route is `SUPPLY_BEACHHEAD` (0.85) while hulls are in
reach and `SUPPLY_CUTOFF` (0.45) when they are not. The landing assault itself
now takes the same path as everybody else rather than being handed an answer —
and gets the same result, because its own hull is by definition within range.
**The difference shows on the turns after the landing**, when the fleet may not
be.

Answered lazily and cached with the country's land map: only contested provinces
ever ask — about 500 in a 60-turn world, against ~4,000 provinces times every
hull afloat, which is the difference between a cheap question and a scan nobody
would ship.

### Measured

| seed | no land route | supplied by sea | cut off |
|---|---|---|---|
| 4242 | 277 | **87 (31%)** | 190 |
| 777 | 932 | **117 (13%)** | 815 |
| 31337 | 332 | **114 (34%)** | 218 |

Between one in eight and one in three of the forces with no road home are being
held up by a fleet. That is the distinction the constant could not make, and it
is now visible in `[SUPPLY]`.

### Why it was safe to land

`tests/supply_rules_test.cpp` (51 checks) pins the ordering property that made
this a change rather than a gamble: **sea supply only ever relaxes.** No
supplied position is worse off than it was under the constant, and a landing at
the moment it lands is unchanged *exactly*. The single deliberate exception is
the case the feature exists for — a landing with no fleet, which used to be
supplied and is now cut off.

---

# A decision, gathered: what is this game for?

Four rules landed on 2026-09-05 whose two measurements **disagreed in the same
direction every time**. Individually each looked like a metric problem. Together
they are one design question, and it belongs to the person whose game it is.

| rule | seat bench (how well one country is played) | world (how the map ends up) |
|---|---|---|
| Guarantor bar | mean 211 → **228**, shipped model 131 → 206 | survival **down** 3/3, concentration **up** 3/3 |
| Supply and cut-off | mean 210 → **218** | survival sold, invaded seats pay |
| Standing battles | frozen five 218 → **226**, shipped 131 → 161 | — |
| Call-to-arms reflex | (benching) | survival 64.2 → **62.3**, one seed |

The instruments are not in conflict and neither is broken. **A competent AI that
stops blundering also stops self-destructing** — and a power that survives its
own mistakes goes on to absorb the neighbours those mistakes used to save. Better
play and a more crowded map pull against each other.

So the question is not which number to trust. It is:

> **Should OpenDoctrines be a world where competent powers consolidate the map,
> or a messier one where blunders keep more countries on it?**

Some things worth weighing:

- The community complaint that started all of this was that **war was shallow**,
  not that the map was too empty. These rules make war less shallow.
- But "a handful of great powers by 1960" is a different game from "fifty
  countries still arguing", and the second is closer to what the shipped
  scenarios look like at turn 0.
- Every one of these is a **knob** — `OD_GUARANTOR_BAR`, `OD_CALL_REFLEX`,
  supply's constants — so this is reversible, and it can be split: keep the ones
  that make war interesting, drop the ones that only make the AI ruthless.
- It is four decisions pointing one way. Deciding it once, on purpose, is better
  than accumulating it.

Not a recommendation, deliberately. The measurements are here; the answer is a
statement about what the game is, and nobody measuring it can supply that.

---

## A class of bug, seen twice

Both times a learned head was **saturated** and returned the same number for
every input — the economy head with 8 of 12 actions at 0.0, and the
`call_to_arms` head returning 0.277 for Britain at 6.4M men and Russia at 7.9M
alike.

**The tell is not a wrong answer. It is the SAME answer.** A rule built on such a
head is not merely mis-tuned; it collapses to whatever else is in the formula —
`log1p(army) × 0.277` is `log1p(army)`, so an acceptance-weighted picker and a
strength picker were literally the same function, and produced byte-identical
worlds.

Which is also the diagnostic: **byte-identical output from a changed rule means
the rule is not running**, and that is the same signature that caught attrition
being shipped inert. When a new selection rule measures exactly the same as the
old one, check whether its inputs vary before concluding the change was
harmless.

---

# Phase 10 — An army made of something *(PLAN, 2026-09-05)*

Asked for: *"more detailed than just throwing men at a province"* — unit types,
a readable list of who is in a province, clear display on the map, commanding by
type and by province, per-type recruitment cost, better troops costing more
manpower, and a research tree and doctrine set to match.

This is Stage 4 from Phase 8, which was deliberately left unscheduled as the most
expensive change in this document. It is now scheduled. What follows is grounded
in what the code actually is, not in what a unit system usually looks like.

## The good news: this game already has typed units

`ARTY_COSTS[]` in `BuildCosts.h` is a table of eight ammunition types, each with
money, fuel and munitions costs, unlocked by research nodes carrying
`artilleryType`, read by the panel, the AI, the resolver and the refund path.
**One table, six callers** — and the comment above it records that this table was
consolidated precisely because its money cost had previously lived in six places
and adding two currencies to six copies "is not a risk of divergence, it is a
plan for it."

So the pattern for typed, researched, differently-priced military things is
already here, already proven, and already survived the failure mode that would
kill a second attempt. **Infantry types should copy it, not invent something.**

## What differentiates a type — and the rule that decides it

The temptation is rock-paper-scissors. The better answer is that **types should
modulate the terms `weighAssault` already computes**, because those terms are
built, measured, and instrumented:

| Axis | Plugs into | Why it matters here |
|---|---|---|
| **Frontage cost per man** | `combatWidth` / `depthFactor` | The one that changes everything. A type that uses the frontage efficiently is the counter to "bring more men". |
| **Attack vs defence** | `armyAtkPct` / `armyDefPct`, already per-side | Militia hold, assault troops take. Asymmetry without new maths. |
| **Supply appetite** | `supplyFactor`, `ARMY_FUEL_PER_10K` | Mechanised burns fuel and suffers when cut off; foot infantry barely notices. Makes supply matter *differently* per army rather than uniformly. |

No new combat formula. Three multipliers on terms that exist, all of which
already print counters.

### Why this is the fix for what was measured today

Today's instrumentation found that **numbers win twice**: `depthFactor` rewards a
deeper stack in the comparison, and the attrition race rewards surplus, so an
attacker at 0.73 power and a 0.74 exchange still wins by having brought more.
Forts, supply and terrain only *delay* that arithmetic.

Frontage-per-man and the manpower cost below are the first mechanisms proposed
all day that **change** it rather than delaying it. "Bring more men" starts
competing with "bring better men" for the same manpower, and a type that fights
efficiently at the frontage beats a bigger stack that does not. That is the
answer to "more than throwing men at a province", and it is also the answer to
the open design question two sections up.

## Manpower — the requested rule, and the reason it is the right one

Today recruitment is a **per-turn cap**: 20% of a province's population, scaled by
`conscriptionPct` research and by unrest, at $1 per 10,000 men. There is no pool
and no scarcity beyond the tick.

The request — *better troops mean less manpower available* — introduces the
scarcity the combat system has been missing. Recommended shape:

- A country-level **manpower pool** that regenerates from population, replacing
  the per-province per-turn cap as the binding constraint.
- Each type costs **manpower per man** as well as money and munitions: elite
  types draw several times the pool per soldier fielded.
- `conscriptionPct` research raises the pool; the mobilisation doctrines built
  earlier today already move recruitment and upkeep costs and slot straight in.

The effect is a real curve: a mass army of cheap troops, or a small good one, and
the doctrines already decide which a country is built for.

## The interface, which is half the request

- **A scrollable list per province** of what is stationed there, by type and
  count, replacing the single number. The province panel already has the space
  where the garrison total sits today.
- **Command by type and by province.** The existing move order is
  `{from, to, pct}`; it gains an optional type filter. "Move all" stays one
  click; "move the armour" becomes possible. The split arithmetic already proved
  fragile once (the `<1` marker), so `tests/army_split_test.cpp` extends to
  per-type shares.
- **On the map**, the shield marker gains composition — the honest options are a
  stacked bar, or the dominant type's icon with a count. This wants a look before
  it is built, and it is exactly the kind of thing to prototype in the browser
  preview rather than argue about.

## Research and doctrines

Unit types need unlocks, which is a research tree change rather than an addition:
`ResearchNode` gains `troopType` exactly as it carries `artilleryType`, and the
army branch is re-cut so the tree is *about* what your army is made of instead of
flat percentage buffs.

Doctrines follow the same route as the mobilisation levers — new lever names in
`gen_policies.py`, printed forms in `kTradeoffForms`, regenerated, **re-embedded
into all seven maps**. That last step is not optional and has already been
forgotten once today.

## What this costs, honestly

`ArmyUnit` is `{countryId, count}`. Every one of these has to learn a type:

- `SaveManager::ArmyDelta::Unit` — the save format, versioned;
- `state.json`, and the `Battle` record's committed men;
- the multiplayer wire, and every host-side order validator;
- **the AI's feature vector and every trained model** — a retrain from scratch,
  not a re-gate;
- the province panel, the map marker, the move UI, the recruit UI;
- `countryTroops()` and the four things that count soldiers.

## Sequencing — and the first stage is shippable alone

1. **The table and the types, with no UI.** `TROOP_TYPES[]` beside `ARTY_COSTS`,
   a type on `ArmyUnit` defaulting to the current one, save and wire carrying it,
   everything behaving exactly as today. Byte-identical eval is the acceptance
   test — the same guarantee used for the sea-supply change.
2. **Manpower.** The pool, per-type costs, the doctrine levers. Measurable on its
   own, and it is the piece that changes the strategy.
3. **The three combat axes.** Frontage, asymmetry, supply appetite. This is where
   the AI must retrain.
4. **The interface.** List, map, command-by-type.
5. **Research re-cut and new doctrines.**

Stage 1 is the risky one and the one worth doing carefully, because it is a save
format change that must load every existing save. Stages 2 and 3 are where the
game changes. Stage 4 is where the player finds out.

## Decisions needed before Stage 1

- **How many types, and what are they?** Recommendation: **four** — line
  infantry, militia/garrison, assault infantry, mechanised — chosen because each
  has a clearly different answer on all three axes and four is the largest number
  that stays readable on a map marker. More can be added; the table makes that
  cheap.
- **Does the manpower pool replace the per-turn recruitment cap or sit beside
  it?** Recommendation: replace. Two scarcity mechanisms doing the same job is
  how the artillery price ended up in six places.
- **Do existing saves upgrade, or is this a new-world feature?** Recommendation:
  upgrade — every existing soldier becomes line infantry. Anything else strands
  campaigns in progress.


---

## Phase 10, Stage 1 — an army made of something *(BUILT 2026-09-05)*

The foundation, built to be **provably invisible**.

### What is in

`TROOP_TYPES[]` in `BuildCosts.h`, beside `ARTY_COSTS` and shaped like it: four
types — line infantry, militia, assault infantry, mechanised — each with money,
munitions and **manpower** costs and three combat columns (`frontage`, `atk`/`def`,
`fuel`). Every column of `line` is exactly **1.0**, which is load-bearing: every
soldier in every world today is line infantry, so the multipliers can be wired in
later and change nothing until a second type exists.

`ArmyUnit` carries a `TroopType`. A province holds one entry per **(country,
type)** — `addTroopsTo` merges on both, because merging by country alone would
quietly turn militia into mechanised, which surfaces as a balance complaint
months later rather than as a crash.

### The save format, which is the part that could have gone badly

The army block in a turn delta is **fixed-width — u16 country, u32 count — with
no version field**, and it sits in the *middle* of the stream. One extra byte per
unit would shift every byte after it and break **every save ever written and
every client on an older build at once**.

`SaveManager.cpp` had already solved this exact problem for populations too large
for their field, and said so: written in the trailer, "a build that has never
heard of them stops reading at the end of the research block and still walks the
province stream at the right offsets." Troop types take the same route, under
trailer flag bit 2.

Three properties fall out, and `tests/troop_types_test.cpp` asserts all of them:

- **A world of only line infantry writes nothing at all**, so its saves are
  byte-for-byte what the previous build produced.
- **An old save loads as an army of line infantry** — which is exactly what it
  was.
- **A new save read by an old build yields the right totals**: it cannot tell the
  kinds apart, but it is never wrong about how many men are there. That is what
  lets a mixed save cross the multiplayer wire to a client that predates types.

### Verified

Seed 4242, 60 turns: battles `42/101/19/13/2/23` and survival 62.3% — **identical
to the run before the change**. Determinism 3/3. Full suite ALL PASSED, including
the existing `SaveDeltaTest` (28 checks) which exercises the format this touched.
`tests/troop_types_test.cpp` 14 checks.

Nothing is wired into recruitment or combat. No new type can be built. The AI's
world is unchanged and no retrain is needed yet.

### Next

**Stage 2 — manpower.** The pool, per-type costs, the doctrine levers. It is the
piece that makes the choice real, and it is measurable on its own.

**Stage 3 — the three combat axes**, which is where the AI must retrain and where
the ruler has to be held.


## Phase 10, Stage 2 — manpower *(BUILT 2026-09-05)*

### The plan said build a pool. Reading the code said don't.

Stage 2 was specified as "a country-level manpower pool that regenerates from
population, replacing the per-province per-turn cap". That was wrong, and the
code says so plainly: **recruitment already deducts the men from the province's
population, permanently, in the resolver.** There is a comment there recording
why — a hand bench once produced a 102-million-man army from eighty provinces
without the population moving at all — and there is already an
`availableManpower(provinceId)` accessor built on it.

So the scarcity the plan wanted to introduce already exists, is already
depleting, and is already enforced in the one place that binds the player, the
AI, multiplayer orders and the tutorial alike. Adding a national pool on top
would have been **a second scarcity mechanism doing the first one's job** — the
exact mistake the plan itself warned about two paragraphs earlier, in the
sentence about the artillery price living in six places.

### What was built instead, which is much smaller

**The type multiplies the population draw.** `count` stays the number of
*soldiers*; the province pays `count × TROOP_TYPES[type].manpower`. Ten thousand
mechanised take forty thousand people; ten thousand militia take six.

That is the whole of *"the better the troop, the less manpower would logically be
available"* — and the reading is better than a pool would have given: an elite
soldier is not merely expensive, he **represents** more of the population. The
training base, the crews and the mechanics behind him are people too.

Also wired: `recruitPrice` takes a type and multiplies money and munitions by it,
and `availableManpower` charges pending orders at their real cost, so an order for
mechanised has already claimed four times its own size. Short of people, an order
now **shrinks to what the population covers** rather than raising soldiers out of
nobody.

One incidental fix: `processRecruitments` had its own copy of the army-merge
`find_if`, matching on country alone. It now calls `addTroopsTo`, which knows a
province holds one stack per (country, **type**) — the copy would have merged
militia into mechanised the first time a second type existed.

### Verified — still provably inert

Line infantry is 1.0 in every column, so with no other type recruitable this
changes nothing, and it is measured rather than asserted: seed 4242 reproduces
Stage 1 **exactly** — survival 62.3%, battles `42/101/19/13/2/23`.

That took one careful step. The first reading appeared to have moved (64.2%,
battles 25/67/…) and it was **the AI session flipping its call reflex to off by
default** between the two runs, not this change. Checked with the knob rather
than assumed — the fourth time today the shared tree moved under a measurement.

Suite ALL PASSED; `tests/troop_types_test.cpp` now 23 checks, covering the
population arithmetic and the short-of-people guard while they are still inert.

### What this means for the staging

Stages 2 and 3 are both **inert until a second type is recruitable**, which was
not the original plan's expectation. That is a better shape, not a worse one: all
the machinery gets built and proven with byte-identical evidence, and then a
single change — research unlocks plus a type picker — brings it alive in one
measured step, with one retrain and one held ruler.


## Phase 10, Stage 3 — the combat axes *(BUILT 2026-09-05)*

Both sides of every fight now carry a **composition** rather than a headcount,
and the three columns of `TROOP_TYPES` are wired into `weighAssault`.

### What changed

`ForceComposition` — a fixed array of four counts, no allocation — replaces the
`int` that an assault used to be. It had to be **both** sides: typed defence
against untyped attack is not a rule, it is one half of every fight quietly
getting the good numbers.

- **Frontage** is consumed by men *weighted by their kind*, so the line holds
  more mechanised than militia. Depth is measured against how many men the line
  actually holds, not against the raw width.
- **Attack and defence** weight each side's men by their type before the
  comparison.
- **Supply** was already per-stack, so it needed nothing.

Casualties fall proportionally across the kinds committed: an army that goes in
mixed comes out mixed, rather than turning into whichever type the array happens
to list first. A move order takes its share across **all** of a country's stacks
in the province — "half the garrison" is half of everything it has there, not
half of whichever stack the search found first.

### Two integer fixes made on their merits

`count × n / total` in floating point loses a soldier once the product passes
2⁵³, and one man's difference in who was on the line cascades through a
campaign. Both the movement shares and the casualty removal now **telescope in
integers**, the same pattern the garrison-split fix established — cumulative
total, exact shares, sum exactly right.

### How this was proved, which is the part worth keeping

Stage 3 was measured against the Stage 2 baseline and **did not match**: 64.2%
against 62.3%, battles 54/123 against 42/101. Two floating-point suspects were
found and fixed; neither moved the number. Then the timestamps: `AISystem.cpp`
was 18:12:27 and the baseline was 17:39:38. **The tree had moved again — the
fifth time in one day.**

So the comparison was abandoned and a **property** put in its place.
`OD_TYPE_INVARIANT=1` recomputes the pre-types arithmetic beside the typed
arithmetic *on every call* and reports any difference for an all-line fight:
the men on the line, both sides' share of the frontage, and both powers.

| seed | divergences |
|---|---|
| 4242 | **0** |
| 777 | **0** |
| 31337 | **0** |

That is a per-call proof that the typed resolver **is** the old resolver for a
world with one kind of soldier — and unlike a two-run comparison it cannot be
wrong about whose change it is. The harness is kept, not deleted: the env is read
once into a static so it costs nothing when off, and once a second kind exists it
will still prove that every all-line fight is untouched.

**The general lesson, earned five times today:** every argument about *"whose
change was that"* came from comparing two runs across a tree somebody else was
editing. A property computed inside a single run does not have that failure mode,
and it is usually about ten lines.

**And the difference was attributed afterwards, from the other side.** It was the
AI session's *bombard mask gate*: the navy head was choosing bombard 1,124 times
an eval and **1,123 of them did nothing**, and gating the choice on "affordable
researched ammunition and an enemy port in range" cut that to 4. Warships that
stop trying to bombard nothing spend those turns moving and engaging instead —
which is why battles went 42 → 54. Another rule that fired constantly and
achieved nothing, found by counting.

The pattern generalises further than this document. Every mask gate on the AI
side claims *"the executor would have refused this anyway"* — which is a property
assertable per call rather than inferred from a bench: run the executor's own
precondition beside the mask and count disagreements. Zero proves the gate
removes only no-ops; non-zero is a gate quietly removing real choices, which no
benchmark would show.

### Verified

Suite ALL PASSED, determinism 3/3, invariant 0/0/0. Nothing recruitable, so the
AI's world is unchanged and no retrain is needed yet.

### Next

**Stage 4** is the interface — the scrollable per-province list, the map marker,
and command by type. **Stage 5** makes a second kind recruitable, which is the
single moment all of this becomes visible at once: one measurement, one retrain,
one held ruler.


## Phase 10, Stage 4 — the interface *(BUILT 2026-09-05)*

The first UI work all day that was **looked at while it was written** — the
machine's display came back, so the screenshot tour runs again.

### What the army view showed before

A flat, unbounded list of `"<country>: N soldiers"` lines drawn straight down the
panel, one per stack, with nothing to stop it running off the bottom — and no way
to say *what* the soldiers were now that they have kinds.

### What it shows now

Ours broken down by type, then everybody else's by country, in a box **sized to
its contents** and scrollable when there are more rows than fit. An allied stack
on your ground is named, because that is the difference between a province you
can hold and one you cannot.

**Clicking a row is the command interface.** One of our type rows aims the next
order at that kind alone; the "All our troops" row aims it at the whole
garrison. That is the entirety of *"command them both by type, and a province as
a whole"* — one filter with two settings, and no second way to give an order.

The filter is **stored on the order, not read at execution time**. An order given
for the militia stays an order for the militia even if the player clicks a
different row before pressing the turn button; the alternative makes an order
mean whatever the interface happened to be showing when the turn resolved, which
is not a thing anybody can reason about.

One consequence caught while wiring it: the per-province share bookkeeping had to
become **per province *and kind***. "50% of the militia" and "50% of the line
infantry" are two orders with two different denominators, and on a single
per-province key the second would have taken half of what the first left — which
is exactly the garrison-split bug, reintroduced one level up.

### On the map

The marker gains a **stacked composition bar** under the soldier, one segment per
kind in proportion — drawn *only* where a province actually holds more than one
kind. A bar that is always a single flat colour is not information, it is clutter
on every marker on a world map. So it is invisible today and appears exactly
where there is something to say. It goes in the shapes pass, not the labels pass,
or it would undo the two-pass batching that loop is built around.

### Verified — and this time by looking

`OD_SHOT_ONLY=army` was added to the screenshot tour, so the army panel is
photographed whenever the store images are retaken and can never again be built
blind. The first shot showed the new list **covering the old garrison readout**
and stretched to four times its content; both were fixed and re-shot.

Behaviour: invariant 0 divergences, and survival 64.2% with battles
`25/67/10/6/1/6` — **exactly the Stage 2 baseline** once the AI session reverted
the two mask gates that had been moving it. Suite ALL PASSED.

### A harness fix, in passing

The determinism check reported *"run 3 diverged from run 1"* with an empty B. The
windowed binary intermittently produces no trace at all under load — a different
run each time, while every run that did trace agreed. That is the display losing
a race, not the simulation disagreeing with itself, and "diverged" is the least
helpful true statement available. An empty run is now a **note**, the pass line
says how many runs were actually compared, and it warns when too few were
compared to mean anything.


## Phase 10, Stage 5 — a second kind becomes recruitable *(BUILT 2026-09-05)*

The stage that turns four inert ones on.

### What landed

An **Army > Formations** research branch, shaped exactly like the artillery
branch beside it: each node carries a `troopType` the way an artillery node
carries an `artilleryType`. Militia hangs off basic training (a country that can
drill conscripts can raise a levy); assault infantry needs combined arms;
mechanised needs logistics *and* assault. Line infantry has no node — gating it
would strand every existing save behind a technology it never researched.

A **kind picker** in the recruit panel, offering only what
`unlockedTroopTypes()` says the country has — one reader, so the panel cannot
offer something the resolver would refuse. A country with only line infantry
sees no picker at all.

On the wire, the host checks `troopTypeUnlocked` rather than trusting the
client: owning a province is not authority to raise mechanised divisions.

### Verified by looking, and one number that looked wrong

Photographed: the picker with all four kinds; and the *same province* with line
infantry and with mechanised selected —

| kind | raises | costs |
|---|---|---|
| Line Infantry | 21.5m | $139,586 |
| Mechanised | 8.3m | $187,950 |

Fewer men and more money, both right — but 21.5 ÷ 8.3 is 2.6, and mechanised
cost **4×** the manpower. Chased rather than waved past: the province holds ten
billion people (an old bench save), so the line-infantry count hit the
`INT32_MAX` clamp — the true figure was 3.3e9, capped to 2.147e9. Mechanised at
3.3e9 ÷ 4 = 8.3e8 was not capped. **The 4× rule is exact; only the numerator was
clipped** — and the overflow clamp added in the last stage is doing real work on
this save.

### The finding that matters more than the feature

```
[RECRUITS] 67,456,039 raised: line=100%  militia=0%  assault=0%  mech=0%
```

**The AI cannot raise anything but line infantry**, because its recruit action
has no kind on it. That is the AI session's half and is untouched here — but it
is worth having as a number *before* a retrain rather than after: there is
nothing for a model to converge on yet.

It also meant every combat column added in Stage 3 was unreachable through play,
which is indistinguishable from a system that does not work. So `OD_SEED_KINDS=1`
assigns kinds round-robin to the starting garrisons once — a development aid, off
by default — and with it:

| | all line | mixed kinds |
|---|---|---|
| survival | 73.6% | **75.5%** |
| assaults | 18,722 | 18,077 |
| battles | 35 | 33 |
| supply biting | — | 19.8% |

So the frontage, attack/defence and fuel columns **do** fire and **do** change
outcomes. They are simply unreachable through play until the AI's recruit action
grows a kind.

### Verified

Suite ALL PASSED. `OD_TYPE_INVARIANT` still reads 0 on all-line fights — it can
no longer prove overall inertness (by design, since the change now does
something) but it goes on proving the untouched cases. Determinism 6/6 on the
windowed check.

---

## How Greater Diplomacy 4 does the middle state *(researched, 2026-09-05)*

Read from the actual project (`Greater Diplomacy 4.13.7(network hotfix).sb3`,
214 MB, 24 MB of `project.json`, 99 sprites). Recorded because the Orders view
was built from a description and this is the thing it was described from.

**Dragoman was the wrong place to look and does not have it** — it is a map
translator, and its only GD-side turn knowledge is three lines of scenario
metadata it carries through (`fog_of_war` default **true**, `casus_belli_required`,
`days_per_turn`, plus `active_players` / `current_player_index`). Worth one
note, though: **GD has fog of war and OpenDoctrines does not**, so GD's
intermission can afford to "show everything" — its limit lives upstream in
vision. In OD the orders view is the *only* place such a limit can live, which is
why the ship-plan rule had to go there.

### The architecture

`Screen Type` is a single global string, and **every sprite that draws or takes
input compares against it** — Province Arrows, the disband/train display, the
map UI bar, the province outlines, the skybox, even the FPS counter. The
intermission is `Screen Type = "Watching AI Moves"`: a **screen**, not an overlay.

Three consequences worth having:

1. **The UI bar is not "buttons hidden", it is a different bar.** `Map UI Bar`
   carries its own costumes — `Watching AI Moves`, `Watching AI Moves2`,
   `Watching AI Moves Challenge`. The phase swaps the bar rather than suppressing
   controls one at a time.
2. **The same renderers draw both phases.** `Province Arrows` and
   `disband / train display` are the sprites that show your own orders while you
   are giving them, and the same ones that show everybody's during the
   intermission. Only the data changes, not the drawing.
3. **It is passed through, not toggled.** Finalize Orders → the phase → *"Click
   me to continue"* → resolution. There is a persisted setting,
   `Skip viewing AI moves?`, surfaced on the order screen as
   *"Skip Viewing AI Moves? YES/NO"*, and a keybind costume for the continue
   button. There is also a `Next Player Ready` screen, which with Dragoman's
   `current_player_index` says the whole loop is hot-seat sequenced.

### How OpenDoctrines' version differs

| | GD4 | OpenDoctrines today |
|---|---|---|
| shape | a screen you pass through | a toggle over the live map |
| entered | automatically after Finalize Orders | on demand, any time |
| left | "Click me to continue" | toggle off |
| skipping | persisted `Skip viewing AI moves?` | n/a |
| the UI | a phase-specific bar | three sidebar tabs hidden |
| content | everyone's moves | everyone's moves |

The **content** matches — arrows for movement, markers for disbanding, drawn
over the map. What differs is the *shape*: GD4's is a beat in the turn loop, and
OD's is a lens you can pick up whenever you like.

Left as it is for now, at the user's word. Making it faithful would mean a
turn-phase state rather than a boolean — which is a bigger change than it looks,
because in OD every panel decides its own visibility, and the thing that makes
GD4's version clean is that **one variable decides for all of them**.


## The turn-phase refactor *(BUILT 2026-09-05)*

The Orders view was a **lens you could pick up**. It is now a **beat in the
turn loop**, which is the shape GD4 uses.

### One variable, and it was already there

`TurnState` existed with a single value, `TURN_NORMAL` — a degenerate enum that
decided nothing. But **thirteen call sites already asked
`m_turnState == TURN_NORMAL`** before deciding whether a control was live.

Adding one value turned all thirteen into phase checks for free. No button had
to learn about the new phase; they had all already been written to ask. That is
the property that makes GD4's version cheap — one `Screen Type` compared against
by every sprite — and OpenDoctrines turned out to be half-way there without
anybody having used it.

```cpp
enum TurnState {
    TURN_NORMAL,          // playing: orders may be given
    TURN_VIEWING_ORDERS,  // the turn resolved; the map shows what everyone did
};
```

### The phase

Entered automatically at the end of `processTurn`, left by a deliberate press.
A banner names it, a subtitle counts the orders, and one button continues —
placed where Process Turn sits, because it is the same beat in the loop and the
hand is already there. `Skip this from now on` is offered beside it and
persisted in the config, exactly as GD4 keeps `Skip viewing AI moves?`.

**Every automated driver is excluded, and the list is the point rather than an
afterthought**: a phase that waits for a person is a hang for anything without
one. `m_aiTraining` covers `--train-ai` and `--eval-ai`, `m_walk` the tutorial
walk, and a screenshot tour is identified by its output directory. Verified:
`--tutorial-walk` reads *108 pages walked, 0 problems*, and the headless eval is
untouched.

### What the phase suppresses, and the rule it exposed

Three sets of controls had to be told, and **two of them were bugs the phase
merely revealed**:

- The **toolbar row** ("Disband all") had its click handler gated on
  `TURN_NORMAL` and its **draw ungated** — so it went on painting through the
  phase, straight over the Continue button.
- The **province panel's action buttons** and **Fire Artillery** were the same:
  drawn, dead, and in the way.

A control that is drawn and dead is worse than one that is absent. This is the
same shape as two earlier findings — a war-declaration cap written in the
diplomacy panel that bound the player and not the AI, and a landing range
written in the move overlay that bound the mouse and not the game. **A rule
written on one side of a draw/handle pair is a rule that is only half there.**

The garrison **list** stays up: it is information about the board, which is what
the phase is for.

### Verified

Suite ALL PASSED, tutorial walk clean, headless eval unaffected, both trees
build. Photographed at 1600x900 and on a 402x874 phone canvas, and added to the
screenshot tour as `orders-phase` so it cannot regress unseen.

---

## Ships crossing land: the same bug, one level down

Reported from a screenshot: Russian vessels sailing over Crimea.

`buildNavGrid` marks a 32-px cell navigable if it holds **any** water, and
remembers "a real water pixel inside the cell" — which may be anywhere in it.
Crimea is a peninsula about 40 px across, so the cells along it hold Black Sea
water on the north side and Black Sea water on the south side. Both are
navigable. Both are the same water body. They touch on the grid. Every test in
the function passed, and the leg between them ran overland.

The function already documents fixing this exact mistake once, for
*connectivity*: cell-level flood fill had declared Lake Titicaca reachable from
the mid-Atlantic, and the fix was to decide bodies from the raster instead, so
the grid "can no longer invent a canal that the map does not have." That fix
never reached **adjacency**. Connectivity was made honest; the edges were not.

### What it cost, before

Worst overland run along a real route, in raster pixels, on `map.odmap`:

| voyage | before | after |
|---|---|---|
| Odessa → Kerch | **53** (straight across Crimea) | 11 |
| Brest → Kiel | 49 (across Jutland) | 11 |
| Gibraltar → Suez | 46 | 16 |
| Naples → Venice | 37 (across Italy) | 14 |
| Athens → Odessa | 36 | 15 |

### The tolerance is not the strait width, and assuming it was sealed the Black Sea

The obvious implementation reuses `STRAIT = 8`, the width the component pass
already calls a strait. It is wrong, and measurably so: `STRAIT` is measured
across a run, along a row or column, while a leg meets the same neck at whatever
angle the two cell pixels happen to make. The first build put Odessa and the Sea
of Azov in a body of their own that **no hull on Earth could enter**.

Swept 8/12/16/20/24/28/32. 16 is the first value that returns the Black Sea and
the Sea of Azov to the world ocean while leaving the Caspian and the Great Lakes
in bodies of their own, and nothing that should stay separate reconnects above
it. Bottom of the correct plateau, not a value that merely works.

### The part of the fix that was measured and then thrown away

Making the edges honest raises an obvious follow-on: `component` decides
`navReachable`, so if it still answers over the loose adjacency it will say yes
where the router says no, the AI will sail at a target it can never reach, and
the order sticks — the failure mode that produced 71 stuck orders once before.
So the seas were relabelled by flood-filling the honest edges.

It was the wrong call, and only measurement says so. With honest edges the
router's failure rate is **0.0% of 747 voyages**: there was nothing for the
stricter label to protect against. And it cost real play. One procedural map, 60
turns, seed 4242:

| | embarks | landings | arrive |
|---|---|---|---|
| loose edges + raster seas *(shipping today)* | 54 | 20 | 37% |
| **honest edges + raster seas** | **78** | **45** | **58%** |
| honest edges + link seas | 56 | 24 | 43% |

The relabelling takes half the landings back. It over-fragments — 198 seas
against the raster's 79, mostly pockets a noisy procedural coastline makes and
no hull cares about, and every pocket is a port the AI stops even *trying* to
reach. Kept behind `OD_NAVSEAS=links`, because the reasoning still holds the day
the failure rate stops being zero.

### And the win is not clean across seeds

Three seeds, 60 turns, landings before → after:

| seed | before | after |
|---|---|---|
| 1717 | 14 (33%) | 10 (28%) |
| 9090 | 7 (33%) | 8 (42%) |
| 4242 | 20 (37%) | 45 (58%) |

41 → 63 landings in total and 34% → 43% mean arrival rate, but **one seed of
three got worse**, on small counts. Seed 4242 is the seed the three-arm
comparison above was chosen on, and it is the seed with the largest gain —
exactly the shape the project has been burned by before. The change is
justified as a **correctness** fix, not on these numbers: ships were sailing
over land, and now they are not.

### The seat bench, which is the instrument that answers "good"

`tools/od_bench.py` (6 seats x 3 seeds x 120 turns, difficulty 3, ParrotZero
8.2.0), same binary both arms, the control selected with `OD_LEGNECK=9999`:

| | OD BENCH | survival | worst seat |
|---|---|---|---|
| loose edges | 80 | 69 | 1 |
| **honest edges** | **114** | **71** | **5** |

Each arm was run three times. All three controls returned 80 / 69 / 1 and all
three treatments 114 / 71 / 5, exactly -- so the gap is the change and not
weather, and the harness is bit-reproducible at this length. Note the **worst seat moving 1 -> 5**: the seats a fake
sea route hurt most are the ones that cannot be defended without a navy, and
they are the seats the aggregate is least able to see.

NOTE ON THE INVOCATION: `--seed` on od_bench applies only to `--record`. A plain
run is already 3 seeds; passing `--seed` to it does nothing and three "different
seeds" are three identical runs.

## Turn phase: the land holds still

"The land should not change until we get to next turn." The phase was showing
the post-resolution map, so borders had already moved while the player was being
shown the attacks that moved them.

No snapshot was needed. The map on screen is a texture and only changes when
something re-uploads it, so the turn stops asking for the upload and leaves a
note (`m_politicalRepaintPending`); the note is honoured on the first frame
after the phase ends. Zero copies, zero extra passes, and it *removes* work: a
turn with four ceasefires used to regenerate the whole 8192×4096 buffer five
times to show one final picture.

## The screenshot fixture was fabricating nonsense

`orders-phase` and `orders-desktop` built their order log by pairing `mine[k]`
with `mine[k+1]` — two entries of a hash map, in iteration order. Every
photograph they produced was a spray of arrows between random continents,
Norway to Australia and Peru to Siberia. Not a thing the game can draw, and not
a thing anyone could check: the arrows were unreadable, so nobody could see
whether they were right. Orders now run to provinces that actually adjoin their
source and carry the detail a real order carries, and the fixture covers the two
standing kinds as well.

---

## Research groups

Asked for: up to three groups with separate budgets, unlocked on "some ratio of
economy to people", at least one always available; plus an auto-advance that
walks a tree until it meets a decision.

### There were two copies of the research rule, and the live one was the inline one

`Game::addResearchPoints` and `Game::updateResearch` are defined in
Game_Research.cpp and **called from nowhere**. The rule that actually ran was a
second, inline copy inside `processUpgrades`. They never disagreed only because
one of them never executed.

Groups would have had to be written into both, and the dead copy is the one that
would have quietly won the day anybody called it. So the inline block is gone and
`processUpgrades` calls the function. One reader.

### Per head was the wrong axis, and the British Empire is the case that shows it

The first rule read income per head against the world median. It gave the
British Empire ONE group, below Switzerland's three, and the same for the Soviet
Union. Rejected on sight, and correctly: that measures how DEVELOPED a country
is, and a research programme is not bought with development. It is bought with
the absolute size of an industrial base.

Measured on 1939 at turn 40, gross income against the world median of 34.3:

| | grossX | perX | groups |
|---|---|---|---|
| United States | 30.3x | 2.93x | 3 |
| French Republic | 27.2x | 3.95x | 3 |
| British Empire | **19.9x** | 0.86x | 3 |
| Soviet Union | **11.9x** | 0.87x | 3 |
| German Empire | 6.2x | 1.00x | 3 |
| Autocracy of Japan | 7.0x | 0.97x | 3 |
| Autocracy of Italy | 3.4x | 0.94x | 2 |
| Autocracy of China | 5.3x | **0.21x** | 1 |
| Switzerland | 1.1x | 36.9x | 1 |

The great powers are unmistakable on size and invisible per head. So size
decides, and **per head became the gate it was always described as** -- "if the
income compared to amount of people is way too low, we still have only one
science group." China has the fifth largest economy on the map and a fifth of
the world's income per head; the gate catches exactly that country and leaves
Britain at 0.86x and the USSR at 0.87x alone. Distribution: 1939 gives 29/6/6
countries at 1/2/3 groups, 1914 gives 23/8/4.

The locked-group line names **whichever of the two is actually stopping you**.
Telling a big poor empire to grow its economy is telling it to do the one thing
that will not help, and that empire is precisely the case the gate exists for.

### Both axes are multiples of the world median, and the first attempt was not

Absolute income-per-head thresholds were chosen against measured data — 1939 at
turn 40 spans two orders of magnitude, China 0.37 to Switzerland 64.1 — and they
worked on 1939 and 1914. Then the panel was photographed on a save whose
provinces carry populations three orders of magnitude larger, and it read:

> Group 2 locked: needs 3 income per 1M people (have 0.0)

Nothing was wrong with that world. The quantity is simply not comparable between
maps, and a mod or a procedural generator may scale population however it likes.
A constant would have handed every country in such a world one group forever
while explaining the shortfall in units that world does not use.

Multiples of the **world median** are the same judgement and are scale-free,
which is why both axes are expressed that way and neither is a constant.

Cached once a turn. Both the unlock and the panel explaining it are asked per
frame, and answering honestly is a pass over every province of every country.

### Two rules a screenshot cannot check

`tests/research_groups_test.cpp`, 21 checks. The share is normalised over the
groups actually **working**, not over 100 -- otherwise a country running one
project researches at a third speed and nothing on screen says why. And a locked
group must not enter the divisor, or two unlocked groups quietly run at
two-thirds. Auto-advance is scoped to the **branch**: one open node is a
continuation, two is the decision the player asked to be stopped for, none is
the end. It will not cross into another branch, because choosing a branch is
exactly the judgement the setting promises not to make.

### The save stays byte-identical when the feature is unused

Groups ride in the turn-delta trailer under flag bit 0x08, and the block is
written **only when it says something** -- any group past the first active, any
non-default share, any Auto on. A country running one programme writes nothing,
so its saves are byte-for-byte what the previous build produced. The first
implementation wrote the block unconditionally and the suite caught it
immediately: `a save that needs no wide table is byte-identical to before --
48 bytes vs 30`. That is the acceptance test the troop-type trailer set, and it
is the right one.

---

## "Show orders": the toggle meant the wrong thing

Photographed by the player: the live map with a labelled text box on every
province on Earth, several thousand of them, over the troop counts already
there. Unreadable, and the map underneath invisible.

The clutter was a symptom. The toggle painted every order in the world onto the
map *while you were still giving orders*, and orders belong in the pause AFTER a
turn, where nothing else is competing for the same pixels. So the tick now means
"stop after each turn and show me what happened" -- which is the same setting the
phase's own "Skip this from now on" writes. `m_showMiddleState` is gone: it was a
second answer to a question `Config::skipViewingOrders` already answered, and the
two could disagree.

### The symbols already existed

The game has drawn these marks for the local player all along: a green plus for
an industry, fort or port going up, an orange S for a specialisation, a green
plus above the stack for a levy, a yellow B for a hull. Each appears only in the
view its subject belongs to. The phase had invented a *second* visual language
for the same facts.

So the drawing moved into `Game::drawActionCue` and all five callers use it --
the four local-player loops and the phase. A player who has learnt what a green
plus means has learnt it once. `actionCueTab()` decides which view each belongs
in, so the industry map is not also an army map.

Verified by counting rather than by eye, because a 4-px disc at world zoom is not
something a screenshot proves: `[ORDERCUE] tab 5 marks 28 cues drawn 3 hidden 0`
-- three levies drawn in the army view, and the three construction orders in the
same log correctly absent from it.

### Density is a zoom question

Standing orders are drawn from 3x minimum zoom, marches always. Nothing is hidden
that cannot be reached by zooming, and the banner says how many are waiting --
something withheld without saying so is indistinguishable from something broken,
and this view exists to be believed.

## Research budgets that add up

Three sliders each independently 0-100 meant two groups could both read 90% and
the panel showed a country spending 180% of its research. Nothing was actually
overspent -- `researchGroupPoints` normalises before paying out -- which is
WORSE than overspending: the numbers on screen meant nothing and quietly
disagreed with what the game did.

Ten checks in `research_groups_test.cpp` pin it, including the one that is easy
to miss: integer division loses a point or two and the remainder has to go
somewhere, or the row reads 99 and the rule on screen is a lie.

### The AI half, and an A/B with one arm

"Obligation both for player and ai" is right, and the memory says why: a rule
written in the renderer binds the local player and nothing else.

The AI could not simply be given the player's groups. Its research is chosen by
the policy net through mask bits 9-11, gated on the country being *idle*, and
this file already records what re-aiming those bits costs -- the research-focus
fix is correct, measured, and still OFF by default because it cost every frozen
model (265->220, 238->189). So the net keeps deciding exactly what it decided
before: its chosen node is group 1, and the extra groups fill themselves with
the cheapest available node, which is the same fallback the net's own action
uses. No mask change, no retrain.

**The first measurement of this was worthless and said so with a perfect null.**
Both arms ran `OD_RGROUPS_OFF`, a flag invented on the command line and never
implemented -- so the control was the same build as the treatment and reported
11.29 against 11.29. With the gate actually implemented:

| | research nodes / 1k country-turns |
|---|---|
| 1939, groups off | 7.38 |
| 1939, groups on | **11.29** |
| 1914, groups off | 10.33 |
| 1914, groups on | **15.37** |

### And it costs 9 points on the seat bench, for a reason worth naming

`od_bench`: 76 -> 67, survival 54 -> 48.

Read carefully before treating that as a regression. The bench scores ONE seat's
share of a world every other country is playing in, and this change gives every
one of those countries half again as much research while giving the benched seat
**nothing** -- the seat's own groups are never populated in a headless run, since
nothing picks nodes for the player. So the number is measuring an asymmetry the
bench creates, not the merit of the rule. It is the shape the project has already
written down: a rule that helps everyone lowers every seat's score.

Gated on `OD_RGROUPS_OFF` if it wants revisiting.

## The economy screen

Reported as "the pie charts need fixing", with a screenshot showing the income
graph's legend drawn underneath the pies.

- **The second pie was drawn on top of the first one's legend.** It advanced 100
  px from the first chart's TITLE; the chart is 110 px of circle below that
  title, and its legend runs further still -- seven expense categories at 16 px
  each. Now it starts below whichever of the two actually reaches lower.
- **The income graph was five quantities on one pair of axes sharing a maximum**,
  so net income -- the one line anybody watches -- was a flat scratch along the
  bottom whenever gross income was large. Replaced by graphs that each answer one
  question, with the legend in the title line where it cannot collide.
- **National value** is new: the price list run backwards over what the country
  actually holds -- every industry level, fort and port at what it cost, every
  soldier and hull at what it cost to field, plus the treasury -- built from the
  same tables the build buttons charge from, so it cannot drift into describing a
  different game. Everything else on that screen is a rate, and none of it
  answers how much there IS.

The tour only ever photographed the global league tables, so every layout fault
on the local side went unseen for as long as it existed. `economy-local` is now
a shot.

## Research groups as cards

A group carries four things: what it is building, how far along it is, what
share of the budget it gets, and whether it walks its branch alone. Laid out as
a 21 px strip those became a name clipped to fit, NO progress indication at all,
a slider the width of a thumbnail and a button squeezed against the screen edge.
It is the primary control of a whole subsystem and it was the smallest thing on
the screen.

Side by side rather than stacked, because the question asked here is a
comparison -- which programme gets the budget -- and three stacked rows make you
read three lines to answer it.

Two faults the first draft had, both caught by photographing it:

- **Drawn through the economy readout.** The cards started at x=700; the
  allocation slider runs 300-600, its percentage sits at 608 and the
  "(+N RP/turn)" figure after it reaches about 780. So the card covered the one
  number that says what the allocation actually buys. Moved to 800.
- **The progress figure was dark text ON the bar**, which read on the filled
  part and vanished into the empty part -- so a project just started, which is
  exactly when the figure matters, was the case that could not be read. Now
  light text with a shadow, sitting above the bar.

And one the screenshot could not have caught, because the tour had no shot at
that size: the cards need the first 800 px for the slider, and a phone canvas is
402 px wide. The loop that skips a card it cannot fit would have skipped ALL
THREE, putting the research system out of reach on that device with nothing on
screen to say so. Below about 380 px of free width they drop to a row of their
own and the bar grows to hold them. `research-portrait` is now a shot.

## Translating the new content

86 strings were untranslated in all twenty languages -- the turn phase, the
research groups, the troop kinds, the economy graphs, and a set of older map
editor and goods strings that had never been done. All twenty are now at
1507/1507.

Three things this turned up.

**A checker that reported fifteen faults in code nobody had touched.** The first
specifier comparison read `%[-+ #0]*...` -- with the space flag -- so `10% of
troops` parsed as a conversion (`% o`) and every prose percentage in the file
looked like a broken translation. The space flag is not something this codebase
uses and admitting it makes ordinary English text look like format strings.

**Turkish put a `%s` in front of a `%d`.** Turkish wants the percent sign before
the number and the possessor before the possessed, and following that word order
turned `"%d%% of %s"` into `"%s ... %d"`. printf consumes arguments IN ORDER, so
that reads an int as a pointer. Two other reorderings were harmless -- a literal
`%%` moving -- but the checker flags all three the same way, and it should:
the class of fault is what matters, not whether this instance happens to be
survivable. Now `"%d%% (%s içinden)"`.

**Slovenian "Samodejno" did not fit the Auto button**, 51 px in 44. Found by
measuring rather than by looking: `i18n_fit.py` asks the game through
`--measure-text`, because the three text paths (glyph atlas, raylib's variable
font, HarfBuzz for Urdu) disagree and an offline model of them was wrong by 17%.
Worth knowing: `sl` measures the Latin word "Auto" at 51 px where `de` measures
the same four letters at 23, so a Latin fallback is not a safe assumption for a
tight box. "Samod." at 32 px.

Two fixes to the tooling on the way through:

- `i18n_fit.py` had `GAME` hard-coded to `build/`, and reported a missing binary
  as `the game could not measure:` with nothing after the colon, because a
  missing executable sets no stderr. It takes `--binary` now and says so when
  the path does not exist. The repo learned this once already with the trainer
  picking up whichever tree was newest.
- The measure output is keyed by `(code, size)` and collapses duplicates, so
  three Slovenian strings at size 10 all came back with the first one's width.
  Measure one string per `(code, size)`, or one per run.

`i18n_quality.py` also caught two of mine: Arabic had `الإقليم` for province
where the rest of the game says `مقاطعة`. That linter exists because Turkish
once used three different words for a claim. 42 warnings down to 40, 0 errors.

---

## Releasing a nation as a term, and choosing its ground

Two requests that are the same request: a release should be something you can be
ASKED for, and either way it should be possible to say what the new country
actually gets.

### The terms

`CeasefireTerms` grows four fields -- a tag and a province list for each side.
**The provinces are carried, not recomputed from the minority at resolution.**
What a release consists of is a judgement, and recomputing it when the treaty
resolves would hand over a different country than the one both sides agreed to
on any turn where the front had moved in between.

`applyCeasefireTerms` frees them AFTER the cessions in the same treaty, so
ground changing hands belongs to its new owner before anyone asks whether it can
be released. `releaseNation` re-checks everything and refuses a whole release
rather than half of one; a refusal leaves the rest of the treaty standing, which
is the right answer -- the war still ends, the money still moves, and the nation
that could not be freed simply is not.

### The AI had to be taught that this costs something

`theirReleaseProvs` leaves the recipient and arrives nowhere the sender owns. It
is a straight loss, priced like any other ground -- without that a country would
sign away a third of itself for free, because the net read zero. The other
direction is worth something and not the same thing: when the SENDER dismantles
itself the recipient gains no ground, it gains a weaker rival and a buffer.
Valued at a quarter, which is a guess made explicit rather than a zero
pretending the event did not happen.

And the ruin gate -- "nobody volunteers to be dismantled" -- counted only ceded
provinces. A demand to release half the country is precisely the offer that gate
exists to refuse and it would have walked straight past it. It now counts ground
LEAVING, whoever ends up with it.

### Contiguity is the part that breaks quietly

`releaseSubsetOk` walks the chosen set only. Trimming the ENDS off a region is
fine; trimming the MIDDLE leaves two countries with somebody else's ground
between them, reads as one release, and is two. Nine checks in
`release_rules_test.cpp`, including that the walk cannot be fooled by listing
the connected half first -- it starts at the first entry, so a lazier check
would pass `{5, 6, 1, 2}`.

The same function is asked twice: by the panel while composing, and by the
sender before the offer goes out. A half-picked release is dropped from the
offer rather than sent to fail at resolution, where the other side would have
agreed to something that could never happen -- and the composer says so while it
can still be fixed, because a term that vanishes silently between composing and
sending is worse than one that was never offered.

### Two layouts, one order

The ceasefire sidebar draws its mode buttons walking `curY` down, and a second
block re-walks the same arithmetic to find where they ARE. The comment there
already says "curY must match drawCeasefireScreen EXACTLY". Adding two buttons
means adding them to both, and a button in one and not the other is drawn and
dead -- the draw/handle asymmetry this codebase has produced repeatedly.

## Naval bombardment in the orders view

`m_pendingShipBombardOrders` existed and was never recorded, so the view showed
land batteries and not carriers: a coast being worked over read as a quiet turn.

It is its own mark kind rather than an Artillery mark, because it starts at a
HULL. `arrow()` takes two province ids and a carrier standing off a coast has
none; giving it the nearest province would draw the shell's flight from whatever
land it happened to be closest to.

---

## The blurry font

Reported with a screenshot of Ukrainian at small sizes on a large display.

`kAtlasSize = 16`. Every glyph is baked once at 16 px and every size drawn is a
scale of it, so at 20 px each glyph is a 16-pixel bitmap being stretched, and
bilinear filtering makes that soft rather than broken. The comment there records
an earlier fix -- point filtering was DROPPING strokes, turning Ukrainian into
"Ьільшу ... буду⌐ь", and bilinear stopped that. Blur is what bilinear does
instead.

**Rasterising at 32 had already been tried and reverted**, and the reason was
kept: it quadrupled every atlas and the game wedged inside raylib's image
conversion during a map load. That is a real failure and must not come back.

So the size is chosen against a budget, and the budget is the biggest atlas the
game already builds. Measured at 16 px:

| | glyphs | texture | |
|---|---|---|---|
| de / uk / bg / tr / hi / ur | ~900-950 | 1024x512 | 2 MB |
| ar | 1154 | 1024x512 | 2 MB |
| ko | 1615 | 1024x1024 | 4 MB |
| ja | 1956 | 2048x1024 | **8 MB** |

At 32, with an 8 MB ceiling: Latin, Cyrillic, Greek, Arabic, Devanagari and
Turkish sharpen (2 -> 4 MB, and Arabic 2 -> 8); Japanese and Korean exceed it and
stay at 16, byte-identical to before. **No atlas is ever bigger than one this
game already ships.**

### The estimate was wrong in the direction that mattered

The first implementation predicted the packed size as glyphs x (size + 2)^2
rounded to raylib's next power of two. It cleared Japanese AND Korean for 32 and
both came back at 2048x2048 -- 16 MB, twice the budget the estimate existed to
enforce. raylib's packer is less dense than that arithmetic assumes.

So the atlas is now BUILT at 32, its real texture measured, and rebuilt at 16 if
it exceeds the budget. One wasted build at load, only for the languages that
fall back, and it cannot be wrong about the number it enforces because it reads
it rather than predicting it.

### A second cause, and it was the bigger one

`SetConfigFlags(FLAG_WINDOW_RESIZABLE)` -- **`FLAG_WINDOW_HIGHDPI` is not set.**
On a Retina display that means the framebuffer is the logical size and the OS
upscales the whole frame: the map, the UI and the text all rendered at half
resolution. That would be a larger cause of "blurry on a big display" than the
atlas, and it would explain why it is worse on big displays specifically.

**And the fear that it was a rewrite was wrong.** Reading raylib 5.5 rather than
remembering it: `SetupViewport` in rcore.c has an `__APPLE__` branch that sizes
the GL viewport by the content scale while leaving `rlOrtho` in LOGICAL units,
and the GLFW backend deliberately skips its own mouse and screen scaling on
Apple -- "system should manage window/input scaling". So on macOS the flag is
coordinate-transparent: every draw call and every hit test keeps the numbers it
already uses and only the resolution they are rasterised at changes.

It is on by default, behind `"highDpi"` in config.json.

#### The check that mattered was the one for the platform I cannot test

Off Apple, raylib DOES scale the mouse itself when the flag is set --
`SetMouseScale(screen/framebuffer)` in rcore_desktop_glfw.c -- so the cursor
already arrives in logical units there too. The game multiplies the pointer by
the content scale, and with the flag on that becomes a SECOND correction: on a
150% Windows display every click would land half a screen from what it was over.

The first version of this change had that bug. It is now one function,
`Game::pointerScale()`, which answers "how many drawing units per unit the
pointer reports" -- one when high-DPI is on, whoever did the scaling, and
otherwise exactly what it read before, so no platform tuned against the old
behaviour moves. Four call sites read the content scale directly; they read this
instead.

#### What is and is not verified

The flag is live -- `highdpi_flag=1` under `OD_DPI_PROBE` -- and nothing
regressed: 107 tutorial pages with 0 problems (which is the whole UI hit-tested),
six determinism runs agreeing, and no screenshot changing size.

The sharpening itself is NOT verified here. This machine reports `scale=1.00`
with `render == screen` even with the flag set, so the 2x path never executes on
it. `OD_DPI_PROBE=1` prints the three numbers that say whether it is doing
anything on a given display.

## The suite was testing the wrong binary

`tests/run_all.sh [build-dir]` defaults to `build/` -- the tree the other session
in this repo builds into. The unit tests are compiled from current source
whatever directory is named, so those results hold, but the determinism check
runs the BINARY found there, and that binary was ten hours old.

It surfaced as a determinism failure -- `run 6 diverged from run 1`, armies and
money differing at turn 21 -- which is alarming and was not this work: four runs
of the current binary agree exactly, and `tests/run_all.sh build-roadmap` reports
`6 runs of seed 4242 agree over 25 turns`.

Pass the build directory. It is the same lesson the trainer taught when it
silently picked up whichever tree was newest.

---

## "The saves are still deterministic" -- and they were

Reported with the flags as evidence, and the flags are exactly the right tell: a
flag is drawn from a country's identity, an identity is classified from its
political compass, and the compass is read straight out of the map's
`country_compass.json`. The world seed reached `g_simRng` and nothing else, so
two new games on one map differed in what HAPPENED and not at all in what they
started as.

`generateProcedural` is only ever called from the map editor -- it makes a map
file, not a world -- so nothing at new-game time was ever going to vary.

A fresh world now jitters each country's compass by up to 18 of 200. Bounded on
purpose: enough to carry a country sitting near an ideology threshold across it,
which is where the interesting ones sit, and small enough that the map still
describes the world it made. Drawn from the sim RNG, which `chooseWorldSeed` has
just seeded from `random_device`, so:

    seed 777 -> compass hash 17855601694521614012
    seed 777 -> compass hash 17855601694521614012
    seed 111 -> compass hash 11161037830592767160
    seed 222 -> compass hash  8536473935534218243

Same seed, same world; different seed, different world. `OD_WORLD_SEED` still
pins it and the determinism check still passes, because that check replays one
seed.

**It went in the wrong branch first.** There are TWO compass parsers -- one for
loose files and one for the JSON inside an `.odmap` -- and the call landed in the
loose-file branch that no shipped map takes, so it did nothing and said nothing.
It printed a count and the count was the same either way, which is why the log
line now carries a HASH: a number that changes when the world changes is a
question with an answer, and a count of countries is not.

## The black screen was the slow turn

Two reports, one event. Deferring the political map regeneration to the draw
loop was right for the orders phase and wrong for every other turn: the turn
returned in a blink and the game then sat on a black screen for seconds doing
the work with nothing drawn. It runs inside the turn again, behind
"Generating political map...", and only waits when the orders phase is actually
coming -- where `flushMapRepaint` now puts a loading screen up itself.

A suspected second cause was measured and cleared: `recordIncomeSnapshot` calls
`countryNationalValue` per country and each walks every province, which is
O(countries x provinces) per turn and looked like the culprit. Timed both ways
over 60 turns: 5.85/5.15 s against 5.60/5.18 s. It is not the slowdown. The
single-pass version is kept because it is the right shape, not because it fixed
anything.

## Orders belong to a view

Marches, artillery, voyages and naval bombardment were drawn over the industry
map, the population map and the resource map alike -- the same fault the
standing cues had already been fixed for. Each kind now appears only in its own
view, and the banner names what the current view carries, because a filter
nobody is told about is indistinguishable from a fault.

## Two more layout faults the tour could not see

The claims tabs sat every 140 px whatever they said. In Ukrainian "Мої
претензії" and "Претензії до мене" are wider than the gap, so the first ran into
the second. `MeasureText` is language-aware -- it is shadowed alongside DrawText
-- so the widths were always available; only the spacing refused to use them.

The claims screen had NO shot in the tour, which is why nobody saw it. It has
one now, in Ukrainian, because that is the language that breaks it.

---

## Districts

Pacification was one number for a whole country. The same suppression fell on a
quiet heartland and on a province in open revolt, so the only choice a player had
was how much to overpay for the calm half in order to reach the loud one.

### The rule, and why it costs nothing until it is used

A district's share of the BUDGET against its share of the GROUND. One holding a
tenth of the country on a tenth of the budget multiplies by exactly 1.0 -- so an
undivided country, and a country whose districts are drawn proportionally, both
play the game that shipped. That property is what makes this additive rather
than a rebalance, and `tests/district_rules_test.cpp` pins it in 19 checks along
with the degenerate shapes: a district with no ground, no budget anywhere, a
country holding nothing.

The multiplier is capped at 6x. Without it a one-province district holding the
whole budget of a large country multiplies suppression by the province count,
and no amount of grievance could ever reach the threshold again.

Ground changing hands calls `reconcileDistricts` on BOTH sides: territory that
stays in the loser's district would be policed with their budget on somebody
else's land, and territory arriving in none of the winner's would be policed
with nobody's.

### The AI draws them and it changes nothing, for a reason worth writing down

`updateAIDistricts` splits the worst fifth of a country's provinces off by the
same rebellion chance the resolver uses, and aims the budget there. It is a
reflex rather than a net action, for the reason the research groups already
established: the net's action set is fixed and every frozen model was fitted
against it.

Measured on 1939 and 1914, on and off: **byte-identical**. The debug line says
why -- `pacification=0.000` for every AI country. The bankruptcy handler zeroes
`m_countryPacification` every turn for anybody who cannot pay for what they
already have, and AI countries run at or near zero treasury as a matter of
course. There is no budget to redistribute, so redistributing it changes
nothing.

A funding rule was added and measured too, and it is inert for the same reason:
it spends only out of positive net income, and there is none. Both are left in
because they are correct and free, and because the thing standing in their way
is the AI's economy rather than this mechanic. Making it bite means giving AI
countries spare income, which is a change to how they budget and was measured as
expensive the last time it was tried. `OD_AI_DISTRICTS_OFF` and
`OD_AI_PACIFY_OFF` compare the arms.

### Two things the screenshot found

The inline map painted nothing at all: `m_provincePixels` is built lazily -- 128
MB, so nothing pays for it until something needs per-province pixels -- and the
overlay is exactly that. Three districts, three colours in the list, and a map
with no districts on it.

Then it painted them and they still could not be seen. Half-lit district colours
blended into the political map underneath and came out as neither: measured,
ZERO pixels on screen matched the colour in the list, which is the one thing
that map has to make findable. Opaque now, and measured again -- 7143, 8103 and
5076 pixels of the three colours.

### Regional law is its own content, not the doctrine list filtered

The first version let a district run any national doctrine whose effect happened
to be per-province. It worked and it was the wrong idea: a doctrine is a
statement about what a COUNTRY is -- where its compass sits, who it lets in --
and filtering that list gives you the same content under a smaller heading, most
of which means nothing applied to a third of a country.

So regional law is a separate set, in `data/district_laws.json`. Ten laws, and
they are the things a provincial administration actually decides: a curfew, a
tax holiday, whose language the courts run in, martial law, company towns, grain
requisition. Three of them make the ground ANGRIER on purpose -- company towns
and grain requisition buy money with resentment -- so the list is a menu of
trades rather than a menu of improvements, and the panel colours each term by
which way it cuts.

Priced PER PROVINCE, so a law costs what it costs to administer: the same law
over twice the ground is twice the money. That is also what stops a district
being a way to buy a national effect cheaply.

Three effects, each wired where that quantity is actually computed rather than
at the country total: unrest in the rebellion sum, income in
`provinceIndustryIncome`, growth in `growCountryPopulation`. A tax holiday in one
district and company towns in another are two different rates inside one country
on the same turn, which is the whole point and is not expressible at the country
level.

**And then the effect turned out to be dead.** `unrest_reduction` is stored as a
fraction -- Secret Police is 0.05 -- and every display multiplies by 100 to
advertise "5%". The resolver subtracted the raw 0.05 from a figure that has to
clear a loyalty floor of 6.0. A hundredfold unit mismatch, which means every
"reduces unrest" doctrine in the game has always done nothing.

It is diagnosed exactly in `docs/review-response-2026-08.md`, which also records
it as FIXED and says the field was renamed to `unrestReductionPct`. No such field
exists. That document has been wrong about what is in the tree before, and this
is another instance: read it as a plan, never as a description.

Fixed in one function so the resolver and the label cannot drift apart again.
Measured, `OD_UNREST_UNIT_OLD` restoring the mismatch:

| | rebellions / 1k country-turns |
|---|---|
| 1939, mismatch | 81.3 |
| 1939, fixed | **29.8** |
| 1914, mismatch | 76.9 |
| 1914, fixed | **30.3** |

And on the seat bench, same binary and same model in both arms:

| | OD BENCH | survival | worst seat |
|---|---|---|---|
| mismatch | 72 | 49 | 9 |
| fixed | **121** | **87** | **23** |

That is a large balance change and it is the correct one: unrest was running hot
precisely because every countermeasure against it was inert.

## District refinements

**Nearest, not first.** New ground joined `v.front()` on the reasoning that a
district is something the player drew and silently extending one is
presumptuous. Wrong in practice: conquer a province on your eastern border and
it landed in whatever district was first in the list, usually on the far side of
the country. Adjacency wins outright over distance -- a shared border is a
better answer than a centre-to-centre measurement -- and distance catches what
adjacency cannot, because an island touches nothing and still has to land
somewhere. Four checks in `district_rules_test.cpp`.

**Named after the ground.** The first implementation named a district after its
largest province, which cannot work: measured, ZERO of the 1298 provinces on the
1939 map carry a name. `Province::name` is empty on every shipped map, so that
plan could only ever have produced the bare word "County" for every district in
the game.

What the map does know is who lives on the ground and where the ground is. A
people holding 55% of a district lends it their name -- a district drawn around
them is usually drawn around them on purpose -- and otherwise the compass
bearing from the country's own centre, against a span-relative bar so "northern"
means northern OF THIS COUNTRY rather than of some absolute pixel count.

The word itself is a property of the MAP, hashed from its own content (province
count and the first province's name) rather than from a file path that is not
always known or a world seed that changes every new game. One world has
counties, another has oblasts, and it is the same word every session.

**Two collisions caught by looking rather than by shipping.** "March" was in the
region vocabulary until the string table showed it already translated as the
MONTH -- a district would have been called "Bavaria Березень". And the Roman
numerals that disambiguate duplicate names were being harvested as translatable
strings; `i18n-ignore` has to sit within three lines of the table, which is why
the first attempt at that marker did nothing.

## Country profiles

Any country's province panel opens a profile of whoever holds it: how long it
has existed, its population and where that has been going, gross income against
expenses, where it sits on the compass, and the flags it has flown with the turn
each was raised. `Country::foundedTurn` is -1 for a country that was on the map
when the session began, which is why the age line can say "existing since the
start of the session" rather than inventing a birthday.

**The interesting part is the three fields a country chooses to publish** --
expense composition, doctrines in force, treasury at the start of last turn --
because publishing is not flavour. Migrants read it. A country whose disclosed
figures are good draws people to it, so the choice is a real one and a country
with bad figures is better off saying nothing.

**Two measurements shaped it.** The first version handed +50% attractiveness for
one tick box, on a condition most solvent countries meet by default -- a bonus
everyone takes on turn one is not a decision. The ceiling came down to a fifth.

Then the AI turned out to reach that ceiling anyway: 374 published decisions
over a 60-turn world, mean appeal 0.197, 94% of them pinned at the cap, because
the expenses term alone could reach 0.7. **A pull that every country gets in
full moves nobody anywhere.** Each field now has its own share of the ceiling
and the three sum to it exactly, so reaching the cap takes being frugal, calm
AND solvent at once. Re-measured: mean 0.126, 6% at the cap, all four
combinations of published fields appearing.

**The AI decides by asking the same formula the player is shown.** A field goes
out if publishing that field, on its own, is worth something to that country
right now. The alternative -- writing the AI its own rule of thumb -- is a
second copy of a judgement the game already makes, and the two would drift apart
the first time the formula changed. 41 checks in `country_profile_test.cpp`.

## Districts in the map editor

The Countries tab gained a Districts brush beside the claims one: add, name,
colour, set each district's claim on the budget, drag to assign. Districts
partition a country where claims overlap it, which is the whole difference in
the code -- painting a province into one takes it out of the one that held it,
so the invariant the game relies on holds at authoring time too. It travels as
`districts.json`; a country the author divided starts the game divided.

**The AI will not redraw what an author drew.** Its reflex re-cuts a country
into "worst fifth / everything else" every five turns, which would throw away a
scenario's crown lands on turn five. Where the map carries an authored division
the AI keeps the shape and decides only where the money goes.

That re-weighting first produced 94/6 on Austria-Hungary, because the clamp was
applied before the rescale to 100 and the rescale undid it -- a district the
author drew, funded at nothing. Bounded the way the reflex already bounds its
own split, it lands at 75/25.

**Two layout faults the shot caught.** The panel was laid out for 1920 and
photographed at 1600, so the entire side panel sat off the right edge; the
fixture now takes its size from the frame being captured. And the first
district's generated colour came out a sandy tan almost exactly the colour of
unpainted land, so a fully-assigned country looked like a country with no
districts at all.

**The editor's bottom bar was printing its labels through its buttons.** Fixed
pixel offsets measured against English: "Tools" is 33px wide and "Інструменти"
is 96. It measures the labels and lays out from them now, and one function
returns the geometry for both the click pass and the draw pass, so the two
cannot drift.

## Scripting the game the game has become

The language knew about treasuries, populations and borders, and nothing added
since. It can now ask a country for its army by kind, its income and expenses,
what it is worth, how many people live in it, how many research programmes it
runs and how it is cut into districts; and a province for its garrison, its
district and the resolver's own rebellion figure. Garrisons are writable per
kind. Research groups can be forced open or shut and the override is saved.

Sugar: `wait N turns`, `set x to <expr>`, `{value}` interpolation in `print`,
`foreach district`. Every one of them has a palette block in the editor -- which
now scrolls, having quietly outgrown its own panel at seventeen entries and been
painting the rest outside the box.

**Three faults found by running it rather than reading it.** `wait N turns`
parked nothing, because the segment splitter only broke on `waitUntil` and the
statement fell inside a block where "Unknown command: wait" was the only sign.
`foreach district` walked an empty list at map load, because districts are built
lazily and a script runs before anyone has asked -- asking is what creates them.
And the script never ran at all on the test map, because the loader scans the
archive for `scripts/` and then lets `metadata.json` overrule what it found: a
map whose metadata predates its scripts carried scripts that silently never ran.
Metadata can only turn scripting on now, never off.

## The AI was reading the wrong country's unrest

`getProvinceRebellionChance(pid)` is the one-argument form, and it answers "how
likely is this province to revolt AGAINST THE PLAYER" -- the only government the
panels that call it ever ask about. The AI's district reflex called it for every
country it was deciding for, so it cut districts and aimed suppression by a
number belonging to somebody else's government, and in a headless run to nobody
at all.

With the right government asked, the distribution is: over 203 reviews on the
shipped scenarios, median worst-province risk 0.00, p90 3.34, and the worst
province anywhere 10.14. **The bar for spending anything on suppression is 12.**
So the rule could not fire for any country however rich it was, and the empty
treasury documented beside it was never the only thing in its way.

### What it would cost to make the AI actually pacify

With the bar moved down and a floor funded out of GROSS income (both behind
`OD_AI_PACIFY_BAR` and `OD_AI_PACIFY_GROSS`, so an arm that pacifies and an arm
that does not are the same binary), the rule fires: 19 of 217 reviews fund
suppression, at about 6% of income.

The seat bench, three seeds, six seats:

| arm | rating | survival | worst seat |
|---|---|---|---|
| off (today's default) | 124 | 88 | 26 |
| bar 5, gross floor | 120 | 88 | 26 |
| bar 3, gross floor | 111 | 83 | 23 |

A seat score is a SHARE of a world, so a rule that only helps AI countries
lowers it by construction -- the 13 points at bar 3 are what a harder game looks
like, not necessarily a worse one. The question is whether the money buys
anything, and **the world aggregate cannot answer it at this sample size**:
rebellions per 1k country-turns come out 12.87 with the rule off, 11.57 at bar 3
and 17.42 at bar 5. That is not a dose-response, it is three worlds fragmenting
differently, and reading the 10% fall at bar 3 as an effect would have been
reading noise.

So the default stays off, and what is now known is written down rather than
rediscovered: the obstacle was never only the AI's empty treasury -- **the bar
was above the entire distribution of the thing it measured.** Deciding this
properly needs an instrument with more worlds behind it than three.

## Districts in the profile, and the AI that governs them

**What is public and what is not.** A country's districts are drawn on the map
and their borders are visible to anyone looking at it, so the profile shows the
division and the budget split without being asked. What each district *does* --
the regional law it runs -- is a fourth publishable field beside expenses,
doctrines and the treasury. A curfew in one province and a tax holiday in
another is the kind of thing a government says out loud or does not.

Publishing it is worth something only when the laws are worth publishing: the
appeal term reads the same two numbers the resolver uses -- what the law does to
unrest and to growth -- so a country under martial law gains nothing by
announcing it. The four field maxima were rebalanced to 0.07/0.05/0.05/0.03 so
they still sum to the ceiling exactly.

**The expense breakdown is a pie**, from `Game::expenseSlices` -- the same table
the economy screen has drawn since before profiles existed. Two copies of that
table would have drifted the first time a cost was added, which is exactly how
industry upkeep once ended up in the denominator with no wedge.

**AI countries now govern their districts.** The reflex cut them and stopped
there, and since an AI pacification budget is zero the split moved nothing.
Regional law is the part of districts that works without a budget: a curfew
costs money per province and changes unrest whether or not anyone is paying for
suppression. Priced as a share of GROSS income rather than out of what is spare,
for the reason the pacification reflex beside it never fires.

The first pricing counted the bill and ignored the income. A tax holiday costs
0.00 per province in cash and a tenth of the district's earnings, so it looked
free and won **45 times out of 45**. Counting income forgone as money spent gives
a spread over a longer run: 123 tax holidays, 62 grants of language rights, 33
public works, chosen by how much each district has to lose.

Measured on the seat bench, one binary with `OD_AI_DLAW_OFF` separating the arms:

| seeds | rating | survival | worst seat |
|---|---|---|---|
| fixed, off | 109 | 79 | 26 |
| fixed, on | **124** | **87** | 23 |
| hold-out, off | 109 | 79 | 28 |
| hold-out, on | 113 | 78 | **15** |

Sign agrees on rating across both seed sets; survival and the floor do not. Kept
on -- it is the behaviour the mechanic was asked for, and the rating agrees --
but written down here rather than claimed as a clean win.

## Can the AI read a country profile? No, and it does not need to

Asked directly, and the answer is in one line of `AISystem.cpp`:

    r[3] = oc ? std::tanh((float)oc->treasury / 500.0f) : 0.0f;

That is the other country's ACTUAL treasury, and every feature beside it is the
same: army, provinces, industry, war weariness, claims. The AI has perfect
information and has never consulted a profile in its life. A country's choice to
publish or withhold its books therefore changes nothing about how the AI treats
it -- **migration is the only reader of publication in the game.**

`OD_AI_FOG_TREASURY` makes that feature respect publication: the real figure
when the country published it, and otherwise a guess from what cannot be hidden
(factories and ground). It is off by default and should stay off until a model
is trained with it on. Feature 3 has meant "their treasury" for every model in
this repository, and changing what an input MEANS under a frozen policy measures
worse than the idea is -- the same lesson as the masking work.

### And then it was turned on

The switch is now the default, at the user's instruction, with
`OD_AI_FOG_TREASURY_OFF` as the way back. Measured before flipping it, so the
cost is known rather than assumed:

| arm | rating | survival | worst seat |
|---|---|---|---|
| perfect information (old) | 124 | 87 | 23 |
| publication respected (new) | 122 | 87 | 23 |

Two points, inside the noise floor, and the floor and survival do not move at
all. **The reason it is that cheap is worth keeping:** 11.1% of relational reads
are fogged -- 10,972 of 98,420 over a 60-turn scenario run -- because AI
countries publish their treasury about 73% of the time. The feature still
carries the true figure in eight reads out of nine, so a policy that was never
trained on the proxy is only occasionally reading one.

That also says what the retrain is for. The point is not to recover two points;
it is for the policy to learn that feature 3 is sometimes a guess, and to lean
on the rest of the relational slice when it is. Sent to the training session
with the arms spelled out: train with the fog ON, keep the off-switch for the
control arm only, and bench head-to-head in one binary -- a fog-trained model
against a fog-free baseline measures the environment rather than the model.

ParrotZero is 8.4.0 as of this work: the district reflex, the regional law, the
publication decisions and the changed meaning of feature 3 all mean that a
measurement taken before today does not describe this code.

### The district law, measured properly

The first two numbers for this rule were +16 on the bench's three fixed seeds
and +4 on three it had not seen, and it shipped on that. A peer session then
made two arguments that both turned out to matter: those three seeds are FITTED
(every default in AISystem.h was chosen against them; one knob measured +52 on
them and -28 on unseen ones), and the 120-turn horizon is BIASED rather than
merely noisy (rules that ship get bigger at 400, and several that looked good at
120 reverse there).

So it was re-measured at 400 turns, on both world sets, one binary with
`OD_AI_DLAW_OFF` separating the arms:

| instrument | rating | survival | worst seat |
|---|---|---|---|
| 120, fixed | +16 | +8 | **-3** |
| 120, hold-out | +4 | -1 | **-13** |
| 400, hold-out | +29 | +12 | **-10** |
| 400, fixed | +15 | +11 | **-12** |

It did not invert -- it grew, which is what the horizon argument predicts for a
real rule rather than a fitted one. But the column that agrees with itself is
the LAST one, and it says the same thing four times.

**The honest description is not "worth +29". It is "lifts the median seat and
costs the worst one".** An AI country that legislates its way out of trouble is
hardest on a player who is already in the weakest position on the map, which is
where a player feels the AI most. The peer session hit that same trade six times
today under different names -- campaign share, the aggression dial, the war
limit, the reinforcement guard -- and shipped none of them.

Left on here, because the thing worth fixing was an AI that drew districts and
did nothing with them, and because the user asked for exactly that. But the
trade is written down rather than buried in a rating, and it is one environment
variable to reverse.

### Does the policy even read the treasury feature?

Answered by ablation rather than by a gradient, on the peer's suggestion:
`OD_AI_REL_ABLATE_TREASURY=<v>` replaces relational feature 3 with a constant
for every read, and `OD_AI_REL_TREASURY_STATS` reports its mean so the constant
is the feature's own average (0.1041 over 98,420 reads) rather than a flattering
number.

| arm | rating | survival | worst seat |
|---|---|---|---|
| perfect information | 124 | 87 | 23 |
| publication respected (default) | 122 | 87 | 23 |
| feature destroyed | 119 | 86 | 21 |

So the policy does read it, weakly: destroying the feature outright costs 5
points, and fogging 11% of its reads costs 2. **The ablation is an upper bound
on the fog** -- replacing the feature entirely must be at least as damaging as
replacing part of it -- and the fog sits comfortably inside it. **As a post-retrain check the unconditional version cannot fail**, which the
peer session caught before it was used for anything: a policy that has genuinely
learned the mechanic and one that has leaned on the proxy BOTH lose more to it,
the first because it now trusts a real figure it can identify and the second
because it trusts a guess. One number, two opposite stories.

`OD_AI_REL_ABLATE_WHERE=all|fogged|published` separates them by destroying only
one half of the reads. The two conditional modes partition the reads exactly
(15.9% fogged, 84.1% published, verified by counting), and the controls against
today's policy are:

| ablation | rating | survival | worst seat | vs baseline |
|---|---|---|---|---|
| none | 122 | 87 | 23 | — |
| fogged reads only (16%) | 123 | 87 | 23 | ~0 |
| published reads only (84%) | 117 | 84 | 21 | −5 |
| all reads | 119 | 86 | 21 | −3 |

So the test has a BEFORE for both halves. After a retrain: the published loss
growing while the fogged loss stays flat means the policy learned to tell the
two cases apart; the fogged loss growing means it leaned on the proxy; and the
published loss shrinking toward zero means it stopped reading the feature at
all -- a third failure neither reading had named, **and the likeliest of the
three, because it reads as success.** A policy trained where an input is a guess
one read in six can simply stop using it, which improves every aggregate while
discarding a true figure in the 84% of reads where it is true.

**Do these controls belong to a lineage or to the game?** The a priori answer
was "a lineage": a child trained from a different parent has no reason to read
the feature by the same amount, and a cross-lineage control would report the gap
between two policies as though it were the effect of the fog training.

Then it was measured on N24 -- a policy that beats the shipped model 226/87/23
to 85/51/4 at 400 turns, and 187 to 122 un-ablated on this instrument:

| ablation | shipped model | N24 |
|---|---|---|
| fogged reads only | ~0 | +2 |
| published reads only | −5 | −5 |
| all reads | −3 | −5 |

**The same shape, and the same published cost to the point, across a 65-point
gap in strength** -- and on N24 survival and the floor do not move in ANY mode,
so whatever this feature buys, it is not seat survival. The caution was right in
principle and did not bite in fact, which is a better thing to know than the
caution was: the −5 can be quoted without naming a model.

It also sharpens the third failure mode. If a fog-trained child loses much MORE
than 5 to the published ablation, that is probably not "it learned to trust the
real figure" -- there appear to be about five points in this feature to win. It
is likelier to have become dependent on r[3] in a way neither of these policies
is, which is a fragility rather than a skill.

## The relational slice is mostly inert *(RETRACTED — see below)*

Eight features describe a neighbour to the policy: the army ratio, the province
ratio, their industry, their treasury, whether we are at war, whether we are
allied, their war weariness, and their claims against us. Each was replaced with
its own mean on every read, one at a time, on N24 at 120 turns and fixed seeds.

| feature ablated | rating | survival | floor |
|---|---|---|---|
| *(baseline)* | 187 | 90 | 38 |
| r0 army ratio | 181 | **87** | **23** |
| r1 provinces | 177 | 90 | 38 |
| r2 their industry | **212** | 90 | 38 |
| r3 their treasury | 182 | 90 | 38 |
| r4 at war with them | 198 | **87** | **23** |
| r5 allied with them | 188 | 90 | 38 |
| r6 their war weariness | 170 | 90 | 38 |
| r7 their claims | 191 | 90 | 38 |

**Two features of eight touch seat competence, and they are the two anyone would
name: how big their army is next to mine, and whether we are at war.** Both cost
exactly the same, -3 survival and -15 floor. The other six move rating between
-17 and +25 and leave survival and the floor untouched to the digit.

**The rating column is visibly incoherent here**, which is the most direct
evidence yet for reading this bench on survival and floor. Ablating the policy's
knowledge of enemy INDUSTRY *improves* rating by 25, and ablating "at war with
them" -- demonstrably load-bearing, since it is one of the two that costs floor
-- improves it by 11. Ranked on rating, this sweep recommends making the AI
blind to enemy industry as the single best available improvement.

Two things make the table readable rather than suggestive, and both were fixed
before it was run:

- **The arms that should move were named in advance.** r0 and r4 were predicted
  to be the load-bearing pair, in writing, before any number existed. They were.
- **One arm was included that MUST NOT move.** r5 is nearly constant already
  (alliances are rare; its mean is 0.0370), so ablating it to its own mean is a
  no-op by construction. It came back +1/0/0. A sweep without that arm cannot
  tell a finding from a broken knob.

r3 also reproduced its -5 through a second, independently written knob, which
cross-checks the two implementations.

**What it means for the fog.** The treasury feature is one of the six inert
ones, so the disclosure mechanic cannot cost seat survival however the retrain
goes -- it can only move the column that changes sign between world sets. A
fog-trained child should therefore be judged on whether it holds its seats, and
the r3 ablation delta is a diagnostic rather than a verdict.

**And what it means more generally.** Whatever makes a strong model strong, it
is not detail in how it reads its neighbours: six of the eight inputs describing
one are not paying for seat competence at all.

### Except that none of the above survived the second mode

Everything in the section above was measured by replacing a feature with its
mean. Rotating it between candidates instead is equally defensible, and the two
disagree on six of eight features:

| feature | constant | rotated |
|---|---|---|
| army ratio | 181 | 182 |
| provinces | 177 | 203 |
| their industry | 212 | 179 |
| treasury | 182 | 199 |
| at war | 198 | 180 |
| allied | 188 | 182 |
| weariness | 170 | 188 |
| their claims | 191 | 205 |

**"Two features carry the seat" is dead, not narrowed.** Rotated, `r4` costs
nothing at all and leaves survival and floor at baseline — so the constant mode
did not mis-measure that row's magnitude, it INVENTED the effect: the -15 floor
came entirely from the uniform false war state a mean imposes on a binary. Those
are different failures and the second is the one that happened.

`r5` also stops being a valid control under rotation: rotating a rare binary is
informative, because it tells a country its ally is somebody else, which is a
lie about a real fact. It moves play by 5 points. So the rotate sweep has no bug
check — a rotate-mode control must be a feature CONSTANT ACROSS CANDIDATES, and
the only one that qualifies is `r6` since the weariness fog.

**And the bench is deterministic**: two runs of the same configuration agree to
four decimals on all six seats, so there is no noise to blame. The numbers are
real changes in play and still unrankable — one altered decision cascades over
120 turns into a different world, so 4% of the map can be a genuine consequence
of a trivial perturbation rather than a measure of the feature. Real and
unrankable are not in tension.

**What ablation can still do** is show that a feature does something: `r5`
rotated moves play, `r5` constant does not. What it cannot do is say what a
feature is worth. For that the comparison has to run across many worlds rather
than many corruptions of one — the seed-set lesson arriving from a new
direction.

The one thing both modes agree on is that weariness barely does anything, which
is a coherent story rather than a coincidence: an inert feature has no cascade
to amplify, so both corruptions of it land in the same world. That is also why
fogging it cost nothing.

### The floor is one seat, and it was hiding the district law's real shape

The district-law rule was reported here as "lifts the median seat and costs the
worst one", on a floor that fell in all four instruments. Then a peer session,
chasing a different question, opened the per-seat tables that were sitting in
`build/od_bench_results.json` the whole time -- and found that the floor is
**1939:NOR:hood in every arm of every run either of us has done.**

Read per seat, the district law looks nothing like the summary line:

| instrument | Norway, off → on | the other five seats |
|---|---|---|
| 120 fixed | 0.3 → 0.3 | four up, one flat |
| 120 hold-out | 0.4 → 0.2 | four up, one flat |
| 400 hold-out | 0.4 → 0.3 | four up, one down 0.2 |
| 400 fixed | 0.5 → 0.3 | three up, two down 0.4 |

Those are percentages of the world's land. **The seat that carries the floor
holds half a percent of the world**, so its par is tiny and a tenth of a percent
of land becomes fifteen points of normalised score. The "floor -13" that was
treated as the stable signal, four times, is Norway losing 0.2% of the map --
while France gains 3.9 and the United States 3.0 in the same arm.

Two lessons, and the second is the one worth keeping:

- **A normalised score divides by par, and a seat with a small par amplifies.**
  The floor column is not wrong, but on a seat that holds almost nothing it
  converts noise-sized changes in land into large-looking changes in score.
- **When two arms disagree, read the seats before theorising about why.** This
  is the third time in one session that the aggregate hid the answer: it hid
  which features were load-bearing (until the sweep), it hid that r0 and r4
  break the same seat rather than sharing a pathway, and it hid that this rule
  helps five seats and moves a sixth by a fifth of a percent.

And the correction runs further than this rule. The floor was preferred to
rating because it is CONSISTENT across world sets -- but it is consistent
*because* it magnifies whichever seat is nearest zero, which is the same fragile
seat every time. Stability is not validity. Read in land rather than in score,
the ablation sweep's own headline weakens too: r0 costs Norway 0.2% of the map
(15 points of floor) and China 1.57% (five times the territory, reported only in
the rating column). **Neither normalised column is trustworthy on a small-par
seat; the land table is the one that does not amplify.**

- **Verify an ablation at the vector, not at the outcome.** A no-op control
  proves a knob does not fire spuriously; it cannot prove it fires COMPLETELY,
  and a multi-feature ablation that quietly applied only its first index would
  pass one. `OD_REL_DUMP` prints the candidate vectors, so three turns of it
  shows exactly which features are held -- where a bench arm can only infer it
  from eighteen runs of outcome. Confirm what is held, then measure.

- **And verify it WITHIN one run.** The obvious way to check a vector-level
  change is to dump a baseline run and an ablated run and compare them. That is
  invalid, and it produced a convincing false alarm here: the ablation changes
  decisions, the two runs diverge immediately, and by the time the same country
  is dumped again it is in a different world. A rotation that had preserved its
  multiset perfectly looked like it was deleting values. The check has to print
  before and after side by side inside the run that does the work.

### You cannot ablate a bit to nothing, only to a lie

`r[4]` (at war) and `r[5]` (allied) are binary, and holding a bit at its mean
does not remove information -- it injects a falsehood. Their "means", 0.2267 and
0.0370, are simply the fraction of country-pairs at war and allied, so holding
`r[4]` there tells the policy *"you are 23% at war with everybody"*, including
the country it is actually fighting and the five it is not. That is a different
intervention from "you do not know who you are at war with", and it is the one
the sweep ran -- so the `r4` row measures believing a uniform false war state.
It also partly explains `r5` being flat: at 0.0370 the ablation barely changes
the input for the 96% of pairs that are not allied, so the arm was quieter than
the policy's indifference alone would make it.

**Rotating the feature across the candidates is the neutral version.**
`OD_AI_REL_ABLATE_MODE=rotate` moves each candidate's value to its neighbour, so
the multiset is preserved exactly -- the policy still sees that it is at war with
one of these six -- and only the PAIRING is destroyed, which is the information
the feature actually carries. Deterministic by construction, because a shuffle in
a simulation that must replay identically is not available, and a rotation by one
is a derangement whenever there are two or more candidates. Verified in-run over
1,908 rotations, 774 of them with a war flag set: multiset preserved and order
changed in every one.

It is the stricter ablation for the six continuous features too. A constant
removes "which neighbour" AND "what the neighbourhood looks like"; a rotation
removes only the first.

## Auditing the rest of what the AI knows

The treasury fog raised the obvious question: what else does the AI read that a
player cannot? Answered feature by feature against the UI rather than by
argument.

| feature | can a player see it? |
|---|---|
| army ratio | yes — foreign garrisons are listed in the province panel |
| provinces | yes — the map |
| their industry | yes — the industry view, for any province |
| their treasury | only when published (fogged since this release) |
| at war / allied | yes — relations are listed on a country |
| their claims | yes — the claims screen |
| **war weariness** | **no — drawn nowhere in the game** |

`warWearinessOf` had exactly one caller, the resolver. So a country's exhaustion
was invisible to a player by any route, and the AI read it exactly. It now reads
the world's MEAN — fogged to a value rather than to a proxy, because unlike the
treasury there is no visible quantity to build a proxy from.

**And this is where the lineage caution finally bit.** On the shipped model the
fog is free: 122 → 123 rating, survival and floor untouched. On N24 it costs 8:
187 → 179, survival and floor untouched. Same change, same binary, two policies.

The earlier result that the treasury ablation was identical across those same
two lineages — which retired the caution as "right in principle, did not bite" —
does NOT generalise from one feature to another. It was true of `r3` and is
false of `r6`. So the conclusion to carry forward is the weaker one: sensitivity
has to be checked per feature, and a number quoted without naming the model is a
number missing a column.

The price sits entirely in the unstable column either way. The argument for the
change is symmetry with the player, not a rating.

**What is left is effort, not access.** The AI reads country-wide aggregates a
player would have to assemble by clicking every province. That is a real
advantage and a different kind: a spreadsheet closes it, a fog does not. Its
trend features (96-103) read only its own country, so there is no second leak
hiding there.

**And the audit found a missing UI element rather than only an AI advantage.**
The player cannot see their OWN war weariness either, though it drives their
unrest — so a country could slide toward revolt because of a war it answered ten
turns ago with nothing on screen naming the cause. It now appears under the
unrest bar in Politics.

### A side effect: rotate mode finally has a bug check

The rotate sweep replaced the constant sweep and lost its control in the process
— `r5` (allied) works as a no-op under a constant near zero, but rotating a rare
binary is informative (it tells a country its ally is somebody else, which is a
lie about a real fact), and it moves play by 5 points. A rotate-mode control has
to be a feature that is CONSTANT ACROSS CANDIDATES, since rotating identical
values is a no-op by construction, and the slice had none.

It has one now. With the weariness fog on, `r6` is the world mean for every
neighbour — identical by construction — so rotating it must return the baseline
exactly. If it does not, the rotate path is broken.

### How far back the magnifier reaches

Once the floor turned out to be a magnifier rather than a microscope, the
obvious question was how much of this project's measured history it touches.
Answered from the stored results rather than by argument -- every labelled run
keeps its per-seat table:

    which seat carries the floor, across all 688 stored runs
      1939:NOR:hood      531
      modern:CHN:rung     76
      1914:SWE:rung       57
      1914:FRA:rush       17
      1939:USA:rung        4
      1914:FRA:rung        3

    NOR:hood land share over the 683 runs that include it:
      min 0.00   median 0.37   max 13.37   (percent of the world)

**Three quarters of every measurement stored here used one seat holding about a
third of one percent as its floor**, on a score that divides by par and so
multiplies that seat's wobbles by roughly 77.

The first old finding checked against this did not survive it. Campaigns were
recorded as "+66 rating and -20 worst seat", and the trade in that sentence is
what a session was later spent trying to split. In land:

| seat | campaigns off | campaigns on | delta |
|---|---|---|---|
| 1939:NOR:hood | 0.50 | 0.23 | **-0.27** |
| 1914:SWE:rung | 0.87 | 3.20 | +2.33 |
| modern:CHN:rung | 2.50 | 4.13 | +1.63 |
| 1914:FRA:rush | 9.40 | 10.20 | +0.80 |
| 1914:FRA:rung | 19.80 | 24.27 | +4.47 |
| 1939:USA:rung | 19.73 | 21.83 | +2.10 |

Campaigns take a quarter of one percent off one seat and add between 0.8 and 4.5
percent to the other five. The "-20 worst seat" was that first row and nothing
else, and the experiments aimed at recovering it -- recall, home-first, a share
sweep -- were aimed at a cost that mostly did not exist.

**So the rule is not "the floor lied about campaigns". It is that any floor
delta in this project's history is suspect until it has been re-read in land**,
and the conversion is in the stored tables for anything measured since they
existed. The entry in `AIVersion.h` claiming "worst seat 28 -> 110" predates
them and is annotated as unsourced rather than deleted, because it is one of the
two measurements a shipped default rests on.

## A loader that answered "no file" for a file it had just read

Found by a peer session trying to bench two league snapshots: the AI's model
loader reported every kind of refusal as an absence.

    [AI] Fresh model (no file at /.../model.LG1.bin)

for a file that existed and had just been copied there on purpose. Wrong magic,
corrupt container, a format byte from another architecture, a pre-trunk
layout -- all of them returned the same `false` to a caller whose only message
was "no file". **So an evaluation pointed at a model the loader disliked did not
fail. It played an untrained network and printed an ordinary-looking score**, and
every number from such a run is a measurement of random weights under a trained
model's name.

The refusal now names its reason and says what the run is about to do, on
stderr. And the mirror-image fault -- `OD_EVAL_MODEL` naming a path that does
not exist, which fell through in silence to `data/ai/model.bin` and evaluated a
different model under the requested one's name -- now stops the run. That one
was the likelier accident: it needs no format mismatch, only a typo in a
variable that gets set by hand all day.

**Whether it ever bit cannot be settled from the archive.** All 684 six-seat
runs were ranked by total land held (median 49.37%) on the theory that a fresh
net sinks to the bottom; the low end is populated by 400-turn runs of models
that were genuinely weak. The one row that could not be accounted for from its
label -- `i18-B`, holding 3% of the map over 120 turns -- was chased down and is
a real model: 3,749,187 bytes, within 2% of a known-good file, so it parsed and
loaded and was simply playing badly.

But that chase is the actual finding. **Why `i18-B` existed cannot be
recovered** -- its `ai_version` is null, which dates it before the AI carried a
version at all, and the journal has no entry for that series. So "was this an
intentionally bad arm or a failure" is unanswerable, and a fresh-net run and a
genuinely weak model leave the same trace either way. The stored results record
the seeds and the turn count and nothing about which file was loaded, which
build produced it, or what was being asked. The fix prevents the loader fault
going forward; no retrospective audit can clear the past.

The obvious follow-up is to store the model path and its size beside the seeds,
so the question is answerable next time rather than arguable.

## The retrain, and the one result that did not shrink

The treasury fog was shipped with a retrain attached: the policy had to learn
that relational feature 3 is sometimes a proxy. The run happened — 6,289 turns
of self-play from N24, 358M updates, fog on, `data/ai/model.bin` protected by a
redirected data tree and hash-verified untouched throughout — and the child was
far worse than its parent, head-to-head on one binary at 400 turns:

| seed set | N24 land | child land | rating | survival |
|---|---|---|---|---|
| C | 55.73% | 11.60% | 226 → 73 | 87 → 53 |
| D | 69.50% | 9.60% | 286 → 63 | 90 → 52 |

**It does not reverse on the second set, it deepens.** That makes it the only
conclusion of the day that survived a second world set without shrinking, and
the contrast is the lesson: every effect that needed a careful instrument turned
out to be noise or artefact — the feature ablations, the floor column, the
district-law trade — and the one real effect was visible from across the room.

Neither of the two corrections made to the plan was the cause. The fog was in
the training environment as intended, and the parent was N24 rather than the
weaker shipped model. So the problem sits upstream of both: **a short
unsupervised self-play run from a strong parent, with this reward, makes the
policy worse.** Not evidence that training cannot work here — N24 came out of
training — but evidence that a retrain needs a selection step rather than a
duration.

**And it cannot be diagnosed, which is its own finding.** `--train-ai`
overwrites `model.bin` every sixty seconds, so the run kept one moving file and
no checkpoints. Monotonic decline from the parent, a flat run then a cliff, and
up-then-down all produce this end state and need different fixes — the third
would mean a better model existed in the middle and selection alone recovers it.
Separating them now needs a rerun. Four snapshots would have cost 15 MB.

What this leaves for the fog: it is a rule the current model plays under
slightly imperfectly, at a measured cost of 2 rating points and nothing on
survival or floor, with `r3` inert under both ablation modes. A run that lost
44-60% of the map did not lose it to one input in nine reads.

### Did the magnifier reverse a decision? No — and that is the better lesson

The floor artefact was found while re-reading a rule that had been SHIPPED on
it. The more expensive direction turned up afterwards: a rule that had been
REJECTED on it.

`concurrent wars 2`, hold-out set A, 120 turns, read in land:

| seat | before | after | delta |
|---|---|---|---|
| 1939:USA | 18.13 | 30.83 | **+12.70** |
| 1914:FRA | 16.70 | 21.10 | +4.40 |
| 1914:SWE | 2.00 | 1.83 | −0.17 |
| modern:CHN | 6.00 | 4.77 | −1.23 |
| 1939:NOR | 2.00 | 0.23 | −1.77 |
| 1914:FRA:rush | 15.67 | 12.43 | −3.23 |
| **total** | **60.50** | **71.20** | **+10.70% of the world** |

It was rejected because the rating was flat and the floor "collapsed" from 154
to 18 — which is Norway going from 2.00% to 0.23% of the map. **A verdict on a
change worth ten percent of the world was decided by the smallest seat on the
bench.**

It is the same shape both sessions kept meeting — large safe seats gain, exposed
ones lose — but in land the gains are an order of magnitude larger than the
losses, where in floor-score the smallest seat dominates and flips the sign.

That was measured at 120 turns on one set, and the concern registered before the
re-test was that +10.70% resting on one seat gaining 12.70 is one seat's story
until a second measurement repeats it. **It did not repeat. It swapped:**

| seat | 1 war | 2 wars | delta |
|---|---|---|---|
| 1914:SWE:rung | 2.67 | 12.27 | +9.60 |
| modern:CHN:rung | 5.73 | 0.20 | −5.53 |
| 1939:USA:rung | 19.93 | 9.63 | **−10.30** |
| 1914:FRA:rung | 18.17 | 23.77 | +5.60 |
| **total** | **54.63** | **53.00** | **−1.63** |

The United States goes from +12.70 at 120 turns to −10.30 at 400; Sweden from
−0.17 to +9.60. At the horizon that decides, two concurrent wars costs 1.6% of
the world.

**So the original rejection was right and its reasoning was invalid, and those
are separable.** It was rejected on a floor collapse that was Norway moving 0.2%
of the map through a 77x magnifier — bad reasoning — and the verdict it produced
was correct anyway. A broken metric can still point the right way.

That is the useful form of this whole episode. Finding that a metric is broken
licenses re-examining the REASONING, not automatically the conclusion; and
re-examining costs a bench run, so the correct response to "that metric was
broken" is to re-measure rather than to assume the verdict flips. Both sessions
made the opposite assumption once each on the same day — about the district law
and about this rejection.

All three rejections re-read this way survived: two on the land re-reading (a
share of 0.20 loses 2.2% of the world; a cap of 2 is +0.6% and within noise) and
this one on re-measurement. **Three for three on a metric there was good reason
to distrust**, which is worth stating beside the magnifier finding so it does not
read as "everything measured on the floor is wrong".

## The districts tab was paying for an index it never needed

Opening Politics → Districts stalled for an eighth of a second, and every stroke
of the district brush cost most of that again. Timed rather than guessed:

    ensureProvincePixels      68 ms   (33,554,432 px, 1247 provinces)
    district overlay repaint  58 ms   (8192x4096)

`ensureProvincePixels` builds a province-to-pixels index across the whole map --
one int per map pixel, 128 MB, held for the rest of the session -- and the
overlay then cleared, repainted and uploaded a full-map RGBA buffer, 134 MB,
every time a district changed.

**The index was never needed here.** `m_countryPixels[cid]` is built during load
and already lists every pixel a country owns: about 1.9 million for the largest
empire on the 1914 map, thirty times less than the world. The only pixels this
overlay ever touches are that country's, so the same pass can clear and repaint
them, and the upload can be the rectangle they occupy rather than the map.

The overlay is also kept at HALF resolution in each direction. It is drawn into
a panel a few hundred pixels wide -- scaled down twentyfold before any zoom --
so full resolution bought detail nothing could see and cost four times the
clear, the copy and the upload. The draw halves its source rectangle to match.

    before   126 ms   (68 index + 58 repaint), and 58 ms per brush stroke
    after     23 ms   and no 128 MB index

Measured on the worst case in the game: an empire whose provinces are scattered
across every continent, so its bounding box is the whole map and nothing is
saved by cropping.

**And the same map now appears in the country profile**, framed on the country
rather than the world -- a list of names and shares says how a country is
divided without saying where, and the same rows describe a country split
east-to-west and one split between a heartland and an island chain. The first
attempt drew the world three times side by side: a source rectangle pushed past
the texture's edge does not clip, it tiles, and a globe-spanning empire's
bounding box does exactly that once it is fitted to the frame's aspect.

### A string table that moves the simulation

District naming needed `properPlaceName` — the irregular table that turns a
demonym into a place, "french" into France — which lived in an anonymous
namespace beside the breakaway namer. Moving it into `politid` so both callers
share one table is inert. **Adding eight entries to it is not.**

Inside `deriveTerritory`, that lookup sits at the top and the first `simRand()`
is fifty lines below, in the suffix trials that invent a name when derivation
fails. An entry that now HITS returns before those draws happen, so a breakaway
named from one of those demonyms consumes fewer draws than it used to — and
every subsequent draw in that world shifts.

Nothing in the diff says "RNG". The table is a string map, the edit is data, and
a reviewer looking for simulation changes greps for `simRand` and finds nothing
in the patch.

Measured, same seed, both arms on one tree:

    with the new irregulars    war 17.32 / 8.48   unrest 12.65 / 190.65
    without them               war 17.32 / 8.48   unrest 12.65 / 190.65

Identical to four figures — none of the eight demonyms produced a breakaway over
3 maps × 120 turns. **That is not a clean bill of health**, and it was reported
to the training session as "did not fire is not cannot fire": a 1500-turn run
with Russian and Italian minorities on the map is a different proposition.

It also separates two things that had been running together all day:

- **Comparison validity** — every *benched* arm must share one binary. Fatal
  when broken, and cheap to satisfy.
- **Environment match** — a model *trained* on one binary and judged on
  another. Weaker, sometimes worth accepting deliberately: the fog-trained
  child's mismatch measured 8 rating points, and a name table that may never
  fire is smaller than that, against a training collapse of 44-60% of the world.

The second is acceptable when the effect being looked for is far larger than the
mismatch. It has to be named and sized rather than assumed to be zero.

## A doubled doctrine, found by a screen that names things

The country profile gained a list of which doctrines a country publishes, and it
immediately printed four entries for two doctrines. The map data was clean, so
it was the load:

    unloadGameData        clears m_activePolicies and its per-country index
    map load              applyStartingPolicies() FILLS both
    loadStateJson         APPENDS the save's policies to both

So every continued game held each doctrine twice. Doctrine upkeep is summed over
the active list, and so is `policyUnrestPct` -- meaning doubled spending and
doubled unrest reduction, for the player and for every AI country, in any game
that was ever loaded rather than started.

**Nothing displayed it because nothing enumerated it.** A doubled cost inside a
sum is invisible; the profile made it visible by printing names, and two
identical names in a list is a thing a person notices instantly.

The clear goes INSIDE the `contains("activePolicies")` guard, which matters: a
save carrying the key holds the whole truth and should replace the map's
starting set, while a save written before that key existed carries none, and
clearing unconditionally would strip every doctrine off an old game instead of
doubling them.

**The generalisation is the load sequence's assumption**, not the policies: the
save read assumes a clean slate that the map load has already filled. Everything
else `loadStateJson` writes was audited for the same shape -- the pending order
queues start empty, research nodes go into a `set` (idempotent), compasses,
ethnic policies and relations are assigned rather than appended, and claims,
battles and districts clear themselves first. The policy pair was the only one
in that position.

It also cannot have reached any stored measurement: `--eval-ai` and `--train-ai`
enter the game only through `startNewGameWithName`, and `loadStateJson` has
three callers, none of which is on that path. The bench archive is clean of it.

### Stopping it was not the same as repairing it

A save WRITTEN during the bug window holds the doubled list, and after the fix
that save replaces the map's set with a doubled one and stays doubled forever --
carrying the key is precisely the signal that it holds the whole truth.

Worse, the old behaviour COMPOUNDED: each load added the starting set to
whatever the save held, so a copy accumulated per load-and-save cycle. Measured
rather than assumed, across all 1,301 saves in `data/saves`:

    clean 1251    with exact duplicates 48    unreadable 2
    worst: x7 multiplicity, 572 policy entries of which 529 distinct

A country in that save pays seven times its doctrine upkeep and gets seven times
the unrest reduction, and the symptom is indistinguishable from the game simply
being hard. So duplicates are collapsed as the save is read, and such a game
repairs itself the first time it is opened — verified on that save: `repaired 43
duplicate active doctrine(s)`, exactly 572 − 529.

**The key is the whole tuple**, not the policy id: country, policy, target
province, target minority. The same doctrine can legitimately be active twice
against different targets — an ethnic policy applied to two minorities is two
rows differing only in `targetMinority` — so collapsing by id would silently
repeal one of them. Two rows identical in all four fields are one fact stated
twice, and there is no state in which that is meaningful.

**Two different claims, and only one of them was measured.** Across all 1,301
saves: 48 affected, 1,172 duplicate rows in total, and **zero** `(country,
policy)` pairs carrying more than one distinct target. That last number says the
repair was safe on every file available to inspect. It does NOT say the key is
right — the key is right because of what `ActivePolicy` holds, which is an
argument from the data model and applies to saves nobody here can see. The
measurement is what stops that argument from being the only thing standing
between a repair and a silent repeal, and the two are worth keeping apart: a
scan of the corpus can only ever license "safe here", never "correct".

The damage is also a function of how much a save has been PLAYED, since a copy
accrues per load-and-save cycle. It falls hardest on the longest games — the
ones a player is least willing to abandon, and most likely to blame themselves
for finding hard.
