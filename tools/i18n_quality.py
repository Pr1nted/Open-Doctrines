#!/usr/bin/env python3
"""What `--lint` cannot see: translations that are complete, well-formed, and
still wrong.

`i18n_sync.py --lint` answers "is this file structurally sound" -- specifiers
match, no stray scripts, no invisible marks. Every language passed it while
the Japanese drew half-width punctuation in Japanese sentences, Turkish used
three different words for a claim, and a harbour was labelled with the word
for a TCP port in seven languages. None of that is malformed. All of it is
visible to a native reader in the first minute.

    python3 tools/i18n_quality.py            # errors fail, warnings report
    python3 tools/i18n_quality.py --strict   # warnings fail too
    python3 tools/i18n_quality.py --warn     # show the warning detail
"""

import json
import os
import re
import sys
import unicodedata as ud

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LANG = os.path.join(ROOT, "data", "lang")

CODES = ["uk", "be", "kk", "ja", "zh", "de", "it", "fr", "es", "cs", "sl", "sk",
         "pl", "af", "ar", "hi", "ko", "bg", "tr", "ur"]

CJK = ("ja", "zh")
ARABIC_SCRIPT = ("ar", "ur")

# The domain nouns a player reads on every screen. One concept should be one
# word: the tab, the button and the tooltip that all mean the same thing
# should not each pick a synonym.
TERMS = ["Province", "Country", "Unrest", "Industry", "Manpower", "Doctrine",
         "Claim", "Claims", "Treasury", "Turn", "Alliance", "Truce", "Research",
         "Economy", "Fortification", "Stability", "Prestige", "Supply",
         "Garrison", "Militia", "Coalition", "Embargo", "Tariff"]


# Pairs the stemmer cannot see are the same word, checked by hand and kept
# here so the check stays at zero and a NEW drift is visible on sight.
#
#   kk  Одақ / одағы          possessive, with the к->ғ mutation
#   cs  Výzkum / Zkoumejte    the imperative drops the vy- prefix
#   sk  Výskum / Skúmajte     the same
#   af  Bondgenootskap /      a generated country name is a different
#       Alliansie             register from the interface label
#   hi  दावे / दावा            plural against singular
#   ur  صوبہ / صوبے           oblique case; the final letter changes
#   ur  دعویٰ / دعوے           the same
ACCEPTED = {
    ("kk", "Alliance"), ("cs", "Research"), ("sk", "Research"),
    ("af", "Alliance"), ("hi", "Claims"), ("ur", "Province"), ("ur", "Claim"),
}


def is_cjk(ch):
    o = ord(ch)
    return 0x3040 <= o <= 0x30FF or 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF


def is_arabic(ch):
    o = ord(ch)
    return 0x0600 <= o <= 0x06FF or 0xFB50 <= o <= 0xFEFF


def stem(word):
    """Enough of a word to recognise it through inflection.

    Slavic and Turkic languages decline everything, so "Даследаванні" and
    "даследуйце" are the same word and a naive prefix of 60% says they are
    not -- which is how the first run of this check reported four languages
    as inconsistent when all four were correct.

    Two things had to give. The prefix is FOUR characters, because a Slavic
    verb and its noun share only the root ("Badania" / "zbadaj" share "bad").
    And the accents come off first: Czech writes the noun "Výzkum" and the
    verb "vyzkoumejte", and comparing them with the á intact says they are
    different words.
    """
    flat = ud.normalize("NFD", word.lower())
    flat = "".join(c for c in flat if not ud.combining(c))
    return flat[:4]


def load():
    en = json.load(open(os.path.join(LANG, "en.json"), encoding="utf-8"))
    return en, {c: json.load(open(os.path.join(LANG, f"{c}.json"), encoding="utf-8"))
                for c in CODES}


# ─── errors: a reader would call these broken ───────────────────────────────

def check_context_keys(en, langs, err):
    """A key carrying a '|' note must be translated in every language.

    The note exists because the bare English is ambiguous -- "Port" is a
    harbour on one screen and a TCP port on another. An empty value falls back
    to that bare English, which is the one word that cannot be right in both
    places, so "not translated yet" is not a safe state here the way it is
    everywhere else.
    """
    keys = [k for k in en if "|" in k]
    for c in CODES:
        for k in keys:
            if not langs[c].get(k):
                err.append(f"{c}: context key left untranslated: {k!r}")


def check_script_punctuation(en, langs, err):
    """Punctuation that follows a CJK or Arabic character belongs to that
    script.

    Japanese written with ASCII commas and colons reads as machine output --
    174 strings did, while the Chinese beside them used the full-width marks
    correctly. Only punctuation DIRECTLY after a character of the script is
    judged: the same mark after a digit or a format specifier ("%.1f", "%s: ")
    belongs to the Latin run and is right as it is.
    """
    full = {",": "、", ".": "。", "?": "？", "!": "！", ":": "：", ";": "；"}
    for c in CJK:
        for k, v in langs[c].items():
            if not v:
                continue
            for i in range(1, len(v)):
                # A RUN OF DOTS IS AN ELLIPSIS, NOT A FULL STOP. "浏览文件..."
                # is a button that opens a dialog; reading its first dot as
                # the end of a sentence is how the Japanese pass turned
                # "参照..." into "参照。..".
                if v[i] == "." and (v[i + 1:i + 2] == "." or v[i - 1] == "."):
                    continue
                if v[i] in full and is_cjk(v[i - 1]):
                    err.append(f"{c}: half-width {v[i]!r} after {v[i-1]!r} "
                               f"(want {full[v[i]]!r}) in {v!r}")
                    break
    for c in ARABIC_SCRIPT:
        want = {",": "،", "?": "؟", ";": "؛"}
        for k, v in langs[c].items():
            if not v:
                continue
            for i in range(1, len(v)):
                if v[i] in want and is_arabic(v[i - 1]):
                    err.append(f"{c}: Latin {v[i]!r} after Arabic "
                               f"(want {want[v[i]]!r}) in {v!r}")
                    break


