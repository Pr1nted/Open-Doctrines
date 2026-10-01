#!/usr/bin/env bash
# A headless guest, from one command, with a console you can read.
#
#   tools/qemu_guest.sh create <name> <os>    build it (cloud image + seed)
#   tools/qemu_guest.sh start  <name>         boot it, headless
#   tools/qemu_guest.sh wait   <name> [secs]  block until ssh answers
#   tools/qemu_guest.sh ssh    <name> [cmd]   run something in it
#   tools/qemu_guest.sh push   <name> <src> <dst>
#   tools/qemu_guest.sh console <name> [n]    the last n lines of its boot
#   tools/qemu_guest.sh stop   <name>
#   tools/qemu_guest.sh list
#
#   os: debian12 | debian11 | ubuntu2204 | freebsd14
#
# WHY NOT UTM
#
# tools/linux_vm_create.sh builds UTM VMs and is kept for watching an installer
# with human eyes, which is what a GUI is for. It is the wrong tool for a
# pipeline, and an evening went into learning why:
#
#   - UTM registers a VM only when it LAUNCHES. A bundle written behind its
#     back is complete, valid and invisible; utmctl says "Virtual machine not
#     found" and `open -a UTM` does not import it.
#   - `utmctl attach`, the documented way to reach a guest's console, is NOT
#     IMPLEMENTED in this build. It prints a warning and exits.
#   - UTM runs QEMU in a sandboxed helper that refuses `-serial file:` even on
#     a path inside UTM's own container, and ignores a TcpServer serial.
#   - So a guest that will not boot cannot be read. A FreeBSD guest panicked
#     twelve seconds into every boot through FIVE configurations before a
#     screenshot from a human revealed it, and the same image boots first time
#     in plain QEMU. UTM's display, USB and sound devices are the difference.
#
# Here the console is a file, the ssh port is one we chose, and nothing has a
# window. A guest that dies says so.
#
# NETWORKING IS QEMU'S USER STACK, not vmnet, and that is the other big
# simplification: -netdev user with hostfwd means the guest is at
# 127.0.0.1:<port> from the moment it boots. No DHCP lease to discover, no ARP
# sweep, no guest agent, and no second VM on the machine answering on a port we
# assumed was ours -- which cost an hour once already.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOME_DIR="${OD_GUEST_HOME:-$HOME/VMs/preflight}"
CACHE="$HOME/VMs/cloud"
KEY="${OD_GUEST_KEY:-$HOME/.ssh/od_winvm}"
FIRMWARE="${OD_QEMU_FIRMWARE:-/opt/homebrew/share/qemu/edk2-aarch64-code.fd}"

die()  { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }

# A stable port per guest: 2 bytes of its name, kept well clear of the
# ephemeral range and of anything a developer is likely to be running.
port_for() { printf '%d\n' $(( 33000 + $(printf '%s' "$1" | cksum | cut -d' ' -f1) % 2000 )); }
gdir()     { printf '%s/%s\n' "$HOME_DIR" "$1"; }

ssh_opts() {
    printf '%s' "-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
-o LogLevel=ERROR -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=8"
}

image_url() {
    case "$1" in
        debian12)   echo "https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-generic-arm64.qcow2" ;;
        debian11)   echo "https://cloud.debian.org/images/cloud/bullseye/latest/debian-11-generic-arm64.qcow2" ;;
        ubuntu2204) echo "https://cloud-images.ubuntu.com/jammy/current/jammy-server-cloudimg-arm64.img" ;;
        freebsd14)  echo "https://download.freebsd.org/releases/VM-IMAGES/14.5-RELEASE/aarch64/Latest/FreeBSD-14.5-RELEASE-arm64-aarch64-BASIC-CLOUDINIT-ufs.qcow2.xz" ;;
        *) die "unknown os: $1 (debian12|debian11|ubuntu2204|freebsd14)" ;;
    esac
}

