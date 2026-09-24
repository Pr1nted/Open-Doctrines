/* Prove the card works: say something, and hear something back.
 *
 * Two directions matter and they fail differently. Sending proves the driver,
 * the ring and the checksums. Receiving proves the other half of all of it,
 * and is the one that catches a read pointer walking off the ring.
 */
#include "Net"

#define NT_RX 512

U0 NetTest(I64 port = 15000, I64 secs = 12)
{
  U8 msg[64], rx[NT_RX];
  I64 n, tries;

  if (!RtlInit)
  {
    "$$FG,RED$$no RTL8139 -- is the VM started with one?$$FG$$\n";
    return;
  }
  NetReport;
  NetDiag;

  StrCpy(msg, "hello from TempleOS");
  if (!NetSendUdp(net_gw, port, port, msg, StrLen(msg)))
  {
    "$$FG,RED$$send failed: the gateway never answered ARP$$FG$$\n";
    return;
  }
  "sent %d bytes to %d.%d.%d.%d:%d\n",
      StrLen(msg), net_gw[0], net_gw[1], net_gw[2], net_gw[3], port;
  "waiting %d seconds for a reply...\n", secs;

  for (tries = 0; tries < secs * 20; tries++)
  {
    n = NetPollUdp(port, rx, NT_RX - 1);
    if (n > 0)
    {
      rx[n] = 0;
      "$$FG,GREEN$$got %d bytes from %d.%d.%d.%d:%d$$FG$$ '%s'\n",
          n, net_from[0], net_from[1], net_from[2], net_from[3],
          net_fromport, rx;
      return;
    }
    Sleep(50);
  }
  "$$FG,YELLOW$$nothing came back$$FG$$\n";
}
