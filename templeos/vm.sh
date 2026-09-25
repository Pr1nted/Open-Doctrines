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
#     templeos/vm.sh run         boot the installed disk (OD_TOS_GUI=1 for a window)
#     templeos/vm.sh push F...   copy files in to D:/Home
#     templeos/vm.sh pull N     copy a file back out
#     templeos/vm.sh shot F.png  capture the screen
#     templeos/vm.sh ready       boot all the way to a clean shell
#     templeos/vm.sh settle [s]  wait until the screen stops changing
#     templeos/vm.sh key <keys>  send keystrokes (QEMU key names, space separated)
#     templeos/vm.sh click X Y  click at an absolute point
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
    # OD_TOS_GUI=1 opens a real window instead of running blind. The monitor
    # socket stays either way, so everything that drives this machine by
    # script keeps working while somebody watches.
    #
    # Not daemonized in GUI mode: a Cocoa window wants the process that owns
    # it to stay in the foreground, and -daemonize hands it to a child that
    # has no business drawing one.
    local disp="-display none" bg="-daemonize"
    if [ "${OD_TOS_GUI:-0}" = "1" ]; then
        disp="-display cocoa"
        bg=""
    fi
    running && die "already running (pid $(cat "$PIDF")). vm.sh stop first."
    [ -f "$ISO" ] || die "no $ISO"
    [ -f "$DISK" ] || die "no $DISK -- run: templeos/vm.sh install"
    rm -f "$MON"
    # -boot d is the CD, -boot c the hard disk.
    #
    # THE NIC. TempleOS ships no network stack, so this card is useless until
    # something drives it -- templeos/Rtl8139.HC is that something. An RTL8139
    # rather than the default e1000 because its programming interface is four
    # registers and a ring buffer, which is a driver you can read in one sitting.
    #
    # User-mode networking: the guest is 10.0.2.15, the host is 10.0.2.2, and
    # the forward lets the host open a conversation rather than only answer
    # one. Host 15001, not 15000: a forward BINDS the host port, and would
    # leave nothing for a listener on the host to bind to.
    qemu-system-x86_64 \
        -m "$MEM" \
        -netdev user,id=n0,hostfwd=udp::15001-:15000 \
        -device rtl8139,netdev=n0 \
        -drive file="$DISK",format=raw,if=ide,index=0 \
        -drive file="$ISO",format=raw,if=ide,index=2,media=cdrom \
        $( [ -f "$SWAP" ] && echo -drive file="$SWAP",format=raw,if=ide,index=1 ) \
        $( [ -f "$PAYLOAD" ] && echo -drive file="$PAYLOAD",format=raw,if=ide,index=3,media=cdrom ) \
        -boot "$bootdev" \
        $disp \
        -monitor unix:"$MON",server,nowait \
        -pidfile "$PIDF" \
        $bg 2>&1 | sed 's/^/qemu: /' &
    sleep 2
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
    # ── SWEEP UP AFTER THE HOST ──
    #
    # macOS writes ._resource forks, .DS_Store and a .fseventsd directory into
    # any volume it mounts. The guest does not want them, they accumulate on
    # every push, and a filesystem this old is not the place to find out which
    # of them it can tolerate.
    find "$mnt" \( -name "._*" -o -name ".DS_Store" \) -delete 2>/dev/null
    rm -rf "$mnt/.fseventsd" "$mnt/.Spotlight-V100" "$mnt/.Trashes" 2>/dev/null
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
pull)
    # Files OUT of the guest: the other half of push, and the half that makes a
    # game possible rather than just a display. Same stopped-and-mounted dance,
    # same reason.
    #
    # The name is matched case-insensitively on purpose. A lower-case 8.3 name
    # written by the guest comes back as ORDERS.TXT, because FAT stores the
    # "this was lower case" flag and this OS does not set it.
    shift
    name="${1:?usage: vm.sh pull <name> [dest]}"
    dest="${2:-.}"
    sub="${OD_TOS_DEST:-Home}"
    [ -f "$DISK" ] || die "no $DISK"
    was_running=0
    running && { was_running=1; "$0" stop >/dev/null; sleep 2; }
    dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$DISK" | head -1 | awk '{print $1}')
    [ -n "$dev" ] || die "could not attach $DISK"
    mnt=$(mktemp -d)
    rc=1
    if mount -t msdos "${dev}s2" "$mnt" 2>/dev/null; then
        found=$(ls "$mnt/$sub" 2>/dev/null | awk -v n="$name" 'tolower($0)==tolower(n){print;exit}')
        if [ -n "$found" ]; then
            cp "$mnt/$sub/$found" "$dest/$name" && echo "pulled D:/$sub/$found -> $dest/$name" && rc=0
        else
            echo "no $name in D:/$sub" >&2
        fi
        diskutil unmount "$mnt" >/dev/null 2>&1 || umount "$mnt" 2>/dev/null
    else
        echo "could not mount ${dev}s2" >&2
    fi
    hdiutil detach "$dev" >/dev/null 2>&1
    rmdir "$mnt" 2>/dev/null
    if [ "$was_running" -eq 1 ]; then
        "$0" run >/dev/null
        echo "restarted; it will stop at the boot menu (send: vm.sh key 2)"
    fi
    exit "$rc"
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
click)
    # A click at an absolute point, out of a relative mouse.
    #
    # This OS drives a PS/2 mouse, which reports MOVEMENT, not position -- and
    # a USB tablet, the usual way to get absolute coordinates out of QEMU, is
    # not something it can talk to. So: shove the pointer hard into the
    # top-left corner, where it stops, and then move by exactly the offset
    # wanted. The overshoot is the whole trick.
    shift
    x="${1:?usage: vm.sh click <x> <y>}"; y="${2:?}"
    # ── THE FACTOR OF TWO ──
    #
    # A click asked for at 400,200 arrives at 196,96. It is not ms.scale --
    # setting that to 1.0 inside the guest changes nothing -- so the halving
    # is below it, in the PS/2 path between QEMU and the mouse handler.
    # Measured, not derived, and compensated here because here is where it can
    # be measured. OD_TOS_MOUSE overrides it if a different build disagrees.
    f="${OD_TOS_MOUSE:-2}"
    x=$(( x * f )); y=$(( y * f ))
    # IN SMALL STEPS, BOTH WAYS. A PS/2 packet carries nine signed bits of
    # movement, so one giant shove is not delivered as one giant move -- the
    # first attempt at this pinned the pointer by about 127 pixels instead of
    # 3000, clicked TempleOS's own menu bar and opened the File menu.
    for _ in 1 2 3 4 5 6 7 8 9 10; do mon "mouse_move -100 -100"; done
    sleep 0.3
    dx=$x; dy=$y
    while [ "$dx" -gt 0 ] || [ "$dy" -gt 0 ]; do
        sx=$(( dx > 100 ? 100 : dx )); sy=$(( dy > 100 ? 100 : dy ))
        mon "mouse_move $sx $sy"
        sleep 0.08          # without this only the first packet lands
        dx=$(( dx - sx )); dy=$(( dy - sy ))
    done
    sleep 0.4
    [ "${OD_NO_CLICK:-0}" = "1" ] || { mon "mouse_button 1"; sleep 0.2; mon "mouse_button 0"; }
    ;;
