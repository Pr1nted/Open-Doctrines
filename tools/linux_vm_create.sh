#!/usr/bin/env bash
# Build a Linux test VM from nothing, unattended.
#
#   tools/linux_vm_create.sh                    # Debian 12, arm64
#   tools/linux_vm_create.sh --distro debian11  # the old-glibc one
#   tools/linux_vm_create.sh --distro ubuntu2204
#
# WHAT THIS IS FOR
#
# Four Linux artifacts are built on every tag -- an x64 tarball, an arm64
# tarball, an AppImage, a .deb, an .rpm and a Flatpak -- and CI proves each one
# BUILDS. Nothing proves any of them INSTALLS AND RUNS, because a GitHub runner
# is the machine that built them: the libraries are already there, and that is
# exactly the condition a packaging bug hides in. The Windows installer sat
# untested for the same reason until a real Windows machine ran it and found
# two bugs in an afternoon.
#
# So: a disposable Linux guest to install the packages into and watch them
# start. tools/linux_vm_test.sh drives it.
#
# WHY A CLOUD IMAGE AND NOT AN INSTALLER
#
# The Windows VM answers an installer with autounattend.xml because Windows
# has no other unattended path. Debian and Ubuntu publish qcow2 cloud images
# that are already installed, and cloud-init configures them from a tiny
# second disc on first boot. No installer runs, the VM is up in under a
# minute, and the whole thing is reproducible. That is also why this script is
# a third the size of its Windows counterpart.
#
# ARM64 BY DEFAULT, for the same reason the Windows VM is: the host is Apple
# silicon, so an arm64 guest runs under Hypervisor.framework at close to
# native speed while x86_64 is fully emulated. That is not a compromise here
# the way it is on Windows -- we now BUILD arm64 Linux artifacts, and this is
# the only machine that can run them. Set OD_LINUX_ARCH=x86_64 to test the x64
# packages instead, slowly.
#
# WHICH DISTRIBUTION TO PICK
#
#   debian12   glibc 2.36. The default. Everything should work here.
#   debian11   glibc 2.31. The INTERESTING one: the published tarball needs
#              2.35, so it must FAIL here while the AppImage and the Flatpak
#              succeed. That difference is the entire justification for
#              building them (packaging/linux/README.md), and until now it was
#              argued rather than observed.
#   ubuntu2204 glibc 2.35. Matches the build environment.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
docs="$HOME/Library/Containers/com.utmapp.UTM/Data/Documents"
distro="debian12"
arch="${OD_LINUX_ARCH:-arm64}"
disk_gb="${OD_LINUX_DISK_GB:-24}"
ram_mb="${OD_LINUX_RAM_MB:-4096}"
cores="${OD_LINUX_CORES:-4}"
key="${OD_LINUX_KEY:-$HOME/.ssh/od_winvm.pub}"

die()  { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }
step() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --distro) distro="$2"; shift 2 ;;
        --arch)   arch="$2";   shift 2 ;;
        -h|--help) sed -n '2,46p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

case "$arch" in
    arm64)  qarch=arm64;  utm_arch=aarch64 ;;
    x86_64) qarch=amd64;  utm_arch=x86_64  ;;
    *) die "arch must be arm64 or x86_64" ;;
esac

# The cloud images. All three publish a "generic"/"server" qcow2 per
# architecture at a stable URL, which is what makes this reproducible.
case "$distro" in
    debian12)
        url="https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-generic-${qarch}.qcow2"
        user=debian ;;
    debian11)
        url="https://cloud.debian.org/images/cloud/bullseye/latest/debian-11-generic-${qarch}.qcow2"
        user=debian ;;
    ubuntu2204)
        u_arch=$([ "$qarch" = arm64 ] && echo arm64 || echo amd64)
        url="https://cloud-images.ubuntu.com/jammy/current/jammy-server-cloudimg-${u_arch}.img"
        user=ubuntu ;;
    # FreeBSD publishes a BASIC-CLOUDINIT image, which is the whole reason this
    # script extends to it without a second mechanism: same NoCloud seed, same
    # unattended path, no installer. The plain (non-CLOUDINIT) image would need
    # one. It arrives xz-compressed, so it is decompressed after download.
    freebsd14)
        [ "$qarch" = arm64 ] || die "only the arm64 FreeBSD image is wired up here"
        url="https://download.freebsd.org/releases/VM-IMAGES/14.5-RELEASE/aarch64/Latest/FreeBSD-14.5-RELEASE-arm64-aarch64-BASIC-CLOUDINIT-ufs.qcow2.xz"
        user=freebsd ;;
    *) die "distro must be debian12, debian11, ubuntu2204 or freebsd14" ;;
