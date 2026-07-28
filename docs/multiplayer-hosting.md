# Hosting and joining — design

> **Architecture correction, and it invalidates part of what is built.**
>
> Cloudflare is the ACCOUNT SERVICE and nothing else. It issues tokens and join
> tickets; it never carries game traffic. The relay (`LobbyDO` as a WebSocket
> forwarder) was a mistake -- it made Cloudflare the path every packet took,
> which is exactly what this project is not.
>
> Live connections are **direct, with peer-to-peer as a fallback**. A host is
> reachable at an address they arrange; a player who cannot reach them falls
> back to WebRTC hole-punching.
>
> What this costs, stated once and repeated where it matters: **a direct
> connection reveals IP addresses in both directions.** That cannot be fixed
> from our side, because hiding an address means putting a machine in the middle
> and that machine is a relay. `PRIVACY.md` has been corrected accordingly.

## The three constraints, and why one had to give

- no relay
- no port forwarding
- no NAT traversal

Any two are achievable. All three are not, because a relay exists precisely to
avoid the other two. The choice made here: **drop "no NAT traversal"**, keep the
game off third-party infrastructure, and let hosts who can open a port do the
simple thing.

## How a host becomes reachable

1. **Direct** (default). The host listens; players connect to an address. How
   they become reachable is theirs: a forwarded port, a free tunnel
   (Cloudflare Tunnel, playit.gg, Pinggy, ngrok), or a VPS. `ServerBook` already
   takes any address, so a tunnelled hostname needs no code.
2. **Peer-to-peer** (fallback). WebRTC data channels over ICE/STUN, using free
   public STUN servers. Cloudflare carries the signalling handshake only --
   which is matchmaking, not game traffic.

Roughly 10-20% of peers behind symmetric NAT cannot hole-punch and would need
TURN, which is a relay. Those connections fail rather than being silently
relayed, and the player is told to ask the host for a reachable address.

### The game can open the tunnel itself

"Open another terminal and run cloudflared" is exactly the kind of step that
makes hosting feel broken, so the game starts one and shows players the address
— `Tunnel.h`. It picks from what is **already installed**:

| Provider | Needs |
|---|---|
| `cloudflared` | installing once; no account, no signup |
| localhost.run | nothing — it is plain `ssh`, which every Mac, Linux and Windows 10+ box has |

**It will never download anything.** Fetching and executing a binary on a user's
behalf is how a game becomes a malware delivery mechanism, and no amount of
convenience is worth it. With nothing installed, the host is told what to get and
the manual route still works.

Two things stated rather than glossed over. First, **the tunnel operator carries
the traffic** — that is the trade for hiding the host's IP, and a host who would
rather not make it forwards a port instead. Second, this is **not** the relay
this project rejected: the difference is who chooses. A host arranging their own
reachability is not the same as our infrastructure sitting in everybody's path
by default.

The address handed out is `wss://`, because every one of these terminates TLS at
the provider and forwards plaintext to the loopback port — which is exactly why
the server speaks plain `ws://`.

### What the listener binds

`WsServer::listen(port, bindAll)` has two modes, and the default is the narrow
one:

- **Loopback only** (`bindAll` false) — the tunnelled setup, and the one to
  recommend. The tunnel runs on the host's own machine and connects locally;
  nothing else on the network can reach the port at all. It binds **both**
  `127.0.0.1` and `::1`, because they are different addresses that no single
  socket covers, and which one a tunnel dials depends on the tool and on how
  that machine resolves `localhost`. Binding one would make hosting work for
  some people and fail for others with nothing to see in either case.
- **Every interface** (`bindAll` true) — LAN play, or a host who has forwarded a
  port deliberately. One dual-stack socket serves both families.

The server speaks plain `ws://` on purpose; `WsServer.h` explains why, and why
adding TLS here would be undone by the tunnel in front of it.

## What IP exposure actually means

| | Hideable? |
|---|---|
| Host's address, from players | **Yes.** A tunnel in front of the server. Free options exist and the game should recommend them. |
| Player's address, from the host | **No.** Only by a tunnel or VPN the player runs themselves. |

