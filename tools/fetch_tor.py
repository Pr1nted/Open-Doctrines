#!/usr/bin/env python3
"""Put the Tor Project's own `tor` into a release, so players need not install one.

    python3 tools/fetch_tor.py <dest-data-dir> [--platform macos-aarch64]
                               [--archive file.tar.gz] [--cache DIR]

Writes <dest>/tor/tor (tor.exe on Windows) with the libraries it needs beside
it, and <dest>/tor/licenses/ with the bundle's licence texts. The game finds it
there (src/net/TorClient.cpp) and starts it only when a connection wants Tor.

What is left out of the Expert Bundle, on purpose:
  pluggable_transports/  bridges (lyrebird, conjure) -- ~25 MB, and the game
                         never configures a bridge
  data/geoip, geoip6     ~26 MB; only needed for country-based exit choice

Every archive is checked against tools/data/tor_bundles.json before a byte of
it is unpacked; a mismatch is fatal. On macOS the binary is ad-hoc signed --
an unsigned arm64 binary is killed by the kernel the moment it starts.
"""
import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tools", "data", "tor_bundles.json")


def _host_os():
    if sys.platform == "darwin":
        return "macos"
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform.startswith("linux"):
        return "linux"
    # FreeBSD and OpenBSD used to fall through to "linux" here, which would
    # have put a Linux tor into a BSD release had one ever passed --tor. The
    # Tor Project publishes no BSD bundle, so these names simply have no pin.
    return sys.platform.rstrip("0123456789")


def host_platform():
    m = platform.machine().lower()
    arm = m in ("arm64", "aarch64")
    os_ = _host_os()
    if os_ == "macos":
        return "macos-aarch64" if arm else "macos-x86_64"
    if os_ == "windows":
        return "windows-x86_64"
    # The Tor Project publishes no Expert Bundle for Arm Linux; that platform
    # has no pin, and its release uses an installed tor.
    if os_ == "linux":
        return "linux-aarch64" if arm else "linux-x86_64"
    return f"{os_}-{m}"


# ── THE PLATFORM THE BINARY RUNS ON, NOT THE ONE THAT BUILT IT ──
#
# host_platform() named the build machine. That was the same thing while every
# release was built natively, and stops being so the moment one is not: a 32-bit
# Windows build made on the x64 runner would have carried the x64 tor.exe, which
# a 32-bit Windows cannot start, and a cross-compiled Arm Linux build the x86-64
# tor. So the architecture is read from the executable's own header. The OS
# still comes from the host -- releases cross architectures, never systems.
_PE_MACHINE = {0x014C: "i686", 0x8664: "x86_64", 0xAA64: "aarch64"}
_ELF_MACHINE = {3: "i686", 62: "x86_64", 40: "armv7", 183: "aarch64", 243: "riscv64"}


def binary_arch(path):
    """Architecture of a PE or ELF executable, or None for anything else
    (a Mach-O, an .app directory) -- macOS releases are built natively."""
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as f:
        head = f.read(64)
        if head[:2] == b"MZ" and len(head) >= 0x40:
            pe = int.from_bytes(head[0x3C:0x40], "little")
            f.seek(pe)
            sig = f.read(6)
            if sig[:4] == b"PE\0\0":
                return _PE_MACHINE.get(int.from_bytes(sig[4:6], "little"))
            return None
        if head[:4] == b"\x7fELF" and len(head) >= 20:
            order = "little" if head[5] == 1 else "big"
            return _ELF_MACHINE.get(int.from_bytes(head[18:20], order))
    return None


def target_platform(binary):
    """The Tor bundle name for the system `binary` will run on."""
    arch = binary_arch(binary)
    if arch is None:
        return host_platform()
    os_ = _host_os()
    if os_ == "windows" and arch == "aarch64":
        # The Tor Project builds no Arm Windows tor. The 32-bit x86 one runs
        # under the emulation every Windows-on-Arm release has (x64 emulation
        # is Windows 11 only), so it is the one that works everywhere.
        return "windows-i686"
    return f"{os_}-{arch}"


def pinned(plat):
    return plat in json.load(open(MANIFEST))["sha256"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(dest, plat=None, archive=None, cache=None):
    main([dest] + (["--platform", plat] if plat else []) +
         (["--archive", archive] if archive else []) + (["--cache", cache] if cache else []))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dest", help="the release's data directory")
    ap.add_argument("--platform", default=host_platform())
    ap.add_argument("--archive", help="a bundle already downloaded")
    ap.add_argument("--cache", default=os.environ.get("RUNNER_TEMP", "/tmp"))
    a = ap.parse_args(argv)

    man = json.load(open(MANIFEST))
    want = man["sha256"].get(a.platform)
    if not want:
        sys.exit(f"fetch_tor: no pinned bundle for {a.platform}")
    archive = a.archive
    if not archive:
        url = man["url"].format(version=man["version"], platform=a.platform)
        archive = os.path.join(a.cache, os.path.basename(url))
        if not (os.path.exists(archive) and sha256(archive) == want):
            print(f"fetch_tor: {url}")
            req = urllib.request.Request(url, headers={"User-Agent": "OpenDoctrines-release"})
            with urllib.request.urlopen(req, timeout=120) as r, open(archive, "wb") as f:
                shutil.copyfileobj(r, f)
    got = sha256(archive)
    if got != want:
        sys.exit(f"fetch_tor: checksum mismatch for {archive}\n  want {want}\n  got  {got}")

    out = os.path.join(a.dest, "tor")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "licenses"))
    with tarfile.open(archive) as t:
        for m in t.getmembers():
            n = m.name.lstrip("./")
            if not m.isfile():
                continue
            if n.startswith("tor/") and "/pluggable_transports/" not in "/" + n:
                target = os.path.join(out, n[len("tor/"):])
            elif n.startswith("docs/") and n.endswith(".txt") and \
                    os.path.basename(n) in ("tor.txt", "libevent.txt", "openssl.txt", "zlib.txt"):
                target = os.path.join(out, "licenses", os.path.basename(n))
            else:
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with t.extractfile(m) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
            os.chmod(target, 0o755 if m.mode & 0o111 else 0o644)

    exe = os.path.join(out, "tor.exe" if a.platform.startswith("windows") else "tor")
    if not os.path.exists(exe):
        sys.exit(f"fetch_tor: the bundle had no {os.path.basename(exe)}")
    if a.platform.startswith("macos") and sys.platform == "darwin":
        for f in os.listdir(out):
            p = os.path.join(out, f)
            if f == "tor" or f.endswith(".dylib"):
                subprocess.run(["codesign", "-s", "-", "-f", p], check=True,
                               capture_output=True)
    with open(os.path.join(out, "VERSION"), "w") as f:
        f.write(f"Tor Expert Bundle {man['version']} ({a.platform})\n")
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(out) for f in fs)
    print(f"fetch_tor: {a.platform} {man['version']} -> {out} ({size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main(sys.argv[1:])
