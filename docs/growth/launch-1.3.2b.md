# Launching 1.3.2b — paste-ready

The multiplayer release: a hosted game you join is now the game the host is
playing, and you — not the server's AI — make your country's decisions. Built
for the tournament that runs on a day-a-turn server, and carrying the fixes
players reported in the meantime. Nothing here is posted by a workflow: the
announcement board needs the developer badge, and itch's devlog is
dashboard-only (`docs/itch/README.md`). Images are served from the repo through
jsdelivr:

    https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/<file>

---

## 1. itch.io devlog

**Where:** itch dashboard → **Devlog → New post**. Title below, body in HTML or
Markdown — itch accepts both.

**Title:** 1.3.2b — in a hosted game, you play your own country

**Body:**

Hello strategists!

The first evening on a 24-hour-turn server taught us two things at once: the
game you join was not quite the game the host was playing, and the host's AI was
playing more of your country than it had any business playing. This release is
mostly that — making a long multiplayer campaign the thing it was supposed to be
— and the rest is what you reported while we were at it.

### In a hosted game, you decide your own country

Your research. Your research and pacification budgets. The doctrines you enact
and repeal. The wars you declare, and the alliances, guarantees, pacts,
ceasefires and trades you propose. What you disclose, your minority policy, your
districts, your claims. All of it now reaches the server.

It did not before. The server's AI had been quietly making those choices for
every human player, on top of the orders you gave — so you moved your armies,
and something else ran your economy and your diplomacy. Now every one of those
decisions is yours, and the server checks each against the same rules the
screens use, so a hosted game plays by the rules a single-player one does.

### Requests come to you, not to the server's AI

A call to arms, an alliance offer, a ceasefire, a trade — anything aimed at your
country arrives as the popup you already know. Leave it unanswered and it lapses
as a polite decline: a call to arms you slept through does not cost you your
alliance. And if you would rather your country sat a turn out than had a stand-in
play it, the turn panel has **If I miss the deadline, do nothing** — the server
keeps that choice across a restart.

### You see the game the host is playing

Each turn now brings the wars and the treaties, the claims, the governments and
their flags, rebel states by their real names, and every treasury. Before, a war
declared on turn two reached the map and never the Relations screen, rebel states
showed up as "Rebel State 4", and your money stayed frozen at whatever it was the
moment you joined. **Show orders** works in a hosted game too now.

### The long campaign holds together

A server that restarts — every update, on a hosted box — used to forget which map
its world was on, and then everyone trying to join was told the world could not
be read. Fixed. Connecting from far away works: the desktop game gave the relay
twenty milliseconds to answer a secure connection, so anyone farther than that
simply could not join. And signing in on the web stays signed in — the browser
build never kept the session.

The lobby got its due as well: a saved server's card joins it when you click it,
the country picker no longer shuts the instant it opens, and chat wraps long
lines instead of cutting them off.

### The fixes you reported

- **A marching army keeps its kind.** Militia, assault and mechanised troops
  were arriving as plain line infantry the moment they stepped into a province.
- **The tutorial can be finished.** The recruiting lesson now opens the army
  view, where its slider and its Recruit button actually live.
- **A click stays on its screen.** A press on one screen no longer sets off
  whatever sits under the cursor on the next — leaving the mod menu used to start
  the tutorial.
- **Monuments cost what they are.** Each kind has its own running cost by level,
  and anger at a conquest now stays in the occupied province instead of souring a
  whole people across your empire.
- **Smaller things.** Interface scale survives switching between the flat map and
  the globe. Transports land on an island when you click the water beside it. Web
  saves are written whole or not at all. A map opened in the editor keeps its
  scripts, which exporting used to drop. A turn reaches the map a second sooner in
  a browser.

### Under the hood

Mods and hand-edited maps are checked more strictly now, after a fuzzer we wrote
for the mod API found three values that slipped past the guards. Your 1.3 mods
still run unchanged.

### Closing remarks

This is the release the tournament needed, and the tournament is next — watch
this space. If you host a week-long game of your own, tell us how it holds up:
the long campaign is young, and the edge we have not hit is probably in there
somewhere.

See you on the map.

---

## 2. In-game announcement board

*(Needs the developer badge. Keep it short.)*

**1.3.2b — in a hosted game, you play your own country**

In a multiplayer game, your research, budgets, doctrines, diplomacy, disclosure,
minority policy, districts and claims now reach the server — the host's AI is no
longer quietly running them for you. Requests (calls to arms, alliances,
ceasefires, trades) come to *you* as a popup, and lapse as a polite decline if
you miss them; set **If I miss the deadline, do nothing** to idle instead of
handing your turn to a stand-in. You now see the host's actual world — wars,
treaties, claims, rebel names, every treasury — and a restarted server can be
rejoined. Plus: a marching army keeps its kind, the tutorial can be finished,
monuments cost what they are, and leaving the mod menu no longer starts the
tutorial.

---

## 3. Discord

**#announcements:**

**1.3.2b is up — the multiplayer one.**

**You play your own country now.** In a hosted game your research, budgets,
doctrines, diplomacy, disclosure, minority policy, districts and claims finally
reach the server. Before this, the server's AI was making all of that for you on
top of your orders. Now it is yours, checked against the same rules the screens
use.

**Requests come to you.** A call to arms, an alliance, a ceasefire, a trade —
they arrive as the usual popup, and lapse as a polite decline if you miss them.
A call to arms you sleep through doesn't end your alliance. Want to idle instead
of letting a stand-in play? *If I miss the deadline, do nothing* on the turn
panel.

**You see the real game.** Wars, treaties, claims, governments and flags, rebel
states by name, every treasury — all of it reaches you each turn now, not just
the map. And a server that restarted can be joined again.

**Reported here, fixed here:** a marching army keeps its kind (militia and
mechanised stopped turning into line infantry); the tutorial can be finished;
monuments cost what they are and occupation anger stays local; leaving the mod
menu no longer starts the tutorial; the country picker no longer closes as it
opens; interface scale survives the globe; transports land on small islands.

The tournament runs on this. Details dropping very soon — **limited seats**, so
watch #announcements.
