# OpenDoctrines accounts — privacy policy

**Last updated:** 2026-07-27

> This file was drafted by looking at the code, not by a lawyer. It is accurate
> about the software; it is not legal advice.

OpenDoctrines accounts are run by the **OpenDoctrines project**. Questions, or
any request described below: **opendoctrines@gmail.com**.

We do not publish a postal address. Everything in this policy can be done by
email, and there is nothing we would need one for.

---

## The short version

You sign in with Google, Discord or GitHub so that a nickname can be yours and
nobody else's. We store a **hashed** version of your user ID at that provider,
the nickname you chose, and any badges. That is all of it.

We never ask any provider for your email address, and we do not have one.

When you join a game server, that server is **not** told who you are by us. It
gets a pseudonym that is unique to that one server, plus whatever name you chose
to show. Neither can be used to sign in as you anywhere.

**But your game connects to that server directly, so the host sees your IP
address.** We are not in the middle of a game and cannot hide it. That is the
one thing about you a host learns that we did not choose to tell them, and it is
enough for two hosts to recognise the same person however carefully the
pseudonyms are kept apart. There is a section on this below; please read it.

---

## What we collect, and why

| What | Why | Kept |
|---|---|---|
| A keyed hash of your user ID at each linked provider | So that signing in again finds your account | Until you delete the account or unlink that provider |
| Your nickname | It is your name in game, and it has to be unique | Until you change or delete it |
| Badges (developer, playtester) | So they can be shown next to your name | Until removed |
| Account creation date, and the date you last changed your nickname | To enforce the 7-day nickname-change limit | With the account |
| Whether your account is banned, and why | So you are told, rather than silently failing to join | With the account |

**We do not collect:** your email address, your real name, your avatar, your IP
address, a login history, a list of servers you played on, your gameplay, or
anything about your device.

### About itch.io specifically

itch.io works differently from the other three. It offers only the simpler kind
of OAuth, where your access token comes back through your browser rather than
directly between itch.io and us. We never take that token at face value: we ask
itch.io who it belongs to and use the answer. A token we were handed that
itch.io does not recognise gets nothing.

We ask for `profile:me` and nothing else — your numeric itch.io id and display
name. **Not** what you own, **not** what you have bought, **not** your games.

**itch.io can be linked to an account but cannot create one.** Its profile does
not say when the account was made, and refusing brand-new throwaway accounts at
signup is the only check we have, so it cannot be applied here.

### About the hashed provider ID

We never store the user ID Google, Discord or GitHub gives us. We store
`HMAC-SHA256(secret, "provider:id")` instead. That is enough to recognise you
when you sign in again, and it means a copy of our database cannot be matched
against any other service's data.

### About your provider account's age

When you first sign up we ask the provider how old your account there is, and
refuse brand-new ones. This is to make it tedious to create throwaway accounts
after a ban, not to learn anything about you: the date is compared once and
discarded, never stored.

Google does not tell us how old an account is, and we will not ask for the extra
permissions that would reveal it. So **Google cannot be used to create an
account** — only to add a second way of signing in to one you already have.

### About the scopes we request

- **Google** — `openid` only. This gives us an opaque user ID. It does *not*
  give us your email address, name or picture.
- **Discord** — `identify` only. This gives us your Discord user ID and
  username. It does *not* give us your email address.
- **GitHub** — no scopes at all. This gives us your public profile, of which we
  use only the numeric user ID.

Where a provider gives us a username, we offer it as a *suggested* nickname on
the signup screen. If you pick something else, it is not stored.

---

## What game servers learn about you

Anyone can host an OpenDoctrines server. So a server is told as little as it can
be told and still run a game:

- **A pseudonym (`psid`)** — derived as `HMAC(secret, your-account-id +
  server-id)`. It is the same every time you return to *that* server, so the
  host can keep your stats and enforce its own bans. On a different server it is
  a completely unrelated value. Two hosts comparing notes cannot tell that both
  entries are you. Nobody but us can reverse it, and we do not publish the key.
- **A display name** — your account nickname, or a per-server alias if you chose
  one when joining.
- **Your badges** — only if you chose to present them, and only if that server
  displays badges at all.

A server is **never** given anything it could use to act as you. The token your
game presents when joining is valid for 120 seconds, works for exactly one game
session, and cannot be used a second time.

### But a server does see your IP address

**Read this before joining a game.** Game servers are run by other players on
their own machines, and your game connects to them **directly** — we are not in
the middle of it, by design. That means the person hosting sees your IP address,
the same way they would in any game you connect to directly.

An IP address is more identifying than anything else described in this policy.
It indicates roughly where you are and who your internet provider is, and it is
stable enough that two hosts comparing notes could tell they had met the same
person — which the pseudonyms above are otherwise designed to prevent.

We cannot fix this from our side. Hiding your address would mean putting a
machine of ours between you and every game, which is the arrangement this design
deliberately avoids. What we do instead:

- The game tells you, on the screen where you join, before it connects.
- If you would rather not show your address, connect through a VPN or tunnel of
  your own. The host then sees that instead.

