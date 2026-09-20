# Writing a doctrine

A doctrine — what the politics screen calls a policy, and what this game is
named after — is a JSON object with an id, a price, a place on the political
compass, and a set of levers that move numbers elsewhere in the simulation.
Nineteen of them ship in `data/policies.json`. A mod adds its own through the
**Content** capability, and the definition it sends is *the same JSON the data
file uses*: it goes through `Game::parsePolicyJson`, the one the data file goes
through, not a second reading of the same fields.

That is the whole mechanism. There is no separate mod format to learn, and no
field that works from the file but not from a mod.

**A working one ships with the SDK:** `sdk/examples/custom-doctrine` adds two
doctrines and nothing else — no panel, no turn hook, 3.4 KB of wasm. Its
content lives in `doctrines.json`, in the format below, and `build.sh` turns
that file into the table the C reads, so the doctrines stay data you can edit
and diff. `PolicyRulesTest` loads that same file and checks the doctrines
behave in a real game.

---

## The complete definition

Every field the parser reads, with nothing invented. Anything not on this list
is ignored, silently — so a typo in a key name costs you the whole effect.

```json
{
  "id": "com_example:land_and_liberty",
  "name": "Land and Liberty",
  "description": "Break up the estates and arm the villages that take them.",

  "category": "left",
  "folder": "Reform",

  "cost_per_turn": 12,
  "implementation_turns": 4,
  "propaganda_duration": 0,

  "compass_shift":  { "economic": -18, "social": -6 },
  "requirements":   { "min_economic": -100, "max_economic": 25,
                      "min_social":   -100, "max_social":  100 },

  "levers": {
    "popGrowthPct":    0.8,
    "resourceModPct":  10,
    "conscriptionPct": 8,
    "industryCostPct": -12
  },

  "effects": {
    "minority_growth_rate": 0.02,
    "immigration_boost":    0.0,
    "pacification_cost":    0.0,
    "unrest_reduction":     0.015,
    "public_opinion_shift": 0.0,
    "target_minority":      ""
  },

  "incompatible_with": ["privatization", "flat_tax"],

  "tradeoffs": {
    "gains": ["Population growth +0.8%/turn", "Resource income +10%",
              "Manpower +8%", "Unrest -1.5%"],
    "costs": ["-12/turn income", "Industry cost +12%", "Economic -18",
              "Social -6"]
  },

  "aiVisible": false
}
```

### Identity

| Field | What it does |
|---|---|
| `id` | How the save records a country holding this. **Set it once and never change it** — a save written with the old id has no way back. Lower-case letters, digits, `_`, and at most one `:` for your own namespace; 64 bytes. The registry's id wins over this one if they disagree. |
| `name` | Shown everywhere. Falls back to the id if empty. |
| `description` | One or two sentences on the doctrine's panel. |
| `category` | `left`, `right`, `authoritarian` or `libertarian` — colours the entry. Anything else draws neutral. |
| `folder` | Which group it files under on the politics screen. Empty means *Miscellaneous*. Yours can be a folder no shipped doctrine uses. |

### What it costs

`cost_per_turn` is charged every turn the doctrine is active, out of the same
income the army and the minorities are paid from.

The gate to enact is **full price against spare income**, not against the
treasury balance: income minus army, navy, existing doctrines and minority
costs, plus whatever political capital the country has banked. A doctrine whose
`cost_per_turn` exceeds that is greyed out with the reason spelled out —
"Costs 12/turn and only 7 is spare."

`implementation_turns` is how long it takes to come into force (default 3).
While it builds, the compass shift is applied in equal slices, and nothing else
about the doctrine is live yet.

With `OD_DOCTRINE_TENURE=1` two further rules apply, and they are one mechanic:

* the bill during construction is **phased** — you pay the fraction of
  `cost_per_turn` matching the fraction built;
* a doctrine **in force** grows stronger the longer it is held, from ×1.0 to
  ×1.5 over 30 turns, and cancelling loses all of it. Re-enacting starts from
  zero again.

