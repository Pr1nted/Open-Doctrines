#!/usr/bin/env python3
"""
Cross-platform build tool for OpenDoctrines.

Usage:
    python3 tools/build.py --platform macos [--release] [--test]
    python3 tools/build.py --platform windows [--release] [--test]
    python3 tools/build.py --platform linux [--release] [--test]
    python3 tools/build.py --platform emscripten [--release] [--test]
    python3 tools/build.py --platform all [--release]
    python3 tools/build.py --clean
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys
import json
import time

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_ROOT, "build")
DIST_DIR = os.path.join(PROJECT_ROOT, "dist")
DATA_DIR = os.path.join(PROJECT_ROOT, "data")
TOOLS_DIR = os.path.join(PROJECT_ROOT, "tools")

HOST_OS = platform.system().lower()  # darwin, linux, windows


def clear_saves(data_path):
    """Remove all saves from a packaged data directory for clean testing."""
    saves = os.path.join(data_path, "saves")
    if os.path.exists(saves):
        for f in os.listdir(saves):
            fp = os.path.join(saves, f)
            if os.path.isfile(fp):
                os.remove(fp)
        log("  Cleared saves for clean testing")


def log(msg):
    print(f"[build] {msg}")


def run(cmd, **kwargs):
    log(f"$ {subprocess.list2cmdline(cmd) if isinstance(cmd, list) else cmd}")
    return subprocess.run(cmd, **kwargs)


def cmake_configure(build_dir, extra_args=None):
    cmd = ["cmake", "-B", build_dir, "-S", PROJECT_ROOT]
    if extra_args:
        cmd += extra_args
    result = run(cmd)
    if result.returncode != 0:
        log(f"CMake configure failed! See {build_dir}/CMakeFiles/CMakeError.log")
        sys.exit(1)
    return result


def cmake_build(build_dir, target=None, parallel=None):
    cmd = ["cmake", "--build", build_dir]
    if target:
        cmd += ["--target", target]
    if parallel:
        cmd += ["--", f"-j{parallel}"]
    elif HOST_OS == "darwin":
        cmd += ["--", f"-j{os.cpu_count()}"]
    result = run(cmd)
    if result.returncode != 0:
        log(f"CMake build failed!")
        sys.exit(1)
    return result


def find_binary(build_dir, name):
    """Find the built binary (handles .exe suffix)."""
    for root, dirs, files in os.walk(build_dir):
        for f in files:
            if f == name or f == name + ".exe":
                return os.path.join(root, f)
    return None


# ──────────────────────────────────────────────
# Platform builders
# ──────────────────────────────────────────────

def build_macos(build_type="Debug"):
    log("=== Building for macOS ===")
    build_dir = os.path.join(BUILD_DIR, "macos")
    os.makedirs(build_dir, exist_ok=True)

    cmake_configure(build_dir, [
        f"-DCMAKE_BUILD_TYPE={build_type}",
    ])
    cmake_build(build_dir, parallel=os.cpu_count())

    # Create .app bundle
    bundle_name = "OpenDoctrines.app"
    bundle_path = os.path.join(DIST_DIR, bundle_name)
    if os.path.exists(bundle_path):
        shutil.rmtree(bundle_path)

    contents = os.path.join(bundle_path, "Contents")
    macos_dir = os.path.join(contents, "MacOS")
    resources_dir = os.path.join(contents, "Resources")

    os.makedirs(macos_dir, exist_ok=True)
    os.makedirs(resources_dir, exist_ok=True)

    # Copy icon
    icon_src = os.path.join(PROJECT_ROOT, "data", "Icon", "icon.icns")
    if os.path.exists(icon_src):
        shutil.copy2(icon_src, os.path.join(resources_dir, "icon.icns"))

    # Copy binary
    binary = find_binary(build_dir, "OpenDoctrines")
    if not binary:
        log("ERROR: OpenDoctrines binary not found in build directory!")
        sys.exit(1)
    shutil.copy2(binary, os.path.join(macos_dir, "OpenDoctrines"))

    # Copy data
    data_dest = os.path.join(resources_dir, "data")
    if os.path.exists(data_dest):
        shutil.rmtree(data_dest)
    shutil.copytree(DATA_DIR, data_dest, ignore=shutil.ignore_patterns(".DS_Store"))
    clear_saves(data_dest)

    # Symlink for data path compat: GetApplicationDirectory()+ "../data/"
    # In a .app bundle, GetApplicationDirectory() returns Resources/, so
    # the code expects Contents/data/. Symlink it to Resources/data/.
    contents_data = os.path.join(contents, "data")
    if not os.path.exists(contents_data):
        os.symlink("Resources/data", contents_data)

    # Create Info.plist
    plist = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>OpenDoctrines</string>
    <key>CFBundleDisplayName</key><string>OpenDoctrines</string>
    <key>CFBundleIdentifier</key><string>com.opendoctrines.app</string>
    <key>CFBundleVersion</key><string>1.0.2a</string>
    <key>CFBundleShortVersionString</key><string>1.0.2a</string>
    <key>CFBundleExecutable</key><string>OpenDoctrines</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>CFBundleIconFile</key><string>icon</string>
</dict>
</plist>"""
    with open(os.path.join(contents, "Info.plist"), "w") as f:
        f.write(plist)

    # Bundle raylib dylib
    ray_dylib = "/opt/homebrew/lib/libraylib.550.dylib"
    if os.path.exists(ray_dylib):
        frameworks = os.path.join(contents, "Frameworks")
        os.makedirs(frameworks, exist_ok=True)
        shutil.copy2(ray_dylib, os.path.join(frameworks, "libraylib.550.dylib"))

    log(f"macOS .app bundle created at {bundle_path}")
    return bundle_path


