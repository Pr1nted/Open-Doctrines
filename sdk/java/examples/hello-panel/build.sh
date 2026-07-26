#!/usr/bin/env bash
# Builds hello-panel-java.odmod. Needs a JDK 17+ and Maven.
#
# Maven is looked for on PATH first, then under .toolchains/ where
# tools/sdk_toolchains.sh puts it. The local repository is kept in
# .toolchains/m2 rather than ~/.m2 so that a build here cannot disturb your
# other Java work, and `tools/sdk_toolchains.sh clean` really does remove
# everything this SDK downloaded.
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

# -q so a successful build is silent; TeaVM is chatty otherwise.
"$MVN" -q -f "$root/sdk/java/pom.xml" \
       -Dmaven.repo.local="$TC/m2" \
       package

wasm="$here/target/wasm/classes.wasm"
[ -f "$wasm" ] || { echo "TeaVM produced no wasm at $wasm" >&2; exit 1; }
cp "$wasm" "$here/mod.wasm"

python3 "$root/tools/wasm_imports.py" "$here/mod.wasm"
"$root/tools/pack_odmod.sh" "$here" "$here/hello-panel-java.odmod"

ls -l "$here/mod.wasm" "$here/hello-panel-java.odmod" \
    | awk '{printf "  %-12s %8d bytes\n", $NF, $5}'
