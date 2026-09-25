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

## 1. r/TempleOS_Official  — FINAL, paste as-is

Post as an **image gallery** in this order: menu, game, actions, navy,
truecolor. The menu is the hook — it is instantly legible as a game and
instantly legible as TempleOS.

**Title:**

    Open Doctrines runs natively on TempleOS — 1,632 provinces, all 39 actions, no host

**Body:**

I have spent a while getting a grand strategy game running properly on
TempleOS, and it now does the whole job on the machine: the rules, the map and
the interface, with nothing on the other side of a bridge.

What it carries:

- 1,632 provinces with adjacency, population, armies, harbours and deposits
- all 39 actions the desktop version offers — war, economy, politics, navy
- 86 research nodes and 59 policies, taken from the desktop version's own tables
- a sea map, so fleets sail between harbours across open water
- 1024×768 in 32-bit colour

Three things the OS deserves credit for, because each made this easier than it
would have been anywhere else:

**The compiler is genuinely fast.** Three thousand lines of HolyC compile in
about 1.7 seconds on an emulated machine. For most of this work I stopped
bothering with a build step at all — `#include` and it is running.

**Memory is identity-mapped**, so a pointer *is* a physical address. The
network driver writes its receive buffer's address straight into the card's
register. No translation, no page pinning, none of the ceremony that usually
surrounds DMA. I have written that driver on other systems and this was the
short version.

**It decompresses its own source on read.** I had the machine unpack all 510
of its `.HC.Z` files so I could read them from outside, and after that almost
every question was a grep rather than a guess. The video mode, the loader, the
colour depth — all answered by the source that ships with the OS.

Two things I got wrong, in case they save somebody else the time:

The sixteen-colour limit is one line of `KStart16.HC` — it already calls VBE,
it just asks for mode 0x12. The Bochs DISPI registers set a linear framebuffer
with no BIOS call, which is how it ends up at 1024×768.

And when `Cd("D:/Home")` threw `Except:Drv` I spent an afternoon convinced I
had corrupted the disk. I had not. The live CD does not mount hard drives;
`Mount;` prints a drive list with no hard drive in it. Six prompts and it was
fine.

Release (source and the compiled binary): [link]

Copy the files into `D:/Home` and `#include "ODGame"; ODStart;` — or load the
binary, which is in the README. It wants a linear-framebuffer mode through the
DISPI registers, so QEMU, Bochs or VirtualBox rather than metal.

Happy to answer anything about the internals.

TempleOS is public domain, by Terry A. Davis.

---

## 2. r/osdev  — FINAL, paste as-is

Images: truecolor, navy, game. This audience wants the framebuffer and the
driver, not the map.

**Title:**

    Getting true colour and a network stack onto TempleOS, which ships with neither

**Body:**

TempleOS boots into 640×480 with sixteen colours and has no networking at all.
I wanted to run a strategy game on it, so it needed both. Notes, in case any
of this is useful to somebody else.

**The colour depth is one constant.** `KStart16.HC` does:

    MOV AX,0x4F02
    MOV BX,0x12     //640x480 16 color

That is a VBE Set Mode call, so VBE is already in use — it just asks for mode
0x12. You cannot make a second BIOS call later, because the OS is long mode
with no v86. But the Bochs DISPI interface (ports 0x1CE/0x1CF) sets a linear
framebuffer mode with no BIOS involvement at all. DISPI answered rev 5, PCI
BAR0 gave the aperture, and 1024×768×32 works. The page tables already cover
it.

One wrinkle: the legacy VGA window at 0xA0000 is the *first 64 KB of video
memory*, which in a 1024-wide 32bpp mode is the top sixteen rows. The window
manager keeps repainting its planar screen through it, so it arrives as a band
of hatched garbage across the top of your picture. Setting DISPI's Y offset so
the visible page starts below it is cheaper than fighting the window manager
for the semaphore that gates the refresh.

**The NIC.** An RTL8139, because its whole programming interface is a few
registers and a ring buffer. ARP, IPv4 and UDP on top, polled rather than
interrupt-driven — a turn-based game gains nothing from a handler running at
ring 0 beside the scheduler.

Identity-mapped memory makes the DMA part trivial. `Kernel/Sched.HC` says
memory is *"always fully identity-mapped on all cores"*, so the receive
buffer's pointer goes straight into RBSTART. No translation, no pinning.

The bug that cost most: **CAPR starts at −16, not 0.** The card treats the read
pointer as sixteen bytes behind the writer, so a ring initialised to zero tells
it the reader is ahead and nothing is ever delivered. The driver found the
card, reset it, read its MAC, and received precisely nothing — which looks
exactly like a dead card.

**Ahead-of-time compilation**, if you try it. `CmpJoin` chains a JIT build onto
the running system's symbol table and an AOT build onto `cmp.asm_hash`, the
assembler symbols. So an AOT compile begins with no C types at all and rejects
a two-line `U0 Hi(I64 n)` with "Expecting type at I64" — not your code, a build
with no headers. The fix is a project file shaped like
`/Compiler/Compiler.PRJ`: `KernelA.HH` for the types, `CompilerA.HH` for the
`IC_*` codes the intrinsics are declared with, and `OPTf_EXTERNS_TO_IMPORTS`
so the kernel links by name at load rather than being compiled in.

