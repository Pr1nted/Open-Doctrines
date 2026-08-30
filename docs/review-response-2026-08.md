# Acting on the 5★ itch.io review (Aug 2026)

The tutorial complaint is deliberately out of scope here — it needs an animation
and rigging system that does not exist yet. This document covers the other five.

Everything below was measured against `61dfd20` on a fresh
`cmake --build cmake-build-debug/` build, either with `--eval-ai` or by reading
the shipped `data/STDmaps/map.odmap` raster directly. Numbers are quoted so the
same commands can say whether a change worked.

---

## 0. What the measurements say

`./cmake-build-debug/OpenDoctrines.app/Contents/MacOS/OpenDoctrines --eval-ai 2 200 20260801 2 --scenarios`
— 1914 and 1918, 200 turns each, 53 and 61 countries:

| Metric | Reading | Reviewer's words |
|---|---|---|
| `unrest` | **14.6 rebellions per 1k country-turns** (≈0.7 new rebel states per turn, world-wide, with an AI that keeps mean alignment at 97%) | "around three revolts per turn" |
| `routing` | **26,965 of 28,599 ship moves (94%) stopped dead by land** | "painfully slow… tedious to port armies anywhere" |
| `sea combat` | **13 engagements, 0 hulls sunk** across 400 turns and ~340 hulls | "destroyers and carriers never attack one another" |
| `amphibious` | 81% of 561 embarkations reached a hostile shore; 235 came home | — |
| `compass` | mean government-vs-province gap 0.310, worst 0.697 | — |

Raster analysis of `provinces.png` (8192×4096, 1,642 provinces):

| Reading | |
|---|---|
| **442 provinces (27%) have more than one connected component** | |
| **55 provinces' pixel-mean centre lands outside their own pixels** | worst: 452 px away |
| **9 of those land inside a *different* province** | Russia #490's army marker draws **in Finland**, 3,413 px from its own land |
| 9 provinces below the 80 px merge floor; smallest is 2 px (`Norway #830` is 4 px) | "some absolutely minuscule provinces exist" |
| 64 provinces under 200 px against a median of 2,782 px | |

Doctrine data (`data/policies.json`, `starting_policies.json`):

| Reading | |
|---|---|
| 17 doctrines total | reviewer counted "thirteen plus two" |
| No cap on simultaneous doctrines; the limit is **3 enactments per turn** (`Game_Policies.cpp:965`) | reviewer guessed "six" |
| 8–16 of 17 pass the compass gate at start, median 11 | |
| **41 countries start with 1–4 doctrines already in force** | "depending on the nation… already mostly booked" |

---

## 1. Unrest and revolution

### What is actually happening

Three separate things, only one of which is balance.

**(a) The roll is per province, so revolts scale linearly with how much land you
hold.** `processRebellions` (`Game_TurnLogic.cpp:2360`) walks every owned
province and rolls an independent Bernoulli against
`getProvinceRebellionChance`. A per-province chance that reads as harmless — 1%,
say — is one revolt per turn at 100 provinces and three at 300. The reviewer
found this himself: *"especially when dealing with huge empires"*. No amount of
retuning the chance fixes a model whose expected event count is proportional to
the player's success.

**(b) A country's own people are modelled as half-alienated.**
`getMinorityAlignment` (`Game_Policies.cpp:774`) returns `50 + drift`, and drift
starts empty — so on turn 1 the Han in China, the Russians in Russia and the
English in England are all 50% aligned, and each contributes
`((100-50)/100 × share)² × 5` to their province's unrest. That is exactly the
reported *"even dominant socioeconomic ethnicities OFTEN revolt"*. There is no
concept of a titular or core group anywhere in the model.

**(c) Grievance accumulates and never decays; services can be outrun.** The
policy dial tops out at **+18.5 alignment/turn** with every category set to its
most generous option, and `MINORITY_DRIFT_LIMIT` is ±50 — so maxed services
*should* reach 100% alignment in three turns. They do not, because war stamps
permanent penalties on the same number: `declareWar` applies **−30 per minority**
(`Game_TurnLogic.cpp:3712`), conquest applies **−25 per province taken**
(`:5018`), and holding conquered ground against an enemy you are still fighting
subtracts a further **−5/turn** (`Game_Policies.cpp:851`). A conquering empire
is pinned at the −50 floor no matter what it spends. That is the reviewer's
*"even when these ethnic groups are given the maximum services they require,
they still have very little appeasement"*.

Worth knowing: on the shipped modern map at turn 0, **essentially every province
is below the loyalty floor** — mean base 2.1, ethnic 0.7, claims 0.06 for Russia
against `REBELLION_LOYALTY_FLOOR = 6`. All the pressure the reviewer feels is
dynamic: war weariness, `BANKRUPTCY_UNREST_PCT = 20`, at-war claims (6 per
claimant instead of 2), and the conquest drift above. The static tuning is fine;
the wartime tuning is not.

