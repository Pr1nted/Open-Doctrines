# OpenDoctrines accounts — terms of use

**Last updated:** 2026-07-28

> This file was drafted by looking at the code, not by a lawyer. It is accurate
> about the software; it is not legal advice. If you are relying on it for a
> commercial release, have someone qualified read it first.

These terms cover the **account service** only — the thing that gives you a
nickname and lets other players' servers check that you are you. They do not
cover the game itself, which is free software you run on your own machine, nor
anybody else's game server.

Questions: **opendoctrines@gmail.com**

---

## The short version

- The account service is free, run by hobbyists, and may be switched off.
- Your nickname is yours while you use it. Don't impersonate people.
- Game servers are run by other players. We don't run them and can't police
  them.
- The game connects to those servers directly, so **the host sees your IP
  address**. That is a property of the design, not an oversight.
- The game reports nothing about how you play unless you switch that on.
- Nothing here takes away rights your local law gives you.

---

## What this service is

A small service that does three things:

1. Lets you sign in with Google, Discord or GitHub, so a nickname can be yours.
2. Issues short-lived **join tickets** proving to a game server that the
   nickname is yours.
3. Optionally lists games whose hosts asked to be listed.

It is **never** in the path of a game. Turns, orders and chat travel directly
between you and the host, or through a tunnel the host chose. We could not read
your game traffic if we wanted to, because it does not come to us.

## Using it

You may use the account service to play OpenDoctrines. In return:

- **Don't impersonate anyone.** A nickname that pretends to be another player,
  a public figure, or the OpenDoctrines project itself may be taken back.
- **Don't automate abuse.** Scripted account creation, ticket farming, or
  hammering the service hard enough to affect other players.
- **Don't use it to break the law**, or to harass people.
- **Don't use a second account to get round being removed.** More than one
  account is fine — households share computers, and people keep a separate
  identity for good reasons. Using one to return after a host kicked you, or
  after we suspended an account, is not.

We should be straight about the limits of that last rule: pseudonyms here are
**pairwise**, so we cannot tell that two accounts belong to one person, and we
have deliberately built it that way. It is a rule we can act on when it is
reported and evidenced, not one we can detect.

Badges (like `developer`) are granted by us and are not yours to claim. The game
displays unrecognised issuers' badges as unverified for exactly this reason.

## Your account

You can change your nickname, unlink a provider, or delete the account entirely
from within the game. Deletion is real: the record is removed. See the
[privacy policy](/privacy) for what is stored and for how long.

We may suspend or remove an account that breaks the rules above. If we do it to
yours and you think we got it wrong, email us — a person will read it.

## Other people's game servers

**We do not run game servers.** Anyone can host, and when you join one you are
connecting to a stranger's computer.

- The host sees your IP address. The game says so before you connect, and it
  cannot be avoided without putting a relay in the middle, which this project
  deliberately does not do.
- The host controls the rules, the map, and any mods. Mods run in a sandbox,
  but a sandbox is a boundary, not a promise about intent.
- What other players say and do is theirs, not ours. Report serious problems to
  us and we can act on the *account*; we cannot act on their server.

If a game is listed publicly, that listing is provided by the host. We do not
vet it.

## Content you provide

Nicknames and game names you type are shown to other players. Keep them
civil — there is a profanity filter, and it is not a substitute for judgement.
You keep whatever rights you have in what you write; you give us permission to
show it to other players, which is the whole point of typing it.

## Publishing a mod

The mod directory lists mods; it does not hold them. You publish a description
and a link, and the file stays on your own host. Before your first listing you
agree to the modding guidelines, which are short and are served at `/mods/guidelines`.

- **What you list must be yours to list**, and the download link must fetch the
  mod you described and keep doing so.
- **We do not check that a mod works, is any good, or is safe.** Nobody here has
  run it. A security check is shown where we have one, and a "clean" result
  means nobody has reported those bytes — not that the mod is safe.
