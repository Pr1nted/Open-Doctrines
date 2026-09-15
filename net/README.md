# opendoctrines-net

Accounts and the multiplayer relay, on Cloudflare's free plan.

Three jobs, deliberately kept apart:

- **Accounts** — sign in with Google, Discord or GitHub; hold a unique nickname;
  carry developer and playtester badges. A Worker with one KV namespace.
- **Relay** — one Durable Object per game session. Everyone dials *out* to it
  over `wss://`, including the host, which is the whole answer to "no UPnP, no
  port forwarding, no NAT punchthrough, works from a browser".

- **Mod registry** — a directory of mods. It stores a listing and a link; it
  never stores a mod. See `docs/mod-registry.md`, and `src/mods/`.

The relay never parses a game payload. Rules live in the C++ game, which is the
authoritative server; this moves bytes and vouches for who sent them.

## The one thing to understand before changing anything

There are **two kinds of token** and the split is load-bearing.

| | Session token | Join ticket |
|---|---|---|
| Audience | `od-api` | `od-relay:<sessionId>` |
| Lifetime | 24h, refreshable | 120 seconds, single use |
| Who holds it | the player's client, only | presented once, consumed by the relay |
| Contains | account id | a per-server pseudonym, a display name, badges |
| Can call `/account/*` | yes | **no** |

A game server is run by a stranger. It must never end up holding something it
could replay as that player, and it must not learn which account it is talking
to. So the ticket carries a **pairwise pseudonym**:

```
psid = HMAC(PAIRWISE_KEY, accountId + ":" + serverId)
```

Same on one server forever (so a host can keep stats and enforce bans),
unrelated across servers (so two hosts cannot work out it is the same person).
And the ticket dies at the relay: the host receives a *statement* about the
peer, never the ticket itself.

If you are about to add a field to a ticket, or let a host see a raw token, or
add a KV write to a login path — read `PRIVACY.md` first, because all three
would make it untrue.

## Deploy

### 1. Create the KV namespace

```bash
npx wrangler kv namespace create OD_ACCOUNTS
```

Put the returned id into `wrangler.toml` under `[[kv_namespaces]]`.

### 2. Generate the keys

`setup.sh` does steps 1-6 for you. Run it instead of following them by hand:

```bash
cd net && npm install && ./setup.sh
```

The rest of this section is what it does, for when you would rather do it
yourself or something goes wrong.

```bash
node -e '
const { webcrypto } = require("node:crypto");
webcrypto.subtle.generateKey("Ed25519", true, ["sign","verify"]).then(async (k) => {
  console.log("PRIVATE:", JSON.stringify(await webcrypto.subtle.exportKey("jwk", k.privateKey)));
  console.log("PUBLIC :", JSON.stringify(await webcrypto.subtle.exportKey("jwk", k.publicKey)));
});'
```

```bash
openssl rand -base64 32
```

Run that twice, for `IDENT_KEY` and `PAIRWISE_KEY`, and once more for
`ADMIN_SECRET`.

### 3. Set the secrets

```bash
for s in ED25519_PRIVATE_KEY ED25519_PUBLIC_JWK IDENT_KEY PAIRWISE_KEY ADMIN_SECRET; do npx wrangler secret put "$s"; done
```

Then the OAuth apps you have registered, one command each:

```bash
./add-provider.sh github
```

**itch.io is set up the same way but has no client secret**, because it offers
only the implicit flow — there is no token endpoint to authenticate against.
`./add-provider.sh itch` asks for the client id alone, and `clientCredentials()`
requires a secret for exactly the providers that spend one. Its redirect URI is
`<issuer>/auth/callback/itch`, which serves a small page that reads the token
out of the URL fragment and posts it back; browsers never send a fragment to a
server, so there is no other way to receive it. The service then spends that
token against `api.itch.io/profile` and trusts only what itch.io answers.

itch.io is **link-only**: no creation date on its profile means the signup gate
cannot be applied, so an account has to exist already.

It prompts for the id and secret, uploads both, and then waits until the service
actually offers the provider — because two `wrangler secret put` calls succeed
whether or not the values are right, and without that check you find out at the
consent screen instead.

A provider whose credentials are absent is simply not offered, in the service
listing and in the game's sign-in screen. You can ship with one and add the
others later; no redeploy is needed for either.

