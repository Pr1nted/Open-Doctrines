#!/usr/bin/env bash
# Install the APK on a real Android emulator, start it, and watch it run.
#
#   tools/android_emulator_test.sh [path/to/OpenDoctrines.apk]
#
# WHAT THIS ANSWERS THAT CI CANNOT
#
# CI builds the APK and checks the .so with `nm` and the archive with `unzip`.
# Nothing has ever INSTALLED it. Every question a player's first thirty seconds
# asks -- does it install, does the native activity find its library, does GLES
# come up, does it draw anything, does it survive being started -- has been
# answered by inspecting a zip.
#
# That is the same gap the Windows installer had until a real Windows machine
# ran it, and the Linux packages had until one was installed in a guest that
# did not have the game's libraries.
#
# WHY AN EMULATOR AND NOT A PHONE. A phone is better and cannot be a gate: it
# has to be plugged in, unlocked and present. This runs unattended. What it
# cannot prove is real GPU behaviour -- the emulator renders with SwiftShader,
# so "it drew something" here means the code path works, not that a Mali or an
# Adreno driver likes it.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=io.itch.pr1nted.opendoctrines
ACTIVITY="$PKG/android.app.NativeActivity"
AVD="${OD_ANDROID_AVD:-od_test}"
APK="${1:-$ROOT/build-android/OpenDoctrines.apk}"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/opt/homebrew/share/android-commandlinetools}}"
ADB="$SDK/platform-tools/adb"
EMU="$SDK/emulator/emulator"
AVDMAN="$SDK/cmdline-tools/latest/bin/avdmanager"

checks=0; failed=0
ok()  { checks=$((checks+1)); printf '  ok    %s\n' "$1"; }
bad() { checks=$((checks+1)); failed=$((failed+1)); printf '  FAIL  %s%s\n' "$1" "${2:+  [$2]}"; }
sec() { printf '\n== %s ==\n' "$1"; }

[ -x "$ADB" ] || { echo "no adb under $SDK -- set ANDROID_HOME"; exit 1; }
[ -f "$APK" ] || { echo "no APK at $APK -- build it with tools/package_android.sh"; exit 1; }

# ── a device to test on ──────────────────────────────────────────────────────
sec "an emulator"
"$ADB" start-server >/dev/null 2>&1

if ! "$ADB" devices | grep -q "emulator-.*device$"; then
    if ! "$AVDMAN" list avd 2>/dev/null | grep -q "Name: $AVD"; then
        echo "  creating the '$AVD' AVD"
        img=$(ls -d "$SDK"/system-images/*/*/* 2>/dev/null | head -1)
        [ -n "$img" ] || { echo "no system image installed -- sdkmanager 'system-images;android-34;google_apis;arm64-v8a'"; exit 1; }
        tag=$(printf '%s' "$img" | awk -F/ '{print $(NF-2)";"$(NF-1)";"$NF}')
        echo no | "$AVDMAN" create avd -n "$AVD" -k "system-images;$tag" --device pixel_6 >/dev/null 2>&1 \
            || { echo "could not create the AVD"; exit 1; }
    fi
    echo "  booting '$AVD' (headless)"
    # swiftshader_indirect: a CI machine and a headless session have no GPU to
    # hand out. Slower than the host GPU and it is the only option that works
    # with no window.
    nohup "$EMU" -avd "$AVD" -no-window -no-audio -no-boot-anim \
                 -gpu swiftshader_indirect -no-snapshot > /tmp/od-emulator.log 2>&1 &
    "$ADB" wait-for-device
fi

waited=0
until [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do
    sleep 5; waited=$((waited+5))
    [ "$waited" -gt 300 ] && { echo "  the emulator never finished booting"; tail -5 /tmp/od-emulator.log; exit 1; }
done
ok "a device is up ($("$ADB" shell getprop ro.build.version.release | tr -d '\r') / $("$ADB" shell getprop ro.product.cpu.abi | tr -d '\r'))"

# ── install ──────────────────────────────────────────────────────────────────
sec "installing it"
"$ADB" uninstall "$PKG" >/dev/null 2>&1 || true
out=$("$ADB" install -r "$APK" 2>&1)
case "$out" in
    *Success*) ok "the APK installs ($(du -h "$APK" | cut -f1))" ;;
    *)         bad "the APK installs" "$(printf '%s' "$out" | tail -1)" ;;
esac
"$ADB" shell pm list packages 2>/dev/null | grep -q "$PKG" \
    && ok "the package manager lists it" || bad "the package manager lists it"