esac

name="${OD_LINUX_VM:-OD-${distro}-${arch}}"
bundle="$docs/$name.utm"

command -v qemu-img >/dev/null || die "qemu-img not found -- brew install qemu"
[ -d "$docs" ] || die "UTM's documents directory is not at $docs -- is UTM installed?"
[ -f "$key" ] || die "no public key at $key. Make one:
    ssh-keygen -t ed25519 -f ${key%.pub} -N ''"
[ -e "$bundle" ] && die "$bundle already exists -- delete it first:
    /Applications/UTM.app/Contents/MacOS/utmctl delete $name"

step "the cloud image"
cache="$HOME/VMs/cloud"
mkdir -p "$cache"
img="$cache/$(basename "${url%.xz}")"
if [ -f "$img" ]; then
    note "cached: $img ($(du -h "$img" | cut -f1))"
else
    note "downloading $url"
    curl -fL --progress-bar -o "$img.part" "$url"
    case "$url" in
        *.xz) note "decompressing"; mv "$img.part" "$img.xz.part"
              xz -d -c "$img.xz.part" > "$img.part" && rm -f "$img.xz.part" ;;
    esac
    mv "$img.part" "$img"
    note "$(du -h "$img" | cut -f1)"
fi

step "the cloud-init seed"
# NoCloud: cloud-init looks for a filesystem LABELLED cidata holding user-data
# and meta-data, and configures the guest from it on first boot. That is the
# whole of the unattended setup -- no preseed, no installer, no answer file
# scanning every drive the way Windows setup does.
work=$(mktemp -d)
trap 'rm -rf "$work" "$(dirname "$work")/$(basename "$work")-seed.iso"' EXIT
pub=$(cat "$key")

cat > "$work/meta-data" <<EOF
instance-id: ${name}
local-hostname: ${name}
EOF

# FreeBSD has no "sudo" group -- its wheel group is the equivalent, and asking
# for a group that does not exist makes cloud-init fail the whole user.
# FreeBSD has wheel where Linux has sudo, and has NEITHER audio NOR video --
# cloud-init fails the whole user when a listed group does not exist, which is
# why the first FreeBSD guest came up with no odtest at all and no way in.
case "$distro" in
    freebsd*) groups="[wheel]"
              pkg_list="  - bash"$'\n'"  - unzip"$'\n'"  - git" ;;
    *)        groups="[sudo, audio, video]"
              pkg_list="  - file"$'\n'"  - unzip"$'\n'"  - rpm"$'\n'"  - fuse3" ;;
esac

cat > "$work/user-data" <<EOF
#cloud-config
users:
  - name: odtest
    groups: ${groups}
    shell: /bin/bash
    sudo: ["ALL=(ALL) NOPASSWD:ALL"]
    lock_passwd: false
    # odtest, the same account the Windows VM uses, so one set of notes covers
    # both. Password login is left on for the console; ssh uses the key.
    plain_text_passwd: odtest
    ssh_authorized_keys:
      - ${pub}
ssh_pwauth: false
package_update: true
packages:
  # What the packages under test need in order to be INSTALLED and INSPECTED.
  # Deliberately NOT the game's X11 runtime dependencies: whether the .deb
  # pulls those in by itself is one of the things being tested, and
  # pre-installing them would answer the question before it was asked.
  #
  # FreeBSD has neither rpm nor fuse3 in that spelling, and asking pkg for a
  # package that does not exist fails the whole cloud-init run rather than
  # skipping it -- so that list is per family.
