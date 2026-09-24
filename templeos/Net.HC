/* Networking for TempleOS: an RTL8139 driver and enough IP to be useful.
 *
 * Terry Davis left networking out on purpose, so there is no stack here to
 * plug into -- this is the card, the frames, the addresses and the checksums,
 * all of it, in one file. It exists so Open Doctrines can stop passing turns
 * through a disk image and open a socket like anything else.
 *
 * ── WHY AN RTL8139 ──
 *
 * Because its entire programming interface is a handful of registers and one
 * ring buffer, and you can hold all of it in your head at once. Modern cards
 * want descriptor rings, offload configuration and firmware. This one wants a
 * pointer and a length.
 *
 * ── WHY POLLING, AND NOT INTERRUPTS ──
 *
 * An interrupt handler would have to be registered with the kernel, run at
 * ring 0 alongside the scheduler, and be correct about re-entrancy to avoid
 * taking the machine down. A game that exchanges a few packets per turn gains
 * nothing from any of that. The card is asked whether anything arrived.
 *
 * ── WHAT MAKES THIS SHORT ──
 *
 * TempleOS is "always fully identity-mapped on all cores" (Kernel/Sched.HC),
 * so a pointer IS a physical address. Every other OS spends real effort
 * translating addresses for DMA and pinning pages so they cannot move. Here
 * the buffer's address goes straight into the card's register.
 *
 * ── WHAT THIS IS NOT ──
 *
 * It is not WiFi. 802.11 needs per-chipset firmware, a MAC layer and a WPA2
 * supplicant, and a USB dongle would need a USB stack this OS does not have.
 * It is not TCP either: no retransmission, no ordering, no congestion control.
 * It is ARP, IPv4 and UDP, which is what a turn-based game actually needs.
 */

#define RTL_VENDOR   0x10EC
#define RTL_DEVICE   0x8139

#define RTL_MAC0     0x00
#define RTL_TSD0     0x10       /* transmit status,  four of them  */
#define RTL_TSAD0    0x20       /* transmit address, four of them  */
#define RTL_RBSTART  0x30
#define RTL_CR       0x37
#define RTL_CAPR     0x38
#define RTL_IMR      0x3C
#define RTL_ISR      0x3E
#define RTL_RCR      0x44
#define RTL_CONFIG1  0x52

#define CR_BUFE      0x01       /* receive buffer empty */
#define CR_TE        0x04
#define CR_RE        0x08
#define CR_RST       0x10

#define RX_SIZE      8192       /* plus the 16-byte overrun pad the card wants */
#define RX_PAD       1536
#define TX_SIZE      2048

/* Buffer sizes as names, because HolyC's sizeof takes a TYPE: sizeof(f) on a
 * local array is not a size, it is a parse error reported against a line whose
 * brackets are all balanced. Learned once, in OpenDoc.HC, at some cost. */
#define ARP_FRAME      64
#define TX_FRAME     1536
#define RX_FRAME     1600

I64  rtl_io = 0;                /* I/O base, 0 when there is no card */
U8  *rtl_rx = NULL;
U8  *rtl_tx[4];
I64  rtl_txi = 0;
I64  rtl_rxo = 0;               /* read offset into the ring */
U8   rtl_mac[6];
Bool rtl_up = FALSE;
I64  rtl_last_tsd = 0;          /* for the diagnostic */
I64  rtl_rx_count = 0;

/* Our address and the gateway's. QEMU's user-mode network always hands out
 * 10.0.2.15 and sits on 10.0.2.2, so DHCP buys nothing here and is a protocol
 * this file would rather not also contain. */
U8 net_ip[4]  = {10, 0, 2, 15};
U8 net_gw[4]  = {10, 0, 2, 2};
U8 net_gwmac[6];
Bool net_have_gwmac = FALSE;

/* ── BIG-ENDIAN, BY HAND ──
 *
 * Everything on the wire is most-significant byte first and this machine is
 * least-significant byte first. Writing the shifts out is duller than a
 * byte-swap intrinsic and cannot be wrong about which direction it went. */
U0 Put16(U8 *p, I64 v)
{
  p[0] = v >> 8;
  p[1] = v;
}

I64 Get16(U8 *p)
{
  return (p[0] << 8) + p[1];
}

/* The one's-complement sum every IP header carries. */
I64 NetSum(U8 *p, I64 len, I64 start = 0)
{
  I64 sum = start, i;
  for (i = 0; i + 1 < len; i += 2)
    sum += (p[i] << 8) + p[i + 1];
  if (len & 1)
    sum += p[len - 1] << 8;
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  return (~sum) & 0xFFFF;
}