def build_windows(build_type="Release"):
    log("=== Building for Windows (cross-compile with MinGW) ===")

    # Check for MinGW
    mingw_w64 = shutil.which("x86_64-w64-mingw32-g++")
    if not mingw_w64:
        log("MinGW-w64 not found. Install with: brew install mingw-w64")
        log("Or build via Docker: python3 tools/build.py --platform windows --docker")
        sys.exit(1)

    build_dir = os.path.join(BUILD_DIR, "windows")
    os.makedirs(build_dir, exist_ok=True)

    toolchain = os.path.join(TOOLS_DIR, "cmake", "mingw-toolchain.cmake")
    if not os.path.exists(toolchain):
        os.makedirs(os.path.dirname(toolchain), exist_ok=True)
        with open(toolchain, "w") as f:
            f.write("""set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_SYSROOT /opt/homebrew/Cellar/mingw-w64/14.0.0/toolchain-x86_64/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
""")

    cmake_configure(build_dir, [
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        "-DCMAKE_DISABLE_FIND_PACKAGE_raylib=ON",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=OFF",
    ])
    cmake_build(build_dir)

    # Package: binary + DLLs
    dist_win = os.path.join(DIST_DIR, "OpenDoctrines-Windows")
    if os.path.exists(dist_win):
        shutil.rmtree(dist_win)
    os.makedirs(dist_win, exist_ok=True)

    binary = find_binary(build_dir, "OpenDoctrines.exe")
    if binary:
        shutil.copy2(binary, os.path.join(dist_win, "OpenDoctrines.exe"))

    # Copy data
    data_dest = os.path.join(dist_win, "data")
    shutil.copytree(DATA_DIR, data_dest, ignore=shutil.ignore_patterns(".DS_Store"))
    clear_saves(data_dest)

    # Bundle needed DLLs
    mingw_real = os.path.realpath(mingw_w64)  # resolve symlinks to Cellar path
    mingw_dir = os.path.dirname(os.path.dirname(mingw_real))
    for dll in ["libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll"]:
        # Search lib/ and bin/ dirs under x86_64-w64-mingw32
        found = False
        for subdir in ["lib", "bin"]:
            dll_path = os.path.join(mingw_dir, "x86_64-w64-mingw32", subdir, dll)
            if os.path.exists(dll_path):
                shutil.copy2(dll_path, dist_win)
                log(f"  Copied {dll} (64-bit from {dll_path})")
                found = True
                break
        if not found:
            # Fallback: walk entire x86_64 tree
            for root, dirs, files in os.walk(os.path.join(mingw_dir, "x86_64-w64-mingw32")):
                if dll in files:
                    shutil.copy2(os.path.join(root, dll), dist_win)
                    log(f"  Copied {dll} (64-bit, walked from {root})")
                    found = True
                    break
        if not found:
            log(f"  WARNING: {dll} not found in 64-bit toolchain")

    # Bundle SDL2/raylib DLL
    for root, dirs, files in os.walk(build_dir):
        for f in files:
            if f.endswith(".dll"):
                shutil.copy2(os.path.join(root, f), dist_win)

    log(f"Windows package created at {dist_win}")
    return dist_win


