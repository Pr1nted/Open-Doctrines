# Supported Languages, for the Steamworks matrix

**Installation → Supported Languages.** Tick **Interface** for every row below.
Leave *Full Audio* and *Subtitles* empty: the game has no voice acting and no
subtitled cutscenes, and claiming either invites a review complaining they are
missing.

## Why this field is worth more than it looks

Steam's store has a language filter, and most indie games list one to three
languages. Listing twenty-one puts this game in twenty browse surfaces that are
nearly empty of competition — the same algorithmic-discovery mechanism already
supplying 84% of the itch.io traffic, applied on the axis where this project is
unusually strong.

It also points at the audience with the strongest reason to want it. A free
grand strategy game matters most where €40 for Hearts of Iron IV is a serious
amount of money, and those are largely not English-speaking markets.

## The list

Verified against `data/lang/*.json` on 2026-09-11, not assumed. Every language
below is 96–99% translated, measured as entries whose value differs from the
English key:

| Steam name | file | translated |
|---|---|---|
| English | `en` | baseline, 1880 keys |
| Afrikaans | `af` | 98% |
| Arabic | `ar` | 99% |
| Belarusian | `be` | 99% |
| Bulgarian | `bg` | 99% |
| Czech | `cs` | 98% |
| French | `fr` | 96% |
| German | `de` | 97% |
| Hindi | `hi` | 99% |
| Italian | `it` | 97% |
| Japanese | `ja` | 99% |
| Kazakh | `kk` | 99% |
| Korean | `ko` | 99% |
| Polish | `pl` | 98% |
| Simplified Chinese | `zh` | 99% |
| Slovak | `sk` | 98% |
| Slovenian | `sl` | 98% |
| Spanish - Spain | `es` | 98% |
| Turkish | `tr` | 99% |
| Ukrainian | `uk` | 99% |
| Urdu | `ur` | 99% |

**Simplified, not Traditional.** Checked rather than guessed: `zh.json` contains
462 simplified-only characters and zero traditional ones. Ticking the wrong row
sends Taiwanese and Hong Kong players to a build they cannot read.

Several of these — Afrikaans, Belarusian, Hindi, Kazakh, Slovak, Slovenian,
Urdu — are not Steam *store* languages and appear only in the extended
Supported Languages list. That is normal and they are still tickable; it means
the store page itself will not be translated into them, only the game.

## One caveat to fix before release, or to accept knowingly

The translations are **80 strings behind English**: `en.json` holds 1880 keys,
every other file holds 1800. New UI strings are added to `en.json` — the
`translatable strings` check in `tests/run_all.sh` enforces that — and nothing
propagates them onwards, so the gap grows with each feature.

At 96–99% this is invisible to a player. It will not stay that way if it is
never addressed, and the first sign will be a review in one of these languages
complaining that a new screen is half English.

```bash
# What is missing, for any language:
python3 -c "
import json,sys
en=set(json.load(open('data/lang/en.json')))
x=set(json.load(open(f'data/lang/{sys.argv[1]}.json')))
print('\n'.join(sorted(en-x)))" pl
```
