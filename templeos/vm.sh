#!/bin/bash
# A TempleOS virtual machine, and a way to look at it without a window.
#
# WHY HEADLESS. The obvious `-display cocoa` opens a window on somebody's
# desktop, which is useless to anything automated and impossible to check from a
# script. QEMU's monitor can dump the framebuffer to a file on demand
# (`screendump`), so this runs with no display at all and screenshots are taken
# through the monitor socket. `vm.sh shot out.png` is then the whole of "what is
# on screen right now".
#
# ON APPLE SILICON THIS IS EMULATION, NOT VIRTUALISATION. TempleOS is x86-64
# only, so qemu-system-x86_64 runs it under TCG. Expect boot to take a while and
# do not read the frame rate as anything about the OS.
#
#     templeos/vm.sh install     make the disk, first time only
#     templeos/vm.sh boot        boot the ISO (installer / live)
#     templeos/vm.sh run         boot the installed disk
#     templeos/vm.sh push F...   copy files in to D:/Home
#     templeos/vm.sh shot F.png  capture the screen
#     templeos/vm.sh key <keys>  send keystrokes (QEMU key names, space separated)
#     templeos/vm.sh type "txt"  type a string
#     templeos/vm.sh stop        shut the machine down
#     templeos/vm.sh status      is it running?
set -u

VM="${OD_TOS_HOME:-$HOME/TempleOS-VM}"
ISO="$VM/TempleOS.ISO"
DISK="$VM/TempleOS.raw"
SWAP="$VM/exchange.img"          # the bridge disk; see templeos/README.md
PAYLOAD="$VM/payload.iso"        # files going IN, as a second CD
MON="$VM/monitor.sock"
PIDF="$VM/qemu.pid"
MEM="${OD_TOS_MEM:-1024}"

die() { echo "$*" >&2; exit 1; }

# The monitor speaks a line protocol over a unix socket. macOS nc handles -U;
# a short timeout so a dead socket reports rather than hangs the caller.
mon() {
    [ -S "$MON" ] || die "no monitor socket -- is the machine running? (vm.sh status)"
    printf '%s\n' "$*" | nc -U -w 2 "$MON" >/dev/null 2>&1
}

running() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }

start() {
    local bootdev="$1"
    running && die "already running (pid $(cat "$PIDF")). vm.sh stop first."
    [ -f "$ISO" ] || die "no $ISO"
    [ -f "$DISK" ] || die "no $DISK -- run: templeos/vm.sh install"
    rm -f "$MON"
    # -boot d is the CD, -boot c the hard disk. No network device at all: this
    # OS has no stack to use one, and an emulated NIC it cannot drive is one
    # more thing to go wrong.
    qemu-system-x86_64 \
        -m "$MEM" \
        -drive file="$DISK",format=raw,if=ide,index=0 \
        -drive file="$ISO",format=raw,if=ide,index=2,media=cdrom \
        $( [ -f "$SWAP" ] && echo -drive file="$SWAP",format=raw,if=ide,index=1 ) \
        $( [ -f "$PAYLOAD" ] && echo -drive file="$PAYLOAD",format=raw,if=ide,index=3,media=cdrom ) \
        -boot "$bootdev" \
        -display none \
        -monitor unix:"$MON",server,nowait \
        -pidfile "$PIDF" \
        -daemonize 2>&1 | sed 's/^/qemu: /'
    sleep 1
    running || die "qemu did not start"
    echo "started (pid $(cat "$PIDF")), booting from $( [ "$bootdev" = d ] && echo CD || echo disk )"
    echo "screenshot with: templeos/vm.sh shot /tmp/tos.png"
}

case "${1:-}" in
install)
    mkdir -p "$VM"
    [ -f "$ISO" ] || die "put TempleOS.ISO in $VM first (templeos.org/Downloads/, check md5sums.txt)"
    [ -f "$DISK" ] && die "$DISK already exists; delete it to start over"
    # Terry's own emu_install uses 3G. TempleOS's whole distro is ~17 MB, so
    # this is room for the source tree and anything built in it.
    qemu-img create -f raw "$DISK" 3G
    echo "made $DISK"
    ;;
