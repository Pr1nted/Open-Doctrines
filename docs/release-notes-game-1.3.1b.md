A point release on the first beta. Most of it is one thing — the browser build
runs on a phone now — and the rest is a Cold-War scenario and the fixes that
arrived with the people who turned up for 1.3.0b.

- **You can play in a phone browser.** The world is a single 8192×4096 image,
  and on a 4 GB phone the browser could not hold it next to the textures built
  from it: the map came up black, or the interface came up zoomed in too far to
  use, or the tab was killed outright as the map loaded. The first two are
  fixed, and the canvas no longer asks for more pixels than the phone has. For
  the last, Settings → Experimental now has a **Risky RAM** switch that holds
  the map in a compact form a quarter of the size; with it on, an iPhone loads
  the full world. Borders, province selection, the claim tint and the district
  overlay are all rebuilt from that compact form, so nothing you touch on the
  map is lost — only the timelapse export and turn-rewind still want the full
  image, and the switch says so.

- **A new scenario: The Last Summer.** August 1990. The Wall is down, the
  Warsaw Pact is a signature on a dead treaty, and the Soviet Union still covers
  a sixth of the land. East Germany has six weeks left as a country, the Baltics
  have declared an independence Moscow has not granted, Iraq is in Kuwait and
  the coalition is still unloading, and Yugoslavia and Czechoslovakia are each
  still one country. None of it had to end the way it did.

- **Wars stop grinding into a stalemate the map never showed.** A battle whose
  front was as wide as both armies could hold was being scored like an opening
  blow, so a war could settle into a deadlock nobody could read or break. The
  resolver now counts the width of a standing fight as well as the first clash,
  and a known collapse — Switzerland eating France on one seed — no longer
  happens; France grows where it used to be destroyed.

- **A research group you drop no longer takes your current pick with it.**
  Dropping one option from a mutually-exclusive group cleared the choice you had
  already made inside it.

- **Letters cross the network.** A dedicated server now answers mail, and an LLM
  advisor on the far end can reply, so correspondence works between a host and
  the people connected to it rather than only within one game.

- **The mod directory accepts the capabilities added since it was written.** It
  had been refusing every one added after it shipped, which quietly kept newer
  mods out of the listing.

- **Campaigns work for their whole run.** Patriotic Education, the Opposition
  Smear Campaign and the Literacy Campaign did their job on the turn they took
  effect and nothing for the rest of their countdown. They also vanished from
  the Doctrines line of the economy report. They now act, and cost, on every
  turn until they end, and a running campaign can be repealed. (Reported on
  GitHub, #16.)

- **Fog of war (Experimental).** A new world can be made with fog of war: you
  see your own land, what your armies, coasts, ships and allies see, and
  remember the rest as it was when last seen. It brings an Intelligence
  research tab, spy networks, stolen dossiers and counter-intelligence. Fog
  worlds' saves are sealed, and in multiplayer each player is sent only what
  they can see. Switch on "Fog of War worlds" under Settings → Experimental,
  then pick it from a map's info button. AI countries are held to the fog
  too, and until they are retrained for it they play somewhat weaker in fog
  worlds. See docs/fog-of-war.md.
