/* Sixteen colours, left behind: linear-framebuffer modes for TempleOS.
 *
 * Set a linear-framebuffer mode through the Bochs DISPI registers -- no BIOS
 * call, which matters because a VBE BIOS call needs real mode and this OS is
 * long mode with no v86 -- then write 24-bit pixels straight at the aperture
 * the video card's PCI BAR0 points to.
 *
 * TempleOS's own graphics stack knows nothing about this and will draw planar
 * VGA into a screen that is no longer planar, so while this is up the machine
 * belongs entirely to whoever called it. That is the correct arrangement for a
 * full-screen game and the wrong one for anything else, hence the restore.
 */
#define DISPI_IX   0x01CE
#define DISPI_DA   0x01CF
#define DISPI_ENABLED  0x01
#define DISPI_LFB      0x40

U0 DispiWrite(I64 index, I64 val)
{
  OutU16(DISPI_IX, index);
  OutU16(DISPI_DA, val);
}

U0 DispiMode(I64 w, I64 h, I64 bpp)
{
  DispiWrite(4, 0);            /* disable while changing */
  DispiWrite(1, w);
  DispiWrite(2, h);
  DispiWrite(3, bpp);
  DispiWrite(4, DISPI_ENABLED | DISPI_LFB);
}

/* ── WHAT THIS COSTS ──
 *
 * While a DISPI mode is up, TempleOS's own graphics stack is drawing planar
 * VGA into a framebuffer that is no longer planar. Its windows, its menus and
 * its text are all wrong until the mode is put back. That is the correct
 * arrangement for a full-screen game and an unacceptable one for anything
 * else, so a caller that sets a mode owns the machine until it restores.
 *
 * ── AND WHERE IT WORKS ──
 *
 * QEMU, Bochs and VirtualBox implement the DISPI registers. Real hardware
 * does not: on a physical machine the only way into a high-colour mode is the
 * VBE BIOS, which needs real mode, which this OS does not have. GfxAvailable
 * answers that question before anything depends on the answer.
 */
Bool GfxAvailable()
{
  I64 id;
  OutU16(DISPI_IX, 0);
  id = InU16(DISPI_DA);
  return id >= 0xB0C0 && id <= 0xB0C5;
}

/* The linear framebuffer's address, from the video card rather than a guess.
 * 0xFD000000 is what QEMU happens to use today and is not a promise. */
U8 *GfxFrameBuffer()
{
  I64 bus, dev, fun;
  U32 vend;
  for (bus = 0; bus < 4; bus++)
    for (dev = 0; dev < 32; dev++)
      for (fun = 0; fun < 2; fun++)
      {
        vend = PCIReadU32(bus, dev, fun, 0x00);
        if (vend != 0xFFFFFFFF && vend)
          if (PCIReadU32(bus, dev, fun, 0x08) >> 16 == 0x0300)
            return (PCIReadU32(bus, dev, fun, 0x10) & ~0xF)(U8 *);
      }
  return NULL;
}

/* Put the machine back the way TempleOS booted it: 640x480, sixteen colours,
 * planar -- which is what the rest of the OS is still drawing for. */
U0 GfxRestore()
{
  DispiMode(640, 480, 4);
}
