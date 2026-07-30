#!/usr/bin/env bash
# Compiles the code examples embedded in the documentation.
#
# Documentation that does not compile is worse than none, and a hello-world is
# exactly the code a reader copies verbatim. This extracts the first C block and
# the first JSON block from docs/gearbox-sdk.md, builds them into a real .odmod,
# and loads it with odmod-check.
#
# Skips (exit 0) if no wasm-capable clang is available.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
work="${1:-$root/build/doccheck}"
check="${2:-$root/build/odmod-check}"

CC="${CC:-}"
if [ -z "$CC" ]; then
    # The Windows paths matter: LLVM's installer there does not add clang to
    # PATH, so this skipped on a machine that had just installed it -- while
    # tests/build_test_mods.sh, with a different list, found one fine.
    for c in "$(command -v clang || true)" \
             /opt/homebrew/opt/llvm/bin/clang \
             /usr/local/opt/llvm/bin/clang \
             "/c/Program Files/LLVM/bin/clang.exe" \
             "/c/Program Files (x86)/LLVM/bin/clang.exe" \
             /opt/homebrew/Cellar/emscripten/*/libexec/llvm/bin/clang \
             /usr/local/Cellar/emscripten/*/libexec/llvm/bin/clang; do
        [ -x "$c" ] || continue
        if "$c" --print-targets 2>/dev/null | grep -qi wasm32; then CC="$c"; break; fi
    done
fi
[ -n "$CC" ] || { echo "no wasm32-capable clang; skipping doc example check"; exit 0; }

# A python that runs, not one that merely exists: Windows keeps a python3.exe
# stub in WindowsApps that prints a Store advertisement and exits.
PY="$("$root/tools/find_python.sh")" || {
    echo "no working python3; skipping doc example check"; exit 0; }

rm -rf "$work"; mkdir -p "$work"

$PY - "$root" "$work" <<'PY'
import re, sys
root, work = sys.argv[1], sys.argv[2]
src = open(f"{root}/docs/gearbox-sdk.md").read()
c = re.search(r"```c\n(.*?)```", src, re.S)
j = re.search(r"```json\n(.*?)```", src, re.S)
if not c or not j:
    raise SystemExit("could not find a C block and a JSON block in gearbox-sdk.md")
open(f"{work}/mod.c", "w").write(c.group(1))
open(f"{work}/MANIFEST.json", "w").write(j.group(1))
PY

"$CC" --target=wasm32 -nostdlib -O2 -I "$root/sdk" \
      -Wl,--no-entry -Wl,--allow-undefined -o "$work/mod.wasm" "$work/mod.c"

$PY "$root/tools/wasm_imports.py" "$work/mod.wasm" >/dev/null
"$root/tools/pack_odmod.sh" "$work" "$work/doc.odmod" >/dev/null

if [ -x "$check" ]; then
    "$check" "$work/doc.odmod" >/dev/null 2>&1 || {
        echo "FAIL: the documented example does not load"; exit 1; }
    echo "ok: docs/gearbox-sdk.md example compiles, packs and loads"
else
    echo "ok: docs/gearbox-sdk.md example compiles and packs (odmod-check not built)"
fi