**A HolyC one that will get you:** a bare `$` stops the compiler reading the
file. In a string it is a visible parse error; in a character literal or a
COMMENT it prints nothing, reports success, and every function after it
silently does not exist. I hit it, wrote a comment about it in the file it
happened in, and then hit it again a thousand lines later, where it looked
like a function had lost its parameters.

**And one I got wrong.** `Cd("D:/Home")` threw `Except:Drv` and I spent an
afternoon sure I had corrupted the disk — I had been hard-stopping the VM
mid-write for days, so it fitted. A partition created minutes earlier by
TempleOS's own installer threw the same error. The live CD simply does not
mount hard drives; `Mount;` prints a drive list with no hard drive in it.

Source and binary: [link]

TempleOS is public domain, by Terry A. Davis.

---

## 3. Hacker News (Show HN)

No images — HN has no image support, and the linked release carries them.
Submit the GitHub release as the URL and this as the text.

**Title:**

    Show HN: A grand strategy game running natively on TempleOS

**Text:**

Open Doctrines is a grand strategy game I work on. This is it running on
TempleOS — not streamed from a host, not a thin client: the rules, the map and
the interface all execute on the machine.

TempleOS has no C++ compiler, no OpenGL and no network stack, so the port is a
reimplementation of the rules in HolyC with the world baked into a data file.
It carries 1,632 provinces, all 39 actions the desktop version offers, 86
research nodes, 59 policies, and a sea map for the fleets.

Three things I did not expect going in.

The sixteen-colour limit is one constant in the boot stub — TempleOS already
calls VBE, it just asks for mode 0x12. The Bochs DISPI registers set a linear
framebuffer with no BIOS call, so it runs at 1024×768 in 32-bit colour.

It has no networking, so the port carries an RTL8139 driver with ARP, IPv4 and
UDP, about 380 lines. Identity-mapped memory means a pointer is a physical
address, so the receive buffer's pointer goes straight into the card's
register — none of the usual DMA ceremony.

And the OS decompresses its own source on read, so I had the machine unpack
all 510 of its files and read them from the host. Nearly everything above was
found that way rather than guessed.

What keeps the two versions honest is a check in CI rather than discipline.
The desktop version names its 39 actions, its 8 map views and its 20 policy
levers in C++ source; a tool reads those lists and fails the build when the
TempleOS side falls behind, or when a new one appears that nobody has
classified. It cannot port code, but it can make divergence loud.

Source-available, not open source. TempleOS is public domain, by Terry A.
Davis.

---

## 4. r/grandstrategygames and r/StrategyGames

Images: actions FIRST (it is the argument), then game, views, sail, menu.
Same body both subs; only the title changes.

**Title (r/grandstrategygames):**

    I got my grand strategy game running on TempleOS — an OS with 16 colours and no networking

**Title (r/StrategyGames):**

    My strategy game now runs on TempleOS, an operating system with no network stack and sixteen colours

**Body:**

Open Doctrines is a grand strategy game I have been building. It also now runs
on TempleOS — the hobby operating system written from scratch in its own
language — and not as a screenshot-and-a-prayer tech demo. The whole game is
on the machine.

What made it over:

- 1,632 provinces, each with population, industry, fortification, garrisons,
  harbours and resource deposits
- all 39 actions the PC version has: recruit, march, assault, shell, declare
  war, sue for peace, build industry and forts, raise harbours, lay down
  hulls, embark troops, land them, engage fleets at sea, sign alliances,
  pacts, guarantees and trade agreements
- 86 technologies on the real tech tree, with the real dependency gating
- 59 policies, which actually pull their levers — population growth,
  conscription rates and costs, army attack and defence, maintenance
- an unrest and alignment model, so conquered ground costs you to hold
- fleets that sail across a real sea map rather than teleporting

The combat is the same rule the PC version uses, which is the bit I was most
keen to keep: **frontage**. A province fits only so many men, narrowed further
by fortification, and above that number extra troops do not fight. Ten million
attackers and one million attackers hit a narrow province with the same force
— the difference is only who can afford the losses. It is the rule that stops
grand strategy becoming a spreadsheet race, and it survived the port intact.

The port had to solve some things the PC version never has to think about.
TempleOS has sixteen colours, so the map is drawn at 1024×768 in 32-bit colour
through a video mode the OS does not normally use. It has no networking, so
there is now a network driver. And the text is raylib's bitmap font, baked
glyph for glyph, so it is the same typeface as the desktop build.

Free, runs in a VM: [link]

TempleOS is public domain, by Terry A. Davis.

---

## 5. Lobste.rs

Link to the GitHub release, tagged `osdev` and `gamedev`. Lobste.rs dislikes
promotional bodies; submit the link and add one authored comment:

> Author here. The two findings most likely to be useful to somebody else:
> TempleOS's sixteen-colour limit is one constant in the boot stub — it
> already calls VBE and just asks for mode 0x12, and the Bochs DISPI
> registers will set a linear framebuffer with no BIOS call. And an AOT
> compile chains its symbol table onto `cmp.asm_hash` rather than the running
> system's, so it starts with no C types and rejects a two-line function until
> you give it a project file with the kernel headers. Happy to answer
> anything.

---

## What not to say

- Do not call it open source.
- Do not claim it runs on real hardware. It needs the DISPI registers, which
  means a VM. Say so before somebody else does.
- Do not ship or imply a prebuilt binary until one is actually attached.
- Do not editorialise about Terry Davis beyond the credit line.
- Do not cross-post the same text; each of these answers a different question.
