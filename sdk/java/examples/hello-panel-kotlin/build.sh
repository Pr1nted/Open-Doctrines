#!/usr/bin/env bash
# Builds hello-panel-kotlin.odmod. Needs a JDK 17+ and Maven; kotlinc arrives as
# a Maven plugin, so Kotlin adds no toolchain of its own.
#
# See ../hello-panel/build.sh for why the local repository is kept under
# .toolchains/ rather than ~/.m2.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
TC="$root/.toolchains"

MVN="${MVN:-}"
if [ -z "$MVN" ]; then
    if command -v mvn >/dev/null 2>&1; then
        MVN="$(command -v mvn)"
    elif [ -x "$TC/maven/bin/mvn" ]; then
        MVN="$TC/maven/bin/mvn"
    else
        echo "Maven not found. Install it with:" >&2
        echo "    tools/sdk_toolchains.sh install" >&2
        echo "or put mvn on PATH, or set MVN=/path/to/mvn" >&2
        exit 1
    fi
fi

command -v java >/dev/null 2>&1 || { echo "no java on PATH (JDK 17+ needed)" >&2; exit 1; }

"$MVN" -q -f "$root/sdk/java/pom.xml" \
       -Dmaven.repo.local="$TC/m2" \
       package

wasm="$here/target/wasm/classes.wasm"
[ -f "$wasm" ] || { echo "TeaVM produced no wasm at $wasm" >&2; exit 1; }
cp "$wasm" "$here/mod.wasm"

python3 "$root/tools/wasm_imports.py" "$here/mod.wasm"
"$root/tools/pack_odmod.sh" "$here" "$here/hello-panel-kotlin.odmod"

ls -l "$here/mod.wasm" "$here/hello-panel-kotlin.odmod" \
    | awk '{printf "  %-12s %8d bytes\n", $NF, $5}'
