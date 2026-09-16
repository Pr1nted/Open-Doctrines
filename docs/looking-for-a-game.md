# Looking for a game

Multiplayer worked and nobody could find anybody. An invite code is a fine way
to play with people you already know and no way at all to meet anyone, so the
honest answer to "I want to play this with humans" was "go to Discord, and hope
somebody is posting". This is the other half: a board where people say *I am
hosting, here is the code* and *I am looking for a game*, drawn in the game and
mirrored into `#looking-for-a-game` on Discord.

**It is one board with two windows onto it.** A listing posted in the game
appears in the channel; a listing posted in the channel with `/lfg` appears in
the game. Not two boards that resemble each other — half a dozen people are not
enough to fill two rooms, and a board that is always empty because everyone is
in the chat teaches players to stop opening it.

## What a listing is

| | |
|---|---|
| **Tag** | hosting a game, or looking for one. Nothing else. |
| **Map** | free text, 48 characters |
| **Pace** | rapid (30 s – 1 h a turn) or long-form (1 h – 1 week a turn) |
| **Seats** | hosting only: how many the game takes, how many are in |
| **Invite code** | hosting only, and *required* — the listing is useless without it |
| **Language, region** | optional, short |
| **Note** | optional, 240 characters, no links |
| **Lifetime** | 2 hours by default, 6 at most. It closes itself. |

One open listing per account. Posting again replaces the one you have, because
somebody whose game filled up and who started a new one should not have to
remember to take the old one down — and two listings from one account is the
shape spam takes here. Six listings a day, then the board says come back
tomorrow.

## The guidelines are the code

The four rules pinned in the channel are the four rules the validator enforces,
and they are shown above the form in-game rather than behind a link:

1. **Be respectful.** The nickname blocklist applies to the note, including
   letters spaced out or swapped for lookalikes.
2. **Post the parameters of the game you are hosting.** Map and pace are
   required; seats are required for a hosting listing.
3. **Open Doctrines games only.** The note refuses anything link-shaped —
   `http://`, `www.`, `discord.gg`, a bare `something.com`. A listing carries an
   invite code, not an address, so a link has nothing legitimate to do in one,
   and refusing the *shape* means nobody has to judge where a link points.
4. **Use the right tag.** A hosting listing must carry a code; a looking listing
   must not. That is checked rather than asked.

Enforced in `net/src/lfg/board.ts`, which is the only thing that decides, and
repeated in `src/net/Lfg.h` so a player is told which rule they broke while the
form is still in front of them. If the two ever disagree, the service wins and
its sentence is what the player reads.

## Reporting

Every listing has a Report button, in the game and in Discord, and both open a
box that requires words. A button that files "reported" and nothing else hands a
moderator a listing and no reason. Reports go to `MODERATION_DISCORD_WEBHOOK` —
the same channel ban notices already go to — and one account can report a
listing once, because a second report is not more evidence.

A developer-badged account gets `GET /moderation/lfg` (everything, hidden
included) and `POST /moderation/lfg` with `{op: "hide" | "show" | "purge", id}`.
Hiding a listing also closes its Discord message, so the two never disagree.

The report tells the reporter *"a moderator will look at it"* and never *"it has
been removed"*. The second is a lie that makes the next report feel useless when
nothing visibly happens.

## Where it is in the game

Multiplayer → **Looking for a game**, above Join and Host, because both of those
assume you already know who you are playing with. The hub row says how many
games are open, since "3 games open" is a reason to press and "browse the board"
is a chore.

Joining from the board goes through the same path as a code typed by hand,
including signing in first. There is no address field in a listing, so a listing
cannot point the game at a machine of the poster's choosing.

**The Discord invite is on this screen**, not buried in Community, because
somebody reading this board has just decided they want to play with people, and
that is the only moment the ask means anything.

A host sitting in an empty lobby gets **Find players for this game**, which
fills the listing in from that lobby — code, seats, pace, map — and shows it for
editing. That is where a host actually is when they want players, and asking
them to leave the page and retype what the lobby already knows is asking most
people to give up instead.

## The one ask, after a solo game

Leaving a single-player game to the menu offers the board once: *the AI does not
negotiate — people are playing right now*. It is a sibling of the itch.io rating
prompt and answered on the same terms.