${pkg_list}
runcmd:
  - [ systemctl, enable, --now, ssh ]
  - [ sh, -c, "echo READY > /var/lib/cloud/od-ready" ]
final_message: "od linux vm ready after \$UPTIME seconds"
EOF

# OUTSIDE $work. Written inside it, the file is created empty, makehybrid then
# packs the directory, and every seed carries a zero-byte copy of itself.
seed="$(dirname "$work")/$(basename "$work")-seed.iso"

# ── ISO9660 FOR LINUX, FAT FOR FREEBSD ──
#
# cloud-init's NoCloud datasource finds its seed by FILESYSTEM LABEL, and
# ISO9660 stores volume identifiers uppercased. Linux cloud-init matches
# "cidata" case-insensitively and does not care. FreeBSD's exposes the disc as
# /dev/iso9660/CIDATA and did not match, so the guest booted with no user at
# all: no odtest, no key, and no way in but the console.
#
# A FAT image keeps the label as written, which is what FreeBSD's datasource
# looks for. Same two files either way.
case "$distro" in
    freebsd*)
        rm -f "$seed"
        hdiutil create -size 4m -fs "MS-DOS FAT12" -volname CIDATA \
                -layout NONE -ov -quiet "${seed%.iso}" >/dev/null
        mv "${seed%.iso}.dmg" "$seed" 2>/dev/null || true
        mnt=$(hdiutil attach -nobrowse "$seed" | awk '{print $NF}' | tail -1)
        cp "$work/user-data" "$work/meta-data" "$mnt/"
        hdiutil detach "$mnt" -quiet
        ;;
    *)
        hdiutil makehybrid -iso -joliet -default-volume-name cidata \
                -o "$seed" "$work" -quiet
        ;;
esac
note "seed: $(du -h "$seed" | cut -f1) (user odtest, key $(basename "$key"))"

step "the virtual machine"
mkdir -p "$bundle/Data"
disk="$bundle/Data/disk.qcow2"
# The cloud image is small and fixed; copy it and grow it, so the original
# stays cached for the next VM.
cp "$img" "$disk"
qemu-img resize "$disk" "${disk_gb}G" >/dev/null
cp "$seed" "$bundle/Data/seed.iso"
note "disk: ${disk_gb} GB from $(basename "$img")"