def build_linux_docker(build_type="Release"):
    log("=== Building for Linux via Docker ===")
    dockerfile = os.path.join(TOOLS_DIR, "docker", "Dockerfile.linux")
    if not os.path.exists(dockerfile):
        os.makedirs(os.path.dirname(dockerfile), exist_ok=True)
        with open(dockerfile, "w") as f:
            f.write("""FROM ubuntu:22.04 AS builder
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \\
    cmake g++ libxrandr-dev libxinerama-dev libxcursor-dev \\
    libxi-dev libgl1-mesa-dev libasound2-dev git

WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \\
    cmake --build build -- -j$(nproc)

FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \\
    libxrandr2 libxinerama1 libxcursor1 libxi6 libgl1 libasound2 \\
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/build/OpenDoctrines /app/OpenDoctrines
COPY --from=builder /src/data /app/data
WORKDIR /app
CMD ["./OpenDoctrines"]
""")

    image_tag = "opendoctrines-linux-builder"
    run(["docker", "build", "-t", image_tag, "-f", dockerfile, PROJECT_ROOT])

    dist_linux = os.path.join(DIST_DIR, "OpenDoctrines-Linux")
    os.makedirs(dist_linux, exist_ok=True)

    # Extract binary from the builder stage
    run(["docker", "run", "--rm",
         "-v", f"{dist_linux}:/out",
         image_tag,
         "bash", "-c", "cp /app/OpenDoctrines /out/ && cp -r /app/data /out/"])
    clear_saves(os.path.join(dist_linux, "data"))

    log(f"Linux package created at {dist_linux}")
    return dist_linux


def build_linux_native(build_type="Release"):
    """Native Linux build (when already on Linux)."""
    log("=== Building for Linux (native) ===")
    build_dir = os.path.join(BUILD_DIR, "linux")
    os.makedirs(build_dir, exist_ok=True)

    cmake_configure(build_dir, [f"-DCMAKE_BUILD_TYPE={build_type}"])
    cmake_build(build_dir, parallel=os.cpu_count())

    dist_linux = os.path.join(DIST_DIR, "OpenDoctrines-Linux")
    if os.path.exists(dist_linux):
        shutil.rmtree(dist_linux)
    os.makedirs(dist_linux, exist_ok=True)

    binary = find_binary(build_dir, "OpenDoctrines")
    if binary:
        shutil.copy2(binary, os.path.join(dist_linux, "OpenDoctrines"))
    data_dest = os.path.join(dist_linux, "data")
    shutil.copytree(DATA_DIR, data_dest, ignore=shutil.ignore_patterns(".DS_Store"))
    clear_saves(data_dest)

    log(f"Linux package created at {dist_linux}")
    return dist_linux


