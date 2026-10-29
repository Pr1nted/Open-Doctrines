# Open Doctrines on FreeRTOS

**The game runs on a microcontroller.** Not a terminal onto a server: the
world, the rules and every country's opponent run on the chip, under the
FreeRTOS kernel, in 120 KB of flash and 68 KB of RAM. You play it over a
serial line, the way a ground station talks to a satellite.

```
> play SWE
Sweden (SWE), turn 0
  11 provinces, 0 men, income 25.1 a turn, treasury 25.1
> recruit 154 max
ordered: raise 289,743 men in Sweden #1234, for 28.9
> end
turn 1: 11 provinces, 289,743 men, treasury 20.3, 0 battles world-wide
  raised 289743 men in Sweden #1234
> attack 154 153
(declaring war on Norway)
ordered: 289,743 men from Sweden #1234 against 0 defenders; frontage 83,350
> end
turn 2: 12 provinces, 289,743 men, treasury 43.0, 1 battles world-wide
  took Norway #815
```

1,632 provinces, 183 countries and 174 ships, the desktop game's combat width,
forts, unrest, harbours and fleets, and an opponent for every country that
raises armies and goes to war on its own.

This is an **experiment edition**, like [TempleOS](../templeos/README.md): a
separate build with a separate promise. Read what it does not have before you
play it.

## The boards

| Board | CPU | Runs in | Why it is here |
|---|---|---|---|
| `stm32f405` | Cortex-M4F, 1 MB flash, 128 KB SRAM | QEMU `netduinoplus2`, or a real board | The STM32F4 family is used in CubeSat on-board computers. The same image can be flashed to real hardware. |
| `mps2-an385` | Cortex-M3, 25 MHz | QEMU `mps2-an385` | ARM's reference board, which FreeRTOS's own QEMU demos use. Everything else is checked against it. |
| `riscv32-virt` | RV32IMAC | QEMU `virt` | RISC-V is where small spacecraft computers are going. This is the same game on that ISA and the same kernel. |
| `posix` | your computer | Linux, macOS | The FreeRTOS kernel's POSIX port: the real scheduler and tasks, with stdin and stdout as the serial line. |

The game uses about 120 KB of flash on every board. Most of that is the
world, kept `const` so it stays in flash. RAM holds about 52 KB: only the
numbers that change during a game. The rest is task stacks and a 12 KB
kernel heap. `hk` shows exactly how much is used.

## Running it

Download the release zip and start a board in QEMU:

```
qemu-system-arm -M netduinoplus2 -nographic -semihosting-config enable=on,target=native -kernel stm32f405.elf
```

`README.txt` in the zip has the line for each board. Press Ctrl-A, then X, to
leave QEMU, or type `quit`.

**On a real STM32F405:** flash `stm32f405.bin` at `0x08000000`, for example
with `st-flash write stm32f405.bin 0x08000000`. Connect a 3.3 V serial adapter
to PA9 (TX) and PA10 (RX) at 115200 8N1. The board stays on its internal
16 MHz oscillator. The PLL is never started, so nothing can hang waiting for
it to lock. A turn will take longer at 16 MHz than in QEMU. How much longer
has not been measured, because the game has not yet run on a real board.

**From source:**

```
tools/freertos_kernel.sh                 # FreeRTOS-Kernel V11.1.0, pinned
make -C freertos BOARD=stm32f405 run     # or mps2-an385, riscv32-virt, posix
```

You need `arm-none-eabi-gcc` with newlib, `riscv64-unknown-elf-gcc` with
picolibc, and QEMU. On Debian and Ubuntu these are `gcc-arm-none-eabi
libnewlib-arm-none-eabi gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf
qemu-system-arm qemu-system-misc`.

## Commands

Type `help` for the list. A province is named by its number, from `list` or
`show`, or by its name. Names with spaces work only with `show`.

| | |
|---|---|
| `play ISO` | govern a country |
| `countries [N]` | the largest countries |
| `status`, `list`, `show P` | your country, your provinces, one province and its borders |
| `recruit P N\|max` | raise men this turn, up to a fifth of the people |
| `attack FROM TO` | assault a neighbour with everything in FROM; declares war if needed |
| `bombard FROM TO` | shell a neighbour from the fleet in FROM's harbour |
| `war ISO`, `peace` | one enemy at a time, as in the TempleOS rules |
| `pacify PCT` | spend income on bringing unrest down |
| `end [N]` | resolve the turn, or N turns |
| `hk` | housekeeping: tasks, stack headroom, heap, uptime, the last turn's time |

## How it is built

```
freertos/core/    the game: od_rules.c (the turn), od_world.c (state),
                  od_shell.c (the console), od_main.c (the FreeRTOS tasks)
freertos/ports/   one directory per board: a UART, a linker script, and
                  how to leave QEMU
freertos/host/    the core with no kernel, for the tests
```

**The rules are a port of [`templeos/Rules.HC`](../templeos/Rules.HC)**, with
the same turn in the same order. The combat numbers are not copied.
`od_rules.c` includes `templeos/Sync.HH`, which `tools/templeos_sync.py`
generates from the desktop game's `src/Game.h`, and the suite holds that file
to its source. Retune combat width there, and the desktop, TempleOS and
FreeRTOS builds all change together.

**The world is the TempleOS edition's world.** `tools/freertos_world.py`
reads `templeos/world.odw` and writes it out as C arrays. It drops what a
serial console cannot show: the raster, the colours and the sea grid. Province
names, areas, neighbours and starting numbers go into flash. `od_world_reset`
copies the starting numbers into the one RAM structure the game changes. There
is no heap allocation in the game itself.

**Three tasks.** `console` reads the serial line and prints everything.
`engine` resolves a turn when asked, at the lowest priority, so it never
delays anything time-critical. `beacon` is the heartbeat, counting once a
second. The console waits while the engine runs, so only one task touches the
world at a time. The console polls the UART instead of using an interrupt.
One character a millisecond is faster than anyone types, and it keeps every
port free of interrupt priorities, the easiest part of a FreeRTOS port to get
wrong.

## How it is tested

`tests/freertos_test.sh` plays `tests/freertos/play.txt`, a 31-turn game with
a war, a conquest and a restart, into each build. Each build must print
exactly `tests/freertos/play.expected`. The builds are the bare core on the
host, the POSIX port, and the three boards in QEMU. A rule that behaves
differently on a 32-bit CPU, a task that touches the world mid-turn, or a
serial driver that drops a character each shows up as a diff, on the board
where it happens.

The host run is part of `tests/run_all.sh`. The FreeRTOS workflow runs all
five builds.

A change to the rules changes the transcript on purpose. `--update` rewrites
it, and the diff a reviewer sees is the change to the game, turn by turn.

## What it does not have

**Research, policies and artillery types.** `game.odd` is 43 KB of text this
build does not parse. Every country plays without levers, as the TempleOS AI
does.

**A sea.** Fleets defend their harbours and bombard their neighbours. They do
not sail: the sea grid is left out.

**Pacts and alliances.** One enemy at a time, as in the TempleOS rules.

**A map.** There is no screen. Province numbers and borders are the board.

**Saves.** A game lasts until the board is reset.

**Proof on real hardware.** The STM32F405 image is built for real silicon:
the internal oscillator, USART1 on PA9 and PA10, and no reliance on anything
QEMU emulates and a chip does not. So far it has only run in QEMU.
