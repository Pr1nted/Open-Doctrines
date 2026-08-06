#!/usr/bin/env python3
"""Assert that every frozen Gearbox baseline is still honoured by sdk/abi.json.

WHY THIS EXISTS SEPARATELY FROM ModAbiTest.

ModAbiTest answers "does abi.json describe the host this build has?" -- both
files change together, and it passes happily if a function is deleted from both.
That is the wrong question for compatibility. The right one is "does the host
this build has still satisfy the contract a mod compiled a year ago was written
against?", and answering it needs a copy of that contract from back then. That is
sdk/compat/abi-<major>.<minor>.json: frozen, never regenerated, only ever added
to when a new minor ships.

Within one major version the rule is append-only. A symbol may be ADDED. It may
never be renamed, re-signed, or moved to a different capability, because a
.wasm that already exists imports it by name and signature and will fail to
instantiate if either moved -- and a capability change would silently hand a mod
either less access than it needs or more than the player agreed to.

Run with no arguments from the repo root. Exit status 1 on any break.
"""
import json
import os
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    cur_path = os.path.join(ROOT, 'sdk', 'abi.json')
    with open(cur_path) as f:
        cur = json.load(f)

    cur_imports = {(i['module'], i['name']): i for i in cur['imports']}
    cur_modules = {m['name'] for m in cur['modules']}
    cur_exports = {e['name'] for e in cur['exports']}
    cur_ver = cur.get('gearbox', '?')

    baselines = sorted(glob.glob(os.path.join(ROOT, 'sdk', 'compat', 'abi-*.json')))
    if not baselines:
        print('no baselines under sdk/compat/ -- nothing to check', file=sys.stderr)
        return 1

    breaks = []
    checked = 0

    for path in baselines:
        with open(path) as f:
            base = json.load(f)
        ver = base['gearbox']
        rel = os.path.relpath(path, ROOT)

        if base_major(ver) != base_major(cur_ver):
            print(f'  skip  {rel}: major {base_major(ver)} != current '
                  f'{base_major(cur_ver)}; a major bump is allowed to break')
            continue

        for imp in base['imports']:
            key = (imp['module'], imp['name'])
            sym = f"{imp['module']}/{imp['name']}"
            checked += 1
            got = cur_imports.get(key)
            if got is None:
                breaks.append(f'{ver}: {sym} was REMOVED')
                continue
            if got['signature'] != imp['signature']:
                breaks.append(
                    f"{ver}: {sym} signature changed "
                    f"{imp['signature']} -> {got['signature']}")
            if got.get('capability') != imp.get('capability'):
                breaks.append(
                    f"{ver}: {sym} capability changed "
                    f"{imp.get('capability')} -> {got.get('capability')}")

        for m in base['modules']:
            checked += 1
            if m not in cur_modules:
                breaks.append(f'{ver}: module {m} was REMOVED')

        for e in base['exports']:
            checked += 1
            if e not in cur_exports:
                breaks.append(f'{ver}: export {e} was REMOVED')

        print(f'  ok    {rel}: {len(base["imports"])} imports, '
              f'{len(base["modules"])} modules, {len(base["exports"])} exports')

    print()
    if breaks:
        print(f'{len(breaks)} COMPATIBILITY BREAK(S) against a frozen baseline:')
        for b in breaks:
            print(f'  - {b}')
        print()
        print('Within one major version the ABI is append-only. Either put the')
        print('symbol back, or -- if the break is genuinely intended -- bump the')
        print('major version, which is the thing mods are allowed to notice.')
        return 1

    with open(baselines[-1]) as f:
        newest = json.load(f)
    added = len(cur_imports) - len(newest['imports'])
    print(f'{checked} checks, 0 broken. '
          f'Current ABI is {cur_ver} with {added} imports added since '
          f'{newest["gearbox"]}.')
    return 0


def base_major(v: str) -> str:
    return v.split('.', 1)[0]


if __name__ == '__main__':
    sys.exit(main())
