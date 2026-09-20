#!/usr/bin/env bash
# Builds hello-panel-js.odmod from main.js. All the work is in
# ../../build_mod.sh, which the TypeScript example shares.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
# Content is compiled in, not merely declared: every wasm import must
# resolve at instantiation, so this and MANIFEST.json move together.
export GBX_DEFS="-DGBX_WITH_CONTENT=1"
exec "$here/../../build_mod.sh" "$here" "$here/main.js" hello-panel-js.odmod
