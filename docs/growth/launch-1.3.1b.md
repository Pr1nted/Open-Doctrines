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

**Title:** It runs on your phone now, and nobody has to see the whole map

**Body:**

A lot of you showed up this past week — more people in a few days than in the
whole run before it. Thank you. It means a fair number of you also opened the
browser build on a phone, and some of you got a black screen, or a map that
spun and then reloaded the tab. That one was on us, and it's the first thing
this release fixes.

### You can play it on a phone

The world map is one big image — 8,192 by 4,096 pixels — and a phone browser
would run out of memory trying to hold it next to everything drawn from it. So
the tab died as the map loaded. Two smaller problems rode along: the map came up
black on some phones, and the whole interface came up zoomed in too far to use.

The black maps and the zoom are fixed outright. For the memory, there's a new
switch under **Settings → Experimental → Risky RAM**. Turn it on and the game
holds the map in a compact form about a quarter of the size, and an iPhone loads
the full world — all 1,642 provinces, every border, province selection, claims,
the lot. The only things that still want the full map are the GIF timelapse
export and rewinding to a past turn; the switch tells you so.

It's opt-in and called "Risky" on purpose — it's new, and we'd rather you knew
that than find out. If a world won't load on your phone, that switch is the
first thing to try.

### Fog of war, if you want it

The other half of this release is a thing you have to go and switch on.

Turn on **Settings → Experimental → Fog of War worlds**, and a map's info
button ("i") gains a tickbox. A world made with it ticked is one where nobody
sees the whole map — not you, and not the AI.

You see your own provinces and a ring around them. You see wherever your armies
are standing, at home or in somebody else's country. Your coasts watch the sea
off them; your ships carry their own circles, a carrier's much wider than a
boat's. Your allies and guarantors lend you their sight, one hop — an ally's
ally lends you nothing. Everything else is **remembered**: the owner, the
garrison, the industry, the forts, as they were the last time anyone you share
with looked at it, with the turn written on it. A province taken behind the
front stays the old colour on your map until somebody goes and looks.

A ninth research tab opens, **Intelligence** — coastwatchers, naval radar,
reconnaissance patrols, codebreakers, and counter-intelligence to stop the same
being done to you. From a foreign country's profile you can plant a spy network,
which shows you every province they hold for six turns, or steal their dossier,
which opens their books. Both cost money, both can fail, and a failure is
noticed — far enough into counter-intelligence and the target is told *whose*
spies they were. The diplomats remember that about you.

Two consequences worth knowing before you start one:

- **History, the timelapse and turn rewind are off in a fog world.** All three
  replay the truth turn by turn, which is the entire thing the fog is hiding.
  A fog world's save is also sealed, so a text editor won't show you the map
  either.
- **The AI plays a bit weaker in fog.** It is held to the same fog you are —
  it decides from what it remembers, and guesses at provinces it has never
  seen — but the model shipping with this release was trained on worlds where
  it could see everything. Retraining it for the fog is the next job.

In multiplayer the host is the only machine holding the truth, and each seat is
sent only its own world. Nothing is broadcast. Spectators can't join a fog
world at all, because two clients would be two windows onto the whole map.

It's marked Experimental because it is new and it is a lot. If you'd rather not,
don't tick the box and nothing about your game changes.

### A new scenario: The Last Summer

August 1990. The Wall is down, the Warsaw Pact is a signature on a dead treaty,
and the Soviet Union still covers a sixth of the land. East Germany has six
weeks left as a country. The Baltics have declared an independence Moscow has
not granted. Iraq is in Kuwait and the coalition is still unloading. Yugoslavia
and Czechoslovakia are each, for now, one country.

It's the end of the Cold War frozen at the moment before it finished — three
fuses lit at once and none of them gone off. None of it had to end the way it
did, and this is the map that lets you find out otherwise.

### The fixes that came in with you

- **Wars no longer grind into a stalemate the map doesn't show you.** A fight on
  a wide front was being scored like a first skirmish; now it isn't, and a nasty
  case where Switzerland could eat France is gone.
- **Dropping a research option no longer clears the one you'd already picked** in
  the same group.
- **Letters cross the network** — a host's server answers mail, and an advisor on
  the far end can reply.
- **The mod directory lists newer mods again** — it had been quietly refusing
  every capability added since it was written.

If you've been playing on desktop, nothing here changes your game except the
fixes. If you tried it on a phone and bounced off, give it another go — and if
something's still wrong, the feedback button in-game comes straight to us.

Thanks again for turning up. There's more coming.

---

## 2. In-game announcement board

*(Needs the developer badge. Keep it short.)*

**1.3.1b — it runs on your phone now, and the map can be dark**

The browser build plays on a phone: turn on Settings → Experimental → Risky RAM
and a world loads. **Fog of war** is in too, under Experimental → Fog of War
worlds: make a world with it and nobody sees the whole map, including the AI.
You get an Intelligence research tab, spy networks and counter-intelligence;
history and the timelapse switch off, because they replay what the fog hides.
Plus a new scenario, **The Last Summer** (August 1990, the Cold War's final
summer), and fixes — wars no longer deadlock in a way the map hid, dropping a
research option keeps your pick, networked mail, and the mod directory lists
newer mods again. Thanks to everyone who showed up this week.

---

## 3. Discord

**#announcements:**

**1.3.1b is up.** The big one: **it runs on your phone now** — open the browser
build on mobile, turn on *Settings → Experimental → Risky RAM*, and a full world
loads.

The other big one: **fog of war**. *Settings → Experimental → Fog of War
worlds*, then tick it on a map's info popup, and nobody sees the whole map —
you see your own land, where your armies are standing, what your coasts and
ships and allies can see, and you remember the rest as it was. There's an
Intelligence research tab, spy networks you can plant and dossiers you can
steal, and counter-intelligence for when someone tries it on you. The AI is
held to the same fog, which also means it plays a little weaker there until we
retrain it. History, the timelapse and turn rewind are off in a fog world —
they replay exactly what the fog hides. Experimental on purpose; leave the box
unticked and your games are unchanged.

New scenario too: **The Last Summer**, August 1990, the Cold War frozen one step
from the end. Plus fixes — wars stop grinding into hidden stalemates, research
groups keep your pick, mail crosses the network, mods list again.

Huge week for new players — thank you all for turning up. Tell us if the phone
build still fights you, and tell us how the fog plays.
