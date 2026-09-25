## The posts

One paragraph each, plain words. Long posts get scrolled past, and on Reddit
the detail belongs in the comments anyway — answer what people actually ask
instead of guessing at all of it in advance. The longer versions below are
material for those replies.

Link everywhere:
https://github.com/Pr1nted/Open-Doctrines/releases/tag/templeos-v1.2.2a

---

**r/TempleOS_Official** — one image: `docs/img/templeos-compare.png`

That collage is the post: TempleOS on the left, every other platform on the
right, in the RTX ON / RTX OFF layout. The format already means "same scene,
one thing changed", so it does the explaining for free -- and the two halves
line up element for element anyway (same four buttons down the side in the
same order, same eight tabs along the bottom, same green Process Turn in the
corner). It makes the point before anyone reads a word, which is what you want
from a single image.

This one presents the thing rather than telling war stories. The people there
know the machine; what they have not seen is a grand strategy game on it. Save
the debugging for the comments, where somebody has asked.

> *Title:* Open Doctrines on TempleOS — a grand strategy game running entirely on the machine

> This is Open Doctrines, a grand strategy game I make, running on TempleOS.
> The map, the rules and the AI all run there. Nothing is sent to another
> computer.
>
> It has 1,632 provinces with population, industry, forts and harbours. You
> raise armies and march them, build industry, research 86 technologies, pass
> policies, sign alliances or declare war, and sail fleets between ports. All
> 39 things you can do in the desktop version are here, and so are all 8 map
> views. It draws at 1024x768 in full colour and uses the same font as the
> desktop build.
>
> Copy the files into `D:/Home` and type `#include "ODGame"` then `ODStart;`.
> It builds in about 1.7 seconds and opens the menu. There is a compiled
> binary in the download too if you would rather not build it. You need QEMU,
> Bochs or VirtualBox — it uses a screen mode real graphics cards do not
> offer.
>
> [link]
>
> Happy to go into how any of it works.
>
> TempleOS is public domain, by Terry A. Davis.

---

**r/osdev** — images: truecolor, game

The OS work is the post here, not the game. A "look what I made" on r/osdev
gets removed; a driver and a mode-set is what that sub is for, and the game is
what it was for. Two findings, not five — the AOT compiler and the HolyC
parser bugs are in the longer version below, for when somebody asks.

> *Title:* Getting 1024x768x32 and an RTL8139 working on TempleOS

> I wanted to run a game on it, so it needed a screen mode and a NIC first.
>
> The colour limit is one line in `KStart16.HC` — it already calls VBE and
> just asks for mode 0x12. You cannot call the BIOS again later, since it runs
> in long mode with no v86, but the Bochs DISPI ports at 0x1CE/0x1CF set a
> linear framebuffer without it: 1024x768x32, aperture from PCI BAR0, and the
> page tables already cover it.
>
> The NIC is an RTL8139 with ARP, IPv4 and UDP over it. Memory is
> identity-mapped, so the receive buffer pointer goes straight into RBSTART
> with no translation and no pinning. The bug that cost me most: CAPR
> starts at -16, not 0. Set it to zero and the card thinks the reader is ahead
> of the writer and hands you nothing, which looks exactly like dead hardware.
>
> [link]
>
> TempleOS is public domain, by Terry A. Davis.

---

**Hacker News** — no images, submit the release URL

> *Title:* Show HN: A grand strategy game running natively on TempleOS

> Nothing is streamed from a host. The map, the rules and the AI all run on
> the machine. TempleOS has no C++ compiler, no OpenGL and no networking, so
> this is the rules rewritten in HolyC with the world as a data file: 1,632
> provinces, 39 actions, 86 technologies, 59 policies, fleets and a sea to
> sail them on. The sixteen-colour limit turned out to be one constant in the
> boot code, and the port carries an RTL8139 driver with ARP, IPv4 and UDP
> because the OS ships without any. The two versions stay in step through CI
> rather than good intentions: the desktop build lists its 39 actions, 8 map
> views and 20 policy levers in C++ source, and a tool reads those lists and
> fails the build when the TempleOS side falls behind or something new shows
> up that nobody has classified. It cannot port code, but it makes the gap
> impossible to miss. Source-available, not open source. TempleOS is public
> domain, by Terry A. Davis.

---

**r/grandstrategygames / r/StrategyGames** — images: actions, game, views,
sail, menu

