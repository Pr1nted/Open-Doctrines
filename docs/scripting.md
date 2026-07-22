# OpenDoctrines Map Scripting Engine

## Version: 1 (OD/MapEngine/1)

## Overview

OpenDoctrines supports custom scripts inside `.odmap` files. Scripts are plain
text files placed in the `scripts/` directory of the map archive. They are
executed once after all map data is loaded, before the player starts playing.

Scripts allow map makers to:
- Set country properties (treasury, war/alliance states)
- Set province properties (population, owner, industry, fortification)
- Check map metadata (date, name, turn number)
- Loop over provinces owned by a country
- Apply conditional logic with if/else

## File Format

Each script file must start with the engine version header:

```
#OD/MapEngine/1
```

Lines starting with `#` are comments. The language is line-based (one command
per line). Indentation is cosmetic but recommended for readability.

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
map.date  → (string) map date (from metadata.json)
map.name  → (string) map name (from metadata.json)
map.turn  → (int) current turn number (0 at script execution)
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

### while / endwhile
Loops while a condition is true. Safety limit: 10,000 iterations.

```
while country.USA.treasury > 1000
    set country.USA.treasury 0
endwhile
```

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

## Packaging

To include scripts in a `.odmap` archive, place them in the `data/scripts/`
directory before running `package_odmap.py`. The packager automatically
includes all files in `scripts/` and sets the `has_scripts` flag in
`metadata.json`.

Script filenames should use `.txt` extension. The engine strips the
extension when displaying error messages.