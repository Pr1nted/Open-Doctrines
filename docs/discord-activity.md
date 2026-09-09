# OpenDoctrines as a Discord Activity — scope

An Activity is a web app Discord runs in an iframe inside the client, launched
from a voice channel. Players click it and are in the game: no download, no
install, and — the part that answers "can it update itself" — **whatever is
served is what everybody runs**, the moment it is deployed.

This is a scope, not a plan of record. Numbers below are measured from this
repository on 2026-09-09; the unknowns are named as unknowns.

## What already fits

Most of the risk in this kind of port is in places where this game is already
in the right shape, which is why it is worth doing at all.

| Constraint | Where we stand |
| --- | --- |
| Must be a web app in an iframe | The web build exists and **already ships to itch.io, which serves it in an iframe**. Iframe embedding is not new ground. |
| WebSockets allowed, **WebRTC is not** | Multiplayer uses WebSocket (`WsWeb.cpp`). P2P/WebRTC is compiled out on web already (`OD_ENABLE_P2P AND NOT EMSCRIPTEN`). Nothing to remove. |
| All traffic proxied; unmapped hosts fail `blocked:csp` | The account service **and** the multiplayer relay are the same host — `/session/<code>/ws` is a route on the same Worker. **One URL mapping covers everything.** |
| Cross-origin requests | The Worker already sends CORS on every response (`withCors`). |
| No threads in the browser | Already true and already handled: the LLM work moved to an ASYNCIFY queue (`util/Async.h`). |

## What has to be built

### 1. Hosting (the actual prerequisite)
The build must be served over HTTPS at a stable origin. Today CI produces
`build-web/` and uploads it to itch.io; nothing publishes it to a host we
control. Cloudflare Pages is the obvious choice — the account service is
already a Worker on the same account, and it keeps the mapping list at one
entry.

### 2. The Embedded App SDK layer (`shell.html`)
A JavaScript prologue that runs **before** the wasm starts:
- `DiscordSDK.ready()`, then `patchUrlMappings()` so the C++ code's existing
  absolute URLs resolve through the proxy without the C++ knowing.
- `authorize` → `authenticate`, exchanging a code for a token through a new
  endpoint on the Worker.
- Hand the resulting identity to the game.

`shell.html` already exists (19 KB) and is passed to the linker via
`--shell-file`, so there is a place for this to live.

### 3. Auth, which is the one genuinely new server piece
Inside an Activity the normal OAuth redirect cannot run — there is no browser
to redirect. The SDK returns a code that must be exchanged **server side** for
a token, using the application's client secret. That is a new route on the
existing Worker (`POST /activity/auth`) plus a decision we have not made: does
a Discord Activity player get a full OpenDoctrines account, or a session that
exists only for that game? The account system already has the linking
machinery from the Twitch/YouTube/Kick work, so either is reachable.

### 4. Multiplayer across surfaces
`kNetProtocolVersion` already refuses a mismatch at HELLO with a message naming
both versions, so a stale desktop client and a freshly-deployed Activity fail
*legibly* rather than mysteriously. But the Activity updates instantly and
desktop players update when they choose, so **protocol changes become a
compatibility question in a way they are not today.** That is a policy
decision, not a code change.

### 5. Input and shape
Activities run on mobile as well as desktop. `Touch.h` exists and the menu
already reflows for narrow screens (there is width-fitting code with a comment
about a phone held upright). Unmeasured in an actual Discord mobile frame.

## The risks, with numbers

**Payload — the big one.** Today's web build is **21 MB**:

    OpenDoctrines.wasm   10.4 MB
    OpenDoctrines.data   10.0 MB
    OpenDoctrines.js      0.4 MB

That is a download before anything is on screen, in a context where people
click expecting a game in seconds. It is survivable on itch.io, where somebody
has chosen to visit a page; it is a different proposition in a voice channel.
Mitigations exist — the `.data` bundle is mostly maps (5.4 MB of `.odmap` on
disk) and could be fetched on demand rather than preloaded — but **this is the
thing most likely to decide whether the Activity is good or merely possible**,
and it should be measured on a real connection before anything else is built.

**Distribution.** Activities are open to all developers, and a *Developer
Activity Shelf* (Discord Settings → Advanced) exists for testing during
development. Being discoverable in the App Directory needs verification and
Discovery enabled. So: testable immediately, publicly listed later.

**The proxy is not a normal network.** Every request is rewritten. Anything
that builds a URL at runtime — and the C++ builds several from
`accountIssuer` — has to survive that. `patchUrlMappings` is designed for
exactly this, but it is the part most likely to produce a `blocked:csp` that
only appears inside Discord and not in any local test.

## What I would do first, in order

1. **Measure the load.** Serve today's `build-web/` over HTTPS and open it as
   an Activity via the Developer Activity Shelf. No SDK, no auth — just find
   out what 21 MB feels like in the frame, on desktop and on a phone. This
   answers the question that decides everything else, and costs a hosting
   setup rather than a port.
2. Add the SDK prologue and `patchUrlMappings`; confirm the account service and
   a multiplayer session both work through the proxy.
3. Then auth, and only then the account-model decision.

Step 1 is deliberately not a commitment to the rest.

## Estimate

Rough, and honest about which parts are guesses:

- Hosting + step 1 measurement: **small** — mostly CI and a Pages project.
- SDK prologue + URL mappings: **small to medium**, with the caveat that proxy
  surprises are discovered rather than predicted.
- Activity auth + the account-model decision: **medium**, and the only part
  with a real design question in it.
- Payload reduction: **unknown until step 1**. Could be nothing; could be the
  largest piece of work here.