> *Title (grandstrategy):* My grand strategy game now runs on TempleOS, which has sixteen colours and no networking
> *Title (StrategyGames):* I ported my strategy game to TempleOS

> The whole game is on the machine, not a demo with a map painted on it. 1,632
> provinces with population, industry, forts, garrisons, harbours and
> resources. Everything the PC version lets you do, from recruiting and
> shelling to landing troops from ships and signing trade deals. The real
> 86-node tech tree. 59 policies that actually change your numbers. Unrest, so
> taking land is easier than keeping it. The rule I most wanted to keep
> survived: a province only has room for so many men, and forts shrink that
> further, so past a point extra troops do not fight at all. Ten million
> attackers hit a narrow province as hard as one million; the difference is
> who can afford the losses. TempleOS has sixteen colours and no networking,
> so the port also had to bring a screen mode and a network driver with it,
> and the text is the desktop game's own font copied pixel for pixel. Free,
> runs in a VM: [link]. TempleOS is public domain, by Terry A. Davis.

---

**Lobste.rs** — link, tags `osdev` and `gamedev`, one comment:

> Author here. Two things most likely to be useful to somebody else.
> TempleOS's sixteen-colour limit is one constant in the boot code — it
> already calls VBE and just asks for mode 0x12, and the Bochs DISPI registers
> will set a linear framebuffer with no BIOS call. And an AOT compile chains
> onto `cmp.asm_hash` rather than the running system's symbol table, so it
> starts with no C types and rejects a two-line function until you hand it the
> kernel headers in a project file.

---

# Longer versions

Kept for comment replies, and for anywhere that wants the detail rather than
the pitch.

## Why they are short

Short on purpose. The first drafts of these read as written by a committee:
every one opened with a setup paragraph, every one had a list of three, every
paragraph was the same length. Developers posting about their own work do not
write like that. They state the thing and give the detail.

Keep the specifics -- register names, line numbers, the numbers that were
wrong. That is what makes it credible. Cut the connective tissue.

---

## 1. r/TempleOS_Official

One image only -- that subreddit allows a single attachment. The short
version above is what to post; this is material for the comments.

**Title:** Open Doctrines runs on TempleOS. 1,632 provinces, all 39 actions,
no host.

**Body:**

Rules, map and interface all on the machine. Nothing on the other end of a
bridge.

1,632 provinces, 86 techs, 59 policies, fleets with a sea to cross, 1024x768
in 32-bit colour.

Things the OS made easier than I expected. The compiler does 3,000 lines in
1.7 seconds, so I stopped using a build step. Memory is identity-mapped, so
the NIC's receive buffer pointer goes straight into the card's register. And
it decompresses its own source on read -- I had it unpack all 510 `.HC.Z`
files so I could grep them on the host, and most of what I learned came from
that rather than guessing.

Two I got wrong. The sixteen-colour limit is one line of `KStart16.HC`: it
already calls VBE, it just asks for mode 0x12. And `Except:Drv` out of
`Cd("D:/Home")` is not disk corruption, it is the live CD not mounting hard
drives. I lost an afternoon to that one.

[link] -- copy into `D:/Home`, `#include "ODGame"; ODStart;`. Wants DISPI, so
QEMU, Bochs or VirtualBox.

TempleOS is public domain, by Terry A. Davis.

---

## 2. r/osdev

Gallery: truecolor, navy, game.

**Title:** True colour and a network stack on TempleOS, which has neither

**Body:**

Notes from putting a strategy game on TempleOS.

**Colour.** `KStart16.HC` does `MOV AX,0x4F02 / MOV BX,0x12`. VBE is already
in use, it just asks for 640x480x16. You cannot call the BIOS again later (long
mode, no v86), but the Bochs DISPI ports 0x1CE/0x1CF set a linear framebuffer
with no BIOS at all. Rev 5, aperture from PCI BAR0, 1024x768x32, page tables
already cover it.

Watch the legacy VGA window: 0xA0000 is the first 64 KB of video memory, which
at 1024 wide and 32bpp is your top sixteen rows. The window manager repaints
planar through it and you get a band of hash across the picture. Set DISPI's Y
offset past it; cheaper than fighting for the semaphore that gates the
refresh.

**NIC.** RTL8139 -- a few registers and a ring buffer. ARP, IPv4, UDP, polled.
`Kernel/Sched.HC`: memory is "always fully identity-mapped on all cores", so
the buffer pointer goes straight into RBSTART, no translation or pinning.

