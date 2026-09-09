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

**Payload — measured, and much smaller than it looks.** An earlier draft of
this document called 21 MB the biggest risk. That was the number on disk, not
the number on the wire, and it was wrong to lead with:

    file                 raw     gzip   brotli
    OpenDoctrines.wasm  10.4M     3.2M    2.7M
    OpenDoctrines.data  10.0M     4.3M    3.8M
    OpenDoctrines.js     0.4M     0.1M    0.1M
    TOTAL               20.9M             6.6M

**6.6 MB** is what a CDN can send. What it actually sent, when deployed and
measured in a browser, was **13.2 MB** — because Cloudflare compresses by
content-type and `application/octet-stream` is not on its list, so the 10 MB
`.data` went over the wire whole while the wasm was brotli'd to 3.1 MB. The
loader reads that file as an ArrayBuffer and never looks at its type, so
`_headers` now declares it `application/wasm` and the real transfer is
**7.2 MB**: wasm 3.07, data 4.05, js 0.08.

Nothing local would have shown this. It is the difference between what
compresses and what the CDN *chooses* to compress, and it is only visible from
the outside.

That puts a first load at about two seconds on a typical home connection, five
on a slow one, ten on 4G. A returning player pays one revalidation per file and
no body at all — see `packaging/web/_headers`.

**It is deployed**: <https://opendoctrines.pages.dev>. Verified rendering from
that URL, not only locally.

That is no longer the thing most likely to sink this. Trimming the `.data`
bundle (5.4 MB of it is maps) is now an optimisation rather than a
prerequisite.

**It runs in a sandboxed iframe.** Tested locally against Discord's own sandbox
flags (`allow-scripts allow-same-origin allow-popups allow-forms`): the menu
renders, the canvas sizes to 1280x720, five requests, all 200, and nothing in
the console. This was the other thing worth checking before building anything,
because a web build that will not frame is a non-starter.

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

1. ~~Measure the load.~~ **Done** — see above. 6.6 MB on the wire, runs in a
   sandboxed iframe. `tools/deploy-web.sh` puts it on Cloudflare Pages;
   `packaging/web/_headers` carries the caching and the framing.
2. Add the SDK prologue and `patchUrlMappings`; confirm the account service and
   a multiplayer session both work through the proxy. **This is the next real
   unknown**: the proxy rewrites every request, and a `blocked:csp` only
   appears inside Discord.
3. Then auth, and only then the account-model decision.

## Estimate

Rough, and honest about which parts are guesses:

- Hosting + step 1 measurement: **small** — mostly CI and a Pages project.
- SDK prologue + URL mappings: **small to medium**, with the caveat that proxy
  surprises are discovered rather than predicted.
- Activity auth + the account-model decision: **medium**, and the only part
  with a real design question in it.
- Payload reduction: **unknown until step 1**. Could be nothing; could be the
  largest piece of work here.

## Setting it up — what only you can do

Two halves: hosting (a script, below) and Discord's own settings (a web page,
which no script of mine can click for you).

### 1. Host the build

    tools/deploy-web.sh

The first run opens a browser for Cloudflare's own login and wrangler keeps the
token — nothing here reads or stores a credential. It builds with the release
flags (account service and Discord app id both baked in, or sign-in and
presence go missing) and deploys `index.html`, the three big files and
`_headers`.

This is already done: the project is `opendoctrines` and the site is
<https://opendoctrines.pages.dev>. Re-running the script redeploys it, which
is how every player gets the new build.

A custom domain is worth doing before you tell anybody about it: the URL
mapping in Discord points at whatever you set here, and moving later means
editing it in two places.

### 2. Turn the application into an Activity

On the same application the rich presence uses
(`1547303703370014830`), at discord.com/developers/applications:

1. **Activities → Settings** — enable Activities.
2. **Activities → URL Mappings** — add the root mapping:

       /            ->  opendoctrines.pages.dev

   and one more, because the game talks to the account service and the
   multiplayer relay, and *both are the same host*:

       /api         ->  opendoctrines-net.opendoctrines.workers.dev

   Anything not mapped fails with `blocked:csp` and nothing else. This is the
   step that most often takes two attempts.
3. **Installation** — the Activity needs the `applications.commands` scope so
   it can be launched in a server.

### 3. Test it before anybody else sees it

In the Discord **client**: Settings → **Advanced** → enable **Developer
Activity Shelf**. Your application then appears in the activity picker of any
voice channel you can use, without being published or reviewed.

Join a voice channel, open the picker, launch OpenDoctrines.

### 4. Publishing, later

Discoverability in the App Directory needs the application verified and
Discovery enabled. That is a submission, not a build step, and it is worth
leaving until after step 2 of the plan above — there is no point listing an
Activity whose networking has not been through the proxy yet.

## What is NOT done

The build is hosted and framed; it is not yet an Activity. Without the Embedded
App SDK prologue it will load in the frame and behave as a normal web build:
no Discord identity, no participants, and — the part that will actually bite —
**its network calls have not been through the proxy**, so sign-in and
multiplayer are unverified inside Discord. That is step 2, and it is the next
thing worth doing.