Bool RtlInit()
{
  I64 bus, dev, fun, cmd, i, spin;
  U32 vend;

  rtl_io = 0;
  for (bus = 0; bus < 4 && !rtl_io; bus++)
    for (dev = 0; dev < 32 && !rtl_io; dev++)
      for (fun = 0; fun < 2 && !rtl_io; fun++)
      {
        vend = PCIReadU32(bus, dev, fun, 0x00);
        if ((vend & 0xFFFF) == RTL_VENDOR && (vend >> 16) == RTL_DEVICE)
        {
          /* Bus mastering and I/O space, or the card will neither answer nor
           * be allowed to touch memory. Both bits, every time: a card that can
           * be addressed but cannot DMA fails in a way that looks like a bug
           * in the ring buffer. */
          cmd = PCIReadU32(bus, dev, fun, 0x04);
          PCIWriteU32(bus, dev, fun, 0x04, cmd | 0x05);
          rtl_io = PCIReadU32(bus, dev, fun, 0x10) & ~0x3;
        }
      }
  if (!rtl_io) return FALSE;

  OutU8(rtl_io + RTL_CONFIG1, 0x00);        /* out of any power-saving state */

  OutU8(rtl_io + RTL_CR, CR_RST);
  spin = 0;
  while (InU8(rtl_io + RTL_CR) & CR_RST && spin < 100000)
    spin++;
  if (spin >= 100000) return FALSE;

  rtl_rx = CAlloc(RX_SIZE + 16 + RX_PAD);
  for (i = 0; i < 4; i++)
    rtl_tx[i] = CAlloc(TX_SIZE);

  /* The pointer is the physical address; see the note at the top. */
  OutU32(rtl_io + RTL_RBSTART, rtl_rx(I64));
  OutU32(rtl_io + RTL_IMR, 0x0005);         /* ROK | TOK, for the ISR bits   */
  OutU32(rtl_io + RTL_RCR, 0x0F | (1 << 7));/* all frames, and wrap the ring */
  /* ── WHERE THE READ POINTER STARTS ──
   *
   * CAPR is "current address of packet read", and the card treats it as 16
   * bytes BEHIND where software is actually reading -- so an empty ring is
   * CAPR = -16, not 0. Leaving it at zero tells the card the reader is 16
   * bytes ahead of the writer, and nothing is ever delivered: the first
   * version of this driver found the card, reset it, read its MAC, and then
   * quietly received nothing at all.
   */
  OutU16(rtl_io + RTL_CAPR, 0xFFF0);
  OutU8(rtl_io + RTL_CR, CR_TE | CR_RE);

  for (i = 0; i < 6; i++)
    rtl_mac[i] = InU8(rtl_io + RTL_MAC0 + i);

  rtl_rxo = 0;
  rtl_txi = 0;
  rtl_up = TRUE;
  return TRUE;
}

U0 RtlSend(U8 *frame, I64 len)
{
  I64 spin = 0;
  if (!rtl_up) return;
  if (len < 60) len = 60;                   /* the wire's minimum frame */
  if (len > TX_SIZE) return;

  MemCpy(rtl_tx[rtl_txi], frame, len);
  OutU32(rtl_io + RTL_TSAD0 + rtl_txi * 4, rtl_tx[rtl_txi](I64));
  OutU32(rtl_io + RTL_TSD0 + rtl_txi * 4, len);

  /* Bounded: a card that never reports the send is a card to give up on, not
   * one to hang the machine over. */
  while (!(InU32(rtl_io + RTL_TSD0 + rtl_txi * 4) & 0x8000) && spin < 1000000)
    spin++;
  rtl_last_tsd = InU32(rtl_io + RTL_TSD0 + rtl_txi * 4);
  OutU16(rtl_io + RTL_ISR, 0x04);           /* acknowledge TOK */
  rtl_txi = (rtl_txi + 1) & 3;
}

/* One frame out of the ring, or 0 if it is empty.
 *
 * The card writes a four-byte header before each frame: status, then length
 * including the CRC. Frames are aligned to four bytes and the ring wraps. */
