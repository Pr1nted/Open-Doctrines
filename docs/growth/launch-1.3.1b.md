# Launching 1.3.1b — paste-ready

A point release on top of the beta. Nothing here is posted by a workflow: the
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

**Title:** It runs on your phone now, and the last summer of the Cold War

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

**1.3.1b — it runs on your phone now**

The browser build plays on a phone: turn on Settings → Experimental → Risky RAM
and a world loads. Plus a new scenario, **The Last Summer** (August 1990, the
Cold War's final summer), and fixes — wars no longer deadlock in a way the map
hid, dropping a research option keeps your pick, networked mail, and the mod
directory lists newer mods again. Thanks to everyone who showed up this week.

---

## 3. Discord

**#announcements:**

**1.3.1b is up.** The big one: **it runs on your phone now** — open the browser
build on mobile, turn on *Settings → Experimental → Risky RAM*, and a full world
loads. New scenario too: **The Last Summer**, August 1990, the Cold War frozen
one step from the end. Plus fixes — wars stop grinding into hidden stalemates,
research groups keep your pick, mail crosses the network, mods list again.

Huge week for new players — thank you all for turning up. Tell us if the phone
build still fights you.