**CAPR starts at -16, not 0.** The card treats the read pointer as sixteen
bytes behind the writer. Initialise it to zero and it thinks the reader is
ahead and delivers nothing. Found the card, reset it, read the MAC, received
nothing -- indistinguishable from dead hardware.

**AOT.** `CmpJoin` chains a JIT build onto the running system's symbol table
and an AOT build onto `cmp.asm_hash`. So AOT starts with no C types and
rejects `U0 Hi(I64 n)` with "Expecting type at I64". Give it a project file
like `/Compiler/Compiler.PRJ`: `KernelA.HH`, then `CompilerA.HH` for the
`IC_*` codes the intrinsics use, then `OPTf_EXTERNS_TO_IMPORTS`.

**HolyC:** a bare `$` stops the compiler reading the file. In a string it is a
parse error you can see. In a character literal or a comment it prints
nothing, reports success, and every function after it does not exist. I did it
twice.

**And one I got wrong:** `Except:Drv` from `Cd("D:/Home")` looked like the disk
corruption I had earned by hard-stopping the VM for days. A partition made
minutes earlier by TempleOS's own installer threw the same error. The live CD
does not mount hard drives.

[link]. TempleOS is public domain, by Terry A. Davis.

---

## 3. Hacker News (Show HN)

No images. Submit the release URL, this as the text.

**Title:** Show HN: A grand strategy game running natively on TempleOS

**Text:**

Not streamed from a host, not a thin client. The rules, the map and the
interface all execute on the machine.

TempleOS has no C++ compiler, no OpenGL and no network stack, so this is a
reimplementation of the rules in HolyC with the world as a data file: 1,632
provinces, 39 actions, 86 research nodes, 59 policies, a sea map.

The sixteen-colour limit turned out to be one constant in the boot stub --
TempleOS already calls VBE and asks for mode 0x12. The Bochs DISPI registers
set a linear framebuffer with no BIOS call, hence 1024x768x32. The network
stack is an RTL8139 driver with ARP, IPv4 and UDP, about 380 lines; identity
mapped memory means the receive buffer's pointer goes straight into the card's
register.

The two versions stay honest through CI rather than discipline. The desktop
build names its 39 actions, 8 map views and 20 policy levers in C++ source. A
tool reads those lists and fails the build when the TempleOS side falls behind
or something new turns up unclassified. It cannot port code, but it makes
divergence loud.

Source-available, not open source. TempleOS is public domain, by Terry A.
Davis.

---

## 4. r/grandstrategygames and r/StrategyGames

Gallery: actions first, then game, views, sail, menu. Same body, different
title.

**Title (grandstrategy):** My grand strategy game now runs on TempleOS, which
has sixteen colours and no networking

**Title (StrategyGames):** I ported my strategy game to TempleOS

**Body:**

Not a tech demo with a map painted on it -- the whole game is on the machine.
1,632 provinces with population, industry, forts, garrisons, harbours and
deposits. All 39 actions the PC version has, from recruiting and shelling to
amphibious landings and trade agreements. The real 86-node tech tree with its
real gating. 59 policies that actually pull their levers. Unrest, so conquered
ground costs you to hold.

The rule I most wanted to keep survived: **frontage**. A province fits only so
many men, and forts narrow it further. Past that number extra troops do not
fight at all, so ten million attackers hit a narrow province with the same
force as one million. The difference is who can afford the losses.

TempleOS has sixteen colours and no networking, so the port also carries a
video mode the OS does not normally use and a network driver. The text is
raylib's bitmap font baked glyph for glyph, so it reads the same as the
desktop build.

Free, runs in a VM: [link]

TempleOS is public domain, by Terry A. Davis.

---

## 5. Lobste.rs

Link, tags `osdev` and `gamedev`, one comment:

> Author here. Two bits most likely to be useful: TempleOS's sixteen-colour
> limit is one constant in the boot stub -- it already calls VBE and asks for
> mode 0x12, and the Bochs DISPI registers set a linear framebuffer with no
> BIOS call. And an AOT compile chains onto `cmp.asm_hash` rather than the
> running system's symbol table, so it starts with no C types and rejects a
> two-line function until you hand it the kernel headers in a project file.

---

## What not to say

- Do not call it open source.
- Do not claim it runs on real hardware. It needs the DISPI registers, which
  means a VM. Say so before somebody else does.
- Do not ship or imply a prebuilt binary until one is actually attached.
- Do not editorialise about Terry Davis beyond the credit line.
- Do not cross-post the same text; each of these answers a different question.