I64 RtlRecv(U8 *out, I64 max)
{
  I64 len, got, i;
  U8 *p;

  if (!rtl_up) return 0;
  if (InU8(rtl_io + RTL_CR) & CR_BUFE) return 0;

  p = rtl_rx + rtl_rxo;
  len = p[2] + (p[3] << 8);
  if (len < 8 || len > RX_PAD)
  {
    /* Nonsense length: the ring is out of step and chasing it further only
     * corrupts more. Reset the read pointer and carry on. */
    rtl_rxo = 0;
    OutU16(rtl_io + RTL_CAPR, 0xFFF0);
    return 0;
  }
  got = len - 4;                            /* drop the CRC */
  if (got > max) got = max;
  for (i = 0; i < got; i++)
    out[i] = p[4 + i];

  rtl_rxo = (rtl_rxo + len + 4 + 3) & ~3;
  rtl_rxo = rtl_rxo % RX_SIZE;
  OutU16(rtl_io + RTL_CAPR, rtl_rxo - 16);
  OutU16(rtl_io + RTL_ISR, 0x01);           /* acknowledge ROK */
  rtl_rx_count++;
  return got;
}

/* ── ETHERNET ── */
U0 NetFrame(U8 *f, U8 *dstmac, I64 type)
{
  I64 i;
  for (i = 0; i < 6; i++)
  {
    f[i] = dstmac[i];
    f[6 + i] = rtl_mac[i];
  }
  Put16(f + 12, type);
}

U0 NetArpRequest(U8 *ip)
{
  U8 f[ARP_FRAME], bcast[6];
  I64 i;
  MemSet(f, 0, ARP_FRAME);
  for (i = 0; i < 6; i++) bcast[i] = 0xFF;
  NetFrame(f, bcast, 0x0806);
  Put16(f + 14, 1);             /* ethernet */
  Put16(f + 16, 0x0800);        /* ipv4     */
  f[18] = 6;
  f[19] = 4;
  Put16(f + 20, 1);             /* request  */
  for (i = 0; i < 6; i++) f[22 + i] = rtl_mac[i];
  for (i = 0; i < 4; i++) f[28 + i] = net_ip[i];
  for (i = 0; i < 4; i++) f[38 + i] = ip[i];
  RtlSend(f, 42);
}

/* Answer an ARP request for us, and notice a reply we asked for. Both matter:
 * without the first, the other end cannot find us after its cache expires. */
U0 NetArpIn(U8 *f, I64 len)
{
  U8 r[ARP_FRAME];
  I64 i, op;
  if (len < 42) return;
  op = Get16(f + 20);
  if (op == 2)
  {
    if (f[28] == net_gw[0] && f[29] == net_gw[1] &&
        f[30] == net_gw[2] && f[31] == net_gw[3])
    {
      for (i = 0; i < 6; i++) net_gwmac[i] = f[22 + i];
      net_have_gwmac = TRUE;
    }
  }
  else if (op == 1)
  {
    if (f[38] == net_ip[0] && f[39] == net_ip[1] &&
        f[40] == net_ip[2] && f[41] == net_ip[3])
    {
      MemSet(r, 0, ARP_FRAME);
      NetFrame(r, f + 22, 0x0806);
      Put16(r + 14, 1);
      Put16(r + 16, 0x0800);
      r[18] = 6;
      r[19] = 4;
      Put16(r + 20, 2);
      for (i = 0; i < 6; i++) r[22 + i] = rtl_mac[i];
      for (i = 0; i < 4; i++) r[28 + i] = net_ip[i];
      for (i = 0; i < 6; i++) r[32 + i] = f[22 + i];
      for (i = 0; i < 4; i++) r[38 + i] = f[28 + i];
      RtlSend(r, 42);
    }
  }
}

/* Declared before NetSendUdp calls it: HolyC compiles in one pass, top to
 * bottom, so a function must be known before it is used. */
U0 NetPollOnce();

/* ── UDP OUT ── */
Bool NetSendUdp(U8 *dstip, I64 dport, I64 sport, U8 *data, I64 len)
{
  U8 f[TX_FRAME];
  I64 i, iplen, spin;

  if (!rtl_up || len > 1400) return FALSE;

  /* The gateway's hardware address, asked for once and remembered. Everything
   * beyond this subnet goes through it. */
  spin = 0;
  while (!net_have_gwmac && spin < 40)
  {
    if (!(spin % 10)) NetArpRequest(net_gw);
    NetPollOnce;
    Sleep(50);
    spin++;
  }
  if (!net_have_gwmac) return FALSE;

  MemSet(f, 0, TX_FRAME);
  NetFrame(f, net_gwmac, 0x0800);

  iplen = 20 + 8 + len;
  f[14] = 0x45;                 /* v4, 5 words of header */
  Put16(f + 16, iplen);
  Put16(f + 18, 0x1234);        /* id: nothing fragments here */
  f[22] = 64;                   /* ttl   */
  f[23] = 17;                   /* udp   */
  for (i = 0; i < 4; i++)
  {
    f[26 + i] = net_ip[i];
    f[30 + i] = dstip[i];
  }
  Put16(f + 24, NetSum(f + 14, 20));

  Put16(f + 34, sport);
  Put16(f + 36, dport);
  Put16(f + 38, 8 + len);
  /* Checksum left at zero, which IPv4 explicitly permits for UDP and saves
   * carrying the pseudo-header. */
  for (i = 0; i < len; i++)
    f[42 + i] = data[i];

  RtlSend(f, 14 + iplen);
  return TRUE;
}

