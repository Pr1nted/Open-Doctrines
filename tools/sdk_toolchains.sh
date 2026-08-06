#!/usr/bin/env bash
# Install (or remove) the toolchains needed to build every Gearbox SDK example.
#
#   tools/sdk_toolchains.sh install     # ~2.2 GB, a few minutes
#   tools/sdk_toolchains.sh clean       # removes everything it installed
#   tools/sdk_toolchains.sh env         # prints the exports to eval
#
# Everything lands under .toolchains/ in the repo (gitignored), NOT in your
# home directory and NOT via Homebrew, so "clean" is an rm -rf and cannot break
# anything else on the machine. rustup is pointed at that directory with
# RUSTUP_HOME/CARGO_HOME and --no-modify-path, so it never touches ~/.cargo or
# your shell rc.
#
# Covers: Zig, TinyGo, wabt (wat2wasm), Rust, AssemblyScript, Maven (Java and
# Kotlin), TypeScript. Lua and JavaScript need only Emscripten, which this does
# not install.
# Does NOT cover C#/Swift — see docs/gearbox-languages.md for why.
#
# Verified on macOS/arm64. The URLs below are platform-detected but only the
# darwin-arm64 path has actually been run.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
TC="$root/.toolchains"
cmd="${1:-}"

case "$(uname -s)" in
    Darwin) OS=macos ;;
    Linux)  OS=linux ;;
    *) echo "unsupported OS: $(uname -s)"; exit 1 ;;
esac
case "$(uname -m)" in
    arm64|aarch64) ARCH=aarch64; GOARCH=arm64 ;;
    x86_64)        ARCH=x86_64;  GOARCH=amd64 ;;
    *) echo "unsupported arch: $(uname -m)"; exit 1 ;;
esac

print_env() {
    echo "export PATH=\"$TC/zig:$TC/wabt/bin:$TC/tinygo/bin:$TC/cargo/bin:$TC/maven/bin:$TC/typescript/node_modules/.bin:\$PATH\""
    echo "export RUSTUP_HOME=\"$TC/rustup\" CARGO_HOME=\"$TC/cargo\""
    # TinyGo refuses to build without wasm-opt. AssemblyScript's install brings
    # Binaryen with it, so reuse that rather than installing it twice.
    echo "export WASMOPT=\"$root/sdk/assemblyscript/node_modules/binaryen/bin/wasm-opt\""
}

case "$cmd" in
env)
    print_env
    exit 0
    ;;

clean)
    echo "removing $TC"
    rm -rf "$TC"
    echo "removing AssemblyScript install"
    rm -rf "$root/sdk/assemblyscript/node_modules" "$root/sdk/assemblyscript/package-lock.json"
    echo "removing per-language build caches"
    rm -rf "$root/sdk/rust/target" "$root/sdk/rust/examples/hello_panel/target" \
           "$root/sdk/swift/.build" "$root/sdk/zig/.zig-cache" "$root/sdk/zig/zig-out"
    find "$root/sdk" -name ".zig-cache" -o -name "zig-out" | while read -r d; do rm -rf "$d"; done
    # A Swift SDK, if one was installed, lives outside this directory.
    if command -v swift >/dev/null 2>&1 && swift sdk list 2>/dev/null | grep -q wasm; then
        echo "note: a SwiftWasm SDK is installed in your home directory. Remove with:"
        swift sdk list 2>/dev/null | sed 's/^/      swift sdk remove /'
    fi
    echo "done. Built .odmod files were left in place; delete them with:"
    echo "      find sdk -name '*.odmod' -delete"
    exit 0
    ;;

install) ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac

