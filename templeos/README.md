# Open Doctrines on TempleOS

**The game does not run here, and will not.** TempleOS has no C++ compiler, no
OpenGL and no network stack; Open Doctrines needs all three. What runs here is
the *other half* of a real game.

The engine's agent door prints a turn as plain text and reads one line of
`module:action` tokens back:

```
[AGENT] ===== turn 0/120  Sweden (SWE) =====
[AGENT] land 12/1143 (1.05% of the world)  army 300000  treasury 40.0
[AGENT] economy  e:0 save  e:1 industry  e:2 fort  e:4 specialize  ...
[AGENT] budget e:8 p:3 w:8 n:8
[AGENT] waiting
```

![The turn above, drawn at 80x60 in 16 colours](../docs/img/templeos-mockup.png)

*Turn 0 of `1914:SWE`, from a real capture. Regenerate with
`python3 templeos/mockup.py <capture> out.png`.*

So a client has to read a file, draw 80×60 characters, and write a file. That
is inside what this OS offers, and the turn it plays is a turn of the actual
game — same map, same AI opponents, same rules.

**The host runs the engine. TempleOS runs the player.**

## Status

| | |
|---|---|
| Protocol pinned by a test | ✅ `tests/agent_protocol_test.sh` |
| Screen layout designed against a real capture | ✅ see below |
| Host bridge: a whole game as two files | ✅ `templeos/bridge.py`, tested |
| Reference client | ✅ `templeos/guest_sim.py` |
| `OpenDoc.HC` parses and draws a turn | ⚠️ **written, never compiled** |
| `OpenDoc.HC` sends orders back | ❌ not started |
| Getting the directory into a guest | ❌ the one part needing TempleOS |

`OpenDoc.HC` has not been through HolyC once. It was written against the
protocol and a rendering mockup on a machine with no TempleOS on it. Treat it
as a proposal, not as working code.

## What to check first, when you boot it

These are the things most likely to be wrong, roughly in the order they will
bite. None of them is deep; all of them are the kind of thing you only learn
from the compiler.

1. **The DolDoc escapes.** Colour and cursor moves are written as
   `"$$FG,BLUE$$"` and `"$$CM,%d,%d$$"`. Whether `$` needs doubling inside a
   HolyC string literal is exactly the sort of detail that differs from what
   the documentation looks like it says. If the screen fills with literal `$FG`
   text, that is this. The fallback is setting `Fs->text_attr` directly.
2. **`StrFirstOcc`'s signature.** Used for finding `(`, `army`, `net`. If it
   takes a character rather than a string, or returns an index rather than a
   pointer, the position line will parse to zeros.
3. **`DocClear`** — called without parentheses, which is legal for a HolyC
   function with no arguments, but check it is the right name for clearing the
   window rather than the document.
4. **Default argument on `OpenDoc`.** HolyC supports them; confirm the syntax.
5. **`class` with no methods** is HolyC's struct. Confirm `MemSet(t, 0,
   sizeof(ODTurn))` sizes it the way you expect.

## Producing a turn to feed it

On the host, in the Open Doctrines checkout:

```bash
mkfifo /tmp/a.fifo
./build/OpenDoctrinesServer --bench-agent 1914:SWE:rung /tmp/a.fifo \
    --until 120 --seed 20260801 --data data > turn.txt &
```

Wait for `[AGENT] waiting` to appear in `turn.txt`, then copy it into the
guest. That file is the whole input.

## The bridge

`templeos/bridge.py` runs the engine's door on the host and exchanges two files:

```
<dir>/turn.txt     written by the bridge; one turn, as the door printed it
<dir>/orders.txt   written by the player; one line of module:action tokens
```

It knows nothing about how that directory reaches a guest, deliberately — the
transport is the only part that needs a running TempleOS, so it is kept out of
the part that does not.

```bash
python3 templeos/bridge.py --build build --dir /tmp/odbridge
python3 templeos/guest_sim.py --dir /tmp/odbridge --turns 6   # in another shell
```

**Both files name their turn.** A file left from turn 4 looks exactly like one
written for turn 5, so `orders.txt` opens with `# turn 5` and anything else is
ignored and waited out. Timestamps are no help: they do not survive most ways of
getting a file across.

`tests/templeos_bridge_test.sh` plays five turns between two processes that
share only a path — the guest's situation minus the disk image — and asserts
that **every order the engine received is one the player wrote**.

That assertion was arrived at the hard way. Counting turns was not enough: with
the turn-number check deleted, the bridge ate a stale file, the guest answered
later turns instead, and both counts still came out right. The test passed with
the defence gone. Comparing content catches it, and so does planting the stale
file *during* a turn rather than before the run — a file planted first is
cleared on startup and never read.

## Getting the directory into a guest — the unsolved part

TempleOS has no networking — Terry Davis left it out deliberately — so **the
file is the network**. The intended shape:

```
host                                    guest (QEMU)
────                                    ────────────
engine writes turn.txt   ──▶  disk image  ──▶  OpenDoc reads it
engine reads orders.txt  ◀──  disk image  ◀──  you type a line
```

The fiddly parts, honestly:

- **RedSea is TempleOS's own filesystem.** The simplest exchange is probably a
  second raw image the guest mounts, written by the host between turns rather
  than during them.
- **The guest caches.** Swapping an image under a running guest is not safe in
  general; expect to need a remount, or to pause the VM while the host writes.
- **Turn cadence covers this.** Open Doctrines' long-form multiplayer already
  runs at one turn per hour to one per week. A bridge that takes ten seconds to
  hand a file across is not a problem at that pace — which is why long-form is
  the mode to aim at, not a 2-minute rapid game.

This is the piece to prototype before writing any more HolyC: if files cannot
be moved in and out reliably, nothing above it matters.

## Why not port a C++ compiler instead

It is the wrong target, for two reasons.

**You would not need one.** clang on the host can already cross-compile. The
problem is not producing code, it is that TempleOS has nothing to run it
against: no libc, no libstdc++ (the STL, exceptions, RTTI, unwinding tables),
no threads for the 21 source files that use `std::thread`, no `mmap`, no
process model, no ELF loader, and its own ABI. That is writing a userland, not
porting a compiler.

**And it would not be enough.** raylib needs OpenGL; this OS has a 640×480
16-colour framebuffer and no GPU. Multiplayer needs TCP/IP; this OS has no
network stack and no NIC driver. A perfect toolchain gets you a binary that
cannot draw and cannot connect — you would still be writing a software renderer
and a TCP stack, which is the actual work either way.

If the goal is "TempleOS-flavoured but reachable", **ZealOS** is a maintained
fork with broader hardware support and is worth checking first. Whether it has
networking, I do not know.

## How this stays working

`OpenDoc.HC` depends on the *protocol*, not on the game. New scenarios, AI
retrains and renderer changes cannot touch it. The one thing that can is a
change to what `Game_Agent.cpp` prints — so that is pinned:

```bash
tests/agent_protocol_test.sh build
```

It runs the real door for one turn and asserts the shape of all ten lines a
client reads, plus the two-space separator between menu items, which is
load-bearing because action names contain single spaces. It runs in the normal
suite. It does **not** pin the numbers: a test that fails when the AI improves
is a test somebody deletes.

The checks were verified by breaking the protocol on purpose — renaming
`budget`, and collapsing the item separator to one space — and confirming each
is caught.