Each provider needs its redirect URI registered as
`https://<your-worker>/auth/callback/<provider>`, and **the scopes must stay as
they are in `src/auth/providers.ts`**: `openid` for Google, `identify` for
Discord, none for GitHub. Widening them would start collecting data the privacy
policy says we do not have.

### 4. Key rotation — read this before you rotate anything

`IDENT_KEY` and `PAIRWISE_KEY` are **permanent**.

- Rotating `IDENT_KEY` orphans every linked identity. Nobody can sign in again,
  ever, and the accounts cannot be recovered because the mapping was one-way by
  design.
- Rotating `PAIRWISE_KEY` changes every player's pseudonym on every server. To
  each host it looks like its entire playerbase was replaced overnight: stats
  detach, bans stop applying.

The Ed25519 signing key *can* be rotated, at the cost of invalidating live
sessions and tickets. Serve both public keys from `/.well-known/od-keys.json`
during the changeover so servers with a cached copy keep working.

A note on the key format: Node's `exportKey` adds `alg: "Ed25519"` to the JWK,
and workerd refuses to import a JWK carrying it. `canonicalJwk` in
`src/auth/token.ts` strips that (and `key_ops`/`ext`) before importing, so a key
from any generator works. Do not "simplify" that away -- without it nobody can
log in, and the only symptom is every sign-in reporting that it expired.

### 5. Upload the nickname blocklist

The profanity list is **not** in this repository. `src/accounts/nickname.ts`
carries the reserved and impersonation words (`admin`, `developer`, `staff`…)
because those are useful to read; the rest is deployment data:

```bash
npx wrangler kv key put --binding OD_ACCOUNTS cfg:blocklist --path blocklist/profanity.txt
```

Format: one term per line, `#` for comments, and a leading `!` marks an
**exception** — a name allowed even though it contains a blocked substring.
Every substring filter needs those; see `blocklist/README.md`.

Terms are matched against the *normalized* form of a nickname, so you do not
need leetspeak or spaced-out variants in the list. `b4d w0rd` and `b_a_d_w_o_r_d`
both match `badword`.

### 6. Deploy

```bash
npx wrangler deploy
```

Two things bite here, both once only:

**"You need a workers.dev subdomain in order to proceed."** Open the Workers
page in the Cloudflare dashboard once. Merely loading it creates the subdomain;
there is nothing to click. Then deploy again.

**`ISSUER` must match the deployed URL exactly.** The first deploy tells you
what that is, and it includes your account's subdomain —
`https://<worker>.<subdomain>.workers.dev`, not `https://<worker>.workers.dev`.
Put it in `wrangler.toml` and deploy once more. It is stamped into every token
as `iss` and checked on the way back in, so a mismatch means nothing verifies
and every sign-in reports that it expired.

A newly created `workers.dev` hostname takes a few minutes before it serves
TLS. Until then `curl` fails with error 35 and the game says it could not reach
the account service. That is propagation, not misconfiguration; wait and retry.

## The controller identity, and a gap taken knowingly

`PRIVACY.md` names the **OpenDoctrines project** as controller, with an email and
no postal address. Two separate decisions are in that sentence, and the second
one is a compromise:

**No postal address — fine.** GDPR Art. 13 asks for the controller's identity and
*contact details*. It does not require a street address, and a monitored email is
what practically every online service of this size publishes. Nothing in the
policy needs a letter.

**A project name rather than a person — a known gap.** Strictly, "the identity of
the controller" means the actual legal person, and for an individual that is
their name. Publishing the project name instead is what most hobby projects do,
and it is recorded here as a deliberate choice rather than an oversight. If this
ever stops being a hobby project, the fix is a legal entity: an LLC or sole-trader
registration gives you a business name and a registered address to publish
instead of your own, and the policy becomes fully compliant with one edit.

What matters in the meantime is that the contact address is genuinely monitored
and requests are actually honoured. An unreachable controller is the part a data
protection authority would actually care about.

Not legal advice.

## Anti-alt measures, and their honest limits

"One person, one account" is not enforceable and nothing here claims to enforce
it. Provider accounts are free, instant and unlimited, and by design we store no
email and only a keyed hash of a provider id, so there is nothing to correlate a
person's Google identity with their Discord one. The goal is COST, not
prevention: a ban should mean coming back in a month with a different long-lived
account, not clicking "sign up" twice.

