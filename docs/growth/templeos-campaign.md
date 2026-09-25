# The TempleOS campaign

## What the story actually is

Not "we ported a game". The story is that TempleOS turned out to be capable of
far more than its reputation allows, and the evidence is specific:

- It boots into 640×480 and sixteen colours because **one line of
  `KStart16.HC` asks the BIOS for mode 0x12**. The Bochs DISPI registers set a
  linear-framebuffer mode with no BIOS call, and the game runs at 1024×768 in
  32-bit colour.
- It ships **no network stack at all**, so the port carries one: an RTL8139
  driver, ARP, IPv4 and UDP, about 380 lines. `hello from TempleOS` arrived on
  a host listener; `pong from the host` came back.
- It **decompresses its own source on read**, so the machine can be made to
  unpack all 510 of its files for you to read — which is how most of the above
  was found rather than guessed.

That is a post about an operating system, told through a game. It is far more
interesting than a post about a game that happens to run on one, and it is the
version that is actually true.

## Tone, and one thing to get right

TempleOS's community is protective of Terry Davis, and rightly so. The tone
that works is **respect through effort**: take the machine seriously, show
your working, credit the OS for what it does well. The tone that fails is
novelty — "look at this weird OS" — which reads as a joke at his expense.

Concretely:
- Say what the OS made *easy*. Identity-mapped memory means a pointer is a
  physical address, so the NIC driver needs no DMA translation at all. Its
  compiler builds 3,000 lines in 1.7 seconds. Say so.
- Never imply the port "fixes" TempleOS or that it was hampering you.
- "TempleOS is public domain, by Terry A. Davis" in every post. No more, no
  less; do not editorialise about him.
- Open Doctrines is **source-available, not open source**. Do not call it open
  source anywhere.

## Assets

| File | Use |
|---|---|
| `docs/img/templeos-menu.png` | the lead image everywhere — it is instantly legible |
| `docs/img/templeos-game.png` | the map in play |
| `docs/img/templeos-actions.png` | all 39 actions; the "this is a real port" proof |
| `docs/img/templeos-navy.png` | harbours and fleets |
| `docs/img/templeos-truecolor.png` | the gradient; use only in technical posts |

## The sequence

Post to the technical audiences **first**. They will find the failure modes
and the questions, and the answers make the later posts better. The game
audiences last, when the release has survived contact.

1. **r/TempleOS_Official** — the people whose machine it is. Day 1.
2. **r/osdev** — day 1, a few hours later. Different post; this audience wants
   the driver and the mode switch.
3. **Hacker News**, Show HN — day 2 morning (US). Only after the first two
   have gone quiet enough to answer comments properly.
4. **r/grandstrategygames** and **r/StrategyGames** — day 3. The r/StrategyGames
   post for Open Animal Stage did ~150k views; the format there is known to
   work, so reuse its shape.
5. **Lobste.rs** — only if HN lands. Tag `osdev`, `gamedev`.

Do not post the same text twice. Each community is answering a different
question.

---

## 1. r/TempleOS_Official

**Title:** Open Doctrines runs on TempleOS — 1,632 provinces, all 39 actions,
no host engine

I have spent the last while getting a grand strategy game to run properly on
TempleOS, and it is now doing the whole job on the machine: the rules, the
map and the interface, with nothing on the other side of a bridge.

What it does:

- 1,632 provinces with adjacency, population, armies, harbours and deposits
- all 39 actions the desktop game offers — war, economy, politics, navy
- 86 research nodes and 59 policies, taken from the desktop game's own tables
- a sea map, so fleets sail between harbours across open water
- 1024×768 in 32-bit colour

Three things the OS deserves credit for, because they made this far easier
than it would have been anywhere else:

**The compiler is genuinely fast.** Three thousand lines of HolyC compile in
about 1.7 seconds on an emulated machine. I stopped bothering with a build
step for most of this work.

**Memory is identity-mapped**, so a pointer *is* a physical address. The
network driver writes its receive buffer's address straight into the card's
register — no translation, no page pinning, none of the ceremony that
normally surrounds DMA.

**It decompresses its own source on read.** I had the machine unpack all 510
of its `.HC.Z` files so I could read them from outside, and after that nearly
every question I had was a grep rather than a guess. The colour depth, the
video mode, the loader — all of it answered by the source that ships with the
OS.

Source and data are in the release; copy them into `D:/Home` and
`#include "ODGame"; ODStart;`. It wants a linear-framebuffer mode through the
DISPI registers, so QEMU, Bochs or VirtualBox rather than metal.

TempleOS is public domain, by Terry A. Davis.

[link] [menu screenshot] [actions screenshot]

---

## 2. r/osdev

**Title:** Getting true colour and a network stack onto TempleOS (which has
neither)

TempleOS boots into 640×480 with sixteen colours and ships no networking. I
wanted to run a strategy game on it, so it needed both. Notes, in case any of
it is useful to somebody else.

