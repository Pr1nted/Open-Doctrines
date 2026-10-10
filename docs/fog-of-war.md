# Fog of war

A world can be made with fog of war, and then nobody sees the whole map.

It is a property of the **world**, chosen once, when the world is created, and
never changed: from the map's info ("i") popup in single player, from the host
screen in multiplayer, or `fog-of-war = true` in a dedicated server's config.
Until it leaves Experimental it needs **Settings → Experimental → Fog of War
worlds** switched on before the option appears.

## What a country can see

- **Its own provinces**, and the ring of provinces around them.
- **Wherever its armies stand**, and the provinces around them, at home or
  abroad.
- **The sea off its coasts**: every coastal province watches 90 px out.
- **What its ships can see**: a radar circle per hull (boat 120 px, destroyer
  260, carrier 400, anything else 200).
- **What its allies and guarantors see**, in either direction of a guarantee.
  Shared one hop only: an ally's ally shares nothing with you.
- **All of a country's land**, for a few turns, after a successful spy network
  (see below).

Everything else is **remembered**: the owner, garrison, population, industry,
fortification and port of a province as they were the last time anyone you
share with looked, with the turn that was. A province nobody has ever seen
belongs to whoever held it on the starting map, because the starting atlas is
public.

## What the player sees

- Provinces out of sight are drawn darker, and never-seen ones darker still.
  The sea outside your sight circles is shaded too.
- The map shows the owners you **know of**, not the truth: a province taken
  behind the front is still drawn in its old colour until someone sees it.
- Industry, fort, army and port markers, and ships, appear only in sight.
  Foreign ships you saw recently stay on the map as fading ghosts for 3 turns.
- The province panel for a province out of sight says **"Out of sight"** with
  what was there when it was last seen, and offers no actions.
- A stranger's **country profile** hides population, income and expenses
  ("Unknown -- no intelligence"), and how it is divided into districts. A
  country can choose to **publish** its population, income and army size;
  those three rows appear only in fog worlds.
- The order overlay between turns shows only the foreign orders you saw happen.
- **History, the timelapse and turn rewind are switched off** in a fog world.
  They replay the truth, turn by turn, which is everything the fog hides.

## Intelligence research

A ninth research tab, **Intelligence**, exists only in fog worlds:

| node | effect |
|---|---|
| Coastwatchers | coasts see 50% further out to sea |
| Naval Radar / Long-Range Radar | ships see 35%, then a further 40%, further |
| Reconnaissance Patrols / Aerial Reconnaissance | one more ring of provinces past every border, each |
| Intelligence Service | spying costs 25% less, +10 points of success |
| Codebreakers | +15 points of success |
| Counter-Intelligence / Security Apparatus | foreign spies succeed 15, then 20, points less often |

The nodes are appended at the end of the research list, so worlds without fog
see the same tree, in the same order, as before.

## Spying

From a foreign country's profile, under **Intelligence**:

- **Plant a spy network** (2 turns): on success you see every province they
  hold, as if it were yours, for 6 turns.
- **Steal their dossier** (1 turn): on success their figures are open to you
  (profile, panel statistics, and their true numbers for the AI) for 6 turns.

Both cost money up front, scaled by the square root of how many provinces you
know them to hold. The chance is 60%, plus your Intelligence research, minus
the target's counter-intelligence, held between 10% and 95%. A failure is
noticed. Above 20 points of counter-intelligence the target is told **whose**
spies they were, and the diplomats remember it (see the LLM section).

Your own profile has the **counter-intelligence** dial: levels 0 to 3, each
worth 10 points against foreign operations, each costing 2% of gross income per
turn.

The roll is a hash of (spy, target, kind, launch turn), not a draw from the
turn's random stream, so launching an operation moves no other dice.

## The AI and the language models

- The diplomats (LLM letters and advisor tools) are told the world has fog,
  judge a stranger's strength from the map they **know**, and say "by your last
  reports" when they do. They know when spies were caught either way.
- The country AI sees other countries through `Game::fogEstimate` while it
  decides (`AISystem::fogLensEnter`): what its memory holds, a median prior
  for provinces it has never seen (never zero, so nothing reads as
  undefended), and ships it saw recently. Partners, dossiers and itself read
  true. `OD_FOG_AI_TRUTH=1` gives it the truth again, decision for decision.
  The shipped model was trained on the truth, so in fog worlds the AI plays
  weaker until it is retrained on a mixed curriculum. Its navy reflexes still
  see foreign hulls anywhere.

## Saves

A fog world's save is **sealed**: its turns, state, rebels, fog memory and the
treasuries are encrypted (`ODSL` entries, see `src/SaveManager.cpp` and
`docs/odseal.md`). unzip, a text editor and `tools/read_odsv.py` no longer show
what fog hides. That raises the cost of peeking; it is not secrecy, because the
key is inside the game. A build without the seal cannot open a sealed save and
says so.

## Multiplayer

The host is the only machine that holds the truth. Nothing is broadcast:

- Each seat is sent its **own turn**. The host keeps a mirror of what each
  seat has been told, and sends the difference for the provinces and hulls
  that seat can see. A province out of sight is not sent, so it stays as the
  seat last knew it. One that comes back into sight is sent whole.
- A joiner is sent its **own world**: every province set outright (memory
  values where it cannot see), its own state with every other country's
  orders, books and research cut out, and its fog memory.
- Hulls keep their index, because orders name a hull by it. A hull out of
  sight is sent as a placeholder owned by nobody, so a client learns the total
  number of hulls in the world and nothing else about them.
- **Spectators are not sent a fog world.** A spectator seat would be a second
  window onto the truth for anyone who opened two clients.
- A build that cannot draw fog (one whose hello lacks the `fog` capability) is
  refused with "update to join".
- Long-form turn stores are not written for a fog world: anyone can read a
  store.

The host itself is trusted. It resolves the turn and holds the whole world.

## Where it lives

| file | what |
|---|---|
| `src/Fog.h/.cpp` | the rules: rings, circles, sharing, memory, the codec, the estimate. Pure, tested by `tests/fog_test.cpp`. |
| `src/Game_Fog.cpp` | the game's view: recompute each turn, accessors, display owners, sea mask, the fogged province panel. |
| `src/Game_Spy.cpp` | espionage and counter-intelligence. |
| `src/Game_FogNet.cpp` | what each multiplayer seat is sent. Tested by `tests/fog_net_test.cpp`. |
| `src/SaveManager.cpp` | sealed save entries. Tested by `tests/save_seal_test.cpp`. |
| `docs/design/fog-of-war-plan.md` | the plan this was built from. |