- **Downloading is at your own risk**, exactly as it would be from any other
  site. The game will not install a mod by itself, from here or anywhere.
- **We can take a listing down**, and we can withdraw an account's ability to
  publish, without withdrawing anything else about the account. Somebody who
  should not be publishing is not automatically somebody who should not be
  playing.

You keep every right you have in your mod. Listing it gives us permission to
show what you wrote about it and to count how many people followed your link.

## What other players say

**We do not moderate what players write to each other, and we are not
responsible for it.** Mail and chat travel between your game and the server you
are on. We never see them, we do not store them, and we cannot read them. What
another person writes to you is theirs, and the responsibility for it is theirs.

If somebody behaves badly you have two separate routes, and they do different
things:

- **Report it to the server owner.** They can remove that person from their
  game. They cannot touch the person's account. Most problems belong here.
- **Report it to us.** We can ban or time out the *account*, everywhere. We
  cannot do anything about a particular server. Use this for behaviour serious
  enough to be a problem beyond one game.

Reporting to us means sending us the message, which we keep for 90 days so it
can be judged — see the privacy policy. We look at reports and decide; we do not
promise a timescale, an outcome, or an explanation, and we may decide a report
needs no action.

**Bans are ours to decide.** We may ban or suspend an account for conduct
towards other players, and we may do so without a report if we see it ourselves.
If your account is banned you may say so to us; there is no formal appeal.

## AI advisors, and what they say

If you load the language-model module, some countries write to you as
correspondents. **They are machines, they are labelled as machines in every
message, and they are allowed to lie to you** — deceit is part of diplomacy and
that is the point of them.

Nothing an advisor writes is advice, a statement of fact, or anything we assert.
It is generated text in a game about lying to each other. The model you run is
one you chose and installed, we do not supply it, and we are not responsible for
what it produces.

## If you are a host

Running a game makes you responsible for what happens in it. You choose whether
mail is on and who may use it, you can maintain a list of words your server will
not carry, and you can remove people. If you enable the age prompt, note what it
is: a local, self-declared question, and **not** a verified age check — see the
in-game text, which says so plainly.

## Measuring how much the game is played

The game does not report anything about how you play unless you turn that on in
Settings. It is off when you install it, off after every update, and turning it
off stops it at once.

If you do turn it on, what is sent is one message per play session saying
roughly how long it lasted — as one of five ranges — and whether you were on
web, desktop or Android. Nothing in it identifies you or your installation, and
nothing links two of them together.

Because of that, **we cannot delete "your" reports on request, and we do not
pretend to be able to.** Every report expires by itself after ninety days, and
if you want the whole set gone before then, ask and we will erase all of it.
The privacy policy sets this out in full.

We also count things the service does on its own: how many multiplayer sessions
are opened, how long they run, how many people join them. Those are records of
our own service running, they contain no identifiers either, and they are
published in aggregate at `/stats`.

## Availability, and the honest bit

This is a **free service run by a small project on a free hosting tier**. It may
be slow, may be down, and may be discontinued. There is no uptime promise, no
support commitment, and no guarantee your account survives a catastrophic
mistake on our end. We keep backups where we sensibly can.

If we shut the service down, we will try to say so in advance in the game and on
the repository. Games already running keep working — they don't need us — but
new players could not get a ticket to join.

## Liability

To the extent the law allows, the account service is provided **as is**, without
warranty. We are not liable for lost game progress, for what happens on somebody
else's server, **for what other players say or do**, for what a language model
you installed produces, or for anything arising from your use of the service.

Nothing here limits liability that cannot lawfully be limited — including for
death or personal injury caused by negligence, or for fraud. If you are a
consumer, your statutory rights are unaffected.

## Changes

We may update these terms. The "last updated" date at the top changes when we
do, and material changes will be mentioned in the game. Continuing to use the
service after a change means you accept the new version; if you don't, delete
the account.

## Law

These terms are governed by the law of the place the project is operated from,
and nothing in them prevents you bringing a claim in your own country if your
local consumer law gives you that right.