- **After 25 minutes of play**, counted across the whole install rather than the
  session — below the rating prompt's 45-minute fallback, above its 10-minute
  moment. Somebody who opened the game, looked at the map and quit has not
  decided they like it enough to want company.
- **Asked once.** `mpInviteAsked` is set the moment it is *shown*, not when it
  is answered, and never cleared. A player who ignored it has answered, and the
  second answer is always worse than the first.
- **Never after a multiplayer game** — that player already knows where the board
  is — and never when no account service is configured.
- **It does not promise other people.** With games on the board it says how
  many; with an empty board it says a game is posted from the lobby, which is
  something this player can do rather than a claim that may not hold.

Why the menu and not mid-game: the rating prompt asks during play because what
it wants is an opinion, and an opinion forms while you are playing. This wants
somebody to go and find people, and asking that mid-game is asking a player to
abandon the game they are in.

## The parser is sealed

A listing is written by a stranger, fetched over the network and drawn on your
screen with a button that joins a game. So `src/net/Lfg.h` follows the same
rules as `src/net/Announcements.h`: it is data, every field is bounded, anything
unrecognised is dropped whole, a listing can never name a code path, and no
network means an empty board rather than an error.

`tests/lfg_test.cpp` is the proof, and it runs without a socket.

## Setting up the Discord side

The bot is an **HTTP-interactions app**: Discord POSTs to the Worker when
somebody runs `/lfg` or presses a button, and the Worker answers. There is no
gateway connection and nothing to keep running, so it costs nothing to host and
cannot go offline separately from the service.

The trade-off, stated plainly: an interactions-only app **cannot read ordinary
messages**. It sees `/lfg` and button presses and nothing else. So
`#looking-for-a-game` should be locked to the command — deny *Send Messages* for
@everyone in that channel and leave *Use Application Commands* on. That is not a
restriction working around a limitation; it is what makes the four guidelines
structurally true instead of a thing a moderator enforces by hand.

1. **Make the application** at <https://discord.com/developers/applications>.
   Note the **Application ID** and the **Public Key**; under *Bot*, copy the
   **token**.
2. **Invite it** to the server with the `bot` and `applications.commands`
   scopes, and *Send Messages* + *Embed Links* in the board channel.
3. **Set the Worker's secrets**:

   ```bash
   cd net
   npx wrangler secret put DISCORD_BOT_TOKEN
   npx wrangler secret put DISCORD_PUBLIC_KEY
   npx wrangler secret put DISCORD_LFG_CHANNEL_ID   # right-click the channel → Copy Channel ID
   npm run deploy
   ```

   All three are optional. With none of them set the board still works in the
   game and simply has no Discord half — the right behaviour for a fork that has
   not set one up. Nothing has to be disabled.
4. **Point Discord at the Worker.** On the application's *General Information*
   page set **Interactions Endpoint URL** to
   `https://<your worker>/discord/interactions`. Discord verifies it by sending
   a deliberately *bad* signature and expecting a rejection, so this only saves
   once `DISCORD_PUBLIC_KEY` is set.
5. **Register the command**:

   ```bash
   cd net
   OD_WORKER=https://<your worker> DISCORD_APP_ID=... DISCORD_BOT_TOKEN=... \
     DISCORD_GUILD_ID=<your server> npm run register-lfg
   ```

   With `DISCORD_GUILD_ID` it appears immediately in that one server; without
   it, globally, within the hour.
6. **Lock the channel** to `/lfg` and pin the four guidelines.

`/lfg` needs an Open Doctrines account, and it finds one by the Discord sign-in
the person already linked in the game (Account → Sign in with Discord). That is
the whole trick: no second identity, no second nickname to moderate, and a
rule-breaker is the same account whichever side they posted from. Somebody who
has not linked one is told how, in a message only they can see.

## Why a Durable Object and not KV

A listing is a *write*, and the free plan's 1,000 KV writes a day are the same
budget account creation spends. A board people actually used would spend that
budget and then start failing signups. Durable Object requests bill against
their own 100,000/day, the board is read as a whole, the per-account limit has
to be counted across listings, and expiry is one alarm instead of hundreds — so
one object, with SQLite, sweeping itself every sixty seconds.

An expired listing is worse than no listing: somebody joins a game that ended an
hour ago and concludes the board is dead. The sweep removes it *and* closes its
Discord message.
