# Running a tournament

A tournament is a **live** multiplayer game (chat, diplomacy, the map updating as
turns resolve, players reconnecting whenever they like) whose turns are long: a
day each, for a month or two. It is not play-by-post. A dedicated server holds
the game the whole time, and players open the game whenever it suits them.

This page covers where to run that server for free, how to set it up, and what
keeps it from breaking over weeks of play.

## What makes a long campaign safe

The server treats a power cut as an ordinary event:

| What could go wrong | What happens now |
|---|---|
| The machine is switched off or crashes mid-turn | On restart it reloads the same world, goes straight back into the game, re-holds every seat, and restores orders already submitted (`<save>.odorders`). |
| The turn deadline passes while it is off | The deadline is a wall-clock time (`TurnClock.h`). On return the overdue turn gets `resume-grace-seconds` (default 2 minutes) for people to reconnect, then resolves once. Three missed days resolve one turn, not three. |
| It restarts with time left on the turn | The turn keeps exactly the time it had. A restart gives nobody extra. |
| Players saved the invite code | The server asks the account service to **reopen the same code** (`reopen` on `POST /session`). The code is kept with the campaign. |
| The session descriptor expires (24h) | The server renews it every 12 hours while running. |
| The host's sign-in expires (12h) | The server refreshes its token every 3 hours (`/auth/refresh`). A restart within 12 hours of the last refresh signs in by itself. |
| The relay connection drops | It reconnects with backoff (2 s up to 60 s); relayed players come back through the normal reconnect. This used to end the server with exit code 4. |
| A player's connection drops | Their game reconnects by itself (2, 4, 8, 16, then every 30 seconds) with a banner, and their country is kept. While disconnected, End Turn will not resolve the turn locally. |
| Hundreds of spectators drop in over the month | Only the last 32 who left are remembered. Players are never forgotten. |
| The world sent to a late joiner grows each turn | It is deflated for clients that can read it, which is several times smaller. |

`tests/campaign_restart_test.sh` checks all of the restart cases against the
real server binary, killing it with SIGKILL twice. `tests/campaign_soak_test.sh`
plays 120 turns with visitors joining and leaving, and checks memory, roster
size and the late-joiner world size. In that run memory stayed at about 300 MB
from turn 20 to turn 120.

## Where to run it, for free

There is no free host that is always on, has enough memory, and needs no card.
So the setup is built to run in more than one place and hand off between them.

### 1. Your own computer (simplest)

```bash
cmake --build build --target OpenDoctrinesServer
tools/host_campaign.sh install ~/od-campaign
```

This sets the server to start at login and restart if it stops: a LaunchAgent
under `caffeinate` on macOS, a systemd user unit on Linux. On Windows, create a
Task Scheduler task "At log on" running `OpenDoctrinesServer.exe --config
<dir>\server.json --data <dir>\data`, with "Restart if the task fails" enabled.

Edit `~/od-campaign/server.json` before the first start:

```jsonc
"map": "1914",
"turn-seconds": 86400,          // a turn a day
"turn-at": "18:00",             // due at 18:00 UTC every day
"relay": true,                  // no port forwarding; players join with the code alone
"voice-link": "https://discord.gg/your-invite",
"auto.start-at-players": 6      // or type `start` at the console
```

While your computer is off, players cannot connect. Nothing is lost: deadlines
that pass while it is off resolve a couple of minutes after it comes back.

### 2. Always on: Render's free tier, plus a private git repo

For a game that keeps moving while your computer is off.

1. **Create a private repository** (GitHub, GitLab, anything git) and a token
   that can push to it. A GitHub fine-grained token with read and write access
   to "Contents" on that one repository is enough.
2. **Seed it from your computer**, once you have hosted from the game or run
   the server locally at least once (that is what creates your server
   credential):
   ```bash
   OD_DATA_DIR=~/od-campaign/data \
     tools/campaign_sync.sh init https://x-access-token:<TOKEN>@github.com/you/od-campaign.git
   ```
   The script refuses a GitHub repository that is public. It holds your session
   token and server credential. Those can open sessions as you, but cannot join
   games as you or touch your account.
3. **On Render**: create a Web Service, choose Docker, point it at your fork,
   set Dockerfile path `deploy/server/Dockerfile`, context `.`, plan **Free**,
   health check path `/healthz`. `deploy/server/render.yaml` has the same
   settings as a Blueprint. Set `OD_CAMPAIGN_REPO` to the URL above, plus
   `OD_TURN_AT` and `OD_VOICE_LINK` if you want them.
4. **Keep it awake.** A free Render service sleeps after 15 minutes with no
   traffic. Point a free uptime monitor (UptimeRobot, cron-job.org) at
   `https://<service>.onrender.com/healthz` every 5–10 minutes.
5. Players join with address `wss://<service>.onrender.com` (or just
   `<service>.onrender.com`) and the invite code.

The server pushes the campaign to the repository after every turn and within a
minute of orders arriving, and restores it on every start. A restart costs a
minute, not the campaign.