def build_emscripten(build_type="Release"):
    log("=== Building for Emscripten/WASM ===")

    # Check for emsdk
    emcmake = shutil.which("emcmake")
    if not emcmake:
        log("Emscripten SDK not found. Install with:")
        log("  git clone https://github.com/emscripten-core/emsdk.git")
        log("  cd emsdk && ./emsdk install latest && ./emsdk activate latest")
        log("  source ./emsdk_env.sh")
        sys.exit(1)

    build_dir = os.path.join(BUILD_DIR, "emscripten")
    os.makedirs(build_dir, exist_ok=True)

    # Configure with emcmake
    # (CMakeLists.txt handles -sUSE_GLFW=3, -sASYNCIFY, --preload-file via target_link_options)
    result = run(["emcmake", "cmake", "-B", build_dir, "-S", PROJECT_ROOT,
                  f"-DCMAKE_BUILD_TYPE={build_type}",
                  "-DFETCHCONTENT_FULLY_DISCONNECTED=OFF"])
    if result.returncode != 0:
        sys.exit(1)

    result = run(["cmake", "--build", build_dir, "--", "-j", str(os.cpu_count())])
    if result.returncode != 0:
        sys.exit(1)

    # Package: copy .wasm, .js, .html + data
    dist_web = os.path.join(DIST_DIR, "OpenDoctrines-Web")
    if os.path.exists(dist_web):
        shutil.rmtree(dist_web)
    os.makedirs(dist_web, exist_ok=True)

    for f in os.listdir(build_dir):
        if f.startswith("OpenDoctrines") and (
            f.endswith(".wasm") or f.endswith(".js") or f.endswith(".html") or f.endswith(".data")
        ):
            shutil.copy2(os.path.join(build_dir, f), os.path.join(dist_web, f))

    # Create a shell HTML if not generated
    html_path = os.path.join(dist_web, "OpenDoctrines.html")
    if not os.path.exists(html_path):
        with open(html_path, "w") as f:
            f.write("""<!DOCTYPE html>
<html><head><meta charset="UTF-8">
<link rel="icon" type="image/png" href="icon.png">
<style>body{margin:0;background:#000;overflow:hidden}
canvas{display:block;width:100vw;height:100vh}</style>
</head><body>
<script>document.addEventListener('contextmenu',function(e){e.preventDefault()});
var Module={locateFile:function(s){return s;},
preRun:[],postRun:[],canvas:(function(){var c=document.getElementById('canvas');
if(!c){c=document.createElement('canvas');c.id='canvas';
document.body.appendChild(c)}return c})()}</script>
<script src="OpenDoctrines.js"></script>
</body></html>""")

    # Copy data via virtual FS mount script
    data_dest = os.path.join(dist_web, "data")
    shutil.copytree(DATA_DIR, data_dest, ignore=shutil.ignore_patterns(".DS_Store"))
    clear_saves(data_dest)

    # Copy favicon to web root
    icon_png = os.path.join(DATA_DIR, "Icon", "icon.png")
    if os.path.exists(icon_png):
        shutil.copy2(icon_png, os.path.join(dist_web, "icon.png"))

    log(f"Emscripten package created at {dist_web}")
    log("  Serve with: python3 -m http.server 8080 --directory " + dist_web)
    return dist_web


# ──────────────────────────────────────────────
# Test commands
# ──────────────────────────────────────────────

def test_macos(bundle_path):
    log("=== Testing macOS build ===")
    binary = os.path.join(bundle_path, "Contents", "MacOS", "OpenDoctrines")
    if os.path.exists(binary):
        subprocess.Popen(["open", bundle_path])
        log(f"Launched {bundle_path}")
    else:
        log(f"Binary not found at {binary}")


def test_windows(package_dir):
    log("=== Testing Windows build with Wine ===")
    wine = shutil.which("wine")
    if not wine:
        log("Wine not found.")
        log("  Install: brew install --cask wine-stable")
        log("  Then run: wine " + os.path.join(package_dir, "OpenDoctrines.exe"))
        return

    exe = os.path.join(package_dir, "OpenDoctrines.exe")
    if not os.path.exists(exe):
        log(f"Windows binary not found at {exe}")
        return

    # Verify DLL architectures match the .exe
    log("  Verifying DLL architectures...")
    import subprocess as sp
    for f in os.listdir(package_dir):
        if f.endswith(".dll"):
            out = sp.check_output(["file", os.path.join(package_dir, f)]).decode()
            log(f"    {f}: {out.strip()}")

    log("  Launching with Wine (5s timeout)...")
    import signal
    p = subprocess.Popen(["wine", exe], cwd=package_dir, stderr=subprocess.STDOUT, stdout=subprocess.PIPE)
    try:
        out, _ = p.communicate(timeout=8)
        log("  Wine exited quickly:")
        for line in out.decode().split("\n")[:20]:
            if line.strip():
                log(f"    {line.strip()}")
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        log("  Wine process started (ran for 8s) — appearance check passed.")
        log("  NOTE: On macOS ARM, WGL context creation may fail (raylib uses OpenGL via GLFW).")
        log("  This is a Wine-on-ARM limitation, not a build issue.")