# ── run ──────────────────────────────────────────────────────────────────────
sec "starting it"
"$ADB" logcat -c >/dev/null 2>&1 || true
"$ADB" shell am start -n "$ACTIVITY" >/dev/null 2>&1

# Startup unpacks assets and builds a font atlas, so it is not instant. Poll
# for the process rather than sleeping a fixed amount and hoping.
pid=""; waited=0
while [ -z "$pid" ] && [ "$waited" -lt 90 ]; do
    sleep 3; waited=$((waited+3))
    pid=$("$ADB" shell pidof "$PKG" 2>/dev/null | tr -d '\r')
done
[ -n "$pid" ] && ok "the process is running (pid $pid)" || bad "the process is running" "never appeared"

# Give it a while to get through init, then ask whether it is STILL running --
# a native activity that dies after two seconds would otherwise look identical
# to one that started.
sleep 20
still=$("$ADB" shell pidof "$PKG" 2>/dev/null | tr -d '\r')
[ -n "$still" ] && ok "and it is still running twenty seconds later" \
                || bad "and it is still running twenty seconds later" "it exited"

# TO A FILE, not a shell variable. The first version captured logcat into
# $log and grepped it with printf, and every pattern missed -- on a log that
# demonstrably contained "Initializing raylib 5.5". A whole device's logcat is
# megabytes; this is not the shape to hold it in. The file is also left behind,
# which is what you want when a check fails and you have to ask why.
log="${OD_ANDROID_LOG:-/tmp/od-android-logcat.txt}"
"$ADB" logcat -d > "$log" 2>/dev/null
printf '  (logcat: %s lines -> %s)\n' "$(wc -l < "$log" | tr -d ' ')" "$log"

# raylib's own TraceLog goes to logcat under the "raylib" tag; the game's
# printf does not, so these are the lines that prove the native library
# loaded and brought a context up.
grep -q "Initializing raylib" "$log" \
    && ok "raylib initialised inside the app" || bad "raylib initialised inside the app"
grep -q "PLATFORM: ANDROID: Initialized successfully" "$log" \
    && ok "the Android backend came up" || bad "the Android backend came up"
if grep -qiE "GL: OpenGL device information|OPENGL: Version" "$log"; then
    ok "it got a GL context"
else
    bad "it got a GL context"
fi

# A crash here is a native one -- there is no Java in this app at all -- so it
# arrives as a signal and a tombstone, not an exception.
crash=$(grep -E "Fatal signal|>>> $PKG <<<|FATAL EXCEPTION" "$log" | head -1)
[ -z "$crash" ] && ok "nothing crashed" || bad "nothing crashed" "$crash"

# ── did it DRAW ──────────────────────────────────────────────────────────────
sec "did anything reach the screen"
shot="${OD_ANDROID_SHOT:-/tmp/od-android.png}"
"$ADB" exec-out screencap -p > "$shot" 2>/dev/null
if [ -s "$shot" ]; then
    ok "a screenshot came back ($(du -h "$shot" | cut -f1)) -> $shot"
    # A black frame is what a dead renderer gives, and it is also what a
    # loading screen gives -- so this only claims that SOMETHING was drawn.
    verdict=$(python3 - "$shot" <<'PY'
import sys
try:
    from PIL import Image
except ImportError:
    print("skip no Pillow"); raise SystemExit
im = Image.open(sys.argv[1]).convert("RGB")
cols = im.getcolors(maxcolors=1 << 24) or []
total = sum(c for c, _ in cols)
top, topc = max(cols, key=lambda p: p[0]) if cols else (0, (0, 0, 0))
print("%s %d %.1f %s" % ("ok" if len(cols) > 8 and top / total < 0.995 else "flat",
                         len(cols), 100.0 * top / total, topc))
PY
)
    set -- $verdict
    case "$1" in
        ok)   ok "the frame has real content ($2 colours, most common $3% $4)" ;;
        flat) bad "the frame has real content" "$2 colours, $3% one colour -- a blank screen" ;;
        *)    printf '  skip  colour analysis (%s)\n' "$verdict" ;;
    esac
else
    bad "a screenshot came back" "screencap produced nothing"
fi

# ── and take it away ─────────────────────────────────────────────────────────
sec "removing it"
"$ADB" shell am force-stop "$PKG" >/dev/null 2>&1
"$ADB" uninstall "$PKG" >/dev/null 2>&1
"$ADB" shell pm list packages 2>/dev/null | grep -q "$PKG" \
    && bad "uninstall removes it" || ok "uninstall removes it"

printf '\n%d checks, %d failed\n' "$checks" "$failed"
[ "$failed" -eq 0 ]