Limits, stated plainly:

- **512 MB of memory, which is now plenty.** The dedicated server never draws,
  so it reads each map layer a row at a time straight into compact form (a
  2-byte province index instead of a 128 MB image, a 1-bit land mask instead of
  another) and skips everything that only shades the screen. Measured:

  | | before | now |
  |---|---|---|
  | startup peak, 1914 map | 434 MB | 126 MB |
  | startup peak, full modern map | 422 MB | 129 MB |
  | during play (120 turns, 1914) | 290–470 MB | 94–107 MB |

  The game is unchanged: `tests/campaign_det_test.sh` plays 40 turns on a
  fixed seed and the per-turn fingerprints (every owner, army and treasury)
  match the old server's exactly. The soak test fails if memory ever passes
  256 MB.
- **Shared CPU.** Turn resolution is slower than on a desktop. With turns a day
  long, that doesn't matter.
- Render's free plan is Render's to change. If it changes, the same container
  runs on any Docker host, and option 1 always works.

### 3. Both

Set `OD_CAMPAIGN_REPO` on your computer too (`OD_CAMPAIGN_REPO=... tools/host_campaign.sh install ...`)
and the campaign moves to whichever machine starts it. A heartbeat in the
repository stops two machines running it at once: a second one refuses to start
while the first was alive in the last 15 minutes. `OD_SYNC_FORCE=1` overrides
that, for a machine that died without releasing it.

### Options considered and rejected

- **Hugging Face Spaces**: Docker Spaces need a paid plan as of 2026.
- **Koyeb**: card pre-authorisation since February 2026; it also scales to zero.
- **Fly.io, Railway**: no free tier for new accounts.
- **Oracle Cloud / Google Cloud free VMs**: these are VPSes, which you ruled
  out, and they want a card.
- **GitHub Actions as the server**: it would run, but using Actions for
  something unrelated to building the project is against its terms, and the
  repository at risk would be the game's own.
- **Cloudflare (where the account service runs)**: Workers cannot hold a world
  that needs ~300 MB, and Containers are a paid feature.

## The one-time account-service step

Reopening a saved invite code needs the account service changes in `net/`
(`reopen`, and long-form grace for long sessions), which must be deployed:

```bash
cd net && npm test && npx wrangler deploy
```

Until then a restarted server gets a **new** code. The console and the host
screen say so, and players need the new code.

## Running it

The server console (stdin, or the log on a platform):

| Command | Does |
|---|---|
| `status` | state, turn, time left, players, code, voice link |
| `deadline` / `deadline +2h` / `deadline -30m` / `deadline now` | show or move the open turn's deadline; everyone is told |
| `seat <player> <country>` | give a player or spectator a country (by name, ISO code or id), in the lobby or mid-game |
| `unseat <player>` | make a player a spectator; their country plays as absent until reseated |
| `release <player>` | free the seat held for someone who is not coming back |
| `kick`, `ban`, `unban`, `banlist` | as before |
| `voice <link>` / `voice off` | change the voice chat link for everyone, now |
| `step-go` | resolve the open turn now |

In the game, a hosting player has the same controls. In the lobby, each
person's row has **Give country / Change...**. In game, the turn panel has
**Host tools**, with kick, ban, release and seating a spectator.

## Tor: hosting and playing without trusting anyone in the middle

Every other way to host trusts somebody: Cloudflare's relay or tunnel, or a
forwarded port that shows your IP to every player. Every way to join shows
the player's IP to the host. Tor removes both.

**Hosting as an onion service.** Install Tor on the server machine
(`brew install tor`, `apt install tor`) and set `"tunnel": "tor"` in
`server.json`. On the host screen, tick *Publish as a Tor onion service
instead*. The game starts its own `tor`, publishes the game port, and prints a
`ws://….onion` address. The onion's keys live in `data/onion-service/` beside
the campaign, so the address survives restarts. Back that folder up with the
campaign. Releases carry the Tor Project's own `tor` in `data/tor/` (see
*Tor ships with the game* below); a source build uses an installed one. The
game never downloads Tor at run time.

**Testing it for real.** `tests/tor_live_test.sh` publishes a real onion over
the real Tor network, joins it through a second Tor, then joins a third time
with no Tor running at all and only the release bundle, so the game has to
start its own. It needs the internet and a few minutes, so it is not in
`run_all.sh`. Releases get the bundle from `tools/fetch_tor.py`
(`package.py --tor`), which refuses anything whose sha256 is not pinned in
`tools/data/tor_bundles.json`. Arm Linux has no Expert Bundle, so players
there use an installed `tor`.

**The price, stated to players before they commit:**

- **Every player goes through Tor** to reach an onion. They do not have to
  install or start anything: a release carries the Tor Project's own `tor`
  (the Expert Bundle, in `data/tor/`), and the game starts it the moment a
  connection needs it -- on a port of its own, with its state in
  `data/tor-client/`, and it exits when the game does, crash included. A Tor
  already running (the `tor` service on 9050, Tor Browser on 9150) is used
  instead. A source build has no bundle and uses an installed `tor`.