def check_ellipsis(en, langs, err):
    """A full stop that swallowed the first dot of an ellipsis.

    Converting Japanese punctuation turned "参照..." into "参照。.." -- the
    dot followed a kanji, so it was read as the end of a sentence. It is the
    kind of damage that only a reader of the language would notice, so it is
    checked rather than remembered.
    """
    for c in CODES:
        for v in langs[c].values():
            if v and ("。." in v or "、." in v or "：." in v or ".。" in v):
                err.append(f"{c}: broken ellipsis in {v!r}")


def check_placeholders(en, langs, err):
    """Named placeholders and line breaks, which --lint does not cover.

    It checks printf specifiers; {month}-style tags and \\n are just as load
    bearing and just as easy to drop.
    """
    tag = re.compile(r"\{[a-z_]+\}")
    for c in CODES:
        for k, v in langs[c].items():
            if not v:
                continue
            if set(tag.findall(k)) != set(tag.findall(v)):
                err.append(f"{c}: placeholder tags differ\n      en: {k!r}\n      {c}: {v!r}")
            if k.count("\\n") != v.count("\\n"):
                err.append(f"{c}: line breaks differ\n      en: {k!r}\n      {c}: {v!r}")


def check_edge_space(en, langs, err):
    """A leading or trailing space is layout, and dropping it joins two words.

    With one principled exception: a full-width colon or bracket carries its
    own space, so Chinese ending "文件：" where English ended "File: " is
    correct and must not be reported.
    """
    carries_space = "：、。）」！？"
    opens_wide = "（「『"
    for c in CODES:
        for k, v in langs[c].items():
            if not v:
                continue
            if k.endswith(" ") and not v.endswith(" "):
                if v and v[-1] in carries_space:
                    continue
                err.append(f"{c}: lost the trailing space: {k!r} -> {v!r}")
            if k.startswith(" ") and not v.startswith(" "):
                # A full-width bracket carries its own space too, on the side
                # it opens: " (not coastal)" is correctly "（内陸）".
                if v and v[0] in opens_wide:
                    continue
                err.append(f"{c}: lost the leading space: {k!r} -> {v!r}")


# ─── warnings: a human has to look ──────────────────────────────────────────

def warn_terminology(en, langs, warn):
    """One concept, one word.

    Arabic is exempted from the article: "الصناعة" as a heading and "صناعة I"
    as a tier name are the same word correctly inflected, not two choices.
    """
    for c in CODES:
        d = langs[c]
        for t in TERMS:
            want = d.get(t)
            if not want or (c, t) in ACCEPTED:
                continue
            s = stem(want.lstrip("ال") if c == "ar" else want)
            for k, v in d.items():
                if not v or k == t or not re.search(rf"\b{t}\b", k):
                    continue
                hay = ud.normalize("NFD", v.lower())
                hay = "".join(ch for ch in hay if not ud.combining(ch))
                if s in hay or (c == "ar" and stem(want) in hay):
                    continue
                warn.append(f"{c}: {t!r} is {want!r} but {k!r} says {v!r}")


def warn_identical(en, langs, warn):
    """A value the same as its English key.

    Usually right -- Discord, GitHub, FPS, Napalm, and every month German
    spells the way English does. Occasionally a string nobody got to. Listed
    rather than judged, because only a reader of the language can tell which.
    """
    for c in CODES:
        n = [k for k, v in langs[c].items()
             if v == k and len(k) > 4 and re.search(r"[A-Za-z]{3}", k)]
        if n:
            warn.append(f"{c}: {len(n)} value(s) identical to the English: {n[:6]}")


def warn_collisions(en, langs, warn):
    """Two different English strings that came out as one.

    Mostly legitimate: a language with one construction for "%s Empire" and
    "Empire of %s" should use it for both. Worth a look when the two English
    strings are different ACTIONS -- "Back to Menu" and "Quit to Menu" shared
    a label in seven languages, and only one of them loses your game.
    """
    for c in CODES:
        inv = {}
        for k, v in langs[c].items():
            if v:
                inv.setdefault(v, []).append(k)
        hits = [(v, ks) for v, ks in inv.items()
                if len(ks) > 1 and len(v) > 3 and not any("%s" in k for k in ks)]
        if hits:
            warn.append(f"{c}: {len(hits)} translation(s) serving two English "
                        f"strings, e.g. {hits[0][1]} -> {hits[0][0]!r}")


def main():
    en, langs = load()
    err, warn = [], []
    for f in (check_context_keys, check_script_punctuation, check_ellipsis,
              check_placeholders, check_edge_space):
        f(en, langs, err)
    for f in (warn_terminology, warn_identical, warn_collisions):
        f(en, langs, warn)

    for e in err:
        print(f"  ERROR  {e}")
    if "--warn" in sys.argv or "--strict" in sys.argv:
        for w in warn:
            print(f"  warn   {w}")
    print(f"{len(err)} error(s), {len(warn)} warning(s) across {len(CODES)} languages"
          + ("" if ("--warn" in sys.argv or "--strict" in sys.argv) else "  (--warn to list)"))
    if err:
        return 1
    return 1 if (warn and "--strict" in sys.argv) else 0


if __name__ == "__main__":
    sys.exit(main())
