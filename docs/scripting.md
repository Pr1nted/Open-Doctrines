# OpenDoctrines Map Scripting Engine

## Version: 2 (OD/MapEngine/2)

Version 1 files still run unchanged — everything added in 2 is backward
compatible, and the header declares which set of features a script expects
rather than gating them.

## Overview

OpenDoctrines supports custom scripts inside `.odmap` files. Scripts are plain
text files placed in the `scripts/` directory of the map archive. Entry
scripts start running after all map data is loaded; a script that reaches a
`waitUntil` suspends there and is resumed automatically as turns are played.

Scripts allow map makers to:
- Set country properties (treasury, war/alliance states)
- Set province properties (population, owner, industry, fortification)
- Check map metadata (date, name, turn number) and change the date
- Loop over provinces owned by a country, or over arrays/lists
- Apply conditional logic with if/else
- Store state in global variables, arrays and linked lists
- Wait for a condition (`waitUntil`) — e.g. a turn number — before continuing
- Split code across files with `include`

The map editor has a built-in script IDE in the **Scripts** tab: create or
import scripts, double-click to open them in a syntax-highlighted editor with
completion hints (Tab accepts the highlighted hint, Ctrl/Cmd+S saves).

## File Format — Entrypoints vs Libraries

A file whose **first non-blank line** is the engine version header is an
**entrypoint** — it runs automatically when the map loads:

```
#OD/MapEngine/1
```

A file **without** the header is a **library**: it never runs on its own and
can only be pulled into an entrypoint (or another library) with `include`.

Lines starting with `#` are comments. The language is line-based (one command
per line). Indentation is cosmetic but recommended for readability.

## include

`include "name"` splices another script from the same `scripts/` directory in
place, at the point of the include (the `.txt` extension is optional):

```
#OD/MapEngine/1
include "setup_alliances"
include "cold_war_events"
```

Circular includes are rejected, and includes may nest up to 16 levels deep.
Included libraries must not carry the `#OD/MapEngine/` header.

## Variables & References

### Country References
```
country.ISO.treasury           → (float) country's gold reserve
country.ISO.name               → (string) country display name
country.ISO.iso                 → (string) the ISO code itself
country.ISO.province_count      → (int) number of provinces owned
country.ISO.at_war_with.OTHER   → (bool) is at war with OTHER country
country.ISO.allied_with.OTHER   → (bool) has alliance with OTHER
country.ISO.claims_province.ID  → (bool) claims province number ID
```

### Province References
```
province.ID.population    → (int) province population
province.ID.owner         → (string) ISO code of owning country
province.ID.name          → (string) province name
province.ID.industry      → (int) industry level (0-10)
province.ID.fortification → (int) fortification level (0-5)
```

### Map References
```
map.date  → (string) current date, e.g. "January 2000" — WRITABLE via set
map.name  → (string) map name (from metadata.json)
map.turn  → (int) current turn number (0 when entry scripts first run)
```

The map's starting date is set in the editor's **Metadata** tab
("Start date", `Month Year` format) and advances one month per turn.

### Global Variables
```
var.NAME  → your own global value; create/update with:  set var.NAME <value>
```
Variables are shared between all scripts and persist across `waitUntil`
suspensions (but not across save games — see Limitations).

### Arrays
```
array create NAME            create/reset an empty array
array push NAME <value>      append a value
array set NAME <i> <value>   overwrite index i (0-based)
array remove NAME <i>        delete index i

array.NAME.length            → (int) element count
array.NAME.<i>               → value at index i (e.g. array.targets.0)
```

### Linked Lists
```
list create NAME             create/reset an empty list
list pushfront NAME <value>  insert at the front
list pushback NAME <value>   append at the back
list popfront NAME           remove the front element
list popback NAME            remove the back element

list.NAME.length             → (int) element count
list.NAME.front              → first value
list.NAME.back               → last value
```

## Commands

### set
Sets a property to a value.
```
set country.USA.treasury 5000
set province.42.population 1000000
set province.42.owner CAN
set country.RUS.at_war_with UKR true
set country.USA.allied_with GBR true
```

### if / else / endif
Conditional execution. Supports comparison operators:
`==`, `!=`, `>`, `<`, `>=`, `<=`