ready)
    # Boot to a usable shell, however this particular boot behaves.
    #
    # The tour question does not always appear -- after a fault it is skipped
    # -- and an "n" typed at a shell that never asked becomes the first
    # character of the next command. That produced `n#include "ODGame"` and an
    # afternoon of chasing a compiler bug that was a keystroke. So: answer it
    # if it came, then press return to commit whatever ended up on the line,
    # leaving a clean prompt either way.
    # ── BOOT C:, WORK ON D: ──
    #
    # The installer puts TempleOS on both partitions. D:'s boot files were
    # damaged by repeated hard stops of the VM mid-write -- it faults at
    # DirMk("/Tmp") and drops into the debugger -- while C: still boots
    # clean. The game's files live on D:/Home either way, and a booted C:
    # can read them perfectly well, so this boots the healthy one and
    # changes directory to the other.
    #
    # OD_TOS_BOOT=2 forces the old behaviour if D: is ever repaired.
    "$0" settle 240 22
    "$0" key "${OD_TOS_BOOT:-1}"
    "$0" settle 240 30
    "$0" key n
    "$0" settle 120 6
    "$0" key ret
    "$0" settle 120 4
    "$0" type 'Cd("D:/Home");'
    "$0" key ret
    "$0" settle 120 4
    ;;
settle)
    # ── WAIT FOR THE MACHINE TO STOP DOING THINGS ──
    #
    # Every timing failure in this project has been the same one: a keystroke
    # sent while the guest was still booting or still compiling, which lands
    # in the wrong prompt and produces a mangled command -- "h7elp", or an
    # include swallowed by the tour question. Sleeping a guessed number of
    # seconds is what caused that; waiting for the screen to stop changing is
    # what fixes it.
    #
    # The clock in the title bar ticks every second, so the top rows are
    # ignored. Two identical frames in a row is "settled".
    # A minimum wait as well as a maximum: booting has quiet moments, and the
    # first version of this declared victory five seconds in because two
    # samples four seconds apart happened to match while the machine was
    # still loading. And two matching frames is not enough on its own -- the
    # screen must hold still for STABLE consecutive samples.
    shift
    max="${1:-180}"
    min="${2:-20}"
    stable_want=3
    sleep "$min"
    prev=""
    stable=0
    n=$(( max / 2 ))
    i=0
    while [ "$i" -lt "$n" ]; do
        "$0" shot "$VM/.settle.png" >/dev/null 2>&1
        cp -f "$VM/.settle.png" "$VM/.settle$(( i % 2 )).png" 2>/dev/null
        # ── HOW MUCH CHANGED, NOT WHETHER ANYTHING DID ──
        #
        # A blinking cursor never stops changing, so demanding two identical
        # frames waits for ever. What matters is whether the machine is still
        # DOING something, and a cursor is a handful of pixels while a boot or
        # a compile repaints half the screen.
        cur=$(python3 -c "
import sys
from PIL import Image, ImageChops
try:
    a = Image.open('$VM/.settle0.png').convert('L').crop((0,16,640,480))
    b = Image.open('$VM/.settle1.png').convert('L').crop((0,16,640,480))
except Exception:
    print('x'); sys.exit()
d = ImageChops.difference(a, b)
n = sum(1 for p in d.getdata() if p > 24)
print('same' if n < 600 else 'busy')
" 2>/dev/null)
        if [ "$cur" = "same" ]; then
            stable=$(( stable + 1 ))
            [ "$stable" -ge "$stable_want" ] && exit 0
        else
            stable=0
        fi
        prev="$cur"
        sleep 2
        i=$(( i + 1 ))
    done
    echo "did not settle within ${max}s" >&2
    exit 1
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