python3 - "$bundle" "$ram_mb" "$cores" "$utm_arch" "$distro" <<'PY'
import plistlib, sys, uuid, random, platform
bundle, ram, cores, arch, distro = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
# THE MACHINE TYPE IS PER ARCHITECTURE, and "virt" is the Arm one. An x86_64
# guest wants q35; given virt it does not boot, and UTM reports nothing useful
# about why. Hypervisor.framework can only run the HOST's architecture too, so
# an x86_64 guest on Apple silicon is emulated and must not ask for it -- with
# Hypervisor true and a foreign architecture the VM fails to start at all.
host_arm = platform.machine() in ("arm64", "aarch64")
target = "virt" if arch == "aarch64" else "q35"
native = (arch == "aarch64") == host_arm
bsd = distro.startswith("freebsd") or distro.startswith("openbsd")
# 4600 + a hash of the name: stable for one guest, distinct between guests, and
# clear of anything a developer is likely to be running.
serial_port = 4600 + (sum(ord(c) for c in bundle) % 300)
gpu = "virtio-ramfb" if bsd else "virtio-gpu-pci"
share_mode = "None" if bsd else "VirtFS"
mac = "C2:" + ":".join(f"{random.randint(0,255):02X}" for _ in range(5))
cfg = {
    "Backend": "QEMU",
    "ConfigurationVersion": 4,
    # Icon and IconCustom are load bearing: UTM silently skips a bundle whose
    # Information block lacks them -- it never appears in the list and utmctl
    # never sees it. Learned building the Windows VM; see that script.
    "Information": {"Name": bundle.split("/")[-1].removesuffix(".utm"),
                    "UUID": str(uuid.uuid4()).upper(),
                    "Icon": "linux",
                    "IconCustom": False,
                    "Notes": f"Built by tools/linux_vm_create.sh ({distro}). "
                             "Login odtest / odtest. Disposable: rebuild rather than repair."},
    # ── THE DISPLAY DEVICE IS A DRIVER QUESTION, PER GUEST ──
    #
    # virtio-gpu-pci for Linux: the driver is in the kernel, so the guest picks
    # its own resolution instead of being capped at whatever the firmware set.
    #
    # NOT for FreeBSD. Given virtio-gpu-pci, a FreeBSD/aarch64 guest panics
    # twelve seconds into boot -- "panic: vm_fault failed", stack through
    # data_abort, then an automatic reboot, forever. It looks nothing like a
    # graphics fault from outside: the VM appears to boot, ssh answers on a
    # partial boot and then stops, no address holds, and every layer above
    # (cloud-init, the seed, the user) gets blamed in turn. It cost an evening.
    #
    # This is the same shape as the Windows note in windows_vm_create.sh, for
    # the same reason: virtio-gpu wants a driver the guest does not have.
    # ramfb is what both fall back to -- a firmware-set framebuffer that needs
    # no driver at all.
    "Display": [{"DownscalingFilter": "Linear", "DynamicResolution": True,
                 "Hardware": gpu, "NativeResolution": True,
                 "UpscalingFilter": "Linear"}],
    "Drive": [
        {"Identifier": str(uuid.uuid4()).upper(), "ImageName": "disk.qcow2",
         "ImageType": "Disk", "Interface": "VirtIO", "InterfaceVersion": 1, "ReadOnly": False},
        {"Identifier": str(uuid.uuid4()).upper(), "ImageName": "seed.iso",
         "ImageType": "CD", "Interface": "USB", "InterfaceVersion": 1, "ReadOnly": True},
    ],
    "Input": {"MaximumUsbShare": 3, "UsbBusSupport": "3.0", "UsbSharing": True},
    # virtio-net under vmnet-shared, which gives the guest its own address on
    # the 192.168.64.0/24 the Mac already routes. NO PortForward: UTM runs
    # vmnet-shared, hostfwd belongs to QEMU's slirp, and entries here are
    # accepted and silently ignored. An hour went into that on the Windows VM
    # -- and worse, into authenticating against ANOTHER VM that had bound the
    # forwarded port. Reach this guest at its own IP.
    # IsolateFromHost AND PortForward MUST BE PRESENT, even empty. This is the
    # third key in this file whose absence makes UTM skip the whole bundle in
    # silence -- after Information.Icon and the top-level Sharing block. The
    # config is written, is valid plist, and never appears: utmctl answers
    # "Virtual machine not found", restarting UTM changes nothing, and
    # `open -a UTM` does not import it either. Found by diffing against the
    # Windows bundle this repository's own script builds and UTM accepts.
    #
    # PortForward is EMPTY on purpose. UTM runs vmnet-shared; hostfwd belongs
    # to QEMU's slirp, so entries here are accepted and ignored. Reach the
    # guest at its own 192.168.64.x address -- see the Windows notes for the
    # hour that cost.
    "Network": [{"Hardware": "virtio-net-pci", "IsolateFromHost": False,
                 "MacAddress": mac, "Mode": "Shared", "PortForward": []}],
    "QEMU": {"AdditionalArguments": [],
             "BalloonDevice": False, "DebugLog": False,
             "Hypervisor": native, "PS2Controller": False, "RNGDevice": True,
             "RTCLocalTime": False, "TPMDevice": False, "TSO": False,
             "UEFIBoot": True},
    # A SERIAL CONSOLE, so a guest that never comes up can be READ. Without
    # one, a cloud-init failure is invisible: the VM boots, nothing answers
    # ssh, and there is nothing to look at but a framebuffer. That is how an
    # hour went into a FreeBSD guest whose user-data asked for a group FreeBSD
    # does not have. `utmctl attach <name>` opens it.
    # ── THE BOOT, READABLE OVER A SOCKET ──
    #
    # A guest that will not come up has to be readable or it can only be
    # guessed at -- four theories went into a FreeBSD guest that turned out to
    # be panicking twelve seconds into every boot, and a screenshot from the
    # person at the machine solved it in ten seconds.
    #
    # NOT `-serial file:`: UTM runs QEMU in a sandboxed XPC helper which is
    # refused the open, even on a path inside UTM's own container and even
    # when the file is created first ("could not connect serial device to
    # character backend"). NOT UTM's Ptty mode either: `utmctl attach`, the
    # documented way to reach that, is not implemented in this build.
    #
    # A TCP server is allowed, and the port is ours to choose, so the console
    # can be read with nc from anywhere -- including a pipeline with nobody
    # watching. Derived from the name so two guests never collide.
    "Serial": [{"Mode": "TcpServer", "Port": serial_port,
                "Target": "Auto", "WaitForConnection": False}],
    # SHARING IS NOT OPTIONAL, in the same way Icon is not. A bundle without a
    # top-level Sharing block is written, is valid plist, and is IGNORED: it
    # never appears in UTM and `utmctl start` answers "Virtual machine not
    # found". Found by diffing this against the two bundles UTM does accept,
    # which is the same way the Icon requirement was found. VirtFS rather than
    # WebDAV because the guest is Linux and has 9p in the kernel.
    # VirtFS IS 9p, AND FREEBSD HAS NO 9p DRIVER. Chosen for Linux, where the
    # driver is in the kernel, and then reused for FreeBSD without thinking --
    # which put a device on the bus with nothing to attach it. The guest
    # panicked twelve seconds into boot, "vm_fault failed", every time, and
    # rebooted forever. It survived the display device being changed because
    # the display was never the problem.
    #
    # None for the BSDs: no share is better than a share that panics, and the
    # packages under test arrive over ssh anyway. The Windows VM uses WebDAV
    # for the same reason -- it has no 9p either.
    "Sharing": {"ClipboardSharing": True,
                "DirectoryShareMode": share_mode,
                "DirectoryShareReadOnly": False},
    "Sound": [{"Hardware": "intel-hda"}],
    # No MachineProperties. The working Linux bundle has none, and an unknown
    # key here is another way to be silently skipped.
    "System": {"Architecture": arch, "CPU": "default", "CPUCount": cores,
               "CPUFlagsAdd": [], "CPUFlagsRemove": [], "ForceMulticore": False,
               "JITCacheSize": 0, "MemorySize": ram, "Target": target},
}
with open(f"{bundle}/config.plist", "wb") as f:
    plistlib.dump(cfg, f)
