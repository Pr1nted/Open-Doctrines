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

Two things in this one. The browser build plays on a phone, which a lot of you
asked for by trying it and watching the tab die. And a world can be made where
nobody sees the whole map — including the AI.

### You can play it on a phone

The world map is one image, 8,192 by 4,096 pixels, and a phone browser ran out
of memory holding it next to everything drawn from it. Two smaller problems rode
along: black maps, and an interface zoomed in too far to use.

Those two are fixed outright. For the memory there is a new switch — **Settings
→ Experimental → Risky RAM** — holding the map in a compact form a quarter of
the size, and an iPhone then loads the full world, all 1,642 provinces. Only the
GIF timelapse and turn rewind still want the full image, and the switch says so.
It is called "Risky" on purpose: it is new, and we would rather you knew that
than found out.

### Fog of war

**Settings → Experimental → Fog of War worlds**, then tick it on a map's info
("i") popup.

You see your own provinces and a ring around them, wherever your armies stand,
the sea off your coasts, what your ships can see, and what your allies lend you
— one hop; an ally's ally lends you nothing. Everything else is remembered as it
was when last seen. A province taken behind the front stays the old colour until
somebody goes and looks.

A ninth research tab opens: coastwatchers, radar, reconnaissance, codebreakers,
counter-intelligence. From a foreign profile you can plant a spy network, which
shows you every province they hold for six turns, or steal their dossier, which
opens their books. Both cost money and can fail, and a failure is noticed — far
enough into counter-intelligence and the target is told whose spies they were.
The diplomats remember that about you.

Two things before you start one. History, the timelapse and turn rewind are off
in a fog world: all three replay the truth, which is the entire thing the fog
hides. And the AI plays a little weaker there — it is held to the same fog, but
the model that ships with this was trained on worlds where it could see
everything. Retraining it is the next job.

In multiplayer the host holds the truth and each seat is sent only its own
world. Nothing is broadcast, and spectators cannot join a fog world at all.

### A new scenario: The Last Summer

August 1990. The Wall is down, the Warsaw Pact is a signature on a dead treaty,
and the Soviet Union still covers a sixth of the land. East Germany has six
weeks left. The Baltics have declared an independence Moscow has not granted.
Iraq is in Kuwait. Three fuses lit at once and none of them gone off.

### The fixes

- **Forts always cost full price.** Somebody asked here why Cost Efficiency did
  not make forts cheaper. It did — everywhere except the button they were using
  — and Fortress Doctrine was making them 14% *dearer*. Forts are not industry.
- **Wars stop grinding into a stalemate the map never showed.** A wide front was
  scored like an opening blow. Switzerland no longer eats France.
- **A research group you drop keeps your current pick.**
- **Letters cross the network**, so a server answers mail and an advisor on the
  far end can reply.
- **The mod directory lists newer mods again.**
- **Campaigns act for their whole run**, not one turn in fourteen. (GitHub #16.)

### One bug worth the whole section

A player failing to join saw *the server did not complete the handshake in
time.* The server was fine. There were two handshake timeouts, in two files,
under the same name: the client waited twenty seconds for a reply, the server
dropped anything unfinished after ten. The client's second ten seconds could
never help, and it blamed the peer for a deadline the peer had enforced.

When both ends of a protocol keep their own copy of the same limit, the error
message will tell you about the wrong one.

### For modders, and the launcher

Gearbox 1.4: sixty-two new imports, nothing renamed, a 1.3 mod runs unchanged.
Fog is in it for mods and map scripts alike, and on a map without fog it answers
as though the world were visible, so there is no special case to write.
Monuments, the goods economy, sector taxes and nationalisation too.

Unifico 0.1.1 notices achievements within seconds instead of when you next open
the page, and stops burning a frame rate when nobody is looking. Still optional.

### What is not finished

The AI does not build monuments, and plays weaker in fog until retrained. The
fort change is not benchmarked — eight doctrines' AIs now pay more for forts and
nine pay less, so which way it moves them is unmeasured. Steam is still not
published; that waits on account verification, not on the game.

### Closing remarks

*(Pr1nted — yours, same as last time: download count and where the project is
going.)*

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

**It runs on your phone** — browser build on mobile, turn on *Settings →
Experimental → Risky RAM*, and a full world loads. Black maps and the
zoomed-in interface fixed outright.

**Fog of war** — *Settings → Experimental → Fog of War worlds*, then tick it on
a map's info popup. Nobody sees the whole map, including the AI. You see your
own land, your armies, your coasts and ships, and what allies lend you; the rest
you remember as it was. Intelligence research tab, spy networks, stolen
dossiers, counter-intelligence. History and the timelapse are off in those
worlds — they replay what the fog hides — and the AI is a bit weaker there until
we retrain it. Leave the box unticked and nothing changes.

**The Last Summer** — August 1990, three fuses lit and none gone off.

**Fixes**, one from here: whoever asked why Cost Efficiency wasn't making forts
cheaper — it was, everywhere except the button you were using, and Fortress
Doctrine was making them 14% *dearer*. Also wars no longer deadlock invisibly,
research groups keep your pick, mail crosses the network, mods list again, and
campaigns run their whole course.

**Gearbox 1.4** — 62 new imports, nothing renamed, your 1.3 mod runs unchanged.
**Unifico 0.1.1** — achievements arrive while you watch.

Steam is waiting on account verification, not on the game.

Tell us how the fog plays — it is the biggest thing here and the likeliest to
have an edge we have not hit.
