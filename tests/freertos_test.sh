#!/bin/bash
# Does the FreeRTOS edition still play the game it played?
#
#   tests/freertos_test.sh                 the core on the host: needs only cc
#   tests/freertos_test.sh --boards        and every board whose toolchain and
#                                          emulator are installed, under FreeRTOS
#   tests/freertos_test.sh --all-boards    every board, and a missing one fails
#   tests/freertos_test.sh --update        rewrite the expected transcript
#
# One script of commands (tests/freertos/play.txt) and one expected transcript
# (tests/freertos/play.expected). Every build must print exactly that: the core
# with no scheduler, the kernel's POSIX port, a Cortex-M3, a Cortex-M4F and a
# RISC-V core. A rule that behaves differently on a 32-bit target, a task that
# touches the world while another is resolving a turn, a serial driver that
# drops a character -- each shows up as a diff, on the board where it happens.
#
# A change to the rules changes the transcript on purpose; --update rewrites it,
# and the diff in review is the change to the game, turn by turn.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
PY="$("$root/tools/find_python.sh" 2>/dev/null || echo python3)"
mode="${1:-}"
# The transcript driver waits on a pipe with select(), which Windows' Python
# cannot do; the edition is C99 and is played on Linux and macOS.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) echo "skip: the FreeRTOS transcript driver needs a POSIX host"; exit 0 ;;
esac
for t in make cc; do
    command -v "$t" >/dev/null 2>&1 || { echo "skip: no $t"; exit 0; }
done
dir="$root/tests/freertos"
out="$root/build/freertos/transcripts"
mkdir -p "$out"
fail=0

play() {      # name, command...
    local name="$1"
    shift
    if ! "$PY" "$root/tools/freertos_play.py" "$dir/play.txt" -- "$@" > "$out/$name.txt"; then
        echo "FAILED: $name did not finish the game ($out/$name.txt)"
        tail -5 "$out/$name.txt"
        fail=1
        return
    fi
    if [ "$mode" = --update ] && [ "$name" = host ]; then
        cp "$out/$name.txt" "$dir/play.expected"
        echo "updated tests/freertos/play.expected"
    elif diff -u "$dir/play.expected" "$out/$name.txt" > "$out/$name.diff"; then
        echo "ok    $name: $(grep -c '^> ' "$out/$name.txt") commands, transcript as expected"
    else
        echo "FAILED: $name played a different game:"
        head -40 "$out/$name.diff"
        fail=1
    fi
}

make -s -C "$root/freertos" host PY="$PY" >/dev/null || { echo "FAILED: the host build"; exit 1; }
play host "$root/build/freertos/host/od_host"

if [ "$mode" = --boards ] || [ "$mode" = --all-boards ]; then
    K="$root/build/FreeRTOS-Kernel"
    "$root/tools/freertos_kernel.sh" "$K" || exit 1
    have() { command -v "$1" >/dev/null 2>&1; }
    E="$root/build/freertos"
    for board in posix mps2-an385 stm32f405 riscv32-virt; do
        case $board in
            posix)        need="cc";                                       run=("$E/posix/opendoctrines.elf") ;;
            mps2-an385)   need="arm-none-eabi-gcc qemu-system-arm";        run=(qemu-system-arm -M mps2-an385 -semihosting-config enable=on,target=native) ;;
            stm32f405)    need="arm-none-eabi-gcc qemu-system-arm";        run=(qemu-system-arm -M netduinoplus2 -semihosting-config enable=on,target=native) ;;
            riscv32-virt) need="riscv64-unknown-elf-gcc qemu-system-riscv32"; run=(qemu-system-riscv32 -M virt -bios none) ;;
        esac
        missing=""
        for t in $need; do have "$t" || missing="$missing $t"; done
        if [ -n "$missing" ]; then
            if [ "$mode" = --all-boards ]; then
                echo "FAILED: $board needs$missing"
                fail=1
            else
                echo "skip  $board: needs$missing"
            fi
            continue
        fi
        if ! make -s -C "$root/freertos" BOARD="$board" FREERTOS_KERNEL="$K" PY="$PY" > "$out/$board.build.log" 2>&1; then
            echo "FAILED: $board does not build:"
            tail -20 "$out/$board.build.log"
            fail=1
            continue
        fi
        if [ "$board" != posix ]; then
            run+=(-display none -monitor none -serial stdio -kernel "$E/$board/opendoctrines.elf")
        fi
        play "$board" "${run[@]}"
    done
fi
exit $fail