payload)
    # Files INTO the guest, as an ISO9660 CD. Read-only and one-way, which is
    # all that is needed to get source in -- and it works because TempleOS
    # boots from ISO9660, so the driver is certainly there. Getting files back
    # OUT is the harder half and is not this.
    shift
    src="${1:?usage: vm.sh payload <dir-or-file>}"
    mkdir -p "$VM/.payload"
    rm -rf "${VM:?}/.payload"/*
    cp -R "$src" "$VM/.payload/"
    rm -f "$PAYLOAD"
    hdiutil makehybrid -iso -joliet -default-volume-name ODPAYLOAD \
        -o "$PAYLOAD" "$VM/.payload" >/dev/null || die "makehybrid failed"
    echo "$PAYLOAD  ($(ls -la "$PAYLOAD" | awk '{printf "%.0f KB", $5/1024}'))"
    ls "$VM/.payload" | sed 's/^/  /'
    ;;
push)
    # THE BRIDGE, as one command. TempleOS installs onto FAT32 (confirmed by
    # DrvRep inside the guest), and macOS mounts FAT32 natively -- so the
    # exchange is just: stop, mount the raw disk's second partition, copy,
    # unmount, start. No RedSea driver, no nbd, no vvfat.
    #
    # STOPPED, ALWAYS. Mounting a filesystem that a running VM has open
    # read-write is how you corrupt both views of it. The stop is not a
    # convenience.
    #
    # SEVERAL FILES AT ONCE, because each push is a reboot: the guest has to be
    # stopped for the mount to be safe, and the machine takes half a minute to
    # come back. Pushing a client and the turn it reads as two commands costs
    # that twice for no reason.
    shift
    [ $# -gt 0 ] || die "usage: vm.sh push <file>... (lands in D:/\$OD_TOS_DEST, default Home)"
    sub="${OD_TOS_DEST:-Home}"
    for f in "$@"; do [ -f "$f" ] || die "no such file: $f"; done
    [ -f "$DISK" ] || die "no $DISK"
    was_running=0
    running && { was_running=1; "$0" stop >/dev/null; sleep 2; }
    dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$DISK" | head -1 | awk '{print $1}')
    [ -n "$dev" ] || die "could not attach $DISK"
    mnt=$(mktemp -d)
    # s2 is TempleOS's D:, which is where its Home lives and where it boots
    # from after a default install. s1 is C:.
    if ! mount -t msdos "${dev}s2" "$mnt" 2>/dev/null; then
        hdiutil detach "$dev" >/dev/null 2>&1
        rmdir "$mnt"
        die "could not mount ${dev}s2 -- is the guest really installed to D:?"
    fi
    mkdir -p "$mnt/$sub"
    for f in "$@"; do
        cp "$f" "$mnt/$sub/" && echo "pushed $(basename "$f") -> D:/$sub/"
    done
    diskutil unmount "$mnt" >/dev/null 2>&1 || umount "$mnt" 2>/dev/null
    hdiutil detach "$dev" >/dev/null 2>&1
    rmdir "$mnt" 2>/dev/null
    # `test && { ... }` as the last statement makes the ARM's exit status 1 when
    # the machine was not running, which silently breaks every `vm.sh push ... &&`
    # chain built on it. Hence the explicit if, and the explicit success.
    if [ "$was_running" -eq 1 ]; then
        "$0" run >/dev/null
        echo "restarted; it will stop at the boot menu (send: vm.sh key 2)"
    fi
    exit 0
    ;;
boot)  start d ;;
run)   start c ;;
shot)
    out="${2:-$VM/screen.png}"
    ppm="$VM/.screen.ppm"
    rm -f "$ppm"
    mon "screendump $ppm"
    # The dump is written asynchronously; wait for it to stop growing rather
    # than guessing a sleep, so a slow emulated machine does not yield half a
    # frame.
    for _ in $(seq 1 40); do
        [ -s "$ppm" ] && { a=$(wc -c < "$ppm"); sleep 0.15; b=$(wc -c < "$ppm"); [ "$a" = "$b" ] && break; }
        sleep 0.15
    done
    [ -s "$ppm" ] || die "no framebuffer came back"
    if command -v sips >/dev/null 2>&1; then
        sips -s format png "$ppm" --out "$out" >/dev/null 2>&1 || cp "$ppm" "$out"
    else
        cp "$ppm" "$out"
    fi
    echo "$out"
    ;;
key)
    shift
    [ $# -gt 0 ] || die "usage: vm.sh key <qemu-key-names...>"
    for k in "$@"; do mon "sendkey $k"; sleep 0.08; done
    ;;
type)
    shift
    s="${1:-}"
    SQ=$(printf "\047"); DQ=$(printf "\042")
    BS=$(printf "\134"); GR=$(printf "\140")
    # One key at a time through the monitor. Slow and dull, but it is the only
    # input this machine has: no serial console, no agent inside the guest.
    for (( i=0; i<${#s}; i++ )); do
        c="${s:$i:1}"
        # QEMU wants key NAMES, not characters, and every shifted symbol has
        # to be spelled as shift-<unshifted>. Leaving these out is not a
        # cosmetic gap: `#include "OpenDoc"` arrived as `include OpenDoc` and
        # the compiler reported a parse error that had nothing to do with the
        # source being tested.
        #
        # The quote characters come from printf rather than being written
        # literally, because a bare apostrophe inside a bash case arm is its
        # own small disaster.
        case "$c" in
            " ")  mon "sendkey spc" ;;
            "/")  mon "sendkey slash" ;;
            ".")  mon "sendkey dot" ;;
            ",")  mon "sendkey comma" ;;
            "-")  mon "sendkey minus" ;;
            ";")  mon "sendkey semicolon" ;;
            "=")  mon "sendkey equal" ;;
            "[")  mon "sendkey bracket_left" ;;
            "]")  mon "sendkey bracket_right" ;;
            "$SQ") mon "sendkey apostrophe" ;;
            "$DQ") mon "sendkey shift-apostrophe" ;;
            "$BS") mon "sendkey backslash" ;;
            "$GR") mon "sendkey grave_accent" ;;
            "#")  mon "sendkey shift-3" ;;
            "(")  mon "sendkey shift-9" ;;
            ")")  mon "sendkey shift-0" ;;
            ":")  mon "sendkey shift-semicolon" ;;
            "_")  mon "sendkey shift-minus" ;;
            "+")  mon "sendkey shift-equal" ;;
            "*")  mon "sendkey shift-8" ;;
            "&")  mon "sendkey shift-7" ;;
            "^")  mon "sendkey shift-6" ;;
            "%")  mon "sendkey shift-5" ;;
            "!")  mon "sendkey shift-1" ;;
            "@")  mon "sendkey shift-2" ;;
            "<")  mon "sendkey shift-comma" ;;
            ">")  mon "sendkey shift-dot" ;;
            "?")  mon "sendkey shift-slash" ;;
            "{")  mon "sendkey shift-bracket_left" ;;
            "}")  mon "sendkey shift-bracket_right" ;;
            "|")  mon "sendkey shift-backslash" ;;
            "~")  mon "sendkey shift-grave_accent" ;;
            [A-Z]) mon "sendkey shift-$(echo "$c" | tr 'A-Z' 'a-z')" ;;
            *)    mon "sendkey $c" ;;
        esac
        sleep 0.05
    done
    ;;
stop)
    running || { echo "not running"; exit 0; }
    mon "quit" || true
    sleep 1
    running && kill "$(cat "$PIDF")" 2>/dev/null
    rm -f "$PIDF" "$MON"
    echo "stopped"
    ;;
status)
    if running; then echo "running (pid $(cat "$PIDF")), monitor $MON"
    else echo "not running"; fi
    ;;
*)
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