**(d) Two outright bugs make the countermeasures invisible.**

- `unrest_reduction` is a **unit mismatch of 100×**. The tooltip prints
  `unrestReduction * 100` (`Game_Policies.cpp:1222`) — Secret Police advertises
  "5%" — but the resolver does `total -= p.effect.unrestReduction`
  (`:735`), subtracting **0.05** from a figure that must clear a floor of 6 to
  matter. Every "reduces unrest" doctrine in the game does nothing. This is
  precisely *"I didn't really notice them having much of an effect"*.
- The Analysis tab's hotspot classifier computes political unrest with `+`
  (`Game_Policies.cpp:1437`) where the resolver uses `−` (`:678`) — the same
  sign bug that was already fixed once in the resolver. The *total* it shows is
  right; the Ethnic/Pol/Econ label next to it is derived backwards, so the one
  screen that tells you *why* a province is unhappy names the wrong cause.

### Design choices

**1.1 — One revolt roll per country per turn, not one per province.**
Compute a national revolt pressure from the *worst* province (or the top‑k), roll
once, and if it fires seed the faction BFS at that province and flood outward
through provinces that share its grievance. Consequences:

- Three revolts in one turn becomes structurally impossible.
- A large empire is no more revolt-prone per turn than a small one, but its
  revolts are *bigger* — an entire disaffected region leaves at once instead of
  three scattered one-province states. That is a better story and less clicking.
- The existing BFS at `Game_TurnLogic.cpp:2393` already does the flooding; it
  just needs to be seeded deliberately rather than fed a list of independent
  winners.

Rejected alternative: dividing per-province chance by `sqrt(N)`. It produces the
right event count and no legible model — the player cannot reason about a number
that changes when they take a province on the other side of the world.

**1.2 — An escalation ladder, not instant secession.** Content → Discontent →
Unrest → Insurrection → Secession, with a province required to hold at
Insurrection for K turns before it breaks away. The player gets a named warning,
a visible countdown, and turns in which to garrison, pacify, or concede
autonomy. This is what converts an annoyance into a decision, and it means a
revolt is never a surprise.

**1.3 — Core groups start loyal.** Add a per-country core group set (derivable
from the largest group by national population in `tools/data/ethnic_groups.json`)
whose base alignment is high — 85, not 50 — and whose ethnic-unrest coefficient
is near zero while it holds power. Sanity check for the model: a homogeneous,
peaceful, ideologically-aligned province must sit at exactly zero unrest without
spending anything on pacification.

**1.4 — Grievance decays.** Replace the flat, permanent −30/−25 stamps with a
decaying penalty (half-life ~10 turns) so that services always win eventually
and a war does not permanently disqualify an empire from governing well. The
±50 clamp stays; what changes is that the floor is no longer sticky.

**1.5 — Service costs scale with population share.** `computeCountryIncome`
(`Game_Economy.cpp:94–108`) charges a flat per-group price, deduplicated by name
across the country — so an empire with 40 distinct groups pays 40 × 7.5 = 300/turn
for maximum services, and a 0.3%-of-population group costs exactly as much as
the titular majority. Weight the bill by each group's national population share.
(Same principle as the per-minority rate weighting already noted for
`minorities.json`: share-weight, never sum.)

**1.6 — Show the arithmetic.** The province panel prints a single
`Unrest: X%` (`Game_Render.cpp:729`). Break it out: base / political / ethnic /
claims / war weariness / bankruptcy, minus pacification, minus the loyalty
floor, with the dominant term named and the distance to the floor shown. A
player who can see "Ethnic: +4.2 (Kurdish, 0% aligned)" will go and fix it; a
player who sees "31%" will not.

**1.7 — Fix the two bugs.** `unrest_reduction` unit, Analysis-tab sign. Both are
one-line changes; both make an existing system suddenly work.

**How we will know it worked:** `rebellions_k` in the eval CSV should fall from
14.6 toward 3–5, with `align_pct` holding above 90 and `disaffected_pct` still
responsive — a change that drives rebellions to zero by making the model inert
has failed, not succeeded. Also run a large-empire case explicitly; the AI eval
never produces a 300-province player.

---

## 2. The politics tab is sparse and inert

### What is actually happening

Seventeen doctrines is thin, but the deeper problem is that **the compass is a
one-way ratchet**. Every doctrine shifts the government compass, and
`policyBlockReason` (`Game_Research.cpp:562–573`) gates on absolute windows —
so three or four doctrines pin you in a corner and permanently grey out half the
tree. Add the 41 countries that start with doctrines already in force (and
therefore their incompatibles already locked), and a major power really is
"mostly booked" before turn 1.

And of the effects the seventeen doctrines carry, most are compass shifts plus
an unrest reduction that is 100× too small to observe (§1.7). Two —
`state_industry` and the `pacification_cost` grants — touch anything the player
watches.

### Design choices

