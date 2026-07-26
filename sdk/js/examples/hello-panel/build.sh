#!/usr/bin/env bash
# Builds hello-panel-js.odmod from main.js. All the work is in
# ../../build_mod.sh, which the TypeScript example shares.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
exec "$here/../../build_mod.sh" "$here" "$here/main.js" hello-panel-js.odmod