`propaganda_duration`, if non-zero, is how long the doctrine runs on after it is
repealed.

### What it requires

`requirements` is a box on the political compass. `economic` runs −100 (left) to
+100 (right); `social` runs −100 (authoritarian) to +100 (libertarian). A
country outside the box cannot enact it, and is told which edge it failed:
*"Your government is too authoritarian for this (−40; needs −20 or higher)."*

`compass_shift` is what holding it does to the country — applied in slices while
it implements, then continuously (at 1/50th per turn) while it is in force. A
doctrine that shifts the compass out of its own requirement box stays in force;
requirements are checked when you enact, not every turn.

`incompatible_with` names doctrines that cannot be held alongside this one.
**One side is enough.** The engine reads the pair from either member, which is
the only thing that could work for a mod: you cannot edit `data/policies.json`
to make `land_reform` name you back. Declaring a conflict with shipped content
binds it all the same, and `sdk/examples/custom-doctrine` is tested for exactly
that. Within your own set, naming both sides is still worth doing — it is what
a reader of your data expects, and it survives one of the two being removed.

### What it changes: levers

`levers` is the continuous half, and the only part that touches the wider
simulation. These seventeen names are read by the game; anything else you put
here is summed and never spent.

| Lever | Moves |
|---|---|
| `armyAtkPct`, `armyDefPct` | land combat |
| `navyAtkPct`, `navyDefPct`, `navySpeedPct` | naval combat and transit |
| `conscriptionPct` | manpower available |
| `conscriptionCostPct` | price of recruiting |
| `maintenanceCostPct` | price of keeping an army |
| `industryCostPct` | price of building industry |
| `industryUpkeepPct` | price of running it |
| `navyCostPct` | price of ships |
| `passiveIncome` | flat income per turn |
| `resourceModPct`, `popModPct` | the resource and population halves of income |
| `popGrowthPct` | population growth |
| `migrationRate` | migration in and out |
| `indoctrinationPct` | how fast minorities assimilate |

> **The sign is not uniform, and this is the trap.** For the six *cost* levers —
> `industryCostPct`, `industryUpkeepPct`, `conscriptionCostPct`,
> `maintenanceCostPct`, `navyCostPct` — a **positive number means cheaper**:
> the multiplier is `1 − pct/100`. For everything else a positive number means
> *more*: `1 + pct/100`. Total Mobilisation writes `"maintenanceCostPct": -18`
> to make armies cost 18% *more* to keep. Get this backwards and your doctrine
> does the opposite of what its own description promises, and nothing warns you.

Levers from research and from every active doctrine are summed, so yours adds to
whatever else the country holds.

### What it changes: `effects`

The narrower, population-facing half:

| Field | What it does |
|---|---|
| `unrest_reduction` | subtracted from unrest each turn (0.015 = 1.5%) |
| `public_opinion_shift` | nudges every owned province's compass each turn |
| `immigration_boost` | added to the country's immigration draw |
| `pacification_cost` | scales what pacifying a province costs |
| `minority_growth_rate` | growth of `target_minority`, and **does nothing unless `target_minority` is set** |
| `target_minority` | the group the two minority fields apply to |

### `tradeoffs` is prose, not arithmetic

The two string lists are printed on the doctrine's panel exactly as written.
Nothing derives them, nothing checks them. They are how a player decides whether
to sign, so a `gains` line that does not match the lever above it is a lie your
own mod tells — and it is the easiest thing in this file to leave stale after
you tune a number.

### `aiVisible`

Absent or `false` means the AI never considers this doctrine; it is offered to
players only. That is the default on purpose: content the model was never
trained against is an option it cannot weigh.

Set `"aiVisible": true` when you want the AI to play with it. For a **doctrine**
that is safe. For a **research node** it changes the shape of the model's
feature vector, and a model whose parent no longer matches is silently
re-initialised — so a research mod that opts in should ship with a model trained
against it.