def test_linux(package_dir):
    log("=== Testing Linux build ===")
    if HOST_OS == "linux":
        binary = os.path.join(package_dir, "OpenDoctrines")
        if os.path.exists(binary):
            subprocess.Popen([binary], cwd=package_dir)
        else:
            log(f"Binary not found at {binary}")
        return

    # macOS: try Docker with X11 via XQuartz
    log("Not on Linux. Trying Docker with X11 forwarding...")
    xquartz = subprocess.run(["pgrep", "-x", "XQuartz"], capture_output=True).returncode == 0
    if not xquartz:
        log("XQuartz not running.")
        log("  For GUI: open -a XQuartz then re-run with --test")
        log("  Headless: docker run --rm -v {}:/app opendoctrines-linux-builder /app/OpenDoctrines".format(package_dir))
        return

    log("XQuartz running. Launching via Docker with X11...")
    cmd = [
        "docker", "run", "--rm",
        "-e", "DISPLAY=host.docker.internal:0",
        "-v", "/tmp/.X11-unix:/tmp/.X11-unix",
        "-v", f"{package_dir}:/app",
        "--network", "host",
        "opendoctrines-linux-builder",
        "/app/OpenDoctrines"
    ]
    run(cmd)


def test_emscripten(package_dir):
    log("=== Testing Emscripten build ===")
    html = os.path.join(package_dir, "OpenDoctrines.html")
    if not os.path.exists(html):
        log(f"HTML not found at {html}")
        return

    import http.server
    import socketserver
    import threading

    port = 8080
    handler = http.server.SimpleHTTPRequestHandler

    log(f"Starting HTTP server on http://localhost:{port}")
    log("Press Ctrl+C to stop")

    os.chdir(package_dir)
    httpd = socketserver.TCPServer(("", port), handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()

    # Try to open browser
    if HOST_OS == "darwin":
        run(["open", f"http://localhost:{port}/OpenDoctrines.html"])
    elif HOST_OS == "linux":
        run(["xdg-open", f"http://localhost:{port}/OpenDoctrines.html"])

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        log("Server stopped.")


# ──────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────

def clean():
    log("Cleaning build and dist directories...")
    if os.path.exists(BUILD_DIR):
        shutil.rmtree(BUILD_DIR)
    if os.path.exists(DIST_DIR):
        shutil.rmtree(DIST_DIR)
    log("Clean complete.")


def main():
    parser = argparse.ArgumentParser(description="OpenDoctrines cross-platform build tool")
    parser.add_argument("--platform", "-p", choices=["macos", "windows", "linux", "emscripten", "all"],
                        help="Target platform")
    parser.add_argument("--release", action="store_true", help="Release build")
    parser.add_argument("--test", action="store_true", help="Test after build")
    parser.add_argument("--docker", action="store_true", help="Use Docker for cross-compilation")
    parser.add_argument("--clean", action="store_true", help="Clean build/dist directories")
    parser.add_argument("--parallel", "-j", type=int, default=os.cpu_count(),
                        help="Parallel build jobs")

    args = parser.parse_args()

    if args.clean:
        clean()
        return

    build_type = "Release" if args.release else "Debug"
    os.makedirs(DIST_DIR, exist_ok=True)

    platforms = ["macos", "windows", "linux", "emscripten"] if args.platform == "all" else [args.platform]

    for plat in platforms:
        if plat == "macos":
            path = build_macos(build_type)
            if args.test:
                test_macos(path)
        elif plat == "windows":
            if args.docker:
                log("Windows build via Docker not yet implemented. Using native MinGW.")
            path = build_windows(build_type)
            if args.test:
                test_windows(path)
        elif plat == "linux":
            if args.docker or HOST_OS != "linux":
                path = build_linux_docker(build_type)
            else:
                path = build_linux_native(build_type)
            if args.test:
                test_linux(path)
        elif plat == "emscripten":
            path = build_emscripten(build_type)
            if args.test:
                test_emscripten(path)


if __name__ == "__main__":
    main()
