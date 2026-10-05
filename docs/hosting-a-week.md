# Hosting a tournament 24/7 for a week, on machines you control

This is the step-by-step. `docs/tournaments.md` explains why it works the way it
does; this page is what to type.

No machine of your own that stays on? A free Oracle Cloud VM does it, language
model included: see [hosting-on-oracle.md](hosting-on-oracle.md).

You need **one machine that stays on** for the week: a home server, a Mac mini, an
old laptop with the lid open, a Linux box, a friend's PC. The server uses about
130 MB of memory and a few hundred MB of disk, and any CPU from the last ten
years is fast enough for a turn a day. A **second machine is optional**: it is
the backup that takes over if the first one dies.

---

## 1. Get the server onto the machine

Either download `OpenDoctrinesServer` for your platform from the project's
Releases page (the `server-v*` releases) — there is one for every system and
architecture the game ships on, including a Raspberry Pi (`linux-arm64` on a
64-bit OS, `linux-armv7` on a 32-bit one) — or build it:

```bash
git clone https://github.com/Pr1nted/Open-Doctrines.git
cd Open-Doctrines
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target OpenDoctrinesServer -j4
```

The server finds its maps in `data/` next to the build directory, so keep it inside
the checkout.

## 2. Give it your identity as a host

A server hosts **as your account**, and the server credential decides who every
player is on it. Both come from your own game:

1. On your normal computer, open the game, sign in (Main menu → Account), and
   host any game once (Multiplayer → Host). This registers your server
   credential.
2. That leaves two files in your game's `data/` folder: `account.json` (your
   sign-in) and `config.json` (which now contains `serverCredential`).

**If the server machine IS that computer**, there is nothing to copy; step 4 picks
them up.

**If it is another machine**, carry them over through a private git repository.
That repository also becomes the campaign's backup (step 6):

```bash
# on your computer, once
OD_DATA_DIR=~/path/to/game/data \
  tools/campaign_sync.sh init https://x-access-token:<TOKEN>@github.com/you/od-campaign.git
```

Make the repository **private**; the script refuses a public one. The token is a
GitHub fine-grained token with write access to "Contents" of that repository only.

> Never register a fresh credential for a running campaign. A new credential
> makes every returning player a stranger, and all seats are lost.

## 3. Decide how players reach it

| Option | What players type | Trade-off |
|---|---|---|
| **Relay** (recommended) | nothing but the invite code | Nothing to open on your router. Cloudflare relays the bytes, and players never see your IP. |
| Cloudflare quick tunnel | the `wss://…trycloudflare.com` address + code | Needs `cloudflared` installed. The address changes when the server restarts. |
| Forward a port | `your.ip:27015` + code | Players see your IP and you see theirs. |
| **Tor onion service** | the `….onion` address + code | Nobody in the middle and nobody sees anybody's IP, but every player goes through Tor (the game starts it for them), it is slower, and some networks block Tor. |

## 4. Set it up

```bash
tools/host_campaign.sh install ~/od-campaign
```

The first time, this creates `~/od-campaign/` with a `server.json` and copies
your identity in. It does **not** start anything yet, because the first start
creates the world from whatever `server.json` says. **Edit
`~/od-campaign/server.json`:**

```jsonc
"name": "Weekly war",
"map": "1914",
"turn-seconds": 86400,            // a turn a day...
"turn-at": "18:00",               // ...due at 18:00 UTC
"max-players": 12,
"assignment": "players",          // or "host": you hand countries out
"late-join": "spectate",
"absent": "ai",                   // the AI plays anyone who doesn't submit
"relay": true,                    // option A; or "tunnel": "cloudflared" / "tor"
"voice-link": "https://discord.gg/your-invite",
"auto.start-at-players": 8,       // or start it yourself with the `start` command
"auto.end-at-turn": 7,            // a week of daily turns
"auto.return-to-lobby": false
```

Then run the same command again to start it for real:

```bash
tools/host_campaign.sh install ~/od-campaign
```

That installs a service that starts at boot and restarts the server whenever it
stops. On macOS it's a LaunchAgent that keeps the Mac awake; on Linux it's a
systemd user unit, and you should also run `loginctl enable-linger $USER` so it
keeps running while you're logged out. Running `install` again later restarts it
with changed settings; the campaign folder is kept.