The host's address is their own problem to solve, and many will put a tunnel in
front of their server. That protects them, not you.

### The part you should know about

**A globally unique nickname undoes the pseudonym.** If you join two servers
under the same nickname, both operators can obviously see it is the same name.
The same is true of a rare badge. The pseudonym only gives you unlinkability if
what is *displayed* is also different — which is why the game offers a
per-server alias and lets you withhold badges. The game says so at the point you
choose.

**Servers are separate data controllers.** Whatever a host records in its own
logs — your pseudonym, your display name, what you said in chat — belongs to
that host. We cannot see it and we cannot delete it. If you want it removed you
have to ask them. This is not a loophole we are relying on; it is a limit of
running games on other people's computers, and we would rather state it than let
you assume otherwise.

**Servers using a different account provider.** A host can configure its server
to accept accounts from somewhere other than us. When one does, the game shows
you a warning naming that provider before you join, and any badges it issues are
displayed as unofficial. This policy does not cover those providers.

---

## What we can see

When your game asks for a join ticket, we necessarily learn — at that moment —
that your account requested a ticket for a particular server. There is no way
around it: a ticket that is bound to one server has to be issued by someone who
knows which server.

So we minimise instead: **that request writes nothing.** No join record, no
history, no counter. Request logging is disabled on our infrastructure
(`observability` is off, and the code logs no identifiers anywhere). The
information exists for the length of one request and then it is gone.

---

## Bug reports, suggestions and ratings

Sending one is entirely up to you: nothing here happens unless you fill in the
form and press Send. There is no background telemetry, no crash uploader and no
usage reporting anywhere in the game.

**You need an account to send one, and your report is published.** Bug reports
and suggestions require you to be signed in, and each one is signed with your
account nickname — on the community channel, and for a bug on the public issue
tracker. The form says so, next to the tickbox that changes it, before you type
anything.

**You can ask not to be named.** Tick "Do not show my name" and what is
published says "Anonymous (signed in)" instead of your nickname. You are still
signed in as far as the service is concerned, so the rate limits still apply —
you have simply asked not to be named in public, which is a reasonable thing to
want when reporting that something is embarrassingly broken.

**What is sent.** The title and description you typed, which part of the game
you picked, the game's version, your platform ("macos", "windows", "linux" or
"web"), a random install ID, and your account nickname. If you chose to attach
diagnostics, that block too — and you are shown its exact text, in full and
scrollable, before you send it. There is nothing else and nothing hidden.

Your nickname is taken from the sign-in token by the service, never from
anything the game sends, so no one can file a report under someone else's name.

**The install ID** is 22 random characters generated the first time you send
something and kept in your config file. It identifies a copy of the game, not a
person: it is not derived from your hardware, your account or anything about
you, it is not linked to your OpenDoctrines account if you have one, and its
only job is to let us stop one copy of the game sending a thousand reports.
Deleting it costs you nothing but that day's quota.

**The diagnostics block**, when you attach it, describes the game: version, AI
version, window size, language, the turn number and date, how many countries and
provinces the map has, which country you are playing, your difficulty setting,
which mods are installed, and any errors the map's scripts reported. Save and
mod paths are shown as `~/...` — the game strips your home directory before it
shows you the text, because on every desktop platform that path contains your
account name.

**Where it goes.** A bug or a suggestion goes to a Discord channel the
maintainer reads, and to the public issue tracker on GitHub.

**A report you file under "Security" is handled differently and is never posted
publicly.** It becomes a private draft security advisory on the repository,
readable only by people with admin access — not a public issue, and not a
message in the ordinary bugs channel either. If it cannot be filed there, it is
refused and the game tells you it was not filed anywhere, rather than being
delivered somewhere less private to make it succeed. The form says so when you
pick that category.

Anything you write in a report may end up in a public issue. Please do not put
anything in it you would not want read.

**What is stored here.** Nothing of the report itself. The relay forwards it and
keeps two short keyed hashes: a counter, so one install cannot send more than a
dozen reports a day, and a marker that recognises the same report sent twice.
Neither can be turned back into your install ID, and both expire — see the table
below.

**Ratings do not go through us at all.** The prompt offers to open the game's
itch.io page in your browser, and any rating you leave there is between you and
itch.io under their privacy policy. Nothing about it is sent to this service,
and the prompt appears once — answering it or dismissing it stops it returning.

---

## Mail, and reporting someone

**Letters between players are not sent through us.** Mail in a multiplayer game
travels between your game and the server you are playing on, and we never see
it. What the server's owner keeps is up to them — they are an independent
controller for it, exactly as they are for chat and player names.

**Letters to an AI advisor are not sent through us either.** If you load the
language-model module, your letters go to whatever runner you configured — one
on your own machine, or an API you supplied a key for. That is between you and
that runner under its terms, and nothing about it reaches this service.

**Reporting someone is the exception, and it is deliberate.** If you report a
message to the account service, we store what is needed to judge it:

- the message you reported, and any surrounding lines you chose to attach
- your account id and nickname, and the reported account's
- why you say it was wrong, and anything you wrote to explain it
- which game it happened in, as your game reports it