What is actually in place:

| Measure | Effect |
|---|---|
| One provider identity → one account | Real and absolute. One GitHub account can never become two game accounts. |
| 30-day provider account age gate | Throwaways refused at signup. `MIN_ACCOUNT_AGE_DAYS` in `src/accounts/policy.ts`. |
| Link-only providers | A provider that cannot be age-gated may be *added* to an account but never *create* one. |
| Global ban at ticket-minting | A banned account joins nothing. Servers are never told a ban exists. |
| Identity hold after a banned deletion | Deleting a banned account blocks its provider ids for a year, so erasure is not a ban reset. |

**Google is link-only** and that is the point, not an oversight. It exposes no
creation date under `openid`, and `sub` is documented as opaque with no ordering
to infer age from — so a Google account could not be gated, and allowing it to
create accounts would be a door straight past the gate for everyone refused
elsewhere. As a second sign-in method it is perfectly good, which is what most
people want it for anyway.

If you ever add a fourth provider, the question to ask is: can it be
age-gated? If not, set `canCreateAccount: false` and it costs you nothing.

## Free-tier budget

The limits that actually bind, and what the design does about them:

| Limit | Free plan | What we do |
|---|---|---|
| KV writes | **1,000/day** | Account creation costs 3. Logging in costs **0** — no `lastSeen`, no counters. Minting a join ticket costs 0. |
| KV reads | 100,000/day | Everything reads. Not a constraint. |
| Worker requests | 100,000/day | One per login, one per join. |
| Durable Object requests | 100,000/day | Incoming WebSocket messages bill at 20:1, so a game session is cheap. Mod download counts live here **because** they cannot live in KV — one click would otherwise cost a KV write out of the same 1,000 that account creation spends. |

So the practical ceiling is roughly **330 new accounts a day**, with logins
effectively unlimited. If that ever binds, the fix is to move the login handoff
(`res:` keys in `src/auth/device.ts`) into a Durable Object, which would take it
to the DO request budget instead. It is one function, and it is deliberately
isolated for that reason.

## Long-form turn storage

The server half of `TurnStoreKind::DurableObject`, which is the game's default
long-form store because it needs nothing enabled that a working account does not
already have — R2 does the same job, but Cloudflare wants a card on file before
it will switch R2 on.

Turn data lives in the session's own Durable Object, alongside the lobby state,
and dies with it: 90 days idle for a long-form session, immediately for a rapid
one. Four routes, and the URL shapes are the game's rather than ours — they are
built in `src/net/TurnStore.cpp`:

| Route | Who | What it does |
|---|---|---|
| `PUT /session/<code>/turn/<n>` | the host, only | Publish turn `n`. **Refused if it already exists.** |
| `GET /session/<code>/turn/<n>` | anyone | Read it back. Cacheable forever. |
| `PUT /session/<code>/orders/<n>/<psid>` | that player, only | Submit sealed orders. May be revised. |
| `GET /session/<code>/orders/<n>/<psid>` | anyone | Read them back. |

The body is `{"od":1,"turn":N,"data":"<base64url>"}`, and it is validated rather
than stored as it arrives: `data` must be base64url and the turn in the body
must match the turn in the URL. Storing what came in would make this a general
JSON host rather than a turn store.

**Writes are authenticated with a session token**, not a join ticket — a ticket's
audience is `od-relay:<sid>`, so it fails the check. A turn is refused to anyone
but the host, and orders are refused to anyone but the player they belong to.

**Reads are not authenticated, and that is the design.** A published turn is
public so that people can spectate a tournament without joining it. Orders are
sealed on the player's machine before they are sent (`src/net/TurnSeal.h`), so a
reader without the key holds ciphertext — confidentiality never comes from the
store, which is what lets the same client talk to a world-readable bucket like
jsonblob without contradiction.

One honest difference from that jsonblob backend: there a blob sits at an
unguessable URL, whereas these are derivable from the join code. Nobody gains
readable orders by it, but an observer holding the code can tell *whether* a
player has submitted for a given turn. In a game whose host announces who is
still to move, that is not a secret.

