#!/bin/bash
# Build an installable APK from an existing build-android/ tree.
#
# NO GRADLE. The app has no Java at all (android:hasCode="false" in the
# manifest), so the whole package is: compile the manifest with aapt2, drop the
# .so and the staged assets into the zip, align it, sign it. Gradle would add a
# build system, a wrapper and a daemon to do the same four steps.
#
# Expects the CMake Android build to have run first -- that is what produces
# both libopendoctrines.so and build-android/assets/ with its manifest.
set -euo pipefail
cd "$(dirname "$0")/.."

# Portable across a developer's Homebrew install and a CI runner's preinstalled
# SDK: take whatever ANDROID_HOME/ANDROID_SDK_ROOT says, and pick the newest
# build-tools and platform present rather than pinning a version that may not be
# installed on the other machine.
ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/opt/homebrew/share/android-commandlinetools}}"
BT="$(ls -d "$ANDROID_HOME"/build-tools/* 2>/dev/null | sort -V | tail -1)"
PLATFORM="$(ls "$ANDROID_HOME"/platforms/android-3*/android.jar 2>/dev/null | sort -V | tail -1)"
[ -n "$BT" ]       || { echo "no build-tools under $ANDROID_HOME"; exit 1; }
[ -n "$PLATFORM" ] || { echo "no platform jar under $ANDROID_HOME"; exit 1; }
BUILD=build-android
OUT="$BUILD/OpenDoctrines.apk"
ABI=arm64-v8a

[ -f "$BUILD/libopendoctrines.so" ] || { echo "no .so -- run the cmake android build first"; exit 1; }
[ -d "$BUILD/assets" ]              || { echo "no staged assets -- re-run cmake configure"; exit 1; }

echo "==> staging"
rm -rf "$BUILD/apk"
mkdir -p "$BUILD/apk/lib/$ABI"
cp "$BUILD/libopendoctrines.so" "$BUILD/apk/lib/$ABI/"
# Strip: the unstripped library is ~66 MB, almost all of it debug info that a
# phone will never read. This is the difference between a 25 MB download and a
# 90 MB one.
# The prebuilt directory is named for the HOST, so it differs between a Mac and
# a Linux runner. Glob it rather than naming one.
STRIP="$(ls "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | head -1)"
[ -n "$STRIP" ] && "$STRIP" --strip-unneeded "$BUILD/apk/lib/$ABI/libopendoctrines.so" || \
    echo "  (llvm-strip not found; shipping unstripped)"

echo "==> compiling resources"
# The launcher icon is the only resource, but it still has to go through the
# aapt2 compile/link pair -- a manifest referencing @mipmap/ic_launcher without
# a compiled resource table fails to link, and an APK with no icon shows the
# generic Android robot in the launcher.
rm -rf "$BUILD/res-compiled"; mkdir -p "$BUILD/res-compiled"
"$BT/aapt2" compile --dir android/res -o "$BUILD/res-compiled/res.zip"

echo "==> compiling the manifest and packing assets"
# -A hands the assets directory to aapt2 directly. The first version of this
# re-zipped the APK in python to move files under assets/, which recompressed
# resources.arsc -- and targeting SDK 30+ requires that entry to be STORED and
# 4-byte aligned, so the install was rejected outright.
"$BT/aapt2" link \
    -I "$PLATFORM" \
    --manifest android/AndroidManifest.xml \
    -A "$BUILD/assets" \
    "$BUILD/res-compiled/res.zip" \
    -o "$BUILD/base.apk" \
    --auto-add-overlay

echo "==> adding the native library"
# STORED, not deflated. With targetSdk 34 extractNativeLibs defaults to false,
# so the loader maps the .so straight out of the APK and it has to be
# uncompressed and aligned to do that.
cd "$BUILD/apk"
zip -q -0 -r -X "../base.apk" "lib"
cd - > /dev/null

echo "==> aligning"
"$BT/zipalign" -f -p 4 "$BUILD/base.apk" "$OUT"

echo "==> signing"
KS="$BUILD/debug.keystore"
if [ ! -f "$KS" ]; then
    keytool -genkeypair -keystore "$KS" -alias od -storepass android -keypass android \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Open Doctrines" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android "$OUT"

echo "==> done: $OUT  ($(du -h "$OUT" | cut -f1))"
"$BT/apksigner" verify --print-certs "$OUT" | head -2
