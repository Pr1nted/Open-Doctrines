#!/usr/bin/env bash
# Build the Windows test VM from nothing, unattended.
#
# WHY THIS IS A SCRIPT AND NOT A SET OF INSTRUCTIONS
#
# The last Windows VM was hand-made in 2024 and rotted where nobody could see:
# an Insider EVALUATION build that expired, signature validation broken badly
# enough that UAC refused mmc.exe as "Publisher: Unknown", and no way to tell
# any of that from the outside until a test needed it. A VM that can be rebuilt
# in one command is a VM that can be thrown away the moment it misbehaves,
# which is the only real defence against that.
#
#   tools/windows_vm_create.sh ~/VMs/Win11_25H2_Arm64.iso
#   tools/windows_vm_create.sh --iso-only          # just build the answer disc
#
# WHAT IT MAKES: a QEMU aarch64 VM under UTM with UEFI and a TPM (Windows 11
# checks for both), 6 GB of RAM, 4 cores and a 64 GB growable disk, with two
# CD drives -- the Windows installer, and a small answer disc carrying
# autounattend.xml. Windows setup scans every drive for that file.
#
# WHAT THE ANSWER DISC IS FOR. 25H2 will not finish OOBE without a Microsoft
# account and a network, and BYPASSNRO is gone. An answer file is the supported
# way through, and it also means the install needs nobody watching it: it
# partitions the disk, picks the edition out of the multi-edition image, makes
# a local administrator and logs in.
#
# ARM64, NOT X64, DELIBERATELY. The host is Apple silicon, so an ARM64 guest
# runs under Hypervisor.framework at close to native speed while an x64 guest
# would be fully emulated -- hours to install, painful to use. The product
# ships x64, and Windows on ARM runs x64 applications under its own emulation,
# which is enough for an INSTALLER. The automated installer check does not run
# here at all: it runs on a real x64 Windows runner in release-game.yml. This
# VM is for watching the wizard with human eyes.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
docs="$HOME/Library/Containers/com.utmapp.UTM/Data/Documents"
name="${OD_WIN_VM:-Windows11}"
bundle="$docs/$name.utm"
disk_gb="${OD_WIN_DISK_GB:-64}"
ram_mb="${OD_WIN_RAM_MB:-6144}"
cores="${OD_WIN_CORES:-4}"

die() { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }
step() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }

command -v qemu-img >/dev/null || die "qemu-img not found -- brew install qemu"
[ -d "$docs" ] || die "UTM's document directory is not at $docs -- is UTM installed?"

# ── the answer disc ──
step "the answer disc"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$root/tools/autounattend.xml" "$work/" || die "tools/autounattend.xml is missing"
# Carried along so they are already inside the guest when it first boots, with
# no file sharing needed to get them there.
cp "$root/tools/windows_installer_test.ps1" "$work/" 2>/dev/null || true
answer_iso="$docs/$name-answers.iso"
hdiutil makehybrid -iso -joliet -o "$answer_iso" "$work" -quiet
note "$answer_iso ($(du -h "$answer_iso" | cut -f1))"
[ "${1:-}" = "--iso-only" ] && { note "stopping here as asked"; exit 0; }

win_iso="${1:-}"
[ -n "$win_iso" ] && [ -f "$win_iso" ] ||
    die "usage: $0 <Win11_..._Arm64.iso>   (get it from microsoft.com/software-download/windows11arm64)"

# ── the bundle ──
step "the virtual machine"
[ -e "$bundle" ] && die "$bundle already exists -- delete it first: utmctl delete $name"
mkdir -p "$bundle/Data"
disk="$bundle/Data/disk.qcow2"
qemu-img create -f qcow2 "$disk" "${disk_gb}G" >/dev/null
note "disk: ${disk_gb} GB growable at $disk"
cp "$win_iso" "$bundle/Data/windows.iso"
cp "$answer_iso" "$bundle/Data/answers.iso"
note "installer and answer disc copied in"

python3 - "$bundle" "$ram_mb" "$cores" <<'PY'
import plistlib, sys, uuid, random
bundle, ram, cores = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
mac = "C2:" + ":".join(f"{random.randint(0,255):02X}" for _ in range(5))
cfg = {
    "Backend": "QEMU",
    "ConfigurationVersion": 4,
    "Information": {"Name": bundle.split("/")[-1].removesuffix(".utm"),
                    "UUID": str(uuid.uuid4()).upper(),
                    "Notes": "Built by tools/windows_vm_create.sh. "
                             "Login odtest / odtest. Disposable: rebuild rather than repair."},
    "Display": [{"DownscalingFilter": "Linear", "DynamicResolution": True,
                 "Hardware": "virtio-ramfb-gl", "NativeResolution": False,
                 "UpscalingFilter": "Nearest"}],
    "Drive": [
        {"Identifier": str(uuid.uuid4()).upper(), "ImageName": "disk.qcow2",
         "ImageType": "Disk", "Interface": "NVMe", "InterfaceVersion": 1, "ReadOnly": False},
        {"Identifier": str(uuid.uuid4()).upper(), "ImageName": "windows.iso",
         "ImageType": "CD", "Interface": "USB", "InterfaceVersion": 1, "ReadOnly": True},
        {"Identifier": str(uuid.uuid4()).upper(), "ImageName": "answers.iso",
         "ImageType": "CD", "Interface": "USB", "InterfaceVersion": 1, "ReadOnly": True},
    ],
    "Input": {"MaximumUsbShare": 3, "UsbBusSupport": "3.0", "UsbSharing": True},
    "Network": [{"Hardware": "virtio-net-pci", "IsolateFromHost": False,
                 "MacAddress": mac, "Mode": "Shared", "PortForward": []}],
    # TPMDevice and UEFIBoot are both required by Windows 11; the answer file
    # bypasses the checks that a VM cannot satisfy, not these.
    "QEMU": {"AdditionalArguments": [], "BalloonDevice": False, "DebugLog": False,
             "Hypervisor": True, "PS2Controller": False, "RNGDevice": True,
             "RTCLocalTime": True, "TPMDevice": True, "TSO": False, "UEFIBoot": True},
    "Serial": [],
    "Sharing": {"ClipboardSharing": True, "DirectoryShareMode": "WebDAV",
                "DirectoryShareReadOnly": False},
    "Sound": [{"Hardware": "intel-hda"}],
    "System": {"Architecture": "aarch64", "CPU": "default", "CPUCount": cores,
               "CPUFlagsAdd": [], "CPUFlagsRemove": [], "ForceMulticore": False,
               "JITCacheSize": 0, "MemorySize": ram, "Target": "virt"},
}
with open(bundle + "/config.plist", "wb") as f:
    plistlib.dump(cfg, f)
print(f"  config written: {cores} cores, {ram} MB, aarch64/virt, TPM + UEFI")
PY

step "done"
note "UTM should now list '$name'. Start it and the install runs unattended."
note "It logs in as odtest / odtest."
note ""
note "  /Applications/UTM.app/Contents/MacOS/utmctl start $name"