**2.1 — Requirements become cost, not a gate.** An out-of-window doctrine stays
enactable: it takes longer, costs more, and carries a legitimacy or unrest hit
proportional to how far outside your compass it is. Repealing shifts back. The
player is then always choosing between doctrines rather than reading a greyed-out
list, and an ideological turn becomes an expensive, deliberate act rather than
an impossible one.

**2.2 — Every doctrine must move a number the player already watches.** The
engine has plenty of unclaimed levers: `conscriptionPct`, `industryCostPct`,
research rate, `navyCostPct`/`navySpeedPct`, pacification budget, minority drift
rate, claim pressure, war weariness decay. Bind each doctrine to at least one,
with the magnitude large enough to feel — and make the tradeoff text state the
real number, checked against the resolver.

**2.3 — Political capital instead of "3 actions per turn".** The current limit
(`Game_Policies.cpp:965`) is arbitrary and invisible in its consequences.
A capital pool that accrues per turn (modified by stability, doctrine, and
compass alignment) and is spent on enactment turns doctrine choice into an
economy with saving and timing in it.

**2.4 — More doctrines, in opposed pairs.** Target ~30. Add them as genuine
opportunity costs — Total War Economy vs Consumer Economy, Colonial Office vs
Commonwealth Settlement, Autarky vs Free Trade — rather than as a longer list of
individually-mild modifiers. A handful unlocked by research or by events gives
the tab something to reveal over a campaign.

---

## 3. Ships and moving armies across water

This is the largest win available, and most of it is not balance.

### What is actually happening

**(a) The player cannot give a ship an order longer than one turn.** The move
overlay (`Game_Render.cpp:2994–3035`) refuses any destination beyond
`shipMaxRangePx` *and* any destination whose straight line crosses land
(`waterPathClear`). So an ocean crossing is: select ship → enter move mode →
find a point that is both within ~200 px and on a clear straight water line →
click. Then do it again next turn. Eight times. Per transport.

**(b) The resolver does not path — it clamps.** `processNavyMovement`
(`Game_TurnLogic.cpp:3384–3447`) walks 32 samples along the straight line and
stops at the last water. Any crossing that has to go round a headland stalls.
**94% of ship moves in a 400-turn eval were stopped dead by land.**

**(c) A proper sea router already exists and nothing uses it for movement.**
`navRoute` (`Game_TurnLogic.cpp:5286`) is a BFS over a flood-filled 256×128
water graph and returns waypoints. Its only caller is the AI's aim-point picker
(`AISystem.cpp:4149`). The player's orders never touch it, and neither does the
resolver.

**(d) Raw speed is also slow.** `shipMaxRangePx` (`:5140`) gives a transport
200 px/turn on an 8192 px map = 8.8° ≈ 980 km. `navy5` adds +25%, so the ceiling
is 250 px. An Atlantic crossing is 6–7 turns of sailing plus a turn to embark —
the reviewer's "around eight turns", exactly.

**(e) Naval combat is entirely opt-in and therefore never happens.**
`processNavyCombat` (`:3453`) iterates `m_pendingShipEngageOrders` and nothing
else. Two hostile fleets can sit on top of each other forever. Each order is one
ship firing one shot at one target, so bringing a 20-hull fleet to action is 20
clicks for 20 shots. Measured: **13 engagements and 0 sinkings in 400 turns**.

**(f) Hull types barely differ, and transports are free.** The only distinctions
are `baseDmg` (carrier 35, destroyer 25) and range (450 vs 350), while
`CARRIER_COST` is 40 against `DESTROYER_COST` 15 — 2.7× the price for +40%
damage. And a transport is *conjured for free* on embark
(`Game_TurnLogic.cpp:3166`) and deleted on disembark, so the navy has no
logistics role to protect. That is why "the only point of a navy, as of now, is
to move armies".

### Design choices

**3.1 — Multi-turn routed orders.** Let the player click any water destination
in the same ocean, run `navRoute`, store the waypoint list on the order, and have
`processNavyMovement` advance the hull up to its range *along the route* each
turn. One click, then the fleet gets there. This single change removes the
per-turn re-click, removes the coast stall, and should take the 94% blocked
figure to near zero — for the AI as well, since both paths converge on the same
resolver.

Note the rule placement: the range and water-path checks currently live in
`Game_Render` and therefore bind the player's mouse only. Routing belongs in the
resolver, with the overlay merely *previewing* what the resolver will do.

**3.2 — Speed, so that a routed crossing is worth waiting for.** Target: any
ocean crossing in ≤3 turns.
- Transport base 200 → 350 px.
- Origin port level adds range (+50/level), giving ports a purpose beyond
  unlocking hull types.
- Keep `navy5` at +25%.

**3.3 — Combat on contact.** At end of turn, hostile fleets within range of one
another fight automatically as a fleet action: every hull on each side
contributes, escorts screen the transports and carriers behind them, damage does
not heal. The explicit engage order stays, but as a way to *focus fire* on a
chosen target rather than as the only way combat exists. A fleet action must
decide something — otherwise the reviewer's twenty destroyers remain a 300-
treasury ornament.

