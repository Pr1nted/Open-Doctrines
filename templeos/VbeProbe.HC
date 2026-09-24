/* Can this machine leave sixteen colours behind?
 *
 * TempleOS asks the BIOS for VBE mode 0x12 -- 640x480, sixteen colours -- in
 * one line of KStart16.HC, and builds its whole graphics stack on the planar
 * VGA layout that mode implies. Changing that line would give true colour and
 * break every other thing the OS draws.
 *
 * The other way in is the Bochs DISPI interface: two I/O ports that set a
 * linear-framebuffer mode with no BIOS call at all, which matters because a
 * VBE BIOS call needs real mode and this OS is long mode with no v86. QEMU,
 * Bochs and VirtualBox all implement it. Real hardware does not.
 *
 * This asks three questions and changes nothing:
 *   1. is the DISPI interface there, and which revision,
 *   2. where is the linear framebuffer, per the video card's PCI BAR0,
 *   3. can this program read that memory without faulting.
 */
#define DISPI_IOPORT_INDEX   0x01CE
#define DISPI_IOPORT_DATA    0x01CF
#define DISPI_INDEX_ID       0
#define DISPI_INDEX_XRES     1
#define DISPI_INDEX_YRES     2
#define DISPI_INDEX_BPP      3

U16 DispiRead(I64 index)
{
  OutU16(DISPI_IOPORT_INDEX, index);
  return InU16(DISPI_IOPORT_DATA);
}

U0 VbeProbe()
{
  I64 bus, dev, fun, id, cls, bar, found;
  U32 vend;

  "\n$$FG,YELLOW$$== DISPI ==$$FG$$\n";
  id = DispiRead(DISPI_INDEX_ID);
  "  ID register: 0x%04X  ", id;
  if (id >= 0xB0C0 && id <= 0xB0C5)
    "$$FG,GREEN$$present (rev %d)$$FG$$\n", id - 0xB0C0;
  else
    "$$FG,RED$$absent -- no runtime mode switch here$$FG$$\n";
  "  current: %dx%d at %d bpp\n",
      DispiRead(DISPI_INDEX_XRES), DispiRead(DISPI_INDEX_YRES),
      DispiRead(DISPI_INDEX_BPP);

  "$$FG,YELLOW$$== the display adapter ==$$FG$$\n";
  found = 0;
  for (bus = 0; bus < 4 && !found; bus++)
    for (dev = 0; dev < 32 && !found; dev++)
      for (fun = 0; fun < 2 && !found; fun++)
      {
        vend = PCIReadU32(bus, dev, fun, 0x00);
        if (vend != 0xFFFFFFFF && vend)
        {
          cls = PCIReadU32(bus, dev, fun, 0x08) >> 16;
          if (cls == 0x0300)      /* class 03, subclass 00: VGA controller */
          {
            bar = PCIReadU32(bus, dev, fun, 0x10);
            "  %02X:%02X.%d  vendor %04X device %04X\n",
                bus, dev, fun, vend & 0xFFFF, vend >> 16;
            "  BAR0 = 0x%08X -> framebuffer at 0x%08X\n", bar, bar & ~0xF;
            found = 1;
          }
        }
      }
  if (!found) "  $$FG,RED$$no VGA-class device found$$FG$$\n";
  "\n";
}