The host chose to host; players are exposed involuntarily. So the protection
that matters more is the one the game cannot provide, which is why the join
screen must say so plainly rather than rely on a policy nobody reads.

The pairwise pseudonym is NOT wasted work -- it still prevents a host learning
which account it is talking to, and still prevents account-level correlation.
What it no longer delivers on its own is unlinkability across servers, because
an IP does that job for anyone who wants it.

## Who is allowed to host

A host must be signed in. `POST /server/register` returns a **server credential**
naming the account that owns it, and opening a session requires both that
credential and a live session token — so a leaked credential alone cannot open a
session in someone else's name.

The relay already records the owner's pseudonym at creation, so only that account
can take the host slot. What is missing is telling PLAYERS. `WELCOME` gains:

```
hostPsid      pseudonym of the host on this server
hostName      display name, or empty
hostBadges    badges, if presented
hostIssuer    who vouched for the above
hostVerified  true only when hostIssuer is the official one
```

**A client refuses to join when the host is not declared.** Not a warning — a
refusal, with a sentence naming why. An undeclared host is either a
misconfiguration or someone hiding, and neither is worth a stranger's evening.
An *anonymous* host is a legitimate case, but it has to be stated as such
(`hostName` empty, `hostVerified` false) so the joining player sees "hosted by
someone who did not sign in" rather than nothing at all.

## Which accounts a server accepts

`SessionSettings.acceptedIssuers` — a server states which account services it
trusts. Empty means the official one only. The relay refuses a ticket from any
other issuer at HELLO, before the host ever sees the peer, and the joining client
is told which issuer was expected.

## Two host modes

| Mode | Has a country | Renders |
|---|---|---|
| `HostPlayer` | yes | yes |
| `Dedicated` | no | no — reuses the `--train-ai` headless path |

CLI: `--host [--dedicated] [--mode=rapid|longform]`.

## Two session states

### Lobby

Open for joining. The host configures, players take countries, nobody advances.

**Country assignment**, `SessionSettings.assignment`:

- `hostAssigns` — the host allocates from the player list.
- `playersPick` — players claim from the map.

Either way **one country per player, enforced by the server**. A claim for a
taken country is refused; the claim is not a client-side decision.

**Swap offers.** A player may offer their country to another. The offer is
routed by the server, the recipient accepts or declines, and the server performs
the exchange atomically — never two independent "set my country" messages, which
could interleave into both players holding one country or neither.

### Game

Joining is closed. A late arrival is refused or admitted as a **spectator**,
per `SessionSettings.lateJoin`. A spectator sees the world and the player list,
holds no country, and its orders are discarded rather than merely ignored.

## Rapid mode

Turn every `turnSeconds`. Clients get `TURN_BEGIN` with a deadline and show a
countdown.

**Disconnecting is normal, not exceptional.** With a long interval a player will
close the game between turns. So:

- Orders are held by the SERVER, keyed by psid, the moment they arrive.
- Reconnecting restores them — the player sees what they already submitted
  rather than an empty order queue they must redo.
- The turn processes on the deadline whether or not everyone is connected.

Ends when the host stops gracefully (back to lobby, then close) or drops past
the relay's grace period.

## Long-form mode

No countdown. The host may be offline for days.

Each turn the host publishes a **turn bundle** (the existing `.odsv` delta) to a
store, and each player publishes their orders back. The host processes whenever
it next runs.

**The store is dumb and pluggable** (`TurnStore`):

- `durableObject` — the session's own storage. **Default.** Needs nothing
  enabled that a working account does not already have: Durable Objects are
  already the relay, include 5 GB of SQLite on the free plan, and free-plan
  accounts are not billed for it.
