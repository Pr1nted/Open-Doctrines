# OpenDoctrines Map Scripting Engine

## Version: 1 (OD/MapEngine/1)

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

- Expressions support a single comparison per condition — no arithmetic,
  `and`/`or`, or parentheses (beyond the optional `waitUntil(...)` pair).
- `waitUntil` must be at top level.
- Suspended scripts are **not** saved into save games: when a save is loaded,
  entry scripts run again from the top and re-suspend at their first false
  `waitUntil`. Keep code before a `waitUntil` idempotent (safe to re-run).
- The script IDE has no text selection or undo yet.