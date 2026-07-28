# opendoctrines-net

Accounts and the multiplayer relay, on Cloudflare's free plan.

Two jobs, deliberately kept apart:

- **Accounts** — sign in with Google, Discord or GitHub; hold a unique nickname;
  carry developer and playtester badges. A Worker with one KV namespace.
- **Relay** — one Durable Object per game session. Everyone dials *out* to it
  over `wss://`, including the host, which is the whole answer to "no UPnP, no
  port forwarding, no NAT punchthrough, works from a browser".

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
| Durable Object requests | 100,000/day | Incoming WebSocket messages bill at 20:1, so a game session is cheap. |

So the practical ceiling is roughly **330 new accounts a day**, with logins
effectively unlimited. If that ever binds, the fix is to move the login handoff
(`res:` keys in `src/auth/device.ts`) into a Durable Object, which would take it
to the DO request budget instead. It is one function, and it is deliberately
isolated for that reason.

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
