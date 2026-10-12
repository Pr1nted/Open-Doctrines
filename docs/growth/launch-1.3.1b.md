# Launching 1.3.1b — paste-ready

A point release on top of the beta: the phone build, fog of war, and a
Cold-War scenario. Nothing here is posted by a workflow: the
announcement board needs the developer badge, and itch's description, devlog and
upload order are dashboard-only (`docs/itch/README.md`). Images are served from
the repository through jsdelivr:

    https://cdn.jsdelivr.net/gh/Pr1nted/Open-Doctrines@main/docs/img/<file>

The one thing to flip by hand that is not copy: on the itch **Edit game** page,
tick **Mobile friendly** under the browser embed options — the browser build
runs on a phone now.

---

## 1. itch.io devlog

**Where:** itch dashboard → **Devlog → New post**. Title below, body in HTML or
Markdown — itch accepts both.

**Title:** 1.3.1b — it runs on your phone, and nobody has to see the whole map

**Body:**

Hello strategists!

Two things in this one. The browser build plays on a phone now, which a lot of
you asked for by trying it and watching the tab die. And a world can be made
where nobody sees the whole map — including the AI.

### You can play it on a phone

The world map is one image, 8,192 by 4,096 pixels, and a phone browser ran out
of memory holding it next to everything drawn from it. So the tab died as the
map loaded. Two smaller problems rode along: the map came up black on some
phones, and the interface came up zoomed in too far to use.

The black maps and the zoom are fixed outright. For the memory there is a new
switch under **Settings → Experimental → Risky RAM**: the game holds the map in
a compact form about a quarter of the size, and an iPhone loads the full world —
all 1,642 provinces, every border, province selection, claims, the lot. Only
the GIF timelapse and turn rewind still want the full image, and the switch
says so.

It is called "Risky" on purpose. It is new, and we would rather you knew that
than found out.

### Fog of war

**Settings → Experimental → Fog of War worlds**, then tick it on a map's info
("i") popup. A world made that way is one where nobody sees the whole map.

You see your own provinces and a ring around them. You see wherever your armies
stand, at home or in somebody else's country. Your coasts watch the sea off
them; your ships carry their own circles, a carrier's much wider than a boat's.
Your allies and guarantors lend you their sight, one hop — an ally's ally lends
you nothing.

Everything else is remembered, as it was the last time anyone you share with
looked, with the turn written on it. A province taken behind the front stays
the old colour until somebody goes and looks.

A ninth research tab opens: coastwatchers, naval radar, reconnaissance
patrols, codebreakers, and counter-intelligence to stop the same being done to
you. From a foreign country's profile you can plant a spy network, which shows
you every province they hold for six turns, or steal their dossier, which opens
their books. Both cost money, both can fail, and a failure is noticed — far
enough into counter-intelligence and the target is told whose spies they were.
The diplomats remember that about you.

Two things to know before you start one. History, the timelapse and turn rewind
are off in a fog world: all three replay the truth turn by turn, which is the
entire thing the fog is hiding. And the AI plays a little weaker there — it is
held to the same fog you are, deciding from what it remembers, but the model
shipping with this release was trained on worlds where it could see everything.
Retraining it for the fog is the next job.

In multiplayer the host is the only machine holding the truth, and each seat is
sent only its own world. Nothing is broadcast. Spectators cannot join a fog
world at all, because two clients would be two windows onto the whole map.

### A new scenario: The Last Summer

August 1990. The Wall is down, the Warsaw Pact is a signature on a dead treaty,
and the Soviet Union still covers a sixth of the land. East Germany has six
weeks left as a country. The Baltics have declared an independence Moscow has
not granted. Iraq is in Kuwait and the coalition is still unloading. Yugoslavia
and Czechoslovakia are each, for now, one country.

Three fuses lit at once and none of them gone off. None of it had to end the
way it did.

### The fixes

- **Forts always cost full price.** Somebody asked in Discord why Cost
  Efficiency did not seem to make forts cheaper. It did — everywhere except the
  button they were using. The industry discount was cutting fort prices in bulk
  builds, in multiplayer and for the AI, while the fort button charged full. It
  also ran backwards: Fortress Doctrine carries −14 and was making forts 14%
  *dearer*. Forts are not industry, and now the discount does not touch them.
- **Wars stop grinding into a stalemate the map never showed.** A battle whose
  front was as wide as both armies could hold was scored like an opening blow,
  so a war could settle into a deadlock nobody could read or break. A known
  collapse — Switzerland eating France — no longer happens.
- **A research group you drop no longer takes your current pick with it.**
- **Letters cross the network.** A dedicated server answers mail, and an
  advisor on the far end can reply.
- **The mod directory lists newer mods again.** It had been quietly refusing
  every capability added since it was written.
