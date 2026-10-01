#!/usr/bin/env bash
# A headless guest, from one command, with a console you can read.
#
#   tools/qemu_guest.sh create <name> <os>    build it (cloud image + seed)
#   tools/qemu_guest.sh start  <name>         boot it, headless
#   tools/qemu_guest.sh wait   <name> [secs]  block until ssh answers
#   tools/qemu_guest.sh ssh    <name> [cmd]   run something in it
#   tools/qemu_guest.sh push   <name> <src> <dst>
#   tools/qemu_guest.sh provision <name> <role>  install what a role needs
#   tools/qemu_guest.sh sshenv <name>         exports so other tools can reach it
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
# The game port, forwarded alongside ssh so a guest can HOST and the host
# machine -- or another guest, through the host -- can join it. Derived from
# the ssh port rather than hashed separately, so the two cannot collide and
# `list` can print both without a second lookup.
gameport_for() { printf '%d\n' $(( $(port_for "$1") + 2000 )); }
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
    # FreeBSD DOES NOT SHIP SUDO, and cloud-init will write a sudoers rule for
    # it regardless -- so the account comes up with permission to use a command
    # that is not installed, and every privileged step fails with
    # "sh: sudo: not found". It is a package there, so it is asked for.
    case "$os" in
        freebsd*) groups="[wheel]"
                  pkgs="  - sudo"$'\n'"  - bash" ;;
        *)        groups="[sudo, audio, video]"
                  pkgs="  - file" ;;
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
            # makehybrid APPENDS .iso to whatever -o is given, so asking for
            # seed.img produces seed.img.iso and the guest starts with a drive
            # pointing at a file that does not exist. QEMU says so and dies
            # immediately, which is at least loud -- but only because the
            # console is a file now.
            hdiutil makehybrid -iso -joliet -default-volume-name cidata \
                    -o "$d/seed" "$w" -quiet
            mv "$d/seed.iso" "$d/seed.img" ;;
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
        -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$p-:22,hostfwd=tcp:127.0.0.1:$(gameport_for "$name")-:27015" \
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

# A guest here is reached at odtest@127.0.0.1 on a forwarded port with a
# specific key -- not through an ~/.ssh/config alias, the way a UTM guest from
# linux_vm_create.sh is. Tools written against an alias therefore cannot reach
# one, and the alternative is writing into the user's ssh config, which is
# theirs and not ours to edit.
#
# So: eval "$(tools/qemu_guest.sh sshenv pf-debian)" and the ssh and scp in
# those tools pick up the options they need from the environment. ssh takes
# -p for the port and scp takes -P, which is why there are two of these.
# WHAT A GUEST NEEDS, WRITTEN DOWN RATHER THAN TYPED IN ONCE.
#
# The packages guest was provisioned by hand, and a reboot took it apart: the
# appimagetool on it was a two-line wrapper execing /tmp/appimagetool, which
# survives exactly until /tmp is cleared. Everything then failed with
#
#   /usr/local/bin/appimagetool: 2: exec: /tmp/appimagetool: not found
#
# and nine checks went red for a reason that had nothing to do with the game.
# A gate whose guests cannot be rebuilt is not a gate -- the first time one is
# lost, the gate is lost with it.
#
# Idempotent and cheap on a second run: every step checks before it installs,
# so a stage can call this every time rather than remembering whether it has.
#
#   roles: build     compilers, cmake, and the libraries raylib links
#          packages  fpm, rpm, appimagetool, fuse
#          both      the two together
cmd_provision() {
    local name="$1" role="${2:-both}"
    local os; os=$(cat "$(gdir "$name")/os" 2>/dev/null || echo debian12)
    local p; p=$(port_for "$name")
    local R; R="ssh $(ssh_opts) -p $p odtest@127.0.0.1"

    case "$os" in
      freebsd*) provision_freebsd "$R" "$role" ;;
      *)        provision_debian  "$R" "$role" ;;
    esac
}

provision_debian() {
    local R="$1" role="$2"
    # shellcheck disable=SC2086
    local run="$R"
    $run 'sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq' >/dev/null 2>&1

    if [ "$role" = build ] || [ "$role" = both ]; then
        note "provision: build tools and raylib's libraries"
        $run 'sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
                build-essential cmake ninja-build git pkg-config \
                libasound2-dev libx11-dev libxrandr-dev libxi-dev \
                libgl1-mesa-dev libglu1-mesa-dev libxcursor-dev \
                libxinerama-dev libwayland-dev libxkbcommon-dev' >/dev/null 2>&1 \
            || { note "provision: apt failed (build)"; return 1; }
    fi

    if [ "$role" = packages ] || [ "$role" = both ]; then
        note "provision: packaging tools"
        # fuse for the AppImage's own mount, rpm for fpm's rpm output,
        # ruby+dev because fpm is a gem with native extensions.
        $run 'sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
                ruby ruby-dev rpm fuse squashfs-tools file curl' >/dev/null 2>&1 \
            || { note "provision: apt failed (packages)"; return 1; }
        $run 'command -v fpm >/dev/null' \
            || { note "provision: installing fpm"; $run 'sudo gem install --no-document fpm' >/dev/null 2>&1; }

        # appimagetool is itself an AppImage. Kept in /usr/local/lib rather
        # than /tmp -- see the comment above cmd_provision for what happens
        # otherwise -- and run with --appimage-extract-and-run so it does not
        # need FUSE to build something that does.
        $run 'test -s /usr/local/lib/appimagetool.AppImage' || {
            note "provision: fetching appimagetool"
            $run 'set -e
                a=$(uname -m)
                sudo mkdir -p /usr/local/lib
                sudo curl -fsSL --retry 3 -o /usr/local/lib/appimagetool.AppImage \
                  "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$a.AppImage"
                sudo chmod +x /usr/local/lib/appimagetool.AppImage
                # A QUOTED HEREDOC, not printf with escapes. The wrapper has to
                # contain a literal "$@", and this text passes through a bash
                # single-quoted string, ssh, and a remote shell before anything
                # writes it. Two attempts at escaping that produced a wrapper
                # reading `--appimage-extract-and-run ""`, which silently drops
                # every argument -- appimagetool then ran with no AppDir and
                # the failure surfaced nowhere near here. <<"EOS" expands
                # nothing, so there is nothing left to get wrong.
                sudo tee /usr/local/bin/appimagetool >/dev/null <<"EOS"
#!/bin/sh
exec /usr/local/lib/appimagetool.AppImage --appimage-extract-and-run "$@"
EOS
                sudo chmod +x /usr/local/bin/appimagetool' >/dev/null 2>&1 \
            || { note "provision: could not install appimagetool"; return 1; }
        }
    fi

    $run 'for b in cmake g++ fpm rpm appimagetool; do command -v $b >/dev/null || echo "MISSING $b"; done'
}