There is no way around storing the message: a report nobody can read is a report
nobody can act on. It is readable only by an account carrying the **developer**
badge, and it is **deleted automatically after 90 days**.

Your identity as reporter is kept along with it. That is on purpose — a pattern
of reports against one person, or a pattern of false reports from one person,
is often the most useful thing in the queue, and neither is visible if reports
are anonymous.

**If somebody is banned or timed out, we post that to a Discord channel.** That
message says who and until when, and nothing else — never the message that was
reported, and never who reported it. A dismissed report is never announced at
all.

**How we can tell who was reported.** In a multiplayer game you never see
another player's account — only a pseudonym that means something on that server
and nowhere else. So that a report can be acted on at all, when we issue your
ticket to join a server we keep a note linking that pseudonym to your account
**for 30 days**.

This changes how long we keep something, not what we can see: issuing the ticket
is the moment we calculate the pseudonym from your account and that server, so
the connection exists at that instant either way. The note is never given to
servers, is not readable through any part of the service, and is used in exactly
one place — working out who a report is about. After 30 days it is gone, and a
report about a game older than that can no longer be connected to anyone. We
will tell you that rather than guess.

**Reporting to a server owner is separate and does not involve us.** That report
goes to whoever runs that game, who can remove you from it. They cannot ban your
account; only the account service can, and only through the process above.

---

## How long things are kept

| Thing | Kept for |
|---|---|
| Your account | Until you delete it |
| A pending sign-in | 10 minutes |
| A completed sign-in waiting to be collected by your game | 5 minutes |
| A join ticket | 120 seconds |
| A deleted account's nickname, held so nobody can take it and speak as you | 30 days, then released |
| If a **banned** account is deleted: a keyed hash of its linked provider IDs | 1 year, then released |
| A bug report or suggestion | Not stored by the relay at all; it is forwarded and forgotten |
| The keyed hash counting one install's reports for the day | 2 days |
| The keyed hash that recognises a duplicate report | 6 hours |
| A report about another player, including the message reported | 90 days |
| How many reports one account has filed today | 2 days |
| A note linking a server pseudonym to your account, so reports can be acted on | 30 days |

The 30-day nickname hold stores a nickname and a date, and nothing that links it
back to you or your deleted account.

The one-year hold applies **only if your account was banned** when you deleted
it, and exists so that deleting and re-registering is not a two-click way to
clear a ban. What is kept is a keyed hash of your user ID at that provider and
nothing else -- no name, no nickname, no account. Nobody, including us, can turn
it back into a person; it can only be compared against the same provider ID if
it signs up again. If you were not banned, nothing is kept and you are welcome
back immediately.

We rely on legitimate interests for that one item (GDPR Art. 6(1)(f), and the
Art. 17(3) carve-out for claims and abuse prevention). Everything else is
deleted outright.

---

## Legal basis

We process this data to provide the account service you asked for — GDPR Article
6(1)(b), performance of a contract. There is no advertising, no profiling and no
automated decision-making.

We do not sell or share personal information, and we have not done so in the
preceding 12 months. Under the CCPA/CPRA there is nothing to opt out of, because
there is no sale or sharing to opt out of.

---

## Your rights

You can exercise all of these from the game, or by writing to
**opendoctrines@gmail.com**. We answer as an individual maintaining a free project, not
a company with a support desk, so allow a few days.

- **See what we hold** (GDPR Art. 15, CCPA right to know) — the game's account
  screen has an export button. It returns every field we store, in JSON,
  including the hashed provider IDs.
- **Take it elsewhere** (Art. 20) — the same export.
- **Correct it** (Art. 16) — change your nickname. Once every 7 days, to stop
  name-churn being used to dodge blocks.
- **Delete it** (Art. 17, CCPA right to delete) — the account screen has a
  delete button. It asks you to confirm, tells you exactly what goes, and then
  removes your account, every linked sign-in method and your badges. It cannot
  be undone. Being banned does not take this right away; see the one-year hash
  above for the one thing that survives in that case.
- **Withdraw a linked provider** — unlink it, as long as it is not your last way
  to sign in.
- **Object or complain** — write to us, and if we do not resolve it you have the
  right to complain to your local data protection authority.

We do not charge for any of this and we do not require you to identify yourself
beyond being signed in.

---

## Who else is involved

- **Cloudflare, Inc.** hosts this service (Workers, Workers KV, Durable
  Objects). They process the data on our behalf.
- **Google, Discord or GitHub** — whichever you chose to sign in with. Your
  dealings with them are covered by their own policies. We tell them nothing
  about you; the flow only goes the other way.

There are no analytics, no advertising networks, no third-party scripts and no
cookies on any page this service serves.

---

## Children

This service is not for children under 13, or under the minimum age of digital
consent where you live if that is higher. We do not knowingly hold data for
anyone below it. If you believe a child has created an account, write to
**opendoctrines@gmail.com** and we will remove it.

---

## Changes

If this policy changes materially, the version served at `/privacy` changes with
it and the game shows the new one before you next sign in. The date at the top
is authoritative.