- **Campaigns work for their whole run.** Patriotic Education, the Opposition
  Smear Campaign and the Literacy Campaign acted on the turn they took effect
  and did nothing for the rest of their countdown — one turn in fourteen. They
  now act, and cost, every turn until they end, and a running campaign can be
  repealed. (Reported on GitHub, #16.)

### One bug worth the whole section

A player failing to join a host saw: *the server did not complete the handshake
in time.*

The server was fine. There were two handshake timeouts, in two files, under the
same name: the client waited twenty seconds for a reply, and the server dropped
any connection that had not finished in ten. So the client's second ten seconds
could never help — the server had already hung up — and the client blamed the
peer for a deadline the peer had enforced.

Diagnosing that from the message means reading two constants and knowing which
one won. There is one of them now, in the file whose entire reason for existing
is that the two halves of a handshake have to agree.

When both ends of a protocol each keep a copy of the same limit, the error
message will tell you about the wrong one.

### For modders

Gearbox 1.4: sixty-two new imports, nothing renamed or removed, and a mod built
against 1.3 runs unchanged. Fog is in it — thirteen reads for what a country
can see and what it remembers, and three writes that grant sight, as a
watchpost rather than an uncovering, saved with the world. Map scripts get the
same thing: `province.N.visible_to.ISO`, `seen_turn`, `known_owner`, and `fog
reveal` / `fog sight` / `fog clear`. On a map without fog all of it answers as
though the world were visible, so a script needs no special case.

Also in it: monuments, the goods economy, sector taxes and subsidies,
nationalisation, and the greater-diplomacy acts.

### The launcher

Unifico 0.1.1. Achievements arrive while you watch now — it notices what the
game writes within seconds and tells you when one is confirmed, instead of
waiting until you next open the page. And it stops working when nobody is
looking: it idles at a fraction of the frame rate, decodes the big pictures
only when they are on screen, and no longer reads a 23 MB font at startup for
names it needs only if you open the language picker.

Still optional. The plain download is where it has always been.

### What is not finished

The AI does not build monuments. It plays weaker in fog worlds until it is
retrained, which is the next job. The fort price change above has not been
benchmarked — it is clearly right, but eight doctrines' AIs now pay more for
forts and nine pay less, so which way it moves AI strength is not yet measured.
Steam is still not published; the store page is waiting on account verification
rather than on the game.

### Closing remarks

*(Pr1nted — this one is yours, same as last time. Download count and whatever
you want to say about where the project is going.)*

---

## 2. In-game announcement board

*(Needs the developer badge. Keep it short.)*

**1.3.1b — it runs on your phone, and the map can be dark**

The browser build plays on a phone: Settings → Experimental → Risky RAM, and a
full world loads. **Fog of war** is in too, under Experimental → Fog of War
worlds — make a world with it and nobody sees the whole map, including the AI,
with an Intelligence research tab, spy networks and counter-intelligence.
History and the timelapse switch off in those worlds, because they replay what
the fog hides. Plus **The Last Summer** (August 1990), and fixes: forts no
longer take the industry discount, wars stop deadlocking invisibly, research
groups keep your pick, mail crosses the network, mods list again, and campaigns
run their whole course.

---

## 3. Discord

**#announcements:**

**1.3.1b is up.**

**It runs on your phone.** Open the browser build on mobile, turn on *Settings
→ Experimental → Risky RAM*, and a full world loads — all 1,642 provinces. The
black maps and the zoomed-in interface are fixed outright.

**Fog of war.** *Settings → Experimental → Fog of War worlds*, then tick it on a
map's info popup, and nobody sees the whole map — including the AI. You see your
own land, where your armies stand, what your coasts and ships and allies can
see, and you remember the rest as it was. There's an Intelligence research tab,
spy networks to plant and dossiers to steal, and counter-intelligence for when
someone tries it on you. Fail a spy op and it's noticed; go far enough into
counter-intelligence and you're told whose spies they were.

Two things to know: history, the timelapse and turn rewind are off in a fog
world — they replay exactly what the fog hides — and the AI plays a bit weaker
there until we retrain it for it. Leave the box unticked and your games are
unchanged.

**New scenario: The Last Summer.** August 1990. The Wall is down, the Soviet
Union still covers a sixth of the land, East Germany has six weeks left, Iraq is
in Kuwait. Three fuses lit and none of them gone off.

**Fixes**, including one from here: whoever asked why Cost Efficiency wasn't
making forts cheaper — it was, everywhere except the button you were using, and
Fortress Doctrine was quietly making them 14% *dearer*. Forts don't take the
industry discount at all now. Also: wars stop grinding into stalemates the map
hid, dropping a research option keeps your pick, mail crosses the network, mods
list again, and campaigns act for their whole run instead of one turn in
fourteen.

**Modders:** Gearbox 1.4, 62 new imports, nothing renamed — your 1.3 mod runs
unchanged. Fog is exposed to both mods and map scripts.

**Launcher:** Unifico 0.1.1 — achievements show up while you watch, and it
stops burning CPU when you're not looking at it.

Steam is still waiting on account verification, not on the game.

Tell us how the fog plays. It's the biggest thing we've shipped and the one most
likely to have an edge we haven't hit.
