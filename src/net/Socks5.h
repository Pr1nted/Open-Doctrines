#pragma once

// Reaching a host through Tor: a SOCKS5 client, and the rule for when to.
//
// WHY TOR AT ALL
//
// Every way of hosting here trusts somebody in the middle -- Cloudflare's
// relay or tunnel, a port forward that shows the host's IP to every player --
// and every way of joining shows the player's IP to the host. Tor removes both
// without a party anybody has to trust: a host can publish an onion service
// (Tunnel.h, provider Tor), and a player can route their connection through
// the Tor network so the host sees a Tor relay, never their address.
//
// It costs speed, and some networks block Tor outright. Both are said wherever
// the option is offered.
//
// HOW
//
// The game does not embed Tor. It talks SOCKS5 to a Tor client already running
// on this machine -- the `tor` service on 9050, or Tor Browser on 9150 -- and
// names the destination by HOSTNAME inside the SOCKS request, so the name is
// resolved by Tor and never by this machine's DNS (which would announce to the
// local resolver which server the player was about to visit).
//
// THE RULE
//
//   .onion       always through Tor; there is no other way to reach one.
//   localhost    never; a development issuer is on this machine.
//   anything else  through Tor when the player chose "hide my IP", else direct.

#include <cstdint>
#include <string>
#include <vector>

namespace socks5 {

/** The player's choice: route everything through Tor, or only onion hosts. */
void setRouteAll(bool all);
bool routeAll();

/** A SOCKS port chosen in settings; 0 tries 9050 and then 9150. */
void setPort(int port);
int  port();

/** Whether a connection to `host` should go through Tor under the current rule. */
bool wanted(const std::string& host);

/** The greeting: version 5, one method, "no authentication". */
std::vector<uint8_t> greeting();

/** CONNECT by domain name (address type 3), so Tor does the resolving. */
std::vector<uint8_t> connectRequest(const std::string& host, uint16_t port);

/**
 * How many bytes of reply to expect after the first five, given those five.
 * -1 when the address type is not one SOCKS5 defines.
 */
int replyRemainder(const uint8_t first5[5]);

/** A reply code as a sentence a player can act on. Codes 0x00-0x08 and Tor's own. */
std::string replyText(uint8_t code);

}  // namespace socks5
