# Multiplayer

Status: **built and playable.** Hosting, joining, seats, turns, disconnects and
reconnects all work; `tools/playtest.sh --verify` checks them unattended. The
list at the bottom says what is not built.

## The shape of it

**Players dial out. The host listens.**

```
  player ─┐
  player ─┼──wss──▶  tunnel (cloudflared)  ──▶  host :27015  ── authoritative game
  spectator ┘                                        │
                                                     │ verifies join tickets,
                                                     │ stamps peer identity
                                                     ▼
                              Worker + KV: accounts, nicknames, badges, tokens
```

A joiner needs no router configuration and can be a browser tab. A host binds a
socket (`NetHost::open` -> `WsServer::listen`) and is reachable either by a
forwarded port, a LAN address, or — the intended path — a tunnel the game can
install and open for itself, so the host does not configure anything either.
See [multiplayer-hosting.md](multiplayer-hosting.md).

**Hosting therefore needs a desktop build.** A browser cannot accept an inbound
connection, so the web build can join a game and cannot start one.

### The relay that is not wired up

`net/src/lobby/LobbyDO.ts` implements a different and better shape — everyone,
including the host, dialling **out** to a Durable Object that forwards bytes
between them. Its header still describes the game that way. That relay is
deployed and works; the C++ host simply never adopted it and listens instead.

The difference is worth knowing before reading either side:

- the tunnel exists **because** the host listens. Adopting the relay would
  remove the cloudflared install path from hosting on every platform.
- browser hosting is impossible under the current shape and straightforward
  under the relay's, because `WebSocket` — the class `Session` already uses,
  and which `WsWeb.cpp` already implements for browsers — is all a relay host
  would need.

Whichever is used, the relay is deliberately stupid: it never parses a game
payload. Game rules live in the C++ host, in one place, where they are
authoritative.

## What stops cheating

Not mod verification. **Authority.**

- State only ever flows server → client. A client applies turn deltas; it never
  computes a turn. Whatever a mod does to a client's copy of the world is
  overwritten by the next delta.
- Orders are validated server-side and **re-attributed to the authenticated
  peer's country**. The `countryId` on the wire is discarded. That one rule
  removes the entire "issue orders for someone else" class.
- A `"side": "client"` mod has `GameState.Write` and `GameProcess` masked off
  its grants for the whole session, so on a client the capability is absent
  rather than merely useless.
- The server never asks a client to compute game state. Even a `"both"`-side
  mod runs authoritatively on the server and decoratively on the client.

**Mod matching is an integrity check, not an anti-tamper one**, and the UI says
so in those words. A client is a program on hardware its owner controls; it can
report any mod list it likes, and no open-source game changes that. What the
check catches is the failure that actually happens — a wrong version, a
truncated download, a mod someone edited and forgot to rebuild. A client that
lies desyncs its own display and gains nothing.

See `src/net/ModAttest.h`.

## What a server operator learns about a player

A pseudonym, a display name, and optionally badges. Nothing else.

```
psid = HMAC(server-side secret, accountId + ":" + serverId)
```

Stable on one server forever, so a host can keep stats and enforce bans.
Unrelated on any other server, so two operators comparing logs learn nothing.
Not reversible without the key, which is a Worker secret.

The host is never handed a credential at all: the relay consumes the join
ticket and forwards a *statement* about the peer. A ticket is bound to one
session, lives 120 seconds, is single-use, and has the wrong audience for the
account API — so even a captured one is worth nothing.

The caveat, which the game says out loud: **a global nickname defeats this by
itself.** Two operators can just compare display names, and a rare badge
correlates as well as a name. That is why there is a per-server alias and a
client-side "don't present badges" choice.

Full detail: `net/PRIVACY.md`, and `net/README.md` for deployment.

## Reuse, not reinvention

Three things already in the codebase carry most of the weight:

| Existing thing | Used as |
|---|---|
| `.odsv` per-turn binary deltas (`src/SaveManager.h`) | the state-sync payload, verbatim. No second serializer. |
| `Pending*` order structs (`src/GameStructs.h`) | the client → server orders message |
| The Gearbox capability bitmask (`src/mods/ModPackage.h`) | the client/server mod split, as a mask over the same word the Advanced panel edits |

## Layout

```
net/                    Cloudflare Worker + relay (TypeScript)
  src/auth/             OAuth device flow, tokens, join tickets, pairwise psid
  src/accounts/         KV store, nickname filter, badges, GDPR export/delete
  src/lobby/            LobbyDO relay, session descriptors
  PRIVACY.md            served at /privacy
src/net/
  NetProtocol.h/.cpp    binary wire format. No raylib, no game headers.
  WebSocket.h           transport interface
  WsNative.cpp          desktop: mbedTLS + RFC 6455 framing we own
  WsWeb.cpp             browser: emscripten_websocket_*
  WsUnavailable.cpp     -DOD_ENABLE_NET=OFF builds
  ModAttest.h/.cpp      mod-set comparison
src/util/Sha256.h       shared by the updater and the mod system
```