**3.4 — Hulls get jobs.**
- **Carrier** — strikes well beyond gun range (can engage without being engaged
  in the same round) and can bombard inland provinces; poor at defending itself.
- **Destroyer** — screen. Intercepts fire aimed at transports and carriers in
  the same stack. Cheap and numerous, so twenty of them is a doctrine rather
  than a mistake.
- **Transport** — built, limited and sinkable rather than conjured. Sinking one
  drowns the army aboard (the resolver already tracks `crew` and counts
  `m_navCrewDrowned`), which is what finally makes sea control matter.

**3.5 — Blockade.** A fleet holding station on an enemy port province cuts a
share of that province's income and blocks embarkation from it. Cheap to
implement, and it gives a navy something to do in the long stretches between
invasions.

**How we will know it worked:** `routing` blocked share → near 0%,
`sea combat` engagements and sinkings both non-zero and proportional to fleet
size, `landing_pct` up, and the count of embarkations that "came home" (235 of
561) down.

---

## 4. Bulk industrialisation

### What is actually happening

`m_activeViewTab == 2` draws its Upgrade and Specialize buttons for exactly one
selected province (`Game_Render.cpp:1240–1330`). There is no province
multi-select anywhere. A box-select *does* exist — `ACTION_BOX_SELECT`, bound to
Shift, rebindable — but it is scoped to the navy tab and selects ships
(`Game_Render.cpp:2455–2532`, `Game_Update.cpp:761`).

### Design choices

**4.1 — Extend the existing box-select to provinces.** Reuse
`ACTION_BOX_SELECT`; add an `m_selectedProvinces` set active in the industry and
fortification tabs; have the action panel operate on the whole selection.
Shift-click and ctrl-click add and remove individual provinces.

**4.2 — Aggregate actions state their total before committing.** "Upgrade 34
provinces to their next level — $1,240, 4 turns" with a partial application rule
when the treasury runs short (cheapest first, and say how many were skipped).

**4.3 — Filter selections.** "Select all my provinces with industry below N",
"all my coastal provinces", "all provinces with an unbuilt resource". This is
what turns bulk development from less clicking into a plan.

**4.4 — Route it through the resolver.** Every bulk action must produce the same
`m_pendingUpgrades` entries a single click produces, so multiplayer ingest and
the AI see one order shape and not two.

---

## 5. Province layout

### What is actually happening

Three distinct defects, two of them pure runtime.

**(a) No wraparound in the centre calculation.** `buildProvinceData`
(`renderer/MapRenderer.cpp:365–439`) takes a plain arithmetic mean of x, on a map
that wraps. A province with pixels on both sides of the antimeridian gets a
centre in the middle of the world. `Russia #490` — 26,807 px — has its army
marker drawn **in Finland, 3,413 px from its own nearest land.**

**(b) The centre is a pixel mean, so a split province gets a point between its
parts.** 442 of 1,642 provinces have more than one connected component; 55 have
their computed centre outside their own pixels entirely; 9 of those land inside
a *different* province. This is precisely the reported *"some provinces are split
in two with a province between them, and… the army tab just averages out where
the two are located"*.

**(c) Radius is half the full bounding box** (`MapRenderer.cpp:436`), so a split
province's selection and glow radius spans both parts and everything between,
which is also what the 30 px snap in the ship overlay reads.