provision_freebsd() {
    local run="$1" role="$2"
    note "provision: FreeBSD packages"
    # No fpm, no appimagetool and no dpkg on FreeBSD: a FreeBSD guest is for
    # building and running the game, not for making Linux packages.
    # libglvnd as well as mesa-libs: mesa-libs ships the LIBRARY, libglvnd
    # ships GL/gl.h. Without it raylib's find_package leaves
    # OPENGL_INCLUDE_DIR as NOTFOUND and the generate step fails with a path
    # that explains nothing. It is easy to miss here because the SERVER target
    # stubs raylib out and needs no GL at all -- a local configure was happy
    # for an afternoon while the CI game build was not.
    $run 'sudo pkg install -y cmake ninja git pkgconf python3 \
            libglvnd mesa-libs xorgproto \
            libX11 libXext libXrandr libXi libXcursor libXinerama \
            libxkbcommon wayland' >/dev/null 2>&1 \
        || { note "provision: pkg failed"; return 1; }
    $run 'for b in cmake ninja cc; do command -v $b >/dev/null || echo "MISSING $b"; done'
}

cmd_sshenv() {
    local name="$1"
    local p; p=$(port_for "$name")
    printf 'OD_SSH_HOST=%s\n' "odtest@127.0.0.1"
    printf 'OD_SSH_OPTS=%s\n' "'-p $p $(ssh_opts)'"
    printf 'OD_SCP_OPTS=%s\n' "'-P $p $(ssh_opts)'"
    printf 'OD_GAME_PORT=%s\n' "$(gameport_for "$name")"
    printf 'export OD_SSH_HOST OD_SSH_OPTS OD_SCP_OPTS OD_GAME_PORT\n'
}

cmd_console() { sed 's/\r//' "$(gdir "$1")/console.log" | tail -"${2:-40}"; }

# SHUT THE GUEST DOWN, do not pull its power.
#
# This used to kill(1) qemu, which is a power cut as far as the guest is
# concerned. On the FreeBSD guest that lost work: UFS rolls back writes that
# had not reached the disk, and a source tree unpacked, configured and left
# alone for ten minutes was simply not there after a restart -- which reads
# like the tool wiping the disk rather than the guest never having written it.
#
# So: ask the guest to halt, and give it time. kill is still here as the last
# resort for a guest that has hung, which is the only case it was ever right
# for.
cmd_stop() {
    local name="$1"
    local d; d=$(gdir "$name")
    [ -f "$d/qemu.pid" ] || { note "$name is not running"; return 0; }
    local pid; pid=$(cat "$d/qemu.pid")
    local p; p=$(port_for "$name")

    # shellcheck disable=SC2086
    ssh $(ssh_opts) -p "$p" odtest@127.0.0.1 'sudo poweroff || sudo shutdown -p now' \
        >/dev/null 2>&1 || true

    # 60s: a BSD unmounting a dirty filesystem is not instant, and cutting it
    # short here is exactly the bug this replaced.
    local waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 120 ]; do
        sleep 0.5; waited=$((waited + 1))
    done

    if kill -0 "$pid" 2>/dev/null; then
        note "$name did not halt in 60s -- terminating it"
        kill "$pid" 2>/dev/null || true
        for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$d/qemu.pid"
    note "$name stopped"
}

cmd_list() {
    [ -d "$HOME_DIR" ] || { note "no guests"; return 0; }
    printf '  %-16s %-12s %-7s %-7s %s\n' NAME OS SSH GAME STATE
    for d in "$HOME_DIR"/*/; do
        [ -d "$d" ] || continue
        local n os state
        n=$(basename "$d"); os=$(cat "$d/os" 2>/dev/null || echo "?")
        if [ -f "$d/qemu.pid" ] && kill -0 "$(cat "$d/qemu.pid")" 2>/dev/null
        then state=running; else state=stopped; fi
        printf '  %-16s %-12s %-7s %-7s %s\n' "$n" "$os" \
               "$(port_for "$n")" "$(gameport_for "$n")" "$state"
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
    provision) cmd_provision "$@" ;;
    sshenv)  cmd_sshenv "$@" ;;
    console) cmd_console "$@" ;;
    stop)    cmd_stop "$@" ;;
    list)    cmd_list "$@" ;;
    *) die "unknown command: $sub" ;;
esac