/* ── ONE TURN OF THE CRANK ──
 *
 * Returns the length of a UDP payload delivered to `port`, or 0. ARP is dealt
 * with here rather than by the caller, because a caller that forgets to
 * answer ARP works until the far end's cache expires and then stops. */
I64 net_sport = 0;
U8  net_from[4];
I64 net_fromport = 0;

I64 NetPollUdp(I64 port, U8 *out, I64 max)
{
  U8 f[RX_FRAME];
  I64 len, ihl, ulen, i;

  len = RtlRecv(f, RX_FRAME);
  if (len < 14) return 0;

  if (Get16(f + 12) == 0x0806)
  {
    NetArpIn(f, len);
    return 0;
  }
  if (Get16(f + 12) != 0x0800) return 0;
  if (len < 34) return 0;
  ihl = (f[14] & 0x0F) * 4;
  if (f[23] != 17) return 0;                /* not UDP */
  if (Get16(f + 14 + ihl + 2) != port) return 0;

  ulen = Get16(f + 14 + ihl + 4);
  if (ulen < 8) return 0;
  ulen -= 8;
  if (ulen > max) ulen = max;
  for (i = 0; i < ulen; i++)
    out[i] = f[14 + ihl + 8 + i];
  for (i = 0; i < 4; i++)
    net_from[i] = f[26 + i];
  net_fromport = Get16(f + 14 + ihl);
  return ulen;
}

/* ARP-only pump, for the wait inside NetSendUdp. */
U0 NetPollOnce()
{
  U8 junk[RX_FRAME];
  NetPollUdp(0, junk, RX_FRAME);
}

/* ── WHEN IT DOES NOT WORK ──
 *
 * Three things can be wrong and they look identical from outside: the card is
 * not sending, the card is not receiving, or the far end has nothing to say.
 * This separates them. TOK means the wire saw the frame; a rising RX count
 * means the ring is delivering; both true and still no ARP reply means the
 * frame itself is malformed.
 */
U0 NetDiag()
{
  I64 i, before;

  if (!rtl_up)
  {
    "$$FG,RED$$no card$$FG$$\n";
    return;
  }
  before = rtl_rx_count;
  NetArpRequest(net_gw);

  "  TSD after send: 0x%08X  ", rtl_last_tsd;
  if (rtl_last_tsd & 0x8000) "$$FG,GREEN$$TOK$$FG$$ ";
  else                       "$$FG,RED$$no TOK$$FG$$ ";
  if (rtl_last_tsd & 0x4000) "$$FG,RED$$UNDERRUN$$FG$$ ";
  "\n";

  for (i = 0; i < 40; i++)
  {
    NetPollOnce;
    Sleep(50);
  }
  "  CR 0x%02X  ISR 0x%04X  frames received %d (was %d)\n",
      InU8(rtl_io + RTL_CR), InU16(rtl_io + RTL_ISR), rtl_rx_count, before;
  if (net_have_gwmac)
  {
    "  $$FG,GREEN$$gateway is$$FG$$ ";
    for (i = 0; i < 6; i++) "%02X", net_gwmac[i];
    "\n";
  }
  else
    "  $$FG,YELLOW$$gateway still unknown$$FG$$\n";
}

U0 NetReport()
{
  I64 i;
  if (!rtl_up)
  {
    "$$FG,RED$$no card$$FG$$\n";
    return;
  }
  "$$FG,GREEN$$RTL8139$$FG$$ at I/O 0x%04X  MAC ", rtl_io;
  for (i = 0; i < 6; i++)
    "%02X", rtl_mac[i];
  "  IP %d.%d.%d.%d\n", net_ip[0], net_ip[1], net_ip[2], net_ip[3];
}