cmd_create() {
    local name="$1" os="${2:-debian12}"
    local d; d=$(gdir "$name")
    [ -e "$d" ] && die "$name already exists -- $0 stop $name && rm -rf $d"
    [ -f "$KEY.pub" ] || die "no public key at $KEY.pub"
    command -v qemu-system-aarch64 >/dev/null || die "qemu not found -- brew install qemu"
    [ -f "$FIRMWARE" ] || die "no UEFI firmware at $FIRMWARE"

    local url img
    url=$(image_url "$os")
    mkdir -p "$CACHE" "$d"
    img="$CACHE/$(basename "${url%.xz}")"
    if [ ! -f "$img" ]; then
        note "downloading $(basename "$url")"
        curl -fL --progress-bar -o "$img.part" "$url"
        case "$url" in
            *.xz) note "decompressing"; mv "$img.part" "$img.xz.part"
                  xz -d -c "$img.xz.part" > "$img.part"; rm -f "$img.xz.part" ;;
        esac
        mv "$img.part" "$img"
    fi
    note "image: $(basename "$img") ($(du -h "$img" | cut -f1))"

    cp "$img" "$d/disk.qcow2"
    qemu-img resize "$d/disk.qcow2" "${OD_GUEST_DISK:-24G}" >/dev/null
    cp "$FIRMWARE" "$d/code.fd"
    qemu-img create -f raw "$d/vars.fd" 64M >/dev/null

    # ── the cloud-init seed ──
    # FreeBSD has wheel where Linux has sudo, and has neither audio nor video;
    # cloud-init fails the WHOLE user when a listed group is absent, which is
    # how a guest comes up with no account and no way in.
    local groups pkgs
    case "$os" in
        freebsd*) groups="[wheel]";              pkgs="  - bash" ;;
        *)        groups="[sudo, audio, video]"; pkgs="  - file" ;;
    esac
    local w; w=$(mktemp -d)
    printf 'instance-id: %s\nlocal-hostname: %s\n' "$name" "$name" > "$w/meta-data"
    cat > "$w/user-data" <<EOF
#cloud-config
users:
  - name: odtest
    groups: ${groups}
    shell: /bin/sh
    sudo: ["ALL=(ALL) NOPASSWD:ALL"]
    lock_passwd: false
    plain_text_passwd: odtest
    ssh_authorized_keys:
      - $(cat "$KEY.pub")
ssh_pwauth: false
packages:
${pkgs}
EOF
    # ISO9660 uppercases volume IDs and FreeBSD's datasource wants the label as
    # written, so FreeBSD gets a FAT seed. Linux matches case-insensitively.
    case "$os" in
        freebsd*)
            hdiutil create -size 4m -fs "MS-DOS FAT12" -volname CIDATA \
                    -layout NONE -ov -quiet "$d/seed" >/dev/null
            mv "$d/seed.dmg" "$d/seed.img"
            local mnt; mnt=$(hdiutil attach -nobrowse "$d/seed.img" | awk '{print $NF}' | tail -1)
            cp "$w/user-data" "$w/meta-data" "$mnt/"
            hdiutil detach "$mnt" -quiet ;;
        *)
            hdiutil makehybrid -iso -joliet -default-volume-name cidata \
                    -o "$d/seed.img" "$w" -quiet ;;
    esac
    rm -rf "$w"
    printf '%s\n' "$os" > "$d/os"
    note "guest '$name' ($os) ready at $d, ssh port $(port_for "$name")"
}

cmd_start() {
    local name="$1" d; d=$(gdir "$name")
    [ -d "$d" ] || die "no guest '$name' -- $0 create $name <os>"
    if [ -f "$d/qemu.pid" ] && kill -0 "$(cat "$d/qemu.pid")" 2>/dev/null; then
        note "already running (pid $(cat "$d/qemu.pid"))"; return 0
    fi
    local p; p=$(port_for "$name")
    : > "$d/console.log"
    # NO display, NO usb, NO sound. Those three are what UTM adds and what a
    # FreeBSD guest panicked on; a pipeline needs none of them.
    qemu-system-aarch64 \
        -machine virt -accel "${OD_QEMU_ACCEL:-hvf}" -cpu host \
        -smp "${OD_GUEST_CORES:-2}" -m "${OD_GUEST_RAM:-2048}" \
        -nographic -serial "file:$d/console.log" -monitor none \
        -drive "if=pflash,format=raw,readonly=on,file=$d/code.fd" \
        -drive "if=pflash,format=raw,file=$d/vars.fd" \
        -drive "if=virtio,format=qcow2,file=$d/disk.qcow2" \
        -drive "if=virtio,format=raw,file=$d/seed.img" \
        -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$p-:22" \
        -device virtio-net-pci,netdev=n0 \
        > "$d/qemu.log" 2>&1 &
    echo $! > "$d/qemu.pid"
    note "$name booting (pid $!), ssh on 127.0.0.1:$p, console $d/console.log"
}

