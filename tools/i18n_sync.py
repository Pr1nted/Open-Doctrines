#!/usr/bin/env python3
"""Keep every language file in step with data/lang/en.json.

en.json is generated from the source by i18n_extract.py, so the set of things
to translate changes whenever the interface does. This copies that set into
every other language: a string nobody has translated yet is written as "",
which the loader treats as "fall back to English" rather than as a blank label.

Nothing already translated is ever overwritten, and a key that has left the
game is dropped. So the diff after an interface change is exactly the work the
change created.

    python3 tools/i18n_sync.py             # create or update every language
    python3 tools/i18n_sync.py --report    # how complete each one is
    python3 tools/i18n_sync.py --lint      # Latin letters hiding in Cyrillic words
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LANG = os.path.join(ROOT, "data", "lang")

# Must match kLanguages in src/i18n/Locale.cpp. Checked below rather than
# trusted: a language offered by the game with no file behind it is a menu
# entry that refuses to be selected.
CODES = ["uk", "be", "kk", "ja", "zh", "de", "it", "fr", "es", "cs", "sl", "sk", "pl",
         "af", "ar", "hi", "ko", "bg", "tr", "ur"]


def game_codes():
    """The codes the game itself offers, read out of the source."""
    src = os.path.join(ROOT, "src", "i18n", "Locale.cpp")
    if not os.path.exists(src):
        return None
    text = open(src, encoding="utf-8").read()
    start = text.find("kLanguages = {")
    if start < 0:
        return None
    end = text.find("};", start)
    body = text[start:end]
    out = []
    for line in body.split("\n"):
        line = line.strip()
        if not line.startswith('{"'):
            continue
        code = line.split('"')[1]
        if code != "en":
            out.append(code)
    return out


def main():
    en_path = os.path.join(LANG, "en.json")
    if not os.path.exists(en_path):
        print("data/lang/en.json is missing; run tools/i18n_extract.py first")
        return 1
    en = json.load(open(en_path, encoding="utf-8"))

    offered = game_codes()
    if offered is not None and sorted(offered) != sorted(CODES):
        print("the game offers a different set of languages than this script knows:")
        print(f"  game:   {sorted(offered)}")
        print(f"  script: {sorted(CODES)}")
        return 1

    if "--lint" in sys.argv:
        # THE SPECIFIERS MUST MATCH THE ENGLISH, EXACTLY.
        #
        # A translation is handed to TextFormat with the arguments the CALL
        # SITE pushed, and it has no idea the string changed. One %s too many
        # and it reads a pointer nobody passed -- undefined behaviour that
        # shows as garbage or a crash, in one language, on one screen. Two
        # French strings had three where English had two, from trying to make
        # the participle agree in number as well as the noun.
        import re as _re
        spec = lambda s: sorted(_re.findall(r"%[-+0-9.*]*(?:ll|z|h)?[a-zA-Z%]", s))
        mismatched = 0
        for code in CODES:
            path = os.path.join(LANG, f"{code}.json")
            if not os.path.exists(path):
                continue
            d = json.load(open(path, encoding="utf-8"))
            for key, val in d.items():
                if val and spec(key) != spec(val):
                    print(f"  {code}: specifiers differ\n      en: {key!r}\n      {code}: {val!r}")
                    mismatched += 1
        if mismatched:
            print(f"{mismatched} translation(s) with mismatched format specifiers")
            return 1

        # A LATIN LETTER INSIDE A CYRILLIC WORD IS A TYPO, and an invisible
        # one: "заклік" with a Latin k looks right in every editor and renders
        # right too, because both alphabets have that shape. It is only wrong
        # when somebody searches for the word, or when the font has one glyph
        # and not the other. Two of these got past me by eye.
        #
        # Format specifiers and real Latin tokens (Shift, Ctrl, GitHub) are not
        # typos, so they come out before anything is judged.
        import re
        CYR = set("абвгдежзийклмнопрстуфхцчшщъыьэюяёіїєґўәғқңөұүһ"
                  "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯЁІЇЄҐЎӘҒҚҢӨҰҮҺ")
        LAT = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")
        # INVISIBLE BIDI CONTROLS DRAW AS A BOX.
        #
        # U+200E and friends are the correct way to pin the direction of a
        # Latin word inside an Arabic sentence -- in a renderer that knows to
        # skip them. This game's atlas is built from the codepoints the
        # language file asks for, so a mark it cannot draw becomes a .notdef
        # box, and ".odstate حفظ" came back with a visible LRM tofu in front of
        # it. The one-level reorderer in Arabic.cpp already keeps Latin runs
        # left-to-right, so the marks buy nothing here and cost a box each.
        bidi = {0x200E, 0x200F, 0x061C, 0x202A, 0x202B, 0x202C, 0x202D, 0x202E,
                0x2066, 0x2067, 0x2068, 0x2069}
        marks = 0
        for code in CODES:
            path = os.path.join(LANG, f"{code}.json")
            if not os.path.exists(path):
                continue
            d = json.load(open(path, encoding="utf-8"))
            for key, val in d.items():
                if val and any(ord(c) in bidi for c in val):
                    print(f"  {code}: invisible bidi control in {key!r}")
                    marks += 1
        if marks:
            print(f"{marks} translation(s) with bidi control characters")
            return 1

        # A CHARACTER FROM A SCRIPT THE LANGUAGE DOES NOT USE AT ALL.
        #
        # The mixed-script check below only sees Latin next to Cyrillic in one
        # word. It does not see a lone CJK character sitting in a Belarusian
        # sentence, which is what a slip of the hand produces and what no
        # spellcheck in the pipeline would catch: it renders as a box or, worse,
        # as a plausible-looking mark, in one string, on one screen.
        STRAY = [
            ("CJK", lambda c: 0x3000 <= ord(c) <= 0x9FFF or 0xF900 <= ord(c) <= 0xFAFF),
            ("Hangul", lambda c: 0xAC00 <= ord(c) <= 0xD7AF),
            ("Arabic", lambda c: 0x0600 <= ord(c) <= 0x06FF or 0xFB50 <= ord(c) <= 0xFEFF),
            ("Devanagari", lambda c: 0x0900 <= ord(c) <= 0x097F),
            ("Cyrillic", lambda c: 0x0400 <= ord(c) <= 0x04FF),
        ]
        # Which of those each language is allowed to contain.
        EXPECTED = {
            "ja": {"CJK"}, "zh": {"CJK"}, "ko": {"Hangul", "CJK"},
            "ar": {"Arabic"}, "hi": {"Devanagari"}, "ur": {"Arabic"},
            "uk": {"Cyrillic"}, "be": {"Cyrillic"}, "kk": {"Cyrillic"},
            "bg": {"Cyrillic"},
        }
        strays = 0
        # THE NAMES FILES TOO. data/lang/<code>.names.json is the country and
        # place names, written by hand in twenty scripts, and it is exactly as
        # easy to slip a Hangul syllable into a Belarusian word there as it is
        # in the interface file -- easier, because it is two hundred and fifty
        # proper nouns in a row and the eye stops reading them.
        for code in CODES:
            for suffix in (".json", ".names.json"):
                path = os.path.join(LANG, code + suffix)
                if not os.path.exists(path):
                    continue
                d = json.load(open(path, encoding="utf-8"))
                allowed = EXPECTED.get(code, set())
                for key, val in d.items():
                    if not val:
                        continue
                    for name, test in STRAY:
                        if name in allowed:
                            continue
                        bad = [c for c in val if test(c)]
                        if bad:
                            print(f"  {code}{suffix}: stray {name} {bad!r} in {val!r}")
                            strays += 1
        if strays:
            print(f"{strays} name(s) with characters from the wrong script")
            return 1

        # A LATIN LETTER IN A NAME WRITTEN IN ANOTHER SCRIPT.
        #
        # The mixed-script rule below only fires when both alphabets are in the
        # SAME word, so it cannot see a name half-typed in one and half in the
        # other -- "الفاروaz" for Faroese sat in the Arabic names file looking
        # like a word, because the eye reads an Arabic run and stops. A proper
        # noun in these ten languages is written in that language's own script
        # and nothing else, so any Latin letter at all is a slip.
        latin = 0
        for code, allowed in EXPECTED.items():
            path = os.path.join(LANG, f"{code}.names.json")
            if not os.path.exists(path):
                continue
            d = json.load(open(path, encoding="utf-8"))
            for key, val in d.items():
                if val and any(c in LAT for c in val):
                    print(f"  {code}.names.json: Latin letters in {key!r} -> {val!r}")
                    latin += 1
        if latin:
            print(f"{latin} name(s) with Latin letters in a non-Latin script")
            return 1

        strays = 0
        for code in CODES:
            path = os.path.join(LANG, f"{code}.json")
            if not os.path.exists(path):
                continue
            d = json.load(open(path, encoding="utf-8"))
            allowed = EXPECTED.get(code, set())
            for key, val in d.items():
                if not val:
                    continue
                for name, test in STRAY:
                    if name in allowed:
                        continue
                    bad = [c for c in val if test(c)]
                    if bad:
                        print(f"  {code}: stray {name} {bad!r} in {val!r}")
                        strays += 1
        if strays:
            print(f"{strays} translation(s) with characters from the wrong script")
            return 1

        problems = 0
        for code in CODES:
            path = os.path.join(LANG, f"{code}.json")
            if not os.path.exists(path):
                continue
            d = json.load(open(path, encoding="utf-8"))
            for key, val in d.items():
                if not val:
                    continue
                stripped = re.sub(r"%[-+0-9.*]*(?:ll|z|h)?[a-zA-Z]", " ", val)
                for word in re.findall(r"[^\s.,:;!?()\[\]/|+—–-]+", stripped):
                    if any(c in CYR for c in word) and any(c in LAT for c in word):
                        print(f"  {code}: {word!r} in {key!r}")
                        problems += 1
        print(f"{problems} mixed-script word(s)")
        return 1 if problems else 0

    if "--report" in sys.argv:
        print(f"{'':6} {'done':>6} {'left':>6}   of {len(en)}")
        for code in CODES:
            path = os.path.join(LANG, f"{code}.json")
            if not os.path.exists(path):
                print(f"{code:6} {'-':>6} {'-':>6}   no file")
                continue
            d = json.load(open(path, encoding="utf-8"))
            done = sum(1 for k in en if d.get(k))
            print(f"{code:6} {done:6} {len(en) - done:6}   {100.0 * done / max(1, len(en)):5.1f}%")
        return 0

    os.makedirs(LANG, exist_ok=True)
    for code in CODES:
        path = os.path.join(LANG, f"{code}.json")
        have = {}
        if os.path.exists(path):
            have = json.load(open(path, encoding="utf-8"))
        out = {k: have.get(k, "") for k in en}
        dropped = [k for k in have if k not in en and have[k]]
        with open(path, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=1, sort_keys=True)
            f.write("\n")
        done = sum(1 for v in out.values() if v)
        note = f", {len(dropped)} dropped" if dropped else ""
        print(f"{code}: {done}/{len(out)} translated{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