---

## Adding it from a mod

`MANIFEST.json` must declare the capability, or the module is refused when it
loads:

```json
{
  "schema": 1,
  "id": "com.example.landandliberty",
  "name": "Land and Liberty",
  "version": "1.0.0",
  "gearbox": "1.3",
  "modules": ["Core", "Content"],
  "limits": { "memoryPages": 64, "fuelPerTurn": 200000 }
}
```

Python:

```python
import gearbox, json

DOCTRINE = {
    "name": "Land and Liberty",
    "description": "Break up the estates and arm the villages that take them.",
    "category": "left",
    "folder": "Reform",
    "cost_per_turn": 12,
    "implementation_turns": 4,
    "compass_shift": {"economic": -18, "social": -6},
    "requirements": {"max_economic": 25},
    "levers": {"popGrowthPct": 0.8, "resourceModPct": 10,
               "conscriptionPct": 8, "industryCostPct": -12},
    "effects": {"unrest_reduction": 0.015},
    "incompatible_with": ["privatization", "flat_tax"],
    "tradeoffs": {
        "gains": ["Population growth +0.8%/turn", "Resource income +10%"],
        "costs": ["-12/turn income", "Industry cost +12%"],
    },
}

def mod_load():
    ok = gearbox.contentAdd(gearbox.CONTENT_DOCTRINE,
                            "com_example:land_and_liberty",
                            json.dumps(DOCTRINE),
                            gearbox.CONTENT_PERSIST)
    if not ok:
        # Refused: a malformed id, unparseable JSON, or an id another mod
        # already owns. contentOwnerOf tells you which.
        gearbox.log(gearbox.ERROR, "land-and-liberty: doctrine refused")
        return 1            # non-zero refuses the load
    return 0
```

Rust:

```rust
gearbox::content_add(
    gearbox::Kind::Doctrine,
    "com_example:land_and_liberty",
    DOCTRINE_JSON,
    gearbox::Mode::Persist,
);
```

and the same call exists in all thirteen SDKs — `ContentAdd` in Go,
`contentAdd` in Zig, Java, Kotlin, JavaScript, TypeScript, AssemblyScript and
Lua, `gearbox_content_add` in C and C++.

### Persist or hollow

`CONTENT_PERSIST` writes the definition into the save. A country that adopted
your doctrine still knows what it adopted after your mod is uninstalled —
without this, that save becomes a file full of ids nothing can read. Use it for
anything a player can adopt.

`CONTENT_HOLLOW` is redeclared on every load and never enters the save. Right
for a doctrine you generate from something else, or one you would rather not
have fossilise in other people's campaigns.

Either way, **the id is global within the catalogue**. If another mod already
claimed it, `contentAdd` returns false rather than overwriting — a country
records the doctrine it holds *by id*, and two meanings for one id would make a
save ambiguous. Namespace yours (`com_example:` …) and check `contentOwnerOf`
first if you want to say something useful about the clash.

Re-adding your own id updates the definition in place, which is exactly what a
mod does on every load.

### When it takes effect

The moment `contentAdd` returns true. A doctrine added mid-game appears in the
politics screen that turn; nobody has to reload.

---

## Checking it

```bash
build/odmod-check mymod.odmod
```

loads the mod the way the game does, reports what it exported and what it asked
for, and prints **what it actually put in the catalogue**:

```
content added (2)
  doctrine      com_example:estate_compact               persist
  doctrine      com_example:land_and_liberty             persist
```

Nothing under that heading means nothing was added, whatever the mod's own log
says. A doctrine whose JSON does not parse is refused by `contentAdd` and logged
by the game as `[CONTENT] <your mod>: doctrine '<id>' could not be read`.

Then play it: the doctrine appears under its `folder` on the politics screen,
with its own `tradeoffs` printed underneath and, if the country's compass is
outside `requirements` or the money is not there, the exact sentence saying so.
