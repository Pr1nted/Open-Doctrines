# Multiplayer

Status: **infrastructure landed, game integration outstanding.** What is built is
built properly and tested; the list at the bottom says what is not.

## The shape of it

Everyone dials **out** over `wss://` to a Cloudflare Durable Object, including
the host. There is no listening port on anybody's machine, so there is nothing
to forward: no UPnP, no NAT punchthrough, no router configuration, and it works
from a browser tab.

```
  player ─┐
  player ─┼──wss──▶  LobbyDO (relay)  ◀──wss──  host  ── authoritative game
  spectator ┘             │
                          │ verifies join tickets, stamps peer identity,
                          │ forwards bytes. Never parses a game payload.
                          ▼
                   Worker + KV: accounts, nicknames, badges, tokens
```

The relay is deliberately stupid. Game rules live in the C++ host, in one
place, where they are authoritative.

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
- **Long-form** — designed, not built. The server publishes each turn's delta
  as a paste and players submit orders back as their own paste id, so a
  tournament survives everyone being offline. Deltas are KB-scale, so they fit
  inside free paste size limits.

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
happens to a bad or absent submission. Step 1 of that build order (the protocol)
is done; the rest is listed there.

## Not built yet

The game does not yet join a game. Remaining, roughly in order:

1. **`Session`** — the state machine that ties transport, protocol and
   attestation together: fetch session info, mint a ticket, connect, HELLO,
   handle WELCOME/REJECT, heartbeat, reconnect. The HTTPS client it needs is
   built (`src/net/HttpClient.h`).
2. **`NetOrders`** — encode and decode the `m_pending*` vectors. Needs
   `GameStructs.h`, which is why it is separate from `NetProtocol`.
3. **`Game` wiring** — `NetRole`; skip `processTurn()` on a client and apply
   deltas instead; never construct `m_ai` on a client; force order attribution
   on the server.
4. **Host modes** — `--host [--dedicated]`, reusing the existing headless path
   built for `--train-ai`.
5. **Rapid turn loop** and the multiplayer menu, replacing the
   "Multiplayer not yet available" stub in `Game_Menus.cpp`.