print(f"  console on tcp://127.0.0.1:{serial_port}  (nc 127.0.0.1 {serial_port})")
print(f"  config.plist written ({arch}/{target}, {gpu}, share={share_mode}, {cores} cores, {ram} MB, "
      f"{'hypervisor' if native else 'EMULATED -- slow'})")
PY

step "next"
note "RESTART UTM FIRST. It will not see this VM until you do."
note ""
note "UTM keeps a REGISTRY of virtual machines -- a list of paths and"
note "security-scoped bookmarks in com.utmapp.UTM.plist -- and does not scan"
note "its Documents directory while running. A bundle written behind its back"
note "is complete and valid and simply absent: it never appears in the window,"
note "and utmctl answers 'Virtual machine not found'. Opening the bundle with"
note "'open -a UTM' does not register it either. It is picked up when UTM next"
note "launches. (The same is true of tools/windows_vm_create.sh, which does not"
note "say so.)"
note ""
note "  quit UTM, start it again, and then:"
note ""
note "  /Applications/UTM.app/Contents/MacOS/utmctl start $name"
note ""
note "cloud-init configures it on first boot; give it a minute. Then find it:"
note ""
note "  /Applications/UTM.app/Contents/MacOS/utmctl ip-address $name"
note "  ssh -i ${key%.pub} odtest@<that address>"
note ""
note "It has no port forward on purpose -- see the note in config.plist."
note "Throw it away and rebuild rather than repairing it:"
note ""
note "  /Applications/UTM.app/Contents/MacOS/utmctl delete $name"