```
if country.USA.treasury > 1000
    set country.USA.treasury 500
else
    set country.USA.treasury 5000
endif
```

```
if country.USA.at_war_with RUS
    set country.USA.treasury 100
endif
```

```
if map.date == "Modern Day"
    set country.USA.treasury 10000
endif
```

### foreach / next
Iterates over all provinces owned by a country. Inside the loop body,
the variable `province` refers to the current province ID.

```
foreach province in country.USA
    if province.population > 1000000
        set province.population 500000
    endif
next
```

Inside a foreach loop, these variables are available:
- `province` / `province.id` → current province ID (int)
- `province.population` → population (int)
- `province.industry` → industry level (int)
- `province.fortification` → fortification level (int)
- `province.owner` → ISO code of owner (string)

`foreach` also iterates arrays and lists; the loop variable is `item`:

```
foreach item in array.targets
    set country.USA.at_war_with item true
next
```

- `item` → the current value
- `item.index` → its 0-based position

### while / endwhile
Loops while a condition is true. Safety limit: 10,000 iterations.

```
while country.USA.treasury > 1000
    set country.USA.treasury 0
endwhile
```

### waitUntil
Suspends the script until a condition becomes true. The condition is
re-checked once per processed turn, and when it holds the script resumes on
the next line. `waitUntil(cond)` and `waitUntil cond` are both accepted.

```
#OD/MapEngine/1
set country.RUS.treasury 8000

waitUntil map.turn >= 12
# One year has passed — the cold war turns hot
set country.RUS.at_war_with USA true

waitUntil map.date == "January 2005 AD"
set country.RUS.at_war_with USA false
```

Rules:
- `waitUntil` is only allowed at **top level** — not inside `if`, `foreach`
  or `while` blocks.
- A script can contain several `waitUntil`s; they gate stages in order.
- Global `var.*`, arrays and lists survive across suspensions.

## Expressions

Anywhere a condition is taken — `if`, `while`, `waitUntil` — and on the right
of `set ... =`, the text is a full expression.

| | |
|---|---|
| arithmetic | `+` `-` `*` `/` `%` |
| comparison | `==` `!=` `<` `<=` `>` `>=` |
| boolean | `and` `or` `not` (or `&&` `||` `!`) |
| grouping | `( ... )` |
| functions | `min` `max` `abs` `round` `floor` `ceil` `clamp` `len` |

Precedence runs the way it reads: `*` `/` `%` bind tighter than `+` `-`, which
bind tighter than comparison, which binds tighter than `and`, then `or`. So
`a + 1 > b and c` groups as `((a + 1) > b) and c`, and parentheses override it.

```
if country.USA.treasury > 1000 and not country.USA.at_war_with RUS
if (province.42.population + province.43.population) / 2 > 500000
while var.round < 10 or var.pressure > 3
```

`and` and `or` short-circuit: if the left side settles the answer, the right
side is never evaluated.

Division always produces a decimal — `3 / 2` is `1.50`, not `1` — because
integer division quietly discarding the remainder is the kind of thing a map
only reveals much later. Use `%` for the remainder. `+` joins strings when
either side is one.

`min` and `max` take any number of arguments; `clamp(v, lo, hi)` takes three.

```
set var.share = clamp(province.42.population / 1000, 0, 100)
set var.worst = min(country.USA.treasury, country.RUS.treasury, 0)
```

An expression that cannot be parsed, or that names something unknown, is an
error on that line and the script stops; it is never silently zero.

## set with expressions

`set` takes a plain value as it always did:

```
set country.USA.treasury 10000
set map.date Modern Day
```

Add `=` and the right-hand side becomes an expression instead:

```
set country.USA.treasury = country.USA.treasury * 2
set var.total = province.42.population + province.43.population
set var.label = "Year " + map.turn
```

The `=` is what distinguishes the two, and it is required for exactly that
reason: `set map.date Modern Day` is a legal plain value, so a bare value
cannot be read as arithmetic without turning every hyphenated name into a
subtraction. Scripts written before expressions existed have no `=` and behave
exactly as they did.

`+=`, `-=`, `*=` and `/=` fold against the current value:

```
set country.USA.treasury += 500
set var.round += 1
```

## More ways to say it

