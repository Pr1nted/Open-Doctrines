#!/usr/bin/env python3
"""Collect every English string the game draws, into data/lang/en.json.

WHY THIS EXISTS. There is no string table in this codebase: the words are
written where they are drawn, as literals, across nine hundred and seventy
DrawText calls. That is fine for one language and impossible for ten -- nobody
can translate a game by grepping it, and nobody can tell what still needs
translating.

So the English text IS the key. `T("New World")` looks "New World" up in the
active language and falls back to itself, which means the English build is the
identity function and a missing translation is English rather than a blank.
This script builds the list of keys by reading the source, so the list is a
consequence of the code rather than a second thing to maintain.

WHAT IT TAKES. String literals that reach a drawing or labelling call --
DrawText, MeasureText, the settings tables, the menu item lists -- and nothing
else. A literal that is a filename, a format specifier, a JSON key or a log
line is not something a player reads, and putting those in front of a
translator is how you end up with a translated "%s/%d" and a crash.

    python3 tools/i18n_extract.py            # write data/lang/en.json
    python3 tools/i18n_extract.py --check    # exit 1 if it is out of date
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")
OUT = os.path.join(ROOT, "data", "lang", "en.json")

# Calls whose string arguments are read by a player.
DRAWING_CALLS = (
    "DrawText", "DrawTextEx", "MeasureText", "MeasureTextEx",
    "drawHybridText", "addNotification", "T", "tr",
    # HELPERS THAT DRAW A LABEL THEY WERE HANDED.
    #
    # A literal passed to drawButton() never appears on a DrawText line, so
    # none of these were collected and none of them were translated -- which is
    # every button in multiplayer, the map editor, the account screen and the
    # economy breakdown, ninety-nine strings that looked done because the
    # report said 100%. The helpers now apply T() themselves; this is the other
    # half, so the literals reach en.json to be translated at all.
    "drawButton", "drawButtonCol", "drawActBtn", "drawBreakdownRow",
    # The economy screen's three charts and three lists take their heading the
    # same way, and translate it themselves. "Expenses", "Top Gross Income",
    # "Top Net Income" and "Top Expenses" were English on that screen for
    # exactly this reason -- while "Gross Income" and "Net Income" beside them
    # were translated, because those two also appear on a drawBreakdownRow line
    # and so had reached en.json by another route.
    "drawGraph", "drawScrollableList",
    # The loading screen's thirty-three phases. setLoadingProgress translates
    # what it is handed, so the literals only need to reach en.json.
    "setLoadingProgress",
    # The mod menu's three action bars, same arrangement again.
    "bar",
)

# Tables of literals that are drawn later rather than at the call site. Each is
# a name in the source followed by a braced list.
LABEL_TABLES = (
    "MAIN_MENU_ITEMS", "TAB_NAMES", "PAUSE_MENU_ITEMS", "MENU_ITEMS",
    # The patterns a breakaway state's name is built from. See Locale.cpp.
    "kForms", "kDirections",
    # THE SETTINGS SCREEN, every tab of it.
    #
    # These are `const Setting X[] = {{"Fullscreen", false, -1}, ...}` -- a
    # struct table, which the shape rule below cannot read, and one that lives
    # at namespace scope, where wrapping the labels in T() would run before a
    # language is loaded and freeze English in. So the labels are collected
    # here and looked up when the row is built, in makeSettingLabel().
    "DISPLAY_ITEMS", "CONTROLS_ITEMS", "AUDIO_ITEMS", "KEYBINDS_ITEMS",
    "ADVANCED_ITEMS", "EXPERIMENTAL_ITEMS", "AI_DIFFICULTY_NAMES",
    "COLOURBLIND_NAMES",
    # The keybind rows name the ACTION, and KEYBINDS_ITEMS holds references to
    # this table rather than literals -- so the section headers around them
    # were collected and the nineteen actions between them were not.
    "ACTION_NAMES",
)

# A literal that is one of these is not prose.
# THINGS THAT LOOK LIKE PROSE AND ARE NOT.
#
# Collecting whole `const char*[]` tables found the labels it was meant to find
# and also swept up every id that happens to live in one: "tab.economy" and
# "view.army" are the UI-target names a .oddlg points at, "waitUntil" is a
# scripting keyword, "GBR" is a country code and "MIT" is a licence. None of
# them is ever drawn, but a key invites a translation, and a translated id is a
# tutorial that cannot find what it is pointing at.
_ID_PATTERNS = (
    re.compile(r"^[a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+$"),   # tab.economy, view.army
    re.compile(r"^[a-z]+[A-Z][A-Za-z]*$"),                # waitUntil, camelCase
    # THE SHAPES THE `const char*` RULE BELOW WOULD OTHERWISE SWEEP IN.
    re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)+$"),            # opendoctrines-turn-seal
    re.compile(r"^[^\s]*/[^\s]*$"),                       # STDmaps/1939.odmap
    re.compile(r"^[.;a-z0-9]*;[.;a-z0-9]*$"),             # .ogg;.mp3;.qoa;.wav
    # JSON, not a sentence -- and matched TIGHTLY. `^[{[]` alone also caught
    # "{month} {year} {era}", the one key whose whole point is that a language
    # may put the parts in another order, and dropped it silently.
    re.compile(r'^\{\s*"'),                               # {"turns":[]}
    re.compile(r"^\[\s*[\]{\"]"),                          # [], [{...
)
_NOT_PROSE = {
    # ISO country codes that appear in example/debug tables.
    "GBR", "FRA", "ITA", "POL", "USA", "FR",
    # SPDX licence identifiers -- names of licences, not words.
    "MIT", "GPL-3.0", "CC-BY-4.0", "CC-BY-SA-4.0", "CC0-1.0",
    # Log levels, printed to a console rather than drawn.
    "ERROR", "WARN", "INFO", "TRACE",
    # The product's name. It is the same name on every store page in every
    # country, and a translated one is a different program.
    "OpenDoctrines",
}


def is_identifier(s):
    if s in _NOT_PROSE:
        return True
    return any(p.match(s) for p in _ID_PATTERNS)


def is_prose(s):
    if len(s.strip()) < 2:
        return False
    # AN ALPHABET, not a word: "0123456789abcdef" and the base64url set are
    # the character pools two id generators draw from. What gives them away is
    # that no character repeats -- real prose of this length repeats letters in
    # every language there is.
    if len(s) >= 16 and " " not in s and len(set(s)) == len(s):
        return False
    if is_identifier(s):
        return False
    # Format specifiers, paths, keys, identifiers, punctuation runs.
    if re.fullmatch(r"[%\w./\\:_\-+#*(){}\[\]<>|@!?,.;'\"= ]*", s) and not re.search(r"[A-Za-z]{2,}", s):
        return False
    if s.startswith(("data/", "./", "/", "http", "res://")):
        return False
    if re.fullmatch(r"[a-z0-9_.\-/]+\.(png|svg|ttf|json|odmap|odsv|ogg|wav|txt|md)", s):
        return False
    # A bare identifier with no space is usually a key, not a sentence -- but
    # single words ARE drawn all over the interface ("Economy", "Back"), so the
    # test is the shape of the word rather than the absence of a space.
    if re.fullmatch(r"[a-z][a-z0-9_]*", s):
        return False
    # A FORMAT STRING IS TRANSLATABLE, and has to be: "%s accepted your offer
    # of %s" is the whole point of restructuring the glued-together sentences.
    # What is NOT translatable is a string that is mostly specifier -- "%d ms",
    # "%s/%s" -- where there is no sentence to translate and a translator can
    # only break it. The test is whether real words survive with the
    # specifiers removed.
    if "%" in s:
        # THE LENGTH MODIFIERS COUNT AS PART OF THE SPECIFIER. Without ll/z/h
        # here, "%lld" strips to "ld" -- two letters, which the rule below
        # calls a word -- and a bare specifier became a key for twenty
        # translators to stare at. This is the same pattern tools/i18n_sync.py
        # lints with, and they have to agree.
        bare = re.sub(r"%[-+0-9.*]*(?:ll|z|h)?[a-zA-Z%]", " ", s)
        # TWO LETTERS COUNT AS A WORD.
        #
        # This asked for three, and three quietly dropped "by %s" -- the line
        # under a song's title in the now-playing toast -- along with "%d RP",
        # "ID: %d" and "$%.0f of $%.0f". Every one of those is a real sentence
        # fragment that a translator has to be given: "by" is "van" in
        # Afrikaans, "RP" is an English abbreviation of Research Points and is
        # nothing in Hindi.
        #
        # Lowering it was checked against the whole tree rather than assumed
        # safe: those four strings and "by %s" are the ONLY things the change
        # admits, and no unit abbreviation came with them.
        words = re.findall(r"[A-Za-z]{2,}", bare)
        # One real word is enough. Requiring two quietly threw away "Republic
        # of %s" and "Northern %s" -- the patterns a breakaway state's name is
        # built from. What is being excluded here is a string with NO word in
        # it at all: "%s/%s", a bare specifier with punctuation around it.
        if not words:
            return False
    return bool(re.search(r"[A-Za-z]", s))


# C++ escapes, resolved. The KEY has to be the string the program actually
# holds at runtime: `"No doctrine matches \"%s\""` is eleven characters shorter
# in memory than it is in the source, and tr() is handed the memory one. Left
# escaped, that key sat in en.json looking correct and never matched anything.
_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "0": "\0"}


def unescape(lit):
    out = []
    i = 0
    while i < len(lit):
        if lit[i] == "\\" and i + 1 < len(lit):
            nxt = lit[i + 1]
            out.append(_ESCAPES.get(nxt, nxt))
            i += 2
        else:
            out.append(lit[i])
            i += 1
    return "".join(out)


def literals_in(line):
    """Every double-quoted literal on a line, as the program will hold it."""
    return [unescape(m) for m in re.findall(r'"((?:[^"\\]|\\.)*)"', line)]


def data_strings():
    """Visible text that lives in data files rather than in the source.

    A doctrine's name and description, and the loading-screen tips, are read
    from JSON at runtime and drawn straight onto the screen. They are as
    player-facing as any label, and none of them appear in a DrawText call for
    the source scanner to find -- so a game "fully translated" by that scanner
    still shows forty-five English doctrines.
    """
    out = {}
    pol = os.path.join(ROOT, "data", "policies.json")
    if os.path.exists(pol):
        d = json.load(open(pol, encoding="utf-8"))
        for entry in d.get("policies", []):
            for field in ("name", "description"):
                v = entry.get(field)
                if v:
                    out.setdefault(v, []).append("data/policies.json")
    # THE CREDITS ROLL, which is a text file the menu draws line by line.
    #
    # Only the HEADINGS. A credits file is mostly proper names -- people, song
    # titles, the libraries and fonts the game links -- and a name is the same
    # name in every language; offering "Pr1nted" and "Parade Uniform" to twenty
    # translators invites twenty different spellings of them. The "@" lines are
    # the roles those names sit under ("Lead programmer", "Original music",
    # "Fonts"), and those are words.
    credits = os.path.join(ROOT, "data", "credits.txt")
    if os.path.exists(credits):
        for line in open(credits, encoding="utf-8"):
            line = line.strip()
            if not line.startswith("@"):
                continue
            heading = line[1:].strip()
            if heading:
                out.setdefault(heading, []).append("data/credits.txt")

    tips = os.path.join(ROOT, "data", "tips.json")
    if os.path.exists(tips):
        d = json.load(open(tips, encoding="utf-8"))
        for t in d.get("tips", []):
            if t:
                out.setdefault(t, []).append("data/tips.json")
    return out


def collect():
    found = {}          # string -> [where]
    call_re = re.compile(r"\b(" + "|".join(DRAWING_CALLS) + r")\s*\(")
    for dirpath, _dirs, files in os.walk(SRC):
        for fn in sorted(files):
            if not fn.endswith((".cpp", ".h")):
                continue
            # Third-party headers vendored into src/ are not ours to translate.
            if fn in ("json.hpp", "nanosvg.h", "nanosvgrast.h", "miniz.h"):
                continue
            # Headless self-play prints a telemetry log -- "alive %d wars %lld
            # ceasefire offers %lld" and a dozen like it. Nobody reads that as
            # a player, and a translated column header would only make the
            # numbers harder to line up.
            if fn == "Game_AITrain.cpp":
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, ROOT)
            text = open(path, encoding="utf-8", errors="replace").read()

            # Drawing calls, line by line: enough, because a call that spans
            # lines still has its literal on one of them.
            for n, line in enumerate(text.split("\n"), 1):
                # Comments explain the code and sometimes quote it. A doc
                # comment showing `T("New World")` as an example is not a
                # string the game draws, and putting it in front of a
                # translator wastes their time on a sentence nobody sees.
                stripped = line.lstrip()
                if stripped.startswith(("//", "*", "/*")):
                    continue
                if not call_re.search(line):
                    continue
                for lit in literals_in(line):
                    if is_prose(lit):
                        found.setdefault(lit, []).append(f"{rel}:{n}")

            # ANY LOCAL TABLE OF LABELS, not just the named ones above.
            #
            # DELIBERATELY `const char*[]` AND NOT ANY BRACED TABLE. Widening
            # this to every `T name[] = {...}` was tried and reverted: it swept
            # in WASM signatures like "(iiii)i", Gearbox permission ids, the
            # screenshot tour's shot names and whole pages of scripting
            # documentation -- four hundred keys, most of which must never be
            # translated. A struct table that does hold real labels is handled
            # by wrapping those labels in T() where they are written.
            #
            # `const char* tabs[] = {"Available", "Implementing", ...}` is drawn
            # later as tabs[i], so no literal ever appears on a drawing line and
            # LABEL_TABLES only helps if somebody remembered to add the name.
            # The policy tabs, the analysis columns and several settings rows
            # were all missed that way. Matching the SHAPE rather than the name
            # means a new table is collected the day it is written.
            # A TABLE THE PROGRAM READS RATHER THAN SHOWS.
            #
            # Some of these tables are English VOCABULARY, matched against
            # English data: the forms PoliticalIdentity.cpp strips off a
            # country name before applying a new one, the direction words the
            # breakaway namer takes off a region, the templates it builds an
            # internal name FROM. Translating those does nothing -- the
            # comparison is against the English source string either way -- and
            # collecting them put ninety keys in front of every translator that
            # could not appear on screen whatever they wrote.
            #
            # The names those tables BUILD are translated, as patterns, in
            # src/i18n/Locale.cpp. `// i18n-ignore` on the line above a table
            # says so, and it has to be written down rather than inferred:
            # nothing about the shape of the table distinguishes vocabulary
            # from labels.
            for m in re.finditer(
                    r'\bconst\s+char\s*\*\s*(?:const\s+)?\w+\s*\[\s*\]\s*=\s*\{([^;]*?)\}\s*;',
                    text, re.S):
                n = text[:m.start()].count("\n") + 1
                if "i18n-ignore" in "\n".join(text[:m.start()].split("\n")[-3:]):
                    continue
                for lm in re.finditer(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
                    lit = unescape(lm.group(1))
                    if is_prose(lit):
                        found.setdefault(lit, []).append(f"{rel}:{n}")

            # A LABEL PARKED IN A VARIABLE ON THE WAY TO BEING DRAWN.
            #
            #   const char* hint = page == Hub ? "Esc  back to menu" : "Esc  back";
            #   DrawText(hint, ...);
            #
            # The literal is nowhere near a drawing call, so the scan above
            # never saw it -- and the shadowed DrawText translates whatever it
            # is handed, so the ONLY thing standing between these and a
            # translation was their absence from this file. A hundred and six
            # of them: the multiplayer title, every confirmation dialogue, the
            # map editor's pickers, "You have unsaved changes!".
            #
            # Narrow on purpose, and narrower than it looks: `const char*`
            # only, so a std::string built at runtime is not mistaken for a
            # label, and every id-shaped literal the pattern does reach --
            # env-var names, kebab ids, paths, extension lists, JSON -- is
            # turned away by _ID_PATTERNS above rather than by a list of
            # exceptions that would go stale. A label that IS id-shaped
            # ("ON", "OFF") is wrapped in T() where it is written, which the
            # scan above already collects.
            #
            # ANCHORED TO THE START OF A LINE, which is not cosmetic: without
            # it the pattern matches the declaration inside `if (const char*
            # why = warGoalText(goal))`, whose condition has no semicolon, and
            # then runs on into the NEXT statement and collects a fragment of
            # it. Two of those got in -- '\n\nThey declare it ' and 'rejected
            # entry "' -- both halves of a sentence built with +, neither
            # translatable on its own.
            for m in re.finditer(
                    r'(?:^|\n)[ \t]*(?:static\s+)?const\s+char\s*\*\s*'
                    r'(?:const\s+)?\w+\s*=\s*([^;]*?);',
                    text, re.S):
                # MASK the T()/tr() calls rather than skipping the whole
                # declaration. Skipping it was wrong the moment one branch of a
                # ternary was already wrapped and the other was not:
                #
                #   const char* r = !can ? "Revert (no snapshot for this turn)"
                #                        : TextFormat(T("Revert to turn %d"), n);
                #
                # -- the first branch is as drawn as the second, and it silently
                # left en.json when the second was wrapped. What is already
                # inside a T() is collected by the scan above; what is beside it
                # still has to be collected here.
                rhs = re.sub(r'\b(?:T|tr)\s*\(\s*"(?:[^"\\]|\\.)*"\s*\)', " ", m.group(1))
                # An environment variable's NAME, which is the one other thing
                # that reaches this rule looking like a label. Masked by the
                # call it sits in rather than by its shape: excluding every
                # SCREAMING_CASE literal also excluded "AD", "BC", "ON", "OFF"
                # and "ALWAYS", and the date on every screen went back to
                # English because of it.
                rhs = re.sub(r'\bgetenv\s*\(\s*"(?:[^"\\]|\\.)*"\s*\)', " ", rhs)
                n = text[:m.start()].count("\n") + 1
                # `i18n-ignore` above the declaration, same as for the tables:
                # the no-window message is four adjacent literals that C++ joins
                # into one paragraph, and it is printed before a font exists.
                if "i18n-ignore" in "\n".join(text[:m.start()].split("\n")[-3:]):
                    continue
                for lm in re.finditer(r'"((?:[^"\\]|\\.)*)"', rhs):
                    lit = unescape(lm.group(1))
                    if is_prose(lit):
                        found.setdefault(lit, []).append(f"{rel}:{n}")

            # The research tree is built by calls to a local add() helper,
            # whose second and third arguments are the node's name and the
            # sentence shown under it. Eighty-three nodes, none of which any
            # DrawText call mentions.
            if fn == "Game_Research.cpp":
                for m in re.finditer(r'\badd\(\s*"(?:[^"\\]|\\.)*"\s*,\s*'
                                     r'("(?:[^"\\]|\\.)*")\s*,\s*'
                                     r'("(?:[^"\\]|\\.)*")', text):
                    n = text[:m.start()].count("\n") + 1
                    for lit in (unescape(m.group(1)[1:-1]), unescape(m.group(2)[1:-1])):
                        if is_prose(lit):
                            found.setdefault(lit, []).append(f"{rel}:{n}")

            # Label tables.
            for table in LABEL_TABLES:
                # Up to the closing "};", not the first "}": a table whose
                # rows are themselves braced would otherwise yield row one.
                m = re.search(re.escape(table) + r"[^=]*=\s*\{(.*?)\};", text, re.S)
                if not m:
                    continue
                n = text[:m.start()].count("\n") + 1
                for lit in literals_in(m.group(1)):
                    if is_prose(lit):
                        found.setdefault(lit, []).append(f"{rel}:{n}")
    return found



# The cast: a speaker's KEY is an id a .oddlg branches on and never changes,
# but the `display` beside it and the `role` under it are prose the player
# reads -- "lazy advisor" has no business staying English on a translated
# screen. Only those two fields are taken; the key is deliberately not.
def cast_strings(found):
    path = os.path.join(ROOT, "data", "comms", "cast.json")
    if not os.path.exists(path):
        return
    rel = os.path.relpath(path, ROOT)
    d = json.load(open(path, encoding="utf-8"))
    for name, c in (d.get("characters") or {}).items():
        if not isinstance(c, dict):
            continue
        for field in ("display", "role"):
            v = c.get(field)
            if isinstance(v, str) and is_prose(v):
                found.setdefault(v, []).append(f"{rel}:{name}.{field}")


def main():
    found = collect()
    for k, where in data_strings().items():
        found.setdefault(k, []).extend(where)
    cast_strings(found)
    # The file is the keys in source order of first sighting, with English as
    # its own value: en.json is what every other language is diffed against.
    data = {k: k for k in sorted(found)}
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    if "--check" in sys.argv:
        if not os.path.exists(OUT):
            print("data/lang/en.json does not exist; run tools/i18n_extract.py")
            return 1
        have = json.load(open(OUT, encoding="utf-8"))
        missing = [k for k in data if k not in have]
        extra = [k for k in have if k not in data]
        if missing or extra:
            print(f"data/lang/en.json is out of date: "
                  f"{len(missing)} new string(s), {len(extra)} no longer drawn")
            for k in missing[:10]:
                print(f"  + {k}   ({found[k][0]})")
            for k in extra[:10]:
                print(f"  - {k}")
            return 1
        print(f"ok    {len(have)} strings, en.json matches the source")
        return 0

    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    print(f"wrote {OUT}: {len(data)} strings")
    counts = {}
    for k, where in found.items():
        for w in where:
            counts[w.split(":")[0]] = counts.get(w.split(":")[0], 0) + 1
    for f_, c in sorted(counts.items(), key=lambda kv: -kv[1])[:12]:
        print(f"  {c:5d}  {f_}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