**Windows:** create a Task Scheduler task, triggered "At startup", running
`OpenDoctrinesServer.exe --config C:\od-campaign\server.json --data C:\od-campaign\data`,
with "If the task fails, restart every 1 minute" and "Run whether user is logged
on or not". The rest of this page applies unchanged.

## 5. Check it before anyone joins

```bash
tools/host_campaign.sh cmd ~/od-campaign status
```

You should see `State: lobby`, an invite code, and the turn schedule. Then, from
**another network** (a phone hotspot is fine), open the game and join with the
code. That's the only test that proves strangers can get in. Pick a country,
leave, rejoin: you should come back to the same one.

Tell the players: the invite code (they add it under *Your servers* and
afterwards it's one click), the turn time ("due 18:00 UTC daily"), the voice
link, and, if you chose Tor, that the first connection takes a minute and some networks block it.

## 6. Make it survive the machine (optional, recommended)

With a campaign repository from step 2, set it on the service so the server backs
up after every turn:

```bash
OD_CAMPAIGN_REPO=https://x-access-token:<TOKEN>@github.com/you/od-campaign.git \
  tools/host_campaign.sh install ~/od-campaign
```

A **second machine** can now take over. Set it up the same way, but leave its
service stopped. If the first machine dies, start the second: it pulls the
campaign, keeps the same invite code (on the relay), and players reconnect to
their own countries. A heartbeat in the repository stops both machines running
the campaign at once. A machine that died without saying so is overridden with
`OD_SYNC_FORCE=1`.

## 7. During the week

Everything is one command from the server machine:

```bash
tools/host_campaign.sh cmd ~/od-campaign status              # turn, time left, who's here
tools/host_campaign.sh cmd ~/od-campaign list                # players, countries, orders in
tools/host_campaign.sh cmd ~/od-campaign say Turn 3 is up, diplomacy open
tools/host_campaign.sh cmd ~/od-campaign deadline +2h        # give everyone two more hours
tools/host_campaign.sh cmd ~/od-campaign seat Weimar1920 Ottoman   # a spectator takes over a country
tools/host_campaign.sh cmd ~/od-campaign unseat Bob          # Bob stops playing; the AI covers
tools/host_campaign.sh cmd ~/od-campaign release Bob         # free a seat held for someone gone
tools/host_campaign.sh cmd ~/od-campaign kick Bob being rude
tools/host_campaign.sh log                                   # watch the log
```

The same controls exist in game if you also connect as a player: *Host tools* in
the turn panel.

**Reboots, power cuts, crashes:** do nothing. The service starts the server again.
The campaign resumes at the same turn with the same deadline and the orders
already sent. A deadline that passed while it was down resolves two minutes after
it is back. Players see "Reconnecting…" and are put back into their country.

**Updating the game mid-week:** `git pull`, rebuild, `tools/host_campaign.sh uninstall && install`.
Saves and seats carry over. Tell players to update at the same time; a client and
server speaking different network versions refuse each other with a message
saying so.

## 8. When it's over

The last turn ends the game (`auto.end-at-turn`). The world is in
`~/od-campaign/data/saves/`, and anyone can open it in the game's history screen
to replay the week. Stop the service with `tools/host_campaign.sh uninstall`.

---

## If something is wrong

| Symptom | Look at |
|---|---|
| Nobody can join | `cmd status` shows the code once the session is open; the log says `session open. Join code: …`. Tunnel: `address:` in the log. Port: is it forwarded, and `bind-all: true`? |
| "The invite code changed" in the log | The account service would not reopen the old code (it was gone more than 90 days, or the service has not been updated). Share the new code. |
| Joining via Tor fails | The error says how far Tor got and what it last said. 0% usually means the network blocks Tor; a source build with no `tor` installed says so. |
| A player says they got the wrong country | That shouldn't be possible: seats are tied to their account. `cmd list` shows who holds what. Report it with the log. |
| The log says "could not refresh the account token" | The service can't reach the account service. The game keeps running. If it lasts more than 12 hours, sign in again on your computer and copy `account.json` over (or `campaign_sync.sh init` again). |