```
unless country.USA.at_war_with RUS      # if not (…)
    set var.peace true
endif

if map.turn < 5
    set var.phase "early"
elseif map.turn < 20
    set var.phase "middle"
else
    set var.phase "late"
endif

for var.i = 1 to 10                     # counting loop
    set var.total += var.i
next

repeat 3                                # the same, without the variable
    set var.ticks += 1
next

foreach province in country.USA
    if province.population < 1000
        continue                        # skip to the next one
    endif
    if province.population > 900000
        break                           # leave the loop
    endif
next

print "turn " + map.turn                # goes to the log
```

`break` and `continue` act on the innermost loop that encloses them, whether
that is `foreach`, `while`, `for` or `repeat`.

## Blocks

The language is a tree, and so is a block editor: `2 + 3 * 4` is one block
holding two, and `if a and b` is a boolean block in an `if` block's slot. Text
and blocks are two views of the same structure, and converting between them
does not lose anything.

Two properties make that safe, and both are held by tests:

- **Structure is explicit.** One command per line, and every block that opens
  is closed by name — `endif`, `next`, `endwhile`. Nesting never has to be
  guessed from indentation, which is cosmetic here.
- **Round-tripping is stable.** Reading an expression into a tree and writing
  it back produces text that reads the same way and parses to the same tree,
  with parentheses kept only where dropping them would regroup the expression.

Comments and blank lines belong to the line below them, so a script that goes
to blocks and back keeps them.

## Value Types

- Integers: `42`, `-5`, `0`
- Floats: `3.14`, `-1.5`
- Booleans: `true`, `false`
- Strings: `"Modern Day"` (double-quoted)
- References: `country.USA.treasury` (evaluated to the current value)

## Example Script

```
#OD/MapEngine/1
# Cold War setup script

# Give major powers starting bonuses
set country.USA.treasury 10000
set country.RUS.treasury 8000
set country.CHN.treasury 6000

# Set up alliances
set country.USA.allied_with GBR true
set country.USA.allied_with FRA true
set country.RUS.allied_with CHN true

# Check if it's the modern day map
if map.date == "Modern Day"
    # Reduce population in war-torn provinces
    foreach province in country.SYR
        if province.population > 500000
            set province.population 250000
        endif
    next
endif

# Force a province to change ownership
if country.RUS.at_war_with UKR
    set province.42.owner UKR
endif
```

## Error Handling

If a script has syntax errors or references non-existent entities, the
script engine will:
1. Print the error to the console
2. In debug mode, display the error in the bottom-right corner of the
   screen for 3 seconds: `Failed to load script: [name].txt error: [message]`
3. Continue executing the next script (one bad script doesn't stop others)

## The Map Editor Script IDE

The **Scripts** tab of the map editor manages a map project's scripts:
- **+ Script** creates a new entrypoint (pre-filled with the header),
  **+ Library** creates a header-less library file.
- The list shows every project script with an `[entry]` / `[lib]` badge —
  **double-click** (or select + Edit) opens it in the editor.
- A second list shows loose `.txt` files in `data/scripts/` on disk;
  double-clicking imports a copy into the project.
- Scripts are saved inside `.uodmap` projects and exported into the `.odmap`
  archive automatically (`has_scripts` is set for you).

The editor itself supports full cursor editing (arrows, Home/End,
PageUp/Down, click-to-position), syntax highlighting, paste (Ctrl/Cmd+V),
Ctrl/Cmd+S to save, and ESC to save & close. While typing, matching
completions appear in the hint bar at the bottom with a one-line description —
**Tab** inserts the highlighted one.

## Packaging

Scripts written in the map editor are packaged automatically on export.
For hand-built archives: place files in the `scripts/` directory of the
`.odmap` (or `data/scripts/` before running `package_odmap.py`) and set the
`has_scripts` flag in `metadata.json`.

Script filenames should use `.txt` extension. The engine strips the
extension when displaying error messages.

## Limitations

- Conditions are full expressions (see **Expressions**). What is still missing
  is user-defined functions and a `for i = 1 to N` counting loop.
- `waitUntil` must be at top level.
- Suspended scripts are **not** saved into save games: when a save is loaded,
  entry scripts run again from the top and re-suspend at their first false
  `waitUntil`. Keep code before a `waitUntil` idempotent (safe to re-run).
- The script IDE has no text selection or undo yet.