#!/usr/bin/env python3
"""Build ANGLE at the commit tools/data/angle.json pins, for an OD_ANGLE game.

    python3 tools/build_angle.py --work DIR --out DIR [--cpu x64|arm64 ...]

Needs depot_tools on PATH (gclient, gn, autoninja). Run by
.github/workflows/angle-prebuilts.yml, one runner per platform; it also runs on
a developer machine with ~15 GB free, which is why it is a script and not
workflow steps.

What it produces in --out, flat, beside each other the way the game loads them:

  libEGL / libGLESv2       ANGLE itself (.dylib, .dll or .so)
  libvk_swiftshader        SwiftShader, ANGLE's Vulkan on the CPU -- the last
  vk_swiftshader_icd.json  rung of the game's backend ladder (Game.cpp)
  licenses/                every LICENSE in the checkout that was compiled in

Backends, per platform, are the game's ladder and nothing else: Metal on macOS,
Direct3D 11 and 9 on Windows, and Vulkan and desktop OpenGL everywhere. Several
--cpu values on macOS are built separately and joined with lipo, so one bundle
serves both Mac architectures.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIN = os.path.join(ROOT, "tools", "data", "angle.json")
WIN = sys.platform.startswith("win")
MAC = sys.platform == "darwin"


def run(cmd, cwd=None, env=None):
    print("+", " ".join(cmd), flush=True)
    # shell=True on Windows: gclient, gn and autoninja are .bat files there.
    subprocess.run(cmd, cwd=cwd, env=env, check=True, shell=WIN)


def gn_args(cpu):
    args = {
        "is_debug": "false",
        "is_component_build": "false",
        "symbol_level": "0",
        "dcheck_always_on": "false",
        "angle_build_tests": "false",
        "angle_has_frame_capture": "false",
        "angle_enable_gl": "true",
        "angle_enable_vulkan": "true",
        "angle_enable_swiftshader": "true",
        "angle_enable_wgpu": "false",
        "angle_enable_null": "false",
        "target_cpu": f'"{cpu}"',
    }
    if MAC:
        args["angle_enable_metal"] = "true"
    if WIN:
        args["angle_enable_d3d11"] = "true"
        args["angle_enable_d3d9"] = "true"
    return " ".join(f"{k}={v}" for k, v in args.items())


def lib(name):
    if WIN:
        return name.removeprefix("lib") + ".dll" if name == "libvk_swiftshader" else name + ".dll"
    return name + (".dylib" if MAC else ".so")


LIBS = ["libEGL", "libGLESv2", "libvk_swiftshader"]
# ANGLE's own checkout spells it SwiftShader (Chromium's is swiftshader).
TARGETS = ["libEGL", "libGLESv2",
           "third_party/SwiftShader/src/Vulkan:swiftshader_libvulkan",
           "third_party/SwiftShader/src/Vulkan:icd_file"]


def use_installed_windows_sdk(angle):
    """Point Chromium's toolchain at the newest Windows SDK this machine has.

    build/toolchain/win/setup_toolchain.py hardcodes the SDK it hands to
    vcvarsall, matching Google's packaged toolchain, and no gn argument
    overrides it. A machine with the Visual Studio it already
    has -- DEPOT_TOOLS_WIN_TOOLCHAIN=0, every CI runner -- usually has an older
    one, and gn gen stops on the missing include path. So the constant is
    rewritten in the checkout to the newest SDK that is installed.

    It does not stretch far. chromium/8135 asked for 10.0.28000 and, built
    against a runner's 10.0.26100, failed in Abseil on FILE_INFO_BY_HANDLE_CLASS.
    That is why the pin is a branch whose own toolchain names 10.0.26100 (the
    last are chromium/7982 and older; 7977 is Chrome 152's release branch). Move
    the pin past that and the runner needs the newer SDK installed first.
    """
    import re
    inc = r"C:\Program Files (x86)\Windows Kits\10\Include"
    found = sorted((d for d in os.listdir(inc) if re.fullmatch(r"10\.0\.\d+\.\d+", d)
                    and os.path.isdir(os.path.join(inc, d, "um"))),
                   key=lambda v: tuple(int(x) for x in v.split(".")))
    if not found:
        sys.exit("build_angle: no Windows 10/11 SDK under " + inc)
    sdk = found[-1]
    for rel in ("build/toolchain/win/setup_toolchain.py", "build/vs_toolchain.py"):
        path = os.path.join(angle, rel)
        text = open(path).read()
        new = re.sub(r"^SDK_VERSION = '[0-9.]+'", f"SDK_VERSION = '{sdk}'", text, flags=re.M)
        if new == text and f"SDK_VERSION = '{sdk}'" not in text:
            sys.exit(f"build_angle: no SDK_VERSION line to rewrite in {rel}")
        open(path, "w").write(new)
    print(f"build_angle: building against Windows SDK {sdk}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True, help="where the ANGLE checkout goes (~10 GB)")
    ap.add_argument("--out", required=True, help="where the finished bundle goes")
    ap.add_argument("--cpu", action="append", help="x64 or arm64; repeat on macOS for a universal bundle")
    a = ap.parse_args()
    src = json.load(open(PIN))["source"]
    import platform
    cpus = a.cpu or ["arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"]

    env = dict(os.environ)
    env["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"   # the runner's own Visual Studio, not Google's

    angle = os.path.join(a.work, "angle")
    if not os.path.isdir(os.path.join(angle, ".git")):
        os.makedirs(angle, exist_ok=True)
        run(["git", "init", "-q"], cwd=angle)
        run(["git", "remote", "add", "origin", src["repo"]], cwd=angle)
    run(["git", "fetch", "-q", "--depth", "1", "origin", src["commit"]], cwd=angle)
    run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=angle)
    run([sys.executable, "scripts/bootstrap.py"], cwd=angle, env=env)
    run(["gclient", "sync", "-D", "--no-history", "--shallow", "-j8"], cwd=angle, env=env)
    if WIN:
        use_installed_windows_sdk(angle)

    builds = []
    for cpu in cpus:
        outdir = os.path.join("out", f"Release-{cpu}")
        run(["gn", "gen", outdir, f"--args={gn_args(cpu)}"], cwd=angle, env=env)
        run(["autoninja", "-C", outdir] + TARGETS, cwd=angle, env=env)
        builds.append(os.path.join(angle, outdir))

    os.makedirs(a.out, exist_ok=True)
    for name in LIBS:
        parts = [os.path.join(b, lib(name)) for b in builds]
        dest = os.path.join(a.out, lib(name))
        if len(parts) > 1:
            run(["lipo", "-create", "-output", dest] + parts)
        else:
            shutil.copy2(parts[0], dest)
        if MAC:
            # An unsigned arm64 dylib is refused by the kernel the moment it loads.
            run(["codesign", "-s", "-", "-f", dest])
    shutil.copy2(os.path.join(builds[0], "vk_swiftshader_icd.json"), a.out)

    # Every licence in the tree that was checked out to build this. More than
    # strictly compiled in, never less: provenance.json names the two that matter
    # (ANGLE BSD-3-Clause, SwiftShader Apache-2.0) and these back them up.
    lic = os.path.join(a.out, "licenses")
    os.makedirs(lic, exist_ok=True)
    shutil.copy2(os.path.join(angle, "LICENSE"), os.path.join(lic, "ANGLE.txt"))
    for top, dirs, files in os.walk(os.path.join(angle, "third_party")):
        dirs[:] = [d for d in dirs if d not in (".git", "test", "tests", "testdata")]
        for f in files:
            if f in ("LICENSE", "LICENSE.txt", "LICENSE.TXT", "COPYING", "LICENSE.md"):
                rel = os.path.relpath(top, os.path.join(angle, "third_party")).replace(os.sep, "_")
                shutil.copy2(os.path.join(top, f), os.path.join(lic, f"{rel}.txt"))
    with open(os.path.join(a.out, "ANGLE_VERSION.txt"), "w") as f:
        f.write(f"{src['repo']} {src['branch']} {src['commit']}\n")
    print("built:", sorted(os.listdir(a.out)))


if __name__ == "__main__":
    main()