`OD_ENABLE_NET` is optional the same way `OD_ENABLE_MODS` is: a machine that
cannot fetch mbedTLS still builds, and the multiplayer menu reports that this
build cannot connect rather than the build failing.

## Turn modes

- **Rapid** — a turn is processed every `turnSeconds`. Graceful stop returns
  everyone to the lobby and closes the session; a host that drops ends it after
  the relay's grace period.
- **Long-form** — no countdown, and the host is expected to be away between
  turns. Each resolved turn is published to a store (`TurnStore`) and players
  submit orders back, sealed with a session key the host hands out in the lobby,
  so a tournament survives everyone being offline. Deltas are KB-scale.

  Both sides work with nothing connected: the host collects orders from the
  store when it next runs, and a player who reopens the game reads the details
  back from `<save>.odjoin` and catches up. A submission that will not open is
  **no submission** — never partly applied — which is `NetSubstitution::Malformed`,
  the same case a corrupt socket submission produces.

  Built and unit-tested; not yet played through a real multi-day campaign. See
  the Status section of the root README for what that means.

## Tests

```bash
tests/run_all.sh build        # C++
cd net && npm test            # Worker and relay
```

`net/test/privacy.test.ts` asserts the claims `PRIVACY.md` makes — that a
ticket cannot call the account API, that pseudonyms do not correlate across
servers, that an export is complete and a deletion leaves nothing behind. If
one of those fails, the published policy has become inaccurate, which is a
worse bug than a crash.

## Accounts, in the game

Main menu > **Account**.

- Sign in with Google, Discord or GitHub. The game opens a browser and polls;
  the secret it polls with never appears in the URL the browser visited.
- First sign-in asks for a nickname. It can be changed once every 7 days.
- Delete is two steps: the server says what will go, what lingers and what it
  cannot reach, and the game prints that verbatim rather than paraphrasing it.

The client half is `src/net/AccountClient.h` (non-blocking, raylib-free) over
`src/net/HttpClient.h`, with the screen in `src/Game_Account.cpp`. Point it at a
deployment by setting `accountIssuer` in `data/config.json`; empty means this
build offers no sign-in, which the screen says rather than failing at a request.

The session token is stored at `<dataDir>/account.json`, mode 0600. It is not in
the OS keychain, and the header says so plainly: anyone who can run code as this
user can read it. What it can do is bounded -- rename, link, unlink, delete. It
cannot join a game as you, because joining needs a per-session ticket.

## Built

- Cloudflare Worker: OAuth device flow (Google, Discord, GitHub), account
  linking, nickname filter with homoglyph and leetspeak folding, badges, GDPR
  export and deletion, Ed25519 session tokens and join tickets, pairwise
  pseudonyms. 76 tests.
- `LobbyDO`: WebSocket hibernation, ticket verification with nonce and
  single-use `jti`, peer identity stamping, host-only broadcast, rate limits,
  host-drop grace.
- C++ wire format with a fail-closed reader, and the WebSocket transport on all
  three backends. 58 tests.
- Mod `side` field, grant masking, archive digests, attestation comparison, and
  `net_role` / `gearbox_is_client()` through the ABI to every SDK. 40 tests.

## Hosting

Designed in full in [multiplayer-hosting.md](multiplayer-hosting.md) — host
authorisation, lobby, country selection and swaps, the two turn modes, and what
happens to a bad or absent submission. All of it is built except the dedicated
server (below); host from the main menu under **Multiplayer > Host a game**.

## Playing it

Main menu > **Multiplayer**. The host starts a session and the lobby shows an
address and a code; joiners paste both. Countries are picked in the lobby, the
host starts the game, and from then on the map is the game — the host console
is on Esc.

To try it on one machine, `tools/playtest.sh` runs four clients as four
different players against a local dev issuer, and `tools/playtest.sh --verify`
does the same run unattended and exits non-zero if anything is wrong. Neither
needs a second account or a real service. See
[multiplayer-testing.md](multiplayer-testing.md).

## Not built yet

The session, the orders, the lobby, the turn loop and the menu are all built —
what remains is one thing and two shapes that landed differently from the plan.

- **Dedicated server** — `--host [--dedicated] [--mode=rapid|longform]` is
  designed in [multiplayer-hosting.md](multiplayer-hosting.md) and not
  implemented. Hosting today means a running copy of the game with a window; a
  host that closes it ends the session. The headless path `--train-ai` and
  `--simulate` use is what this would reuse.
- **A long-form campaign played for real** — the mode is built and tested, but
  nobody has yet run one across two machines and several days. See *Turn modes*
  above.

Two things in this document's history are worth recording, because the code
does not match the plan and the plan is what a reader will look for:

- There is no `NetOrders` translation unit. Encoding and decoding the
  `m_pending*` vectors turned out to be inseparable from the ownership checks
  that guard them, so both live in `Game_Multiplayer.cpp`
  (`mpSerializeOrders` / `mpApplyOrders`) — the same pass that reads an order
  is the one that decides the sender owns the province it names.
- There is no `NetRole` enum. A peer's role is which object it holds —
  `m_netHost` or `m_netSession` — surfaced as `mpIsHost()` / `mpIsClient()`.
