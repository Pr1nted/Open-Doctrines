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

**Built.** The prologue is in `shell.html` (`odActivity`, `odDiscordSetup`,
`odDiscordGate`) and runs from `Module.preRun`, so the patch is in place before
`main()` and therefore before any C++ network call. Auth is still step 3.

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
2. ~~Add the SDK prologue and `patchUrlMappings`.~~ **Done** — see "How the
   proxy layer works" below. Confirming a real sign-in and a real multiplayer
   session through the proxy still needs a Discord client, and is the one thing
   here that cannot be tested from this machine.
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
4. **General Information → policy links.** Discord **refuses a `workers.dev`
   URL** for these ("the specified Privacy Policy URL is not allowed"): it is a
   shared suffix, so anyone can hold one. `pages.dev` it accepts. Use:

       Terms of Service URL   https://opendoctrines.pages.dev/terms
       Privacy Policy URL     https://opendoctrines.pages.dev/privacy

   Those pages are rendered from `net/TERMS.md` and `net/PRIVACY.md` at deploy
   time -- the same files the Worker imports and serves to the game -- so the
   published policy and the one players agree to are one document.

   The Worker's own `/terms` and `/privacy` are still what the GAME links to,
   and they now answer two ways: a browser (`Accept: text/html`) is redirected
   to the rendered pages above, and anything else gets the markdown byte for
   byte. So the in-game "Privacy policy" button lands on a page rather than on
   the source of one, without the game needing to know either URL.

### 3. Test it before anybody else sees it

In the Discord **client**: User Settings → App Settings → **Advanced** →
**Developer Mode** on. (There is no switch called "Developer Activity Shelf" —
the shelf is what Developer Mode gives you.) Your application then appears in
the activity picker of any voice channel you can use, without being published,
reviewed, or installed to a server: owning the app in the portal is enough.

Join a voice channel, click the **rocket** button in the voice controls, and
pick OpenDoctrines.

**If it is not in the list, the cause is almost always Supported Platforms.**
The shelf only shows an application on platforms ticked under Activities →
Settings → Supported Platforms in the portal. An app with only Android ticked
is invisible on a desktop client, and nothing anywhere says why.

### 4. Publishing, later

Discoverability in the App Directory needs the application verified and
Discovery enabled. That is a submission, not a build step, and it is worth
leaving until after step 2 of the plan above — there is no point listing an
Activity whose networking has not been through the proxy yet.

## How the proxy layer works

The whole of it is that **the transport is diverted and the identity is not.**

`patchUrlMappings` rewrites outbound URLs — it patches `fetch`, `WebSocket` and
`XMLHttpRequest.prototype.open`, which between them is everything this build
can emit (`emscripten_fetch` is an XHR; `WsWeb.cpp` is a plain `new
WebSocket`). One mapping covers the lot, because sign-in and the relay are
routes on the same Worker:

    https://opendoctrines-net…workers.dev/session/ABCD
      ->  https://<app id>.discordsays.com/api/session/ABCD
    wss://opendoctrines-net…workers.dev/session/ABCD/ws
      ->  wss://<app id>.discordsays.com/api/session/ABCD/ws

**The obvious wrong answer is to repoint `accountIssuer` at the proxy**, and it
would fail in a way that looks nothing like a URL problem. The issuer is an
identity: `Session.cpp:answerChallenge` refuses a host that names a different
account service, and the Worker signs every ticket with its own canonical URL
whatever route the request arrived by. An Activity player carrying a proxy URL
would disagree with every host in existence and be refused from every game. So
the C++ keeps the canonical URL and only the transport moves.

`ready()` is the other half, and it is why the SDK is used rather than fifty
lines of our own rewriter: until it resolves, Discord holds its own loading
screen over the iframe, so an Activity that skips the handshake runs perfectly
underneath something the player cannot see past. That handshake is a private
protocol between the SDK and the Discord client.

Three things keep this from rotting:

- **The shell is stamped, not hand-edited.** CMake substitutes the account
  service and the application id into `shell.html`, from the one definition of
  each that already exists. There is no second copy to fall out of step.
- **The mapping is tested against Discord's own rewriter.**
  `packaging/web/discord/mapping.test.mjs` reads the mappings *out of
  `shell.html`* and runs them through the SDK's `attemptRemap`. It asserts the
  WebSocket keeps its `wss:` scheme, that same-origin files are left alone, and
  that a build with no account service maps nothing. Deliberately breaking the
  prefix, the activity detection and the empty-issuer guard each turns it red.
- **CI asserts the substitution ran.** A page that shipped the literal string
  `__OD_ACCOUNT_ISSUER__` would build, load and play everywhere except inside
  Discord.

The SDK is bundled at deploy time from a pinned version
(`packaging/web/discord/`), not committed as a minified blob. It is fetched by
the page **only** when `frame_id` and `instance_id` are present, so itch.io and
plain-web players never download it.

If the handshake never answers, an 8-second timeout releases the run dependency
and the game starts anyway: a game with no sign-in beats a game that never
starts.

## What is NOT done

The prologue is written, stamped, bundled, tested and **deployed**, and the
proxy layer has been driven end to end on the live build — by opening
<https://opendoctrines.pages.dev/?frame_id=…&instance_id=…>, which makes the
page take the Activity path without a Discord client:

- the SDK was fetched from the deploy and `patchUrlMappings` ran;
- `fetch`, `WebSocket` and `XMLHttpRequest.prototype.open` were all replaced;
- a relay socket URL came out as
  `wss://<origin>/api/session/ABCD/ws` — proxy path, `wss:` scheme intact;
- and the game reached the main menu anyway when the handshake failed, which
  is the boot gate's timeout doing its job.

The handshake is the part that cannot be faked: without a real Discord parent
the SDK stops at `platform query param is not defined`. Discord supplies
`platform` alongside `frame_id` and `instance_id`; the prologue deliberately
does not require it for detection, because refusing to activate on a missing
param would be a silent no-op, while failing at the SDK is a logged one.

So: the routing is proven on the shipped artefact, and **the Discord handshake,
a real sign-in and a real multiplayer join are still unverified**. Those need
the Developer Activity Shelf and a voice channel.

Still genuinely absent: **Discord identity**. The Activity does not know who is
playing it, does not use the participant list, and asks nobody to authorise
anything. `authorize` → `authenticate` → a token exchanged server-side, plus
the decision about whether an Activity player gets a full account, is step 3.