Separately, the generator's `MIN_PROV_PIXELS = 80` (`Generator.cpp:1888`) is both
too low and applied too early: 9 provinces survive below it (skipped as a
country's last province, or created afterwards by the island pass), the smallest
being 2 px. And the merge searches outward to a radius of 200 px for a neighbour
to fold a fragment into, with no requirement that the two actually *touch* —
which is one of the ways a province ends up in two pieces in the first place.

### Design choices

**5.1 — Wrap-aware centre.** Average x on the unit circle (mean of angles), so a
province straddling the antimeridian resolves to the correct side. Fixes (a)
outright.

**5.2 — Anchor to the largest component, and use an interior point.** Label each
province's connected components; take the largest; within it use the pixel
furthest from the province boundary (a cheap distance transform, or a pole-of-
inaccessibility approximation). Guarantees the marker is always *on* the
province, and always on its main body. Take the radius from that component's
extent, not the whole bbox.

**5.3 — Mark large secondary components.** Where a province's second component
is substantial, either draw a secondary marker on it or — better, at generation
time — split it into its own province. An island and a mainland should not be
one clickable thing.

**5.4 — Fix the generator, next map version.** Raise the tiny-province floor,
run the merge *after* the island pass so nothing slips in behind it, and refuse
to merge a fragment into a province it does not physically touch. That last rule
is what stops manufacturing split provinces.

**Scheduling note:** 5.1–5.3 are runtime-only and can ship immediately against
existing saves. 5.4 changes province ids, which invalidates **every `.odsv`**
(see AGENTS.md) — so it must be batched with a map version bump and a regenerated
`data/saves/`.

---

## 6. What was built, and what it measured

Phases 1 and 2 are done. Every number below is the same command on the same
seed, before and after: `--eval-ai 2 200 20260801 2 --scenarios` (1914 and 1918,
200 turns each, 53 and 61 countries). `tests/run_all.sh` passes, including the
determinism check and the save-format byte-identity check.

| | Before | After |
|---|---|---|
| **rebellions** per 1k country-turns | 14.64 | **2.16** |
| **ship moves stopped dead by land** | 26,965 / 28,599 (94%) | **5,379 / 20,866 (26%)** |
| **sea combat** | 13 engagements, **0** hulls sunk | **783 engagements, 85 sunk** — 59 of them loaded transports, 77,368 troops drowned |
| **amphibious** landing rate | 81% | 85% |
| hulls left standing on land | 0 | 0 (invariant held) |
| mean minority alignment | 97% | 98% |
| groups disaffected (<40%) | 1.76% | 1.14% |
| country-turns spent bankrupt | 0.39% | 0.30% |
| AI thinking, ms per country-turn | 2.381 | **1.446** |
| countries still alive at the end | 87.0% | **78.8%** |

Two of those want reading carefully.

**Rebellions landed at 2.16, below the 3–5 this document aimed at.** That is the
rate for a world the AI governs well — it holds mean alignment at 98% and is
bankrupt three turns in a thousand. The mechanic still bites hard where it
should: bankruptcy alone is +20 unrest against a loyalty floor of 6, so every
province of a broke country climbs the ladder and is ready to secede within
three turns. `INSURRECTION_UNREST` (currently 4.0) is the one dial if this wants
raising; it was 8.0 first, which gave 0.34 and was plainly too few.

**Survival fell from 87% to 79%.** That is the price of amphibious invasions
working: transports now arrive, 85% of landings reach a hostile shore, and wars
across water actually decide things. The world is less static than it was. It is
a real change in how a campaign plays and not obviously the wrong one, but it is
a change nobody asked for and it should be watched.

### Phase 1 — bugs and plumbing

1. **`unrest_reduction` unit fix.** The field is now `unrestReductionPct`,
   converted once at load (`Game_Policies.cpp`), so the name carries the unit and
   a reader still expecting the old fraction does not compile. Seven doctrines
   that advertised an unrest reduction and delivered one hundredth of it now
   deliver it.
2. **The Analysis tab's sign bug, by deletion.** Rather than fixing the `+`, the
   whole computation moved into `Game::unrestBreakdown()` — one struct with every
   term itemised. `getProvinceRebellionChance` returns its `.total` and the UI
   reads its fields, so the resolver and the screen that explains the resolver
   cannot drift apart again.
3. **Province centres are wrap-aware and anchored to the largest connected
   component** (`MapRenderer::buildProvinceData`). Components are labelled
   run-by-run with a union-find — 177 k runs rather than 33.5 M pixels, about
   3 MB — and the centre is the pixel of the main body nearest its own centroid.
   Verified against the shipped raster: **all 1,642 provinces now have a centre
   inside their own pixels, against 55 that did not**, and Russia #490's army
   marker moved out of Finland. Radius comes from that component instead of a
   bounding box stretched across the gap.
4. **Multi-turn routed ship orders.** `PendingShipMoveOrder` carries the route
   and survives the turn; `processNavyMovement` plans with the existing
   `navRoute` and spends one turn's range along the polyline. The player can
   click any water in the same ocean, sees the path and the ETA, and the fleet
   sails itself. The one-turn range gate came out of `Game_Render` — it lived in
   the overlay and therefore bound only the local player.
   - The nav graph is now the authority on legs it produced. Sampling the
     straight line between two adjacent water cells and refusing the leg on the
     first land pixel rejected legs the router had already proven; that alone was
     most of the 94%. The final leg, whose endpoint somebody chose rather than
     the router, is still walked and clamped.
5. **Naval combat on contact** (`processFleetActions`, once per turn for the
   whole world). Simultaneous — every shot computed before any lands, so a fleet
   action does not depend on turn order. Escorts screen transports and carriers.
   Transports do not shoot and are the priority target. Explicit engage orders
   still fire first and are now focus-fire rather than the only way combat
   happens.

### Phase 2 — balance

6. **One revolt roll per country per turn**, seeded at the worst province, with
   the revolt flooding outward from there through provinces that share its
   grievance. Expected revolts no longer scale with how much land you hold,
   which was the whole of the "three revolts per turn… especially with huge
   empires" complaint. A side effect worth having: AI thinking time per
   country-turn dropped 39%, because the per-province roll and its faction BFS
   were a large part of it.
7. **An escalation ladder.** Content → Restless → Discontent → Insurrection →
   Ready to secede, with `INSURRECTION_TURNS` (3) of open insurrection required
   before a province may seed anything. The player is told when a province goes
   into insurrection and again when it becomes able to leave, and the province
   panel names the stage. Persisted, so reloading is not a way to postpone it.
8. **Core groups.** `computeCoreGroups()` pins each country's titular nation at
   load from the largest group by national population, and it starts at 85%
   alignment rather than the 50% every group in the world used to get. A
   country's own people are no longer modelled as half-alienated from it.
9. **Grievance decays.** The `-30` war penalty and `-25` conquest penalty moved
   out of `m_minorityAlignmentDrift` and into `m_minorityGrievance`, which fades
   with a half-life of about seven turns. An empire that fights a war is no
   longer permanently disqualified from governing well, which is why maximum
   services bought nothing before. The ethnic tab now shows, per group, whether
   it is the core nation, how much grievance is outstanding and fading, and the
   real per-turn trend including that recovery.
10. **Minority programmes are billed by population share**
    (`minorityProgrammeCost`), scaled by country population, replacing a flat
    price per distinct group name charged from two duplicate loops. How finely a
    population is partitioned is a property of the data, not a decision anyone
    made. The austerity path credits savings in the same weighted units, or it
    would believe it had balanced a budget it had barely touched.
11. **Unrest breakdown in the province panel.** The line reads
    `Unrest: 12.3% (Insurrection)` and hovering itemises every term — ideology,
    political, ethnic, foreign claims, war weariness, bankruptcy, minus
    doctrines, pacification and civil order — with the worst source named and
    the turns remaining before secession.
12. **Transports are as fast as their escorts** (200 → 350 px/turn) and **port
    level extends fleet reach** (+50 px per level, capped at 3), giving dockyards
    a purpose beyond gating hull types. `bestPortLevel` is cached per turn.

### A correction to §4

**The bulk industrialisation feature already existed.** A brush-paint system
(`m_bulkSelection`, `upgradeQuote`, `commitBulkSelection`, with per-province
costing, an all-or-nothing commit and an amber overlay) shipped in 1.0.8a,
covering industry, forts, ports and specialisation. §4 above was written without
finding it, and a parallel implementation was built and then reverted.

What was genuinely missing is the reviewer's actual words — "a way to
**highlight groups** of provinces". Sweeping a pointer over a hundred provinces
one at a time is not the same as drawing a box round a continent. So the existing
system gained:

- **Rectangle select**: hold the box-select key (Shift) while a bulk brush is
  down and drag; every eligible province whose centre is in the box joins the
  selection. It adds rather than replaces, so a box plus a few strokes is one
  selection.
- **"Select all (N)"**, which takes every eligible province in the country and
  says how many that is.
- One `bulkEligible()` gate now shared by the brush, the rectangle and Select
  all, so the count on the button and the total on the bar cannot disagree.

The all-or-nothing commit was left alone: the existing code documents why
(partial purchase from an unordered set gives the player half a plan and no way
to tell which half), and §4's suggestion to change it was wrong.

## 7. Phase 3

Also done. Everything below was verified with `tests/run_all.sh` and the repo's
own `tools/check_map_integrity.py --strict`; the headless eval numbers in the
naval section are from `--eval-ai 1 60 20260801 2 --scenarios` runs taken as each
change landed.

### The navy has a job now

**Hull roles, as arithmetic rather than description** (`Game::navalDamage`, one
rule where there were two copies). A carrier reaches 600 px against a
destroyer's 350, so between those numbers it strikes and cannot be struck back —
that band is what a hull costing 2.7× a destroyer is selling, and it was
previously selling ten points of damage. Inside a third of its own reach a
carrier takes **double**, because the air wing is up and anything that close got
past the screen. Destroyers deal **half again** inside a third of theirs, so
closing is their job. Twenty destroyers is now a doctrine — get inside the
carriers — rather than an ornament.

**Transports are a fleet you can lose.** They used to be conjured free on
embark and deleted on landing, so sealift could not be run out of and losing it
cost nothing. Now a hull survives putting its army ashore, must sail to a
province with a **real port** to be paid off, costs upkeep while it is at sea,
and counts against a capacity of `2 + 2 × best port level`.

Two things had to be got right, and the measurements said so both times:

- A **$10 lump sum** at the quayside was tried and it stopped amphibious
  operations dead: 94% of embarkations reaching a hostile shore fell to **3%**.
  The diagnostic said why — every AI country's liquid treasury sits under a
  dollar, because the economy spends to zero each turn, so any purchase gate at
  the point of embarkation is a prohibition, not a price. Sealift costs upkeep
  and dockyards instead, both of which a country can plan for.
- Empty hulls had to be **sailed home by the resolver** (`returnEmptyTransports`),
  not by the AI: the AI's amphibious policy knows how to carry an army and how to
  unload one and has no opinion at all about a hull with nothing aboard. Left
  alone they filled every country's capacity and the next invasion could not
  sail. Paying them off within a full turn's steaming of any friendly coast was
  the same bug wearing a hat — the hull existed for zero turns and nothing
  changed — so it is a 40 px mooring radius at an actual port, and the voyage
  between is where an enemy gets to sink it.

**Blockade** (`updateBlockades`). A warship — not a transport — sitting within
60 px of an enemy harbour closes it: half that province's industrial and
resource income stops, and **nothing can embark from it**. That is the half that
decides campaigns rather than budgets. It shows on the province panel and as its
own line in the economy breakdown.

**And the routing number finally collapsed.** 26% of ship moves were still
registering as blocked after Phase 1, and instrumenting them by hull type said
**549 of 863 were destroyers** — the AI aims at enemy port *province centres*,
which are dry land, so a fleet already on station was re-ordered at an inland
point every turn and went nowhere every turn. `Game::portApproach` gives the
water beside a harbour, and both `findEnemyPort` and `findHomePort` now use it:

| | Baseline | After Phase 1 | After Phase 3 |
|---|---|---|---|
| ship moves stopped dead by land | 94% | 26% | **2%** |

### The politics tab

**Requirements became a price.** `policyBlockReason` no longer refuses anything
on the compass. `doctrineStrain` measures how far outside its window a doctrine
is, and that buys: more political capital, more implementation turns, and unrest
while it beds in (a `Doctrinal upheaval` term in the province panel's breakdown,
which stops the turn the last one lands). A government that wants to turn is
allowed to and pays for it.

**Repeal gives back exactly what a doctrine moved.** `ActivePolicy` now records
what it has actually applied, so putting a doctrine down returns precisely that,
whether it had finished arriving or not. Together with the gate coming off, that
is the compass ratchet gone — it had two halves, an irreversible shift and a
gate that shift then closed.

**Political capital replaced "3 actions per turn."** A pool that accrues from
`CAPITAL_BASE_PER_TURN`, better when the country is quiet and its provinces
think as it does, worse in insurrection or bankruptcy, capped at 12. The old cap
was the same three for a stable democracy and a bankrupt junta, and there was
nothing to save and no reason to act this turn rather than next.

**Doctrines pull the levers research pulls.** `Policy::levers` uses the research
tree's own effect names, and `getTotalEffect` sums both — so a doctrine and a
research node are the same kind of thing to every caller, and a new doctrine is
pure data. That is what "I didn't really notice them having much of an effect"
was: a doctrine could move the compass and essentially not one other number.

**17 → 30 doctrines**, mostly in opposed pairs with real opportunity cost: Total
War Economy vs Consumer Economy, Autarky vs Free Trade, Naval Supremacy vs
Continental Army, Conscription vs Professional Army, Colonial Office vs
Commonwealth Settlement, Technocracy vs Traditionalism, plus General Amnesty and
Mass Mobilisation.

`data/policies.json` is now **generated** by `tools/gen_policies.py`, which
derives each doctrine's "gains"/"costs" text from its actual effects — including
flipping the sign on cost levers, where the tree stores a reduction as a
positive. `tools/gen_policies.py --check` runs in `tests/run_all.sh`, so the
advertised numbers and the applied numbers are one fact rather than two that
agree today. The doctrine screen now prints the real price of each doctrine on
its button: `Enact 3.2` / `6 turns`, with a line naming the strain when there is
any.

### Province geometry, on all six shipped maps

Fixed **upstream** in `tools/map_generator/Generator.cpp` — floor raised from 80
to 250 px, the merge now requires the fragment to actually *touch* what it is
folded into (the search used to expand to a 200 px radius and take whatever it
found, which is how a province ends up in two places), and a new step 7.5 splits
disjoint provinces *after* smoothing, since smoothing is one of the things that
breaks them apart.

That only helps maps generated from here on, so `tools/fix_map_geometry.py`
applies the same rules to the archives that ship, without a network round trip
or re-deriving anything. Merges resolve through a remap applied in one pass at
the end — painting as it went orphaned a raster id whose province had itself
since been absorbed, and the verifier caught it.

| Map | Provinces | Minuscule merged | Detached pieces split |
|---|---|---|---|
| map.odmap | 1642 → **1635** | 28 | 21 |
| 1914 | 1247 → **1240** | 28 | 21 |
| 1918 | 1284 → **1278** | 28 | 22 |
| 1939 | 1298 → **1295** | 29 | 26 |
| 1945 | 1267 → **1262** | 28 | 23 |
| 1962 | 1538 → **1530** | 28 | 20 |

Verified on every map: raster ids and `provinces.json` agree exactly, every
province-keyed file (`population`, `political_compass`, `minorities`,
`resources`, `ports`, `armies`) covers exactly the raster with no orphans, no
orphaned claims, **zero detached pieces of 250 px or more remain**, and total
population is unchanged to the person. `check_map_integrity.py --strict` reports
0 inconsistencies across all six.

Quantities are split by area share and properties are inherited whole, so a
province cut in two does not become two provinces each as rich as the original;
a port or a garrison goes with the larger piece and is never duplicated.

About 56–61 provinces per map remain under 250 px. They are isolated islands
with nothing to merge into — the Isle of Man, São Tomé, Hong Kong — which is a
different thing from a two-pixel speck beside a neighbour, and folding them into
a distant province is the bug this was fixing.

`tools/fix_map_geometry.py --check` runs in `tests/run_all.sh`.

**Province ids changed, so every `.odsv` written against these maps is dead.**
`data/saves/` holds 390 development saves (`Dbg.odsv`, `Dedicated (N).odsv`) —
they were not deleted, but they will not load. The shipped `default.odsv` and
`sample.odsv` that AGENTS.md describes were already absent before this work.

### Measured

Full run on the current build, same command and seed as the baseline:
`--eval-ai 2 200 20260801 2 --scenarios` (1914 and 1918, 200 turns each).

| | Baseline | After 1+2 | After 3 |
|---|---|---|---|
| **rebellions** per 1k country-turns | 14.64 | 2.16 | **1.87** |
| **ship moves stopped dead by land** | 94% | 26% | **9%** |
| **sea combat** | 13 engagements, **0** sunk | 783 / 85 | **817 / 62** |
| amphibious landing rate | 81% | 85% | **64%** |
| ports built over the run | 3 | 3 | **11** |
| industry levels built | 3,651 | — | **4,800** |
| countries alive at the end | 87.0% | 78.8% | 79.3% |
| country-turns bankrupt | 0.39% | 0.30% | 0.50% |
| AI thinking, ms per country-turn | 2.381 | 1.446 | 1.497 |

Three of those want reading.

**Amphibious fell to 64%, and that is the blockade working.** An embarkation
from a harbour an enemy fleet is sitting on is cancelled rather than sailed, and
it still counts as an embarkation that never landed. Invasions are now something
a navy can prevent, which is the entire point of the feature.

**Ports built went 3 → 11 and industry levels 3,651 → 4,800.** Nobody told the
AI to build ports. Ports now extend fleet reach and set transport capacity, so
they became worth having, and the model found that on its own with the
behaviour it already had. That is the clearest evidence the naval levers are
real rather than decorative.

**Zero loaded transports were sunk this run, against 59 before.** Not a
regression in the combat that sinks them — it is that far fewer loaded
transports are at sea at any moment. Blockade stops many from sailing, and the
ones that do sail unload and come home empty. Sea control now bites before the
convoy leaves rather than in the middle of the ocean, which is a different and
arguably better shape, but it does mean the dramatic mid-ocean interception is
rarer than the Phase 1+2 numbers suggested.

### Verified in the game

The display came back, so the screenshot tour ran against the repaired maps:
all six scenarios load, the world renders, and the Doctrines screen reads
`Political capital: 3.0 (+0.92/turn)` with its folder list intact.

It also surfaced three things the headless eval could not have:

- **A government took office with nothing to spend.** Capital only accrued, so
  turn one offered thirty doctrines and no way to enact any of them — worse than
  the three-actions rule it replaced. `CAPITAL_START` seeds three.
- **`applyStartingPolicies()` was not idempotent.** It checked incompatibility
  and never whether the doctrine was already in force, so calling it from both
  load paths stacked a country's opening doctrines.
- **A save-load bug older than this work.** The save's `activePolicies` block
  was *appended* to what the loader had already created, so Britain read
  "Active: Press Freedom Act, Economic Deregulation, Press Freedom Act, Economic
  Deregulation" — two of each, paying upkeep twice, and reloading added two more
  every time. Harmless-looking before, and not harmless now that doctrines pull
  the research tree's levers: every effect applied twice. The save is
  authoritative and now replaces rather than adds.

### A correction: saves are not dead

An earlier draft of this document said the geometry change invalidates every
`.odsv`. **It does not.** A save embeds its own `map.odmap`
(`SaveManager.cpp:283`), so an existing save is self-contained and keeps playing
the geometry it was made on — `Dbg.odsv` loads and renders fine against the
repaired archives. AGENTS.md's warning applies to regenerating the map *and*
regenerating `data/saves` from it, not to a save already written.

## 8. What is left

Nothing from the original three phases. Outstanding beyond them:

- **The AI model wants retraining.** Phases 2 and 3 changed the rules it was
  trained under — revolts, naval routing, sea combat, blockade, doctrine costs
  and the doctrine set itself are all different games now. `ADVANTAGE` will have
  moved for reasons unrelated to the model. Re-measure both models head to head
  under the *same* binary (`--vs-model`), at 400 turns as well as 300.
- **Survival settled at 79%**, down from 87% before amphibious invasions worked.
  Blockade did not pull it back. Whether a world that loses a fifth of its
  countries in 200 turns is the right one is a design call, not a bug.
- **Loaded transports are rarely intercepted now** (see above). If mid-ocean
  convoy battles are wanted, the blockade radius is the dial.
- **Comoros holds no province** on the modern map. Pre-existing, documented and
  accepted by `check_map_integrity.py`; it needs a content decision, not a fix.
