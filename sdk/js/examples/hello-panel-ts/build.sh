#!/usr/bin/env bash
# Builds hello-panel-ts.odmod: main.ts -> tsc -> QuickJS -> .odmod.
#
# tsc is looked for on PATH first, then under .toolchains/ where
# tools/sdk_toolchains.sh puts it. The compile step is here rather than in
# ../../build_mod.sh because QuickJS only speaks JavaScript, and hiding that
# would make a type error look like a runtime one.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
TC="$root/.toolchains"

TSC="${TSC:-}"
if [ -z "$TSC" ]; then
    if command -v tsc >/dev/null 2>&1; then
        TSC="$(command -v tsc)"
    elif [ -x "$TC/typescript/node_modules/.bin/tsc" ]; then
        TSC="$TC/typescript/node_modules/.bin/tsc"
    else
        echo "tsc not found. Install it with:" >&2
        echo "    tools/sdk_toolchains.sh install" >&2
        echo "or put tsc on PATH, or set TSC=/path/to/tsc" >&2
        exit 1
    fi
fi

# noEmitOnError is set in tsconfig.json, so a type error stops the build here
# rather than producing a mod that draws nonsense.
"$TSC" -p "$here/tsconfig.json"

exec "$here/../../build_mod.sh" "$here" "$here/build/main.js" hello-panel-ts.odmod
