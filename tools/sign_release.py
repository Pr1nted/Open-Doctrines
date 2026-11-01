#!/usr/bin/env python3
"""Sign a release: write a manifest of every asset's SHA-256 and an Ed25519
signature over it, for the client to verify against the baked public key.

    tools/sign_release.py --version 1.3.3b --out <dir> --key <priv.pem> \
        OpenDoctrines-linux-x64.zip OpenDoctrines-windows-x64.zip ...

  * --key <path>, or the env var RELEASE_SIGNING_KEY holding the PEM itself
    (how CI passes the secret). Exactly one must be given.
  * Writes <out>/release.manifest and <out>/release.manifest.sig, which the
    release step uploads as assets alongside the archives.

THE MANIFEST is plain text, signed byte-for-byte:

    opendoctrines-release v1
    version 1.3.3b
    OpenDoctrines-linux-x64.zip <64 hex>
    OpenDoctrines-windows-x64.zip <64 hex>
    ...

asset lines sorted by name so the bytes are reproducible. The signature is raw
Ed25519 (64 bytes) over those exact bytes -- PureEdDSA, the scheme the client's
ed25519Verify checks. GameUpdates downloads both, verifies the signature with the
compiled-in public key, then checks the asset it fetched against the SIGNED
SHA-256 rather than the one the API reported. A release host that swaps an asset
cannot produce a signature for it without the offline private key.

The whole point is that this runs where the SHA-256s are truthful -- the CI job
that just built the archives -- and the key never leaves there. Do not wire this
to sign arbitrary downloaded files.
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

MAGIC = "opendoctrines-release v1"


def sha256_hex(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_manifest(version: str, assets: list) -> bytes:
    lines = [MAGIC, f"version {version}"]
    for name, digest in sorted((os.path.basename(a), sha256_hex(a)) for a in assets):
        if " " in name:
            raise SystemExit(f"asset name has a space, which the manifest cannot carry: {name}")
        lines.append(f"{name} {digest}")
    # Trailing newline so the file is a clean text file; the signature covers it.
    return ("\n".join(lines) + "\n").encode("utf-8")


def sign(manifest: bytes, key_pem_path: str) -> bytes:
    with tempfile.NamedTemporaryFile(delete=False) as mf:
        mf.write(manifest)
        msg_path = mf.name
    sig_path = msg_path + ".sig"
    try:
        subprocess.run(
            ["openssl", "pkeyutl", "-sign", "-inkey", key_pem_path,
             "-rawin", "-in", msg_path, "-out", sig_path],
            check=True, capture_output=True)
        with open(sig_path, "rb") as f:
            sig = f.read()
    finally:
        for p in (msg_path, sig_path):
            try:
                os.unlink(p)
            except OSError:
                pass
    if len(sig) != 64:
        raise SystemExit(f"expected a 64-byte Ed25519 signature, got {len(sig)}")
    return sig


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True, help="directory to write the manifest + sig")
    ap.add_argument("--key", help="private key PEM path (else env RELEASE_SIGNING_KEY)")
    ap.add_argument("assets", nargs="+", help="the release asset files")
    args = ap.parse_args()

    for a in args.assets:
        if not os.path.isfile(a):
            raise SystemExit(f"no such asset: {a}")

    manifest = build_manifest(args.version, args.assets)

    key_path, cleanup = args.key, None
    if not key_path:
        pem = os.environ.get("RELEASE_SIGNING_KEY")
        if not pem:
            raise SystemExit("pass --key, or set RELEASE_SIGNING_KEY to the PEM")
        fd, key_path = tempfile.mkstemp(suffix=".pem")
        os.write(fd, pem.encode("utf-8"))
        os.close(fd)
        os.chmod(key_path, 0o600)
        cleanup = key_path
    try:
        sig = sign(manifest, key_path)
    finally:
        if cleanup:
            os.unlink(cleanup)

    os.makedirs(args.out, exist_ok=True)
    man_path = os.path.join(args.out, "release.manifest")
    sig_path = os.path.join(args.out, "release.manifest.sig")
    with open(man_path, "wb") as f:
        f.write(manifest)
    with open(sig_path, "wb") as f:
        f.write(sig)

    print(f"wrote {man_path} ({len(manifest)} bytes) and {sig_path} (64 bytes)")
    print(manifest.decode("utf-8"), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
