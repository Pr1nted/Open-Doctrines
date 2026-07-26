#!/usr/bin/env bash
# Builds examples/hello_panel into hello-panel-rust.odmod.
#
# Two steps, and the second is the one people forget: cargo gives you a .wasm,
# but the game loads a .odmod, which is a zip with MANIFEST.json as its FIRST
# entry. tools/pack_odmod.sh is what guarantees that ordering; a hand-made zip
# usually gets it wrong and the loader answers with ManifestNotFirst.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
ex="$here/examples/hello_panel"

# Run from sdk/rust so .cargo/config.toml applies: cargo reads config from the
# working directory upwards, not from --manifest-path.
cd "$here"

command -v cargo >/dev/null 2>&1 || {
    echo "cargo not found. Install rustup from https://rustup.rs and then:"
    echo "  rustup target add wasm32-unknown-unknown"
    exit 1
}

# The target is a separate download from the toolchain, and its absence is a
# link error several screens long rather than a clear message. Check for it.
if command -v rustup >/dev/null 2>&1; then
    if ! rustup target list --installed 2>/dev/null | grep -qx wasm32-unknown-unknown; then
        echo "wasm32-unknown-unknown is not installed. Run:"
        echo "  rustup target add wasm32-unknown-unknown"
        exit 1
    fi
fi

cargo build --release \
      --manifest-path "$here/Cargo.toml" \
      --target wasm32-unknown-unknown \
      -p hello-panel

# Workspace target dir, cdylib name from [lib] name in the example's Cargo.toml.
wasm="$here/target/wasm32-unknown-unknown/release/hello_panel.wasm"
[ -f "$wasm" ] || { echo "cargo produced no $wasm"; exit 1; }

# pack_odmod.sh wants a directory holding MANIFEST.json and mod.wasm, so the
# built module goes in next to the manifest under its packaged name.
cp "$wasm" "$ex/mod.wasm"
echo "mod.wasm: $(wc -c < "$ex/mod.wasm" | tr -d ' ') bytes"

"$root/tools/pack_odmod.sh" "$ex" "$ex/hello-panel-rust.odmod"

# Same checks the game runs at load time, on your terminal. Optional: only if
# the tool has been built.
if [ -x "$root/build/odmod-check" ]; then
    "$root/build/odmod-check" "$ex/hello-panel-rust.odmod"
    # Rehearse the user revoking UI in Advanced, which a mod must survive.
    "$root/build/odmod-check" "$ex/hello-panel-rust.odmod" --revoke UI
else
    echo "note: build/odmod-check not built; skipping validation"
fi