mkdir -p "$TC"
cd "$TC"
ok=0; bad=0
step() { printf '\n[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
good() { echo "  ok"; ok=$((ok+1)); }
fail() { echo "  FAILED: $*"; bad=$((bad+1)); }

# ---- Zig -------------------------------------------------------------------
step "Zig"
if [ -x "$TC/zig/zig" ]; then echo "  already installed"; ok=$((ok+1)); else
    ZURL=$(curl -sSL https://ziglang.org/download/index.json | python3 -c "
import json,sys
j=json.load(sys.stdin)
v=[k for k in j if k!='master'][0]
print(j[v].get('$ARCH-$OS',{}).get('tarball',''))")
    if [ -z "$ZURL" ]; then fail "no Zig build for $ARCH-$OS"; else
        curl -sSL "$ZURL" -o zig.tar.xz && tar xf zig.tar.xz && rm zig.tar.xz \
          && mv zig-$ARCH-$OS-* zig && good || fail "download/extract"
    fi
fi

# ---- wabt ------------------------------------------------------------------
step "wabt (wat2wasm)"
if [ -x "$TC/wabt/bin/wat2wasm" ]; then echo "  already installed"; ok=$((ok+1)); else
    # wabt's own naming, which is not this script's and not uname's. It used to
    # ship a single "ubuntu" tarball; it now publishes linux-x64, linux-arm64,
    # macos-arm64 and windows-x64. So "ubuntu" matched nothing and every CI run
    # that installed toolchains failed here -- invisible on a developer machine,
    # where .toolchains/wabt already exists and this branch never runs.
    # macos-aarch64 was wrong for the same reason: wabt says arm64.
    case "$ARCH" in
        aarch64) WARCH=arm64 ;;
        x86_64)  WARCH=x64 ;;
        *)       WARCH="$ARCH" ;;
    esac
    WPAT="$OS-$WARCH"
    WURL=$(curl -sSL https://api.github.com/repos/WebAssembly/wabt/releases/latest \
        | python3 -c "
import json,sys
d = json.load(sys.stdin)
names = [a['name'] for a in d['assets']]
for a in d['assets']:
    if '$WPAT' in a['name'] and a['name'].endswith('.tar.gz'):
        print(a['browser_download_url']); break
else:
    # Name what WAS there. A bare 'no build for X' says nothing about whether
    # the pattern is wrong or the release is, which is the whole question.
    sys.stderr.write('  wabt $WPAT not among: ' + ', '.join(names) + '\\n')")
    if [ -z "$WURL" ]; then fail "no wabt build for $WPAT"; else
        curl -sSL "$WURL" -o wabt.tgz && tar xf wabt.tgz && rm wabt.tgz \
          && mv wabt-* wabt && good || fail "download/extract"
    fi
fi

# ---- TinyGo ----------------------------------------------------------------
step "TinyGo"
if ! command -v go >/dev/null 2>&1; then
    fail "Go is required by TinyGo and is not installed"
elif [ -x "$TC/tinygo/bin/tinygo" ]; then echo "  already installed"; ok=$((ok+1)); else
    TURL=$(curl -sSL https://api.github.com/repos/tinygo-org/tinygo/releases/latest \
        | python3 -c "
import json,sys
for a in json.load(sys.stdin)['assets']:
    if '$OS'.replace('macos','darwin')+'-$GOARCH' in a['name'] and a['name'].endswith('.tar.gz'):
        print(a['browser_download_url']); break")
    if [ -z "$TURL" ]; then fail "no TinyGo build for this platform"; else
        curl -sSL "$TURL" -o tinygo.tgz && tar xf tinygo.tgz && rm tinygo.tgz && good \
          || fail "download/extract"
    fi
fi

# ---- Rust ------------------------------------------------------------------
step "Rust (minimal + wasm32-unknown-unknown)"
if [ -x "$TC/cargo/bin/rustc" ]; then echo "  already installed"; ok=$((ok+1)); else
    export RUSTUP_HOME="$TC/rustup" CARGO_HOME="$TC/cargo"
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o rustup-init.sh \
      && sh rustup-init.sh -y --no-modify-path --profile minimal \
             --default-toolchain stable --target wasm32-unknown-unknown >/dev/null 2>&1 \
      && good || fail "rustup"
    rm -f rustup-init.sh
fi

# ---- AssemblyScript (also supplies wasm-opt for TinyGo) --------------------
step "AssemblyScript"
if ! command -v npm >/dev/null 2>&1; then
    fail "npm is required and is not installed"
elif [ -d "$root/sdk/assemblyscript/node_modules" ]; then echo "  already installed"; ok=$((ok+1)); else
    # A shared npm cache with bad permissions is a common local problem; keep
    # our own so this never needs sudo.
    (cd "$root/sdk/assemblyscript" && npm install --cache "$TC/npmcache" >/dev/null 2>&1) \
      && good || fail "npm install"
fi

# ---- Maven (Java and Kotlin) ------------------------------------------------
# Kotlin needs nothing further: kotlinc arrives as a Maven plugin, and TeaVM
# compiles its bytecode exactly as it compiles javac's.
step "Maven (for the Java/Kotlin SDK)"
if ! command -v java >/dev/null 2>&1; then
    fail "a JDK 17+ is required for the Java/Kotlin SDK and is not installed"
elif command -v mvn >/dev/null 2>&1; then
    echo "  already on PATH"; ok=$((ok+1))
elif [ -x "$TC/maven/bin/mvn" ]; then
    echo "  already installed"; ok=$((ok+1))
else
    MV=3.9.9
    MURL="https://dlcdn.apache.org/maven/maven-3/$MV/binaries/apache-maven-$MV-bin.tar.gz"
    MALT="https://archive.apache.org/dist/maven/maven-3/$MV/binaries/apache-maven-$MV-bin.tar.gz"
    if curl -sSL "$MURL" -o maven.tgz && tar tzf maven.tgz >/dev/null 2>&1; then :
    else curl -sSL "$MALT" -o maven.tgz; fi
    tar xzf maven.tgz && rm maven.tgz && mv "apache-maven-$MV" maven && good \
      || fail "download/extract"
fi

# ---- TypeScript -------------------------------------------------------------
# Only the TS example needs this; a plain JavaScript mod needs nothing beyond
# Emscripten, which this script does not install.
step "TypeScript (for the TS example)"
if ! command -v npm >/dev/null 2>&1; then
    fail "npm is required and is not installed"
elif command -v tsc >/dev/null 2>&1; then
    echo "  already on PATH"; ok=$((ok+1))
elif [ -x "$TC/typescript/node_modules/.bin/tsc" ]; then
    echo "  already installed"; ok=$((ok+1))
else
    mkdir -p "$TC/typescript"
    (cd "$TC/typescript" && npm install --silent --no-audit --no-fund \
        --cache "$TC/npmcache" typescript >/dev/null 2>&1) \
      && good || fail "npm install typescript"
fi

# ---- wasi-sdk 20 + libpython (Python) ---------------------------------------
# The version is pinned deliberately. libpython below is built against
# wasi-sdk 20's libc, and linking it with a newer wasi-sdk yields a module that
# loads and then faults with "out of bounds memory access" inside CPython's
# startup -- no warning, no link error. Do not "upgrade" this without also
# rebuilding libpython.
step "wasi-sdk 20 (for the Python SDK)"
if [ -x "$TC/wasi-sdk-20/bin/clang" ]; then echo "  already installed"; ok=$((ok+1)); else
    WSURL="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-20/wasi-sdk-20.0-$OS.tar.gz"
    if curl -sSL "$WSURL" -o wasi-sdk.tgz && tar tzf wasi-sdk.tgz >/dev/null 2>&1; then
        tar xzf wasi-sdk.tgz && rm wasi-sdk.tgz && mv wasi-sdk-20.0 wasi-sdk-20 && good
    else
        fail "no wasi-sdk 20 build for $OS"
    fi
fi

step "libpython 3.12 for wasm32-wasi"
if [ -f "$TC/libpython/lib/wasm32-wasi/libpython3.12.a" ]; then
    echo "  already installed"; ok=$((ok+1))
else
    # VMware Wasm Labs publishes CPython built for wasm32-wasi, including the
    # static library, headers and the stdlib zip that gen_frozen.py needs.
    LPURL="https://github.com/vmware-labs/webassembly-language-runtimes/releases/download/python%2F3.12.0%2B20231211-040d5a6/libpython-3.12.0-wasi-sdk-20.0.tar.gz"
    if curl -sSL "$LPURL" -o libpython.tgz && tar tzf libpython.tgz >/dev/null 2>&1; then
        mkdir -p libpython && tar xzf libpython.tgz -C libpython && rm libpython.tgz
        # gen_frozen.py and build.sh expect the stdlib zip beside the library.
        if [ -f "$TC/libpython/usr/local/lib/python312.zip" ]; then
            cp "$TC/libpython/usr/local/lib/python312.zip" "$TC/libpython/lib/"
        fi
        good
    else
        fail "download/extract libpython"
    fi
fi

printf '\n%d ok, %d failed. Installed under %s (%s)\n' \
       "$ok" "$bad" "$TC" "$(du -sh "$TC" 2>/dev/null | cut -f1)"
printf '\nAdd them to your shell with:\n    eval "$(tools/sdk_toolchains.sh env)"\nthen:\n    tools/test_all_sdks.sh\n'
exit $([ $bad -eq 0 ] && echo 0 || echo 1)