Blobs are capped at 512 KB. That is orders of magnitude above the KB-scale turn
deltas this carries, and it is chosen against the game's own ceiling rather than
Cloudflare's: `HttpRequest::maxResponseBytes` defaults to 1 MB, so a larger blob
could be written and then never read back.

**The game calls this now.** The host publishes each resolved turn beside its
existing `broadcastDelta`, and players submit orders sealed with the session key
(`src/net/TurnSeal.h`) handed out over the lobby connection. Both sides also work
with nothing connected, which is the point of the mode. The client half lives in
the long-form section of `src/Game_Multiplayer.cpp`, and it has not yet been
played through a real multi-day campaign — see the root README's Status section.

## Abuse and rate limits

Every endpoint is reachable without a credential or is cheap to attempt without
one, and the budgets above are day-long and shared. Two things bound that, and
it is worth knowing which does what.

**A shape check on the join code.** `GET /session/<code>` is the only route that
can instantiate a Durable Object, and `idFromName` plus a fetch creates one
whether or not the session exists — LobbyDO's constructor runs its `CREATE
TABLE`s before `/info` can answer 404. So a code that never existed used to
leave a real object with real storage behind it, no alarm, and nothing that
would ever reclaim it. `isSessionCode` in `src/lobby/session.ts` refuses
anything the generator could not have produced, before the binding is touched.
It is built from `HUMAN_ALPHABET` rather than written out, so the two cannot
drift.

That handles a malformed guess. A **well-formed** one still reaches the object,
so `LobbyDO.noSession()` wipes its own storage before answering 404 — probing
cannot accumulate empty objects either way. Note that `deleteAll()` drops the
tables and the instance keeps serving afterwards, which is why every wipe is
followed by `ensureSchema()`.

**Per-IP rate limiting**, via the two `[[ratelimits]]` bindings in
`wrangler.toml`. These are free and cost neither a KV operation nor a DO
request — a limiter built on KV would spend the budget it exists to protect.
The key is `HMAC(IDENT_KEY, "rl:" + ip)`, never the address: PRIVACY.md says we
do not collect IP addresses and that stays true, because what the limiter holds
is a keyed hash — not reversible without `IDENT_KEY`, never written to KV, never
logged — behind an in-memory counter that lives ten seconds in one colo.

**Know what this does not do.** The bindings are best-effort and per-colo, not a
global ledger. They stop one source hammering one endpoint. They do not stop a
distributed flood, and a limit generous enough not to break eight players
reconnecting behind one NAT is also generous enough that a determined attacker
with a valid join code could still outspend the daily budget. If that ever
happens the answer is a **WAF rate-limiting rule** in the dashboard, which
cannot be expressed in `wrangler.toml` and has to be added by hand.

Note that `wrangler dev` runs the limiter too: miniflare synthesises
`CF-Connecting-IP` as `127.0.0.1`, so every client on your machine shares one
bucket. A local 429 during a multi-client playtest is that, not a bug.

## Bug reports, suggestions and ratings

`POST /feedback` takes what the in-game form sends and forwards it. See
`src/feedback/report.ts`; the reason it exists at all is at the top of that file
and worth reading before changing it. In short: the obvious build posts straight
from the game to a Discord webhook, and a webhook URL shipped inside a binary
belongs to whoever runs `strings` on it first.

**Every report needs an account.** `/feedback` refuses anything without a valid
session token — 401. That is not only spam control: each report is published,
and each is signed with the account nickname, so there has to be something to
sign it with that a stranger cannot invent. The byline is taken from the
verified token and **never** from the request body, because a name the client
chooses is a name anybody can choose, and it lands next to a public accusation
that something is broken. A reporter may tick "do not show my name", which
publishes "Anonymous (signed in)" — the service still knows exactly who they
are, so the ban list and the quotas are untouched.

There is no anonymous write path. Ratings used to be one — one tap, no account,
and it still posted to the chat channel, rationed only by an install ID the
client makes up. Ratings now go to the game's itch.io page instead, where a
rating is public and useful to somebody choosing the game.

**It is optional.** With none of the four variables below set, the endpoint
still validates, still rate-limits, and answers `{"ok":true,"destinations":[]}`
— the report goes nowhere and the player is thanked, which is the right
behaviour for a fork that has not set up a tracker of its own. Nothing about the
feature has to be disabled to leave it unconfigured.

| Secret | What it does |
|---|---|
| `FEEDBACK_DISCORD_WEBHOOK` | Where bugs and suggestions are posted. **Never** security reports |
| `FEEDBACK_GITHUB_TOKEN` | Needs **Issues: write** *and* **Repository security advisories: write** (classic: `repo`) |
| `FEEDBACK_GITHUB_REPO` | `owner/repo` for both the issues and the advisories |
| `FEEDBACK_DISCORD_SECURITY_WEBHOOK` | Optional. Used **only** if an advisory cannot be filed |
| `FEEDBACK_DISCORD_SUGGESTION_WEBHOOK` | Optional. Suggestions land here instead of the bug channel |
| `FEEDBACK_GITHUB_WEBHOOK_SECRET` | Optional. Shared secret for the repo's issues webhook — see below |

```bash
./add-feedback.sh
```

That script prompts for each one, skips the blanks, and checks two things that
otherwise surface much later: that this deployment actually **has** the
`/feedback` route (a secret on a route that 404s is invisible until a player
writes a report and it vanishes), and that the token can really reach the
repository's security advisories.

### Closing an issue tells the thread it came from

`POST /feedback/github` receives the repository's **issues** webhook. When an
issue that began as a report is closed or reopened, the service posts a note
into the Discord thread that report opened — "Closed by Pr1nted", with a link.

The pairing is made when the report is filed: the Discord send asks for
`wait=true`, and a forum post's message lives *in* its new thread, so the
message's `channel_id` **is** the thread id. That is stored against the issue
number for 180 days.

`./add-feedback.sh` sets this up — it generates the shared secret, stores it in
the Worker, and creates the webhook with `gh` (printing the values to add by
hand if `gh` is not available).

**The signature is the entire security model here.** This endpoint is public and
it makes the service post into a Discord channel; unverified, it is a spam relay
with extra steps. Every delivery is checked against an HMAC of the **raw** body
before a single field is read, compared in constant time. Two details are
load-bearing:

- **Raw body, never a re-serialised object.** `JSON.parse` then `JSON.stringify`
  does not reproduce the bytes GitHub signed, so a check against a round-tripped
  body fails for honest requests — and teaches whoever debugs it to disable the
  check.
- **No secret means refuse everything** (503), not trust everything.

Events it does not care about get a **200**, not an error: GitHub retries and
eventually disables an endpoint that keeps failing, and it sends the whole
`issues` family plus a ping on setup. "Not for me" is a success.

**One direction only.** Discord cannot push events to a URL — a webhook only
sends — so hearing a thread being resolved would need a bot holding a gateway
connection: a Durable Object with a permanent WebSocket and a second always-on
credential. That is a different project and it fights the free-tier shape of the
rest of this service.

### Two things a minimal token will not do

**Labels.** Creating an issue and labelling one are different permissions:
with `Issues: read and write` the issue is filed and both label calls answer
`403 Resource not accessible by personal access token`. Widening the token to
fix it means granting write access to the code so reports arrive with a coloured
tag, which is a bad trade — the category is already in the title
(`[Bug/scripting] ...`), which is what the tracker gets searched on. Labels are
best effort; the response carries a `detail` field saying why when they fail.

**Advisories.** `Repository security advisories: read and write` is a separate
toggle from Issues and is easy to miss or set to read-only. Read-only passes a
naive check and then cannot file anything, so `add-feedback.sh` tests **write**
by POSTing an empty advisory: `422` means authorised (it reached validation),
`403` means not. Nothing is created either way, because an empty body never
validates.

### Where a security report goes, and where it must not

A report filed under **Security** becomes a **draft repository security
advisory** — private, visible only to people with admin on the repo, and the
same object a coordinated disclosure is eventually published from.

Three things it is never allowed to become:

- **A public issue.** Filing "here is how to get past the mod sandbox" in a
  public tracker publishes the exploit to everyone before it is fixed.
- **A message in the ordinary bugs channel.** The first version of this fell
  back from the security webhook to the general one when the former was unset,
  which quietly did the thing the whole path exists to prevent. It does not fall
  back any more; `FEEDBACK_DISCORD_SECURITY_WEBHOOK` is used only when it is set
  *explicitly*, and only when the advisory could not be filed.
- **Silently dropped.** If nothing took it, the route answers **502** and the
  game tells the player it was not filed anywhere and to contact the maintainer
  directly — and does not write the duplicate marker, so their retry is not met
  with "already got that" for the next six hours.

There are tests for each of those. If you add a destination, wire
`isPrivateCategory()` into it before anything else.

**Severity is deliberately not set on the advisory.** The API takes `severity`
or `cvss_vector_string` and never both, and either one is a judgement about a
report nobody has triaged. A default of "medium" on every report would make the
field worthless exactly where it matters. The maintainer sets it on the draft.

**Four things stand between this endpoint and being a spam relay**, and none of
them is sufficient alone: the per-IP limiter in front of the route, a per-install
daily cap of 12, a six-hour duplicate window, and hard size caps applied before
the body is parsed. The client enforces its own cooldown and cap as well, so a
stuck key is refused before it becomes a request — but that is a courtesy, not a
defence, and the Worker never assumes it happened.

`allowed_mentions: {parse: []}` on the Discord send is load-bearing. Without it
a report containing `@everyone` pings the whole server, which turns a mailbox
into a megaphone.

## Moderation

`POST /moderation/report` takes a complaint about another player, `GET
/moderation/reports` is the queue, and `POST /moderation/decide` bans, times
out, or dismisses. See `src/moderation/reports.ts`.

**The queue and the decide route are gated by the `developer` badge, not by
`ADMIN_SECRET`.** That is deliberate: the admin routes are gated by a shared
secret in a header, which is right for a command line and wrong for a game menu
— it would mean shipping a client that holds a credential capable of banning
anybody. Both answer **404**, not 403, to anyone without the badge: a 403 would
confirm the route exists.

**To give yourself the badge**, once, from a machine that has `ADMIN_SECRET`:

```bash
curl -s -X POST -H "x-od-admin: $ADMIN_SECRET" -H 'content-type: application/json' \
  -d '{"accountId":"<your account id>","badge":"developer","on":true}' \
  https://opendoctrines-net.opendoctrines.workers.dev/admin/badge