**The colour depth is one constant.** `KStart16.HC` does:

```
MOV AX,0x4F02
MOV BX,0x12     //640x480 16 color
```

That is a VBE Set Mode call, so VBE is already in use — it just asks for mode
0x12. You cannot make a second BIOS call later because the OS is long mode
with no v86, but the Bochs DISPI interface (ports 0x1CE/0x1CF) sets a linear
framebuffer mode with no BIOS involvement. DISPI answered rev 5, PCI BAR0 gave
the aperture, and 1024×768×32 works. The page tables already cover it.

One wrinkle worth knowing: the legacy VGA window at 0xA0000 is the *first 64
KB of video memory*, which in a 1024-wide 32bpp mode is the top sixteen rows.
The window manager keeps repainting its planar screen through it, so it
arrives as a band of hatched garbage along the top of your picture. Setting
DISPI's Y offset so the visible page starts below it is cheaper than fighting
the window manager for the semaphore that gates the refresh.

**The NIC.** An RTL8139, because its whole programming interface is a few
registers and a ring buffer. ARP, IPv4 and UDP on top, polled rather than
interrupt-driven — a turn-based game gains nothing from a handler running at
ring 0 next to the scheduler.

Identity-mapped memory makes the DMA part trivial: `Kernel/Sched.HC` says
memory is *"always fully identity-mapped on all cores"*, so the receive
buffer's pointer goes straight into RBSTART. No translation, no pinning.

The bug that cost the most: **CAPR starts at −16, not 0.** The card treats the
read pointer as sixteen bytes behind the writer, so a ring initialised to zero
tells it the reader is ahead and nothing is ever delivered. The driver found
the card, reset it, read its MAC, and received precisely nothing — which looks
exactly like a dead card.

**Ahead-of-time compilation**, if you try it: `CmpJoin` chains a JIT build onto
the running system's symbol table and an AOT build onto `cmp.asm_hash`, the
assembler symbols. So an AOT compile begins with no C types and rejects a
two-line `U0 Hi(I64 n)` with "Expecting type at I64". The fix is a project
file shaped like `/Compiler/Compiler.PRJ`: `KernelA.HH` for types,
`CompilerA.HH` for the `IC_*` codes the intrinsics are declared with, and
`OPTf_EXTERNS_TO_IMPORTS` so the kernel links by name at load.

And a HolyC one that will get you: **a bare `$` stops the compiler reading the
file.** In a string it is a visible parse error; in a character literal or a
comment it prints nothing, reports success, and every function after it
silently does not exist. I hit it, documented it, and hit it again a thousand
lines later.

TempleOS is public domain, by Terry A. Davis.

[link] [truecolor screenshot] [game screenshot]

---

## 3. Hacker News

**Title:** Show HN: A grand strategy game running natively on TempleOS

Open Doctrines is a grand strategy game I work on. This is it running on
TempleOS — not streamed from a host, not a thin client: the rules, the map and
the interface all execute on the machine.

TempleOS has no C++ compiler, no OpenGL and no network stack, so the port is a
reimplementation of the rules in HolyC with the world baked into a data file.
It carries 1,632 provinces, all 39 actions the desktop game offers, 86
research nodes, 59 policies and a sea map for the fleets.

Two things I did not expect going in:

The sixteen-colour limit is one constant in the boot stub — TempleOS already
calls VBE, it just asks for mode 0x12. The Bochs DISPI registers set a linear
framebuffer with no BIOS call, so it runs at 1024×768 in 32-bit colour.

And the OS decompresses its own source on read, so I had the machine unpack
all 510 of its files and read them from the host. Nearly everything above was
found that way rather than guessed.

What keeps the two versions honest is a check in CI rather than discipline:
the desktop game names its 39 actions, its 8 map views and its 20 policy
levers in C++ source, and a tool reads those lists and fails the build when
the TempleOS side falls behind or a new one appears unclassified. It cannot
port code, but it can make divergence loud.

Source-available, not open source. TempleOS is public domain, by Terry A.
Davis.

---

## 4. r/grandstrategygames

**Title:** I got my strategy game running on TempleOS, an OS with no network
stack and sixteen colours

Reuse the r/TempleOS_Official body, but lead with the *game* and keep the
systems detail to two sentences. This audience wants to know it is a real
grand strategy game — provinces, research, diplomacy, fleets — and that the
port is not a tech demo with a map painted on it. The actions screenshot is
the argument; lead with it rather than the menu.

---

## What not to say

- Do not call it open source.
- Do not claim it runs on real hardware. It needs the DISPI registers, which
  means a VM. Say so before somebody else does.
- Do not ship or imply a prebuilt binary until one is actually attached.
- Do not editorialise about Terry Davis beyond the credit line.
- Do not cross-post the same text; each of these answers a different question.
