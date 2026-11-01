# Open Doctrines on Plan 9

**The game, built by 9front's own compilers.** Not a port of the C++ game --
Plan 9 has no C++ compiler and no OpenGL -- but the
[FreeRTOS edition](../freertos/README.md)'s C core, unchanged, with Plan 9's
libc underneath it: the same rules, the same 1,632 provinces and 183
countries, the same opponent for every country. Two programs:

- `opendoctrines` -- the console. Commands on fd 0, answers on fd 1, so it
  plays in a rio window, over a serial console, or from a script.
- `odmap` -- the map, drawn with libdraw in a rio window. Every province is a
  square at its centre, sized by its area, in its owner's colour. Click one to
  select it (its `show` is printed under the map; its neighbours are ringed);
  button 3 types its number into the command line. The command line takes the
  console's commands. Del or `quit` leaves.

This is an **experiment edition**, like FreeRTOS and
[TempleOS](../templeos/README.md). It has exactly what the FreeRTOS edition
has, and lacks what that lacks: no research, no policies, no saves, no sea to
sail across.

## Building it on 9front

The world is C generated from `templeos/world.odw` by Python, which 9front
does not have. Either take the release zip (`plan9-v*` tags), which carries
it, or generate it on another machine and copy the tree over:

```
tools/plan9_package.py --tree build/plan9/src     # on a machine with Python
```

Then, on 9front, in the tree's `plan9/` directory:

```
mk                  # 6.opendoctrines and 6.odmap
mk install          # into /amd64/bin/games
games/opendoctrines
games/odmap
```

`opendoctrines 7` starts from seed 7, as `new 7` does.

## How the C core compiles with kencc

Nothing in `freertos/core` is forked. `core_world.c`, `core_rules.c` and
`core_shell.c` are one-line files that include the core's sources, and
`include/` stands in for the C99 headers the core names: each
(`stdint.h`, `stdio.h`, `string.h` ...) includes `include/od9.h`, which
includes `<u.h>` and `<libc.h>` once and maps the C99 names onto Plan 9's
(`uint32_t` is `uint`, `uint64_t` is `uvlong`, `size_t` is `ulong`).

Two things needed more than a header:

- **printf.** The core formats with C's `vsnprintf`. Plan 9's `print(2)` is
  a different language -- `%u` is a flag there, not a verb -- so
  `od9fmt.c` is the subset of C's printf the core uses, written out once. The
  host test builds the core with it in place of the host libc's and plays the
  scripted game, so a formatting difference shows up as a transcript diff on
  any machine.
- **The dialect.** The core was C99. It is now written so that kencc takes it
  as well: declarations at the start of blocks, none in `for`, no compound
  literals, and `od_neighbour` a macro rather than an `inline` function. These
  changes are behaviour-neutral, and the FreeRTOS transcripts did not move
  by a byte (`tests/freertos_test.sh --boards`). `tests/plan9_test.sh` holds the
  core to it by compiling it as gnu89 with `-Wdeclaration-after-statement`.

## Tests

```
tests/plan9_test.sh                   the core's dialect, and the game through od9fmt
tests/plan9_test.sh --9front ISO      and built and played inside 9front, in QEMU
```

The second is what `.github/workflows/plan9.yml` runs. `tools/plan9_9front.py`
takes the kernel and `plan9.ini` out of the 9front ISO (as data, with bsdtar),
boots that kernel in QEMU with `-kernel` and `console=0` on its command line,
so rc comes up on the serial line the script holds, answers the boot's
questions with their defaults, mounts a FAT disk image holding the source with
`dossrv`, runs `mk`, plays `tests/freertos/play.txt` into `6.opendoctrines` and
reads the output back between markers. The result is normalised as every
other edition's is (`tools/freertos_play.py --raw-in`) and compared byte for
byte with `tests/freertos/play.expected`.

## What has and has not been verified

Verified, on Linux: the core in the dialect above; the scripted game through
`od9fmt.c`, byte for byte; the FreeRTOS host, POSIX, Cortex-M3, Cortex-M4F
and RISC-V transcripts unchanged; the source tree and zip; the FAT image.

**Not yet verified: anything on 9front itself.** The session that wrote this
edition had no network route to 9front.org and so no 9front image to boot.
The mkfile, `include/od9.h`, `opendoctrines.c`, `odmap.c` and the guest
driver are written to 9front's documented conventions, but have not been
compiled by 6c or booted. CI pins 9front-11952 (`plan9.yml`).

The guest driver reads the kernel out of the ISO with bsdtar, 7z, isoinfo or
its own ISO9660 reader, logging what the image is (size, first bytes, gzip,
ISO9660 and MBR marks). If none can read it, or QEMU's `-kernel` gets nothing
from it, it boots the image itself with SeaBIOS's serial console turned on
(fw_cfg `etc/sercon-port`), so 9boot's screen and keyboard are the serial
line: it presses a key to stop the countdown, types `console=0` and `boot`
at 9boot's `>` prompt, and goes on as before.
The likeliest places for a first run to need a fix: 9boot's prompt as
seen through sercon, the disk names (`/dev/sdC1`, `/dev/sdD0`),
and whether `<u.h>` on the 9front release has every type `od9.h` names.