cmd_wait() {
    local name="$1" secs="${2:-180}" p d
    p=$(port_for "$name"); d=$(gdir "$name")
    local t=0
    while [ "$t" -lt "$secs" ]; do
        # shellcheck disable=SC2086
        if ssh $(ssh_opts) -p "$p" odtest@127.0.0.1 true 2>/dev/null; then
            note "$name is up after ${t}s"; return 0
        fi
        if [ -f "$d/qemu.pid" ] && ! kill -0 "$(cat "$d/qemu.pid")" 2>/dev/null; then
            printf '\033[31m  %s died. Last of its console:\033[0m\n' "$name" >&2
            tail -20 "$d/console.log" 2>/dev/null | sed 's/\r//' | sed 's/^/    /' >&2
            return 1
        fi
        sleep 5; t=$((t + 5))
    done
    # THE WHOLE POINT OF THE CONSOLE FILE. A timeout with no explanation is
    # what UTM gave, and what sent an evening after the wrong cause.
    printf '\033[31m  %s did not answer ssh in %ss. Last of its console:\033[0m\n' "$name" "$secs" >&2
    tail -25 "$d/console.log" 2>/dev/null | sed 's/\r//' | sed 's/^/    /' >&2
    return 1
}

cmd_ssh() {
    local name="$1"; shift
    local p; p=$(port_for "$name")
    # shellcheck disable=SC2086
    if [ $# -eq 0 ]; then exec ssh $(ssh_opts) -p "$p" odtest@127.0.0.1
    else exec ssh $(ssh_opts) -p "$p" odtest@127.0.0.1 "$@"; fi
}

cmd_push() {
    local name="$1" src="$2" dst="$3"
    local p; p=$(port_for "$name")
    # shellcheck disable=SC2086
    scp $(ssh_opts) -P "$p" -r "$src" "odtest@127.0.0.1:$dst"
}

cmd_console() { sed 's/\r//' "$(gdir "$1")/console.log" | tail -"${2:-40}"; }

cmd_stop() {
    local d; d=$(gdir "$1")
    [ -f "$d/qemu.pid" ] || { note "$1 is not running"; return 0; }
    local pid; pid=$(cat "$d/qemu.pid")
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -9 "$pid" 2>/dev/null || true
    rm -f "$d/qemu.pid"
    note "$1 stopped"
}

cmd_list() {
    [ -d "$HOME_DIR" ] || { note "no guests"; return 0; }
    printf '  %-16s %-12s %-7s %s\n' NAME OS PORT STATE
    for d in "$HOME_DIR"/*/; do
        [ -d "$d" ] || continue
        local n os state
        n=$(basename "$d"); os=$(cat "$d/os" 2>/dev/null || echo "?")
        if [ -f "$d/qemu.pid" ] && kill -0 "$(cat "$d/qemu.pid")" 2>/dev/null
        then state=running; else state=stopped; fi
        printf '  %-16s %-12s %-7s %s\n' "$n" "$os" "$(port_for "$n")" "$state"
    done
}

[ $# -ge 1 ] || { sed -n '2,20p' "$0"; exit 1; }
sub="$1"; shift
case "$sub" in
    create)  cmd_create "$@" ;;
    start)   cmd_start "$@" ;;
    wait)    cmd_wait "$@" ;;
    ssh)     cmd_ssh "$@" ;;
    push)    cmd_push "$@" ;;
    console) cmd_console "$@" ;;
    stop)    cmd_stop "$@" ;;
    list)    cmd_list "$@" ;;
    *) die "unknown command: $sub" ;;
esac