```

Your account id is shown in the game under Account. After that the review queue
appears in-game and no secret is involved again.

**Announcements.** `MODERATION_DISCORD_WEBHOOK` receives a line when somebody is
banned or timed out — who, and until when, and nothing else. Never the reported
message, never the reporter, and never anything at all for a dismissal.
Set it with `./add-feedback.sh` (step 7).

**Identifying who was reported.** A multiplayer client only ever sees a pairwise
pseudonym, so `auth/ticket.ts` keeps a note linking that pseudonym to the account
for 30 days. This is a retention change rather than a new capability — minting
the ticket is the moment the service computes the pseudonym from the account and
the server, so the link exists at that instant regardless. It is used in exactly
one place, is never given to servers, and after 30 days a report can no longer
be connected to anyone. The route says so rather than guessing.

## Running a game server

`POST /server/register` once, signed in, and keep the `serverCredential` it
returns in your server config. **Do not regenerate it**: it is what makes the
per-player pseudonyms on your server stable. Register again and every returning
player looks like a stranger.

If you record anything about players — pseudonyms, display names, chat — you are
an independent data controller for it. We cannot see it and cannot delete it on
a player's behalf, so a deletion request that reaches us does not reach you.
Please handle those, and prefer not to persist pseudonyms past the session; the
shipped server config does not.

## Development

```bash
npm install
npm run typecheck
npm test

cp .dev.vars.example .dev.vars   # then fill in the keys, as the file explains
npx wrangler dev --port 8787 --local &
npm run e2e
```

`npm test` runs unit tests inside `workerd`. `npm run e2e` drives the deployed
shape -- the router, a real KV binding, and a signing key generated the way this
README says to generate one. That last part matters: the unit tests generate
their keys inside workerd, so they cannot catch a key format workerd will not
accept, and exactly that bug shipped once already.

Tests run inside `workerd` rather than Node, which matters because Ed25519 is
the one primitive whose availability differs between the two.

`test/privacy.test.ts` asserts the claims in `PRIVACY.md` — that a ticket cannot
call the account API, that pseudonyms do not correlate across servers, that an
export is complete and a deletion leaves nothing behind. If one of those fails,
the published policy has become inaccurate, which is worse than a crash.
