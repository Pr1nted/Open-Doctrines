#!/usr/bin/env bash
# Run the Windows installer test in the UTM VM, from macOS, unattended.
#
# WHY THIS IS THE ONE THING CI CANNOT DO. tools/qualify.sh proves the Windows
# BUILD runs -- GitHub's runner plays a real game through software OpenGL. The
# INSTALLER is a different artifact: NSIS, a Start-menu shortcut, an entry in
# Add or remove programs, an uninstaller. None of it is exercised by a build,
# and a shortcut pointing at a path the binary no longer lives at is a working
# build with a broken install.
#
#   tools/windows_installer_test.sh path/to/OpenDoctrines-x.y.z-win64.exe
#   tools/windows_installer_test.sh --release v1.2.2a     # fetch it first
#
# ── READ THIS BEFORE USING IT: CI IS THE PRIMARY ROUTE ──
#
# The same guest-side script now runs in .github/workflows/release-game.yml, on
# the Windows runner, immediately after cpack builds the installer. That needs
# no VM and no setup: a GitHub Windows runner is real x64 Windows with
# administrator rights, which is all a silent install and uninstall require. It
# was assumed this needed borrowed hardware; it did not.
#
# So this script is for what CI genuinely cannot do -- looking at the installer
# WHILE IT RUNS, with a person watching: the wizard's pages, the shortcut in a
# real Start menu, what "Add or remove programs" actually displays. Reach for
# CI first and this second.
#
# ── THE ONE-TIME SETUP, AND WHY IT CANNOT BE AUTOMATED ──
#
# Everything here goes through utmctl, which talks to the QEMU GUEST AGENT
# inside Windows. Without it there is no exec and no file transfer -- so the
# agent is the one thing that cannot be installed this way, because installing
# it is what would need the thing being installed.
#
# Install SPICE guest tools ONCE, in the guest (it carries qemu-ga AND
# spice-webdavd, so it fixes file sharing at the same time):
#
#   https://www.spice-space.org/download/windows/spice-guest-tools/
#
# Then `utmctl exec Windows --cmd cmd.exe -- /c echo hi` answers, and this
# script runs on its own from then on.
#
# ── WHAT IT LEAVES BEHIND ──
#
# Nothing. The VM is stopped again if this script started it, and the guest
# uninstalls what it installed. A failed run deliberately leaves the install in
# place so it can be looked at.
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
utmctl="/Applications/UTM.app/Contents/MacOS/utmctl"
vm="${OD_WIN_VM:-Windows}"
guest_dir='C:\od-installer-test'

die() { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }
step() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }

[ -x "$utmctl" ] || die "utmctl not found at $utmctl -- is UTM installed?"

installer="${1:-}"
if [ "$installer" = "--release" ]; then
    tag="${2:?--release needs a tag, e.g. v1.2.2a}"
    step "fetching the installer from $tag"
    tmp="$(mktemp -d)"
    gh release download "$tag" --pattern '*win64.exe' --dir "$tmp" ||
        die "could not download the installer for $tag"
    installer="$(ls "$tmp"/*win64.exe | head -1)"
fi
[ -n "$installer" ] && [ -f "$installer" ] ||
    die "usage: $0 <installer.exe> | --release <tag>"
note "installer: $installer ($(du -h "$installer" | cut -f1))"

# ── the VM ──
step "the virtual machine"
started_here=0
state="$("$utmctl" status "$vm" 2>/dev/null)"
[ -n "$state" ] || die "no VM called '$vm' -- set OD_WIN_VM, or check: $utmctl list"
if [ "$state" != "started" ]; then
    note "starting $vm (it was $state)"
    "$utmctl" start "$vm" || die "could not start $vm"
    started_here=1
else
    note "$vm is already running -- leaving it running afterwards"
fi

# ── the guest agent, which everything below needs ──
note "waiting for the guest agent..."
agent=0
for _ in $(seq 1 60); do
    if "$utmctl" exec "$vm" --cmd "cmd.exe" -- /c "exit 0" >/dev/null 2>&1; then
        agent=1; break
    fi
    sleep 10
done
if [ "$agent" -eq 0 ]; then
    printf '\033[31mThe QEMU guest agent never answered.\033[0m\n' >&2
    cat >&2 <<'MSG'

  This is the one-time setup, not a failure of the test. Install SPICE guest
  tools inside the Windows VM -- it carries qemu-ga, which is what utmctl
  talks to, and spice-webdavd for the shared folder:

      https://www.spice-space.org/download/windows/spice-guest-tools/

  Then check it from here:

      /Applications/UTM.app/Contents/MacOS/utmctl exec Windows \
          --cmd cmd.exe -- /c echo hi

MSG
    [ "$started_here" -eq 1 ] && "$utmctl" stop "$vm" >/dev/null 2>&1
    exit 2
fi
note "guest agent is up"

# ── push the installer and the test ──
step "copying into the guest"
"$utmctl" exec "$vm" --cmd "cmd.exe" -- /c "mkdir $guest_dir" >/dev/null 2>&1 || true
exe_name="$(basename "$installer")"
"$utmctl" file push "$vm" "$guest_dir\\$exe_name" < "$installer" ||
    die "could not copy the installer into the guest"
"$utmctl" file push "$vm" "$guest_dir\\windows_installer_test.ps1" \
    < "$root/tools/windows_installer_test.ps1" ||
    die "could not copy the test script into the guest"
note "copied $exe_name and the test script to $guest_dir"

# ── run it ──
step "installing, checking, uninstalling"
"$utmctl" exec "$vm" --cmd "powershell.exe" -- \
    -ExecutionPolicy Bypass -File "$guest_dir\\windows_installer_test.ps1" \
    -Installer "$guest_dir\\$exe_name" \
    -ResultFile "$guest_dir\\result.txt"
run_rc=$?

step "what the guest said"
result="$("$utmctl" file pull "$vm" "$guest_dir\\result.txt" 2>/dev/null)"
if [ -n "$result" ]; then
    printf '%s\n' "$result" | sed 's/^/  /'
else
    note "(no result file came back)"
fi

# ── put the machine back ──
"$utmctl" exec "$vm" --cmd "cmd.exe" -- /c "rmdir /s /q $guest_dir" >/dev/null 2>&1 || true
if [ "$started_here" -eq 1 ]; then
    note "stopping $vm"
    "$utmctl" stop "$vm" >/dev/null 2>&1
fi

printf '\n'
if printf '%s' "$result" | grep -q "RESULT: PASS"; then
    printf '\033[32mthe Windows installer installs, appears and uninstalls cleanly\033[0m\n'
    exit 0
fi
printf '\033[31mthe Windows installer test did not pass\033[0m\n'
exit "${run_rc:-1}"