- `r2` — better quota still (1M writes/month against KV's 1,000/day), but R2
  must be switched on in the dashboard and Cloudflare wants a payment method
  before it will do that, free tier or not. A store that costs somebody a
  billing relationship is not free in the way that matters, so it is not the
  default.
- `jsonblob` — jsonblob.com. Anonymous, no API key, and orders are revisable
  because a blob can be PUT again before the deadline. Verified working.
- `manual` — the game shows text to paste anywhere and a box to paste replies
  back. No infrastructure at all; viable only for a small committed group.

npoint.io was considered and rejected: it no longer issues API keys to new
accounts.

**A long-form session is not torn down when the host leaves.** The relay's
host-gone grace is 90 seconds for a rapid game, because a rapid game without its
server is not a game. For long-form the host being away IS the design, so the
session and its storage persist for 90 days of total silence instead. Getting
this wrong would have deleted a tournament ninety seconds after the host closed
their laptop.

**Confidentiality does not come from the store.** Anyone holding a jsonblob URL
can read and overwrite it. Instead the host publishes a session key in the lobby,
over the authenticated WebSocket, and orders are encrypted and MAC'd to it. The
blob may be world-readable without mattering. A blob failing its MAC is treated
as **no submission** — a case that already has to be handled — so tampering can
destroy a turn but never forge one.

## When a turn is bad

| Case | What happens |
|---|---|
| Orders malformed, or fail their MAC | **The whole submission is discarded.** Server AI plays that country, and every player is told it did and why. |
| Orders valid but some are illegal | Illegal ones dropped, the rest applied. Reported. |
| No submission | Per `SessionSettings.absent`: `ai` plays it, or `idle` leaves it dormant. |

Partial application of a malformed submission is never attempted — half a
player's intent is worse than none, and it is not recoverable into something
they would recognise.

Substitution is always announced. A player must never discover from the map that
something else moved their armies.

## Seeing who is who

- **Player list**, in lobby and in game: name, badges, country, connected,
  orders-submitted. Present in both modes.
- **A "Players" tab** beside the country tabs, present only online, showing
  every country and whether a player or the AI holds it.

Badges from a non-official issuer render differently — see
`badgeIssuerIsOfficial` in `src/net/BadgeStyle.h`, which is an exact match, not
a prefix test.

## Servers are browsed like worlds

`ServerBook` (`src/net/ServerBook.h`) keeps a named list in
`data/servers.json`, added to and renamed like saved worlds. An entry holds a
name you chose, the account service, and the current invite code.

**It holds no credential.** No token, no ticket, no psid. Those live in
`account.json` and nowhere else. The split means the server book can be shared,
synced or committed by accident without handing anyone a session; the two halves
meet only in memory, at the moment of joining, when a ticket is minted for one
session and spent immediately.

An invite code names a SESSION and expires with it, so what persists is the
service plus your name for it. Pasting tonight's code into "Friday game" updates
that entry rather than adding a row every week.

## What is public and what is not

| Direction | Visibility |
|---|---|
| Server publishes the turn bundle (`.odsv`) | **Public and immutable.** Readable by anyone with the link, editable by nobody. |
| Player submits orders | **Encrypted and authenticated.** Unreadable and unforgeable by anyone but the host. |

The public half is what supports spectators: a tournament can be followed
without joining it, and nobody can rewrite a turn that already happened. The
private half is what stops a player reading another's orders before they
resolve.

This is why store choice does not affect confidentiality — see `TurnStore.h`.

## Warnings

`turnStoreWarning()` returns the text for BOTH audiences from one place: what
the host reads when choosing a store, and what every player is shown in the
lobby before the game starts. Written once so they cannot drift — a warning
shown to a host that differs from the one shown to players is how somebody ends
up agreeing to something nobody described to them.

`jsonblob` and `manual` are flagged `requiresConsent()`: they are third-party,
have no uptime guarantee, and choosing one has to be a deliberate act rather
than a default someone clicked past.

## Build order

1. ~~Protocol~~ — done. `NetProtocol.h`.
2. ~~`Session`~~ — done. The client state machine, `Session.h`.
3. ~~Lobby rules and host~~ — done. `Lobby.h` holds every rule and is tested
   without a socket; `Host.h` is transport around it.
4. ~~Turn scheduling and substitution~~ — done. `TurnRunner.h`.
5. **Transport rework.**
   - ~~The server half of RFC 6455~~ — done. `WsServer.h` listens;
     `WsHandshake.h` is the handshake both halves now share.
   - ~~Host-side ticket verification~~ — done. `JoinTicket.h`, on
     `util/Ed25519.h` and `util/Sha512.h`. See below: this is the part of the
     relay's job that could NOT simply be deleted.
   - ~~`Host` and `Session` onto direct addresses~~ — done. The relay envelope
     is gone: every peer has its own socket, so "send to one" is the simple
     case and "send to all" is the loop.
   - ~~Mid-game joining~~ — done. `applyTurnDelta()` was lifted out of
     `replaySaveTurns()`, so a joiner replays the host's turns through the same
     function a save load uses.
   - ~~The turn loop~~ — done. The host resolves; clients submit and wait.
   - ~~Sealed order submissions~~ — done. `TurnSeal.h`, AES-256-GCM.
   - ~~Turn store client~~ — done for `manual` and `jsonblob`; the Cloudflare-hosted
     backends need Worker routes that are **written but not deployed**.
   - ~~Signalling wire format~~ — done. `NetSignal` in `NetProtocol.h`.
   - **Next:** the peer-to-peer transport itself (see below), and `LobbyDO`
     dropping its relay role to carry only signalling and discovery.

### What is stored, and what is sealed

Two directions, two completely different rules:

| | Direction | Rule |
|---|---|---|
| Turn bundle | host → everyone | **public and immutable**. It is the state of a game, not a secret, and publishing it is what lets people spectate. |
| Orders | player → host | **sealed**. Nobody may read or forge them — not the store, not other players, not the host until it opens them. |

Confidentiality never comes from the store, because every backend is a bucket
reached by a URL and on some of them anyone holding that URL can overwrite what
is there. So orders are sealed before they leave the player's machine with
AES-256-GCM, under a key the host hands out over the **authenticated lobby
connection** — never through the store, which would be a store that could read
everything in it.

The turn number and the player's pseudonym are bound in as associated data, so a
sealed submission cannot be replayed as a different turn or attributed to
someone else. A blob that fails to open is treated as **no submission** — a case
the turn logic already handles and announces.

What this deliberately does not buy is availability: somebody who can overwrite
a blob can destroy a turn. They cannot turn it into a different turn, which is
the difference between an outage and a rigged game.

### The peer-to-peer transport

`PeerLink.h` is one interface with two backends, so `Session` and `Host` never
learn which is underneath — what travels over the link is the same framed
`NetProtocol` messages either way.

| | Backend | Ships |
|---|---|---|
| Desktop | libdatachannel — libjuice (ICE), usrsctp, DTLS on the mbedTLS already linked | behind `OD_ENABLE_P2P` |
| Browser | the `RTCPeerConnection` every browser already has | **nothing** |

The joiner offers and the host answers. That is not arbitrary: the offerer
creates the data channel, so a host sitting in a lobby does no work until
somebody actually arrives.

**For the web build this is not a fallback.** A page cannot open a listening
socket and cannot run a tunnel, so WebRTC is the only way a browser can host or
join at all — and it happens to be the one transport that costs zero bytes to
distribute.

Threading is the whole difficulty on the desktop side: libdatachannel calls back
on its own ICE and SCTP threads, and may do so while we are inside one of its
methods. Nothing touches game state from a callback — they append to queues
under a mutex and return, and they never call back into the library, which is
how that would deadlock. The browser side needs none of this, because everything
arrives on the one thread emscripten gives us.

#### Three integration problems worth remembering

libdatachannel does not drop into a FetchContent build cleanly:

1. Its `find_package(MbedTLS)` wants an **installed** mbedTLS. Ours is built
   from source, so at configure time the libraries do not exist yet.
   `cmake/FindMbedTLS.cmake` hands it the **targets** instead, which also keeps
   the dependency edge — pointing it at paths that will exist later would make
   link order a matter of luck.
2. Its `install(EXPORT)` requires mbedTLS to be in an export set. Nothing here
   is installed, so those rules are skipped for that subtree.
3. Its mbedTLS backend references **DTLS-SRTP unconditionally**, even with
   `NO_MEDIA` — the block sits behind `#elif USE_MBEDTLS` rather than the media
   guard. mbedTLS ships that option off, so it is enabled as a `PUBLIC`
   definition, meaning libdatachannel compiles against exactly the configuration
   mbedTLS was built with. A mismatch there links fine and misbehaves at runtime.

### Where the fallback still stands

`OD_ENABLE_P2P` is **off by default** and the transport is **not yet wired**.
What exists is the part that is independent of any ICE library: the `NetSignal`
wire format, tested, and the build integration for libdatachannel configured for
`USE_MBEDTLS` / `NO_MEDIA` / `NO_WEBSOCKET` so it reuses the TLS already linked
rather than dragging in OpenSSL.

The reason it is off rather than on: libdatachannel pulls usrsctp, libjuice and
plog into a build that currently fetches two things, and the direct path works
without any of it. A build with the option off says the fallback is unavailable
instead of failing to compile — the same bargain the mod runtime and the
transport already make.

Signalling goes through the account service, and that is **not** a relay
returning by the back door: it carries a handful of small blobs to arrange a
connection, after which the media path is peer to peer. Matchmaking, not game
traffic.

### How a turn runs

The host resolves turns and **nobody else does**. A client that ran
`processTurn()` locally would compute its own answer to what happened, and the
two machines would quietly diverge — which is the failure this whole design
exists to prevent. So:

1. The host declares a turn (`beginTurn`) with a deadline.
2. Each client submits its orders and waits.
3. The host resolves as soon as everyone has spoken, or when the clock runs out
   — whichever comes first, so nobody sits out a countdown nobody is waiting on.
4. `TurnRunner` decides per seat whether the player's own orders are used or the
   AI stands in, and says **why**; anything substituted is announced.
5. The host runs `processTurn()`, then broadcasts **the delta it actually
   recorded** rather than recomputing the same turn.

Orders are re-attributed on arrival. Every order is checked against what that
peer's country actually owns and dropped otherwise, and `countryId` is taken
from the authenticated seat rather than from the wire. A client may ask for
anything; only orders over its own provinces and ships survive.

### Testing connectivity

Three layers, and each catches things the others cannot:

| | What it proves | Needs |
|---|---|---|
| `tests/connectivity_test.sh` | a real host and a real client agree | node |
| — its `--delay` case | a join survives real-world latency | node |
| `tests/live_smoke_test.sh` | they agree with what is **deployed** | your account |

The stand-in issuer (`tests/mock_issuer.mjs`) generates a throwaway key at
startup, so the loopback tests touch no account and no network. The live one is
deliberately **not** in `run_all.sh`: it needs a real session token, and a test
suite should not fail because the network did.
6. `TurnStore` and the long-form loop, on whatever the host arranges rather than
   on our infrastructure.
7. Screens: lobby, player list, country selection, the Players tab, the host's
   configuration, and the IP-exposure warning at the join screen.

Steps 1-4 are the parts where being wrong is silent. What remains is mostly
wiring and drawing, plus the two store backends.

### How a host knows who is knocking

Cutting Cloudflare out of the connection deleted most of the relay's job, but
not this one: something still has to decide whether a stranger connecting to a
home computer is who they claim to be. That decision moved onto the host.

The host verifies the join ticket itself, and needs **no secret** to do it:

1. It fetches `<issuer>/.well-known/od-keys.json` once and caches the Ed25519
   public key. (That endpoint already existed; nothing on the Worker had to
   change for any of this.)
2. On each connection the host generates a **nonce** and sends it as a
   challenge.
3. The player asks the account service for a ticket bound to that nonce and to
   this session. `/ticket` already takes a caller-supplied nonce, so this needed
   no Worker change either.
4. The host checks the signature, then `iss`, then `aud` **exactly** (a ticket
   for session `ABCD` must not satisfy a check for `ABC`), then the nonce it
   sent on this socket, then freshness, then burns the `jti`.

Two consequences worth stating plainly:

- **A host can verify but never issue.** Verification takes the public key;
  minting takes a private key the host has never seen. So a host cannot forge a
  player, a pseudonym, or a badge.
- **A game survives us being down.** Once the key is cached the host never calls
  home, so an outage of the account service — or its permanent end — stops new
  sign-ins, not games in progress.

The ticket now reaches the host, where the relay used to consume it. That costs
the player nothing: it is still single-session, single-use, 120 seconds, and
carries no account id. `net/src/auth/ticket.ts` explains it from the issuing
side.

### The join, end to end

    player                     host                    account service
      |-- connect ------------->|
      |<- nonce, session, issuer|
      |-- GET /session/<code> ------------------------------->|
      |<- descriptor ----------------------------------------|
      |-- POST /ticket (descriptor + the HOST's nonce) ------>|
      |<- ticket --------------------------------------------|
      |-- ticket -------------->|
      |                         |  verify signature, iss, aud,
      |                         |  nonce, freshness; burn jti
      |<- welcome --------------|

The descriptor comes from the **account service**, not from the host, and that
is not an accident. A ticket's pseudonym is derived from the server it is minted
for, so a malicious host handing over some *other* server's descriptor would be
told the pseudonym that player uses there — precisely the cross-server linkage
the pairwise design exists to prevent. Only the nonce comes from the host.

For the same reason the client refuses a challenge naming an issuer or a session
other than the one the player chose. A host does not get to decide who vouches
for its players.

### Choices made in favour of connecting

A refusal that is technically correct but leaves someone unable to join, with
nothing they can act on, is worse than a slightly wider tolerance. So:

- **The host learns the issuer's clock** rather than trusting its own. A machine
  whose clock is minutes out would otherwise reject every player alive on a
  120-second ticket, and the only symptom would be that nobody can join. This
  removes the cause instead of widening the window: residual tolerance is 30
  seconds, down from the 60 it needed before.
- **A busy port does not stop hosting.** If the configured port is taken the
  host takes any free one — and `listenNote()` says so in words, because a
  forwarded port that moved silently is a game nobody can join for reasons
  nobody can see.
- **Every candidate address is tried**, in order. A host can be reachable by a
  tunnel from outside and only by a LAN address from inside, and a player cannot
  be expected to know which applies to them.
- **The key fetch retries** before giving up, so one dropped request does not
  cost somebody their game.
- **A kick is delivered before the socket closes**, because a kick that arrives
  as a dead socket is indistinguishable from the game crashing.

None of this touches what makes a ticket unforgeable: the signature, `iss`,
exact `aud`, the host's own nonce and the single-use `jti` are all still
required, and the tests refuse a ticket missing any of them.

### A note on the handshake, because it nearly shipped broken

The client and the server each had their own copy of the RFC 6455 magic GUID,
and both copies were mistyped the same way. Our client agreed perfectly with our
server, so nothing looked wrong — and neither could have completed a handshake
with any other WebSocket implementation in the world. It was invisible because
every test compared us against ourselves.

Two things fixed it, and both are worth keeping:

- There is now **one** handshake unit, `WsHandshake.h`, used by both halves. A
  divergence between them is no longer expressible.
- The tests assert the **worked example from the RFC itself**, which is a value
  from outside this codebase. Self-consistency cannot satisfy it.

The same reasoning is why `tests/net_wsserver_test.cpp` drives the server with a
hand-built client rather than with `WebSocket.h`.

## Peer ids are the relay's, not the lobby's

Worth stating because getting it wrong is subtle: the relay issues a peer id per
CONNECTION, and a reconnecting player gets a new one. Identity is the pairwise
pseudonym; the peer id is a routing handle that changes underneath it.

So `Lobby::admit` takes the relay's id rather than inventing a seat number, and
a reconnect updates the handle while keeping the country and the submitted
orders. Any swap offer aimed at the old handle is dropped rather than
re-pointed at whoever next holds that number.
