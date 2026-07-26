#!/usr/bin/env bash
# Builds hello-panel-as.odmod. Needs node; installs AssemblyScript on first run.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

command -v node >/dev/null || { echo "node is required"; exit 1; }
[ -d "$here/node_modules" ] || (cd "$here" && npm install)

(cd "$here" && ./node_modules/.bin/asc assembly/index.ts --config asconfig.json --target release)

python3 "$root/tools/wasm_imports.py" "$here/mod.wasm" >/dev/null
"$root/tools/pack_odmod.sh" "$here" "$here/hello-panel-as.odmod"