- **It is slower.** A first connection can take 10–30 seconds while Tor
  builds a route. Turns of a day long do not care.
- **Some players will not be able to connect at all.** Some countries,
  universities and workplaces block Tor.

**Playing with your IP hidden.** Any player can tick *Hide my IP: connect through
Tor* on the join screen (desktop builds). Everything the game connects to then
goes through Tor, sign-in included. The host sees a Tor relay instead of your
address, and the IP warning is not shown because it no longer applies.
Hostnames are resolved by Tor, never by your own DNS. The setting is saved
(`torRouteAll` in `config.json`). `torSocksPort` picks a port other than
9050/9150.

How it is tested: `build/Socks5Test` runs a stand-in Tor (a SOCKS5 server on
loopback) and checks that a WebSocket to an onion opens through it, that Tor
was given the hostname and not an address, and that each of Tor's refusals
becomes a sentence a player can act on. The Tor network itself cannot be in
a test suite. To try it for real: install Tor on two machines, host with
`"tunnel": "tor"`, and join the printed address from the other.

## Voice

The game carries no audio and never will. A voice service brings moderation,
privacy and legal obligations of its own. Instead the host sets `voice-link`
(or fills in **Voice chat link** on the host screen), and every player gets a
**Join voice chat** button in the lobby and on the turn panel. It opens the link
in their browser and shows the domain first. Only `https://` links are accepted,
and a client drops any other link it is sent.

## For players

- **Your servers** in the multiplayer menu is now one click: pressing a saved
  server joins it. The IP warning is asked once per server address and
  remembered, and asked again only if the address changes. The `...` button
  opens the join form to paste a new code; `x` forgets the server.
- Countries are chosen in a picker with a search box and a clickable map of
  the world as it stands in the host's game.

## Tests

```bash
cmake --build build --target OpenDoctrinesServer CampaignClient TournamentTest
build/TournamentTest                         # the pure rules, 86 checks
tests/campaign_restart_test.sh build         # SIGKILL twice; needs node
tests/campaign_soak_test.sh build 120        # 120 turns; needs node
cd net && npm test                           # reopen and long-form grace
```

### A week, for real

`tests/campaign_week.py` hosts a campaign for a full week of wall-clock time,
with made-up players: one punctual, one who plays in the evenings, one on a
flaky connection, one who claims a country and never returns, one who
disappears for most of a day at a time, and visitors who drop in to watch.
Throughout, it cuts the server's power at random and has the relay hang up on
it. It fails on any of these:

- the server dying by itself;
- memory over 256 MB;
- a turn number repeated, skipped or resumed wrong;
- a turn running past its deadline while the server was up;
- a player handed someone else's country;
- a player unable to get back in;
- a late joiner's world over 4 MB;
- a turn not saved;
- the session never renewed, or the token never refreshed.

It writes `report.md`, `metrics.csv` and `events.log` as it goes.

**On CI it is one leg, not a week.** `.github/workflows/week-soak.yml` runs on
each release tag (`v*`) for about five hours, with ten-minute turns and
everything compressed: a power cut every ~35 minutes, a relay drop every ~25,
players' schedules at 0.15x, so the day-long absentee comes back inside it.
That catches most of what a week would. It cannot catch what only appears
after days (a 24-hour expiry, slow growth crossing a limit on day five).

A full week is opt-in from the Actions tab (hours = 168): a GitHub runner lives
six hours, so it chains ~33 five-hour legs, each starting the next, and the
handover is one more power cut. It is not the default because that is ~170
runner-hours per release, and GitHub's terms let them call that
disproportionate load. Hosting real games on Actions is not allowed at all. To
run the week on a machine of your own instead:

```bash
python3 tests/campaign_week.py --build build --state ~/od-week --hours 168
# a ten-minute rehearsal of the same thing:
python3 tests/campaign_week.py --build build --state /tmp/od-rehearsal --hours 0.2 \
    --turn-seconds 30 --kill-every 3 --relay-drop-every 2 --visit-every 1.5 \
    --renew-seconds 120 --time-scale 0.01
```

The rehearsal passed here, over two legs: 20 turns, 4 power cuts, 4 relay
drops, 25 reconnections and 7 visitors, with every player keeping their
country. Setting an impossible memory limit made it fail and say why.

`tests/run_all.sh` runs the first, plus the restart test and a 60-turn soak
when node is installed. `build/PngRowsTest data/` checks the row-by-row map
decoder against stb_image on every shipped map, byte for byte. To prove a
server change plays the same game, fingerprint 40 turns with the old binary
and the new one and diff the files:

```bash
tests/campaign_det_test.sh build 40 /tmp/old.txt /path/to/old/OpenDoctrinesServer
tests/campaign_det_test.sh build 40 /tmp/new.txt
diff /tmp/old.txt /tmp/new.txt
``` `tests/campaign_client.cpp` is a headless player that
prints one line per event, for scripting anything else against a real server.
