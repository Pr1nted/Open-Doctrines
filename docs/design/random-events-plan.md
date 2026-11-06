# Random events — implementation plan

Working design for the lightweight-content track (a near-term patch; campaigns
are a separate 1.4.0b workstream). Line numbers are as of 2026-11-06 and will
drift — search for the function names.

Random events are a declarative, data-driven layer of stochastic flavour and
decision moments: a trigger condition, flavour text, and effects, optionally
with player choices. They ride on top of the deterministic turn loop and are
authorable as JSON packs — shipped content and mod content use the same format.

This is **not** the ScriptEngine (`src/ScriptEngine.cpp`). That stays for
scripted narrative sequences (imperative DSL, `waitUntil`, tasks). Events are
the lightweight stochastic layer: no code, just declarative data, which is both
simpler to author and far easier to keep deterministic.

## Requirements (from the owner)

1. Simple to add and fun for players — mostly content authoring, not a new C++
   subsystem.
2. Moddable: players author their own event packs in the same JSON the shipped
   content uses.
3. **Deterministic.** Must not break save/replay, multiplayer seat agreement, or
   the AI decision-hash bench. This is the hard requirement and the spine of the
   design below.
4. Off by default, gated per world exactly like fog of war, so a world without
   events behaves byte-for-byte as before.

## The one principle

Event firing is a **pure function of `(m_worldSeed, turn, deterministic
game-state)`**, drawing from a **dedicated event RNG stream** independent of the
AI and combat streams. Get that right and most of the determinism problem
disappears: enabling events never perturbs anything else, and replaying the same
turns reproduces the same events with no stored state.

The one genuinely non-deterministic input is a **player's choice** on a
choice-event. That is input, like a troop order, and is recorded in the save
delta (and synced in MP) accordingly.

## Schema — the JSON event pack

One pack = a list of events. Declarative AND-of-predicates triggers (no
expressions — cheap, replay-stable, and safe to expose to modders).

```json
{
  "format": "od-events/1",
  "events": [
    {
      "id": "gold_rush",
      "weight": 10,
      "cooldown": 8,
      "scope": "province",
      "when": { "owner": "any", "has_resource": "gold", "min_population": 50000 },
      "chance": 0.03,
      "title": "Gold Rush in {province}",
      "text": "Prospectors flood {province}. The treasury swells.",
      "effects": [ { "op": "treasury", "country": "{owner}", "add": "rand:20..60" } ]
    },
    {
      "id": "general_strike",
      "weight": 6,
      "cooldown": 12,
      "scope": "province",
      "when": { "owner": "player_or_ai", "min_unrest": 30, "min_industry": 4 },
      "chance": 0.05,
      "title": "General Strike in {province}",
      "text": "Workers in {province} down tools.",
      "choices": [
        { "key": "crush",   "label": "Crush it",
          "effects": [ { "op": "unrest", "province": "{province}", "add": -15 },
                       { "op": "unrest", "province": "{province}", "add": 25, "after_turns": 3 } ] },
        { "key": "concede", "label": "Concede wages",
          "effects": [ { "op": "treasury", "country": "{owner}", "add": -20 },
                       { "op": "unrest",   "province": "{province}", "add": -10 } ] }
      ],
      "ai_default": "concede"
    }
  ]
}
```

Field notes:

- `scope` ∈ `global | country | province` — decides what the event iterates
  over (`scopeKeysFor`, below).
- `when` — a flat AND of declarative predicates. No expressions, no arbitrary
  code (unlike the ScriptEngine), so a mod pack cannot execute anything.
- `chance` — the per-check probability, drawn from the event's own derived
  stream.
- `rand:lo..hi` — the **only** stochastic value form in effects; draws from the
  same derived stream. Never a global RNG.
- `effects[].op` — a fixed vocabulary mapping to existing engine calls
  (`treasury`, `unrest`, `population`, `attrition`, …). Start with 4–5, grow as
  needed.
- `after_turns` — schedules a deferred effect (stored in the turn delta, see
  "Save / replay").
- `cooldown` — minimum turns before the same event can refire for the same
  scope key; the last-fired turn is per-`(eventId, scopeKey)` state that is
  saved and replayed.
- `ai_default` — the option an AI or absent player takes, so choice-events
  resolve deterministically with no UI.
- `title` / `text` — flavour, with `{province}` / `{owner}` interpolation.

## Deterministic firing — a derived, independent stream

No event ever draws from a shared RNG. Each check derives its own seed purely
from `(m_worldSeed, turn, eventId, scopeKey)`, so firing is **order-independent**
(it does not matter how many other events were checked first, or whether one
early-returned) and consumes nothing the AI or combat RNG can see. This
sidesteps the early-return-skips-RNG class of bug entirely — there is no shared
stream to desync.

```cpp
// Events.cpp — self-contained, cross-platform integer RNG.
// NOTE: do NOT use randRange() (Game_AITrain.cpp:114). It wraps
// std::uniform_*_distribution, which is implementation-defined across
// libstdc++ / libc++ / MSVC and would desync multiplayer. Hand-rolled here.
static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
// Local stream for one (event, scope) resolution — ordered within, independent
// across events.
struct EventRng {
    uint64_t s;
    EventRng(uint32_t seed, uint32_t turn, uint32_t eid, uint32_t key)
        : s(splitmix64((uint64_t)seed * 0x100000001b3ull ^ turn*K1 ^ eid*K2 ^ key*K3)) {}
    uint64_t next() { s = splitmix64(s); return s; }
    // Lemire bounded — identical on every platform (no std::*_distribution).
    uint32_t below(uint32_t n) { return (uint32_t)(((next() >> 32) * (uint64_t)n) >> 32); }
    float    unit()            { return (float)(next() >> 40) * (1.0f / 16777216.0f); }
};

bool Game::eventFires(const EventDef& e, uint32_t scopeKey) const {
    EventRng rng(m_worldSeed, (uint32_t)m_turnNumber, e.idHash, scopeKey);
    return rng.unit() < e.chance;              // consumes nothing shared
}
```

Within one event's resolution the stream is ordered (a weighted province pick
then a `rand:` roll draw in a fixed sequence — fine, it is fixed). Across events
each has its own local state, so order of *checking* is irrelevant. Order of
*applying* fired events still matters (two events touching one province), so
fired events are collected and applied in sorted `eventId` order.

## The gated hook — off by default, bench-safe from commit #1

Mirror `m_fogOfWar` exactly (declaration `Game.h:3221`, accessor `fogOn()`
`Game.h:3253`, set at world creation `Game_Loading.cpp:4010`, env override 4012,
persisted in save metadata).

```cpp
// Game.h — beside m_fogOfWar
bool m_events = false;
bool eventsOn() const { return m_events; }

// Game_Loading.cpp — beside m_fogOfWar = m_newWorldFog;
m_events = m_newWorldEvents;
if (const char* e = std::getenv("OD_EVENTS")) m_events = (*e && *e != '0');
```

The firing phase is a single guarded line in `processTurn()`
(`Game_TurnLogic.cpp:452`), placed at one fixed point — early, after the
supply-cache clear (~line 486), so effects are visible to the rest of the turn.
The placement is a design choice but must stay fixed once chosen: moving it
invalidates replays, like any turn-order change.

```cpp
// in processTurn() after m_supplyCache.clear(); m_seaSupplyCache.clear();
if (m_events) processEvents();     // the ONLY new line on the no-events path

void Game::processEvents() {
    std::vector<FiredEvent> fired;
    for (const EventDef& e : m_eventDefs) {
        if (onCooldown(e)) continue;
        for (uint32_t key : scopeKeysFor(e))          // sorted, stable iteration
            if (matches(e.when, key) && eventFires(e, key))
                fired.push_back({e.idHash, key});
    }
    std::sort(fired.begin(), fired.end());             // deterministic apply order
    for (auto& f : fired) resolveEvent(f);             // passive now; choices queue a prompt
}
```

Because the only change on the no-events path is one `if (m_events)`, a twin
build with events off is provably byte-identical. Verify with `OD_DECISION_HASH`
before the first event is even authored — the fog discipline applied from day
one.

## Save / replay / multiplayer

- **Passive events store nothing.** `resolveEvent` is pure in
  `(seed, turn, state)`, so replaying the same turns reproduces them for free.
- **Cooldown stamps and deferred (`after_turns`) effects are the only
  passive-side storage** — add two small vectors to `TurnDelta`
  (`SaveManager.h:145`): fired-event cooldown stamps, and pending deferred
  effects.
- **Choice events: the chosen option is player input.** Record
  `{eventIdHash, optionKey}` in `TurnDelta`, exactly like a troop order. Replay
  reads it instead of prompting. AI / absent players use `ai_default`, which is
  deterministic and needs no record.
- **Multiplayer:** triggers need no broadcast — every seat computes identical
  fires from the same `m_worldSeed`. Only the human *choice* crosses the wire,
  through the host-authoritative path already landed (commit "the host rolls the
  wheel", `Game_Multiplayer.cpp`). That is most of the MP work done.

## The two gotchas, concretely

1. **Never call `randRange()` (`Game_AITrain.cpp:114`) on the event path.** Its
   `std::uniform_*_distribution` diverges across standard libraries → MP desync.
   Fine where it lives (training, single platform, not replayed); never for
   gameplay randomness. Use `EventRng`.
2. **Any `scopeKeysFor` / province pick iterates a sorted key list**, never a
   raw `unordered_map` walk — unordered iteration order is platform-dependent
   and desyncs the same way.

## Net cost

- Passive events: zero new sync state; one guarded phase call; a fixed effect
  vocabulary.
- Choice events: a couple of recorded input bytes per choice, plus the
  prompt UI and the MP choice sync (host-roll path reused).
- The world flag, its save-metadata bit, and the New World "i" popup toggle
  (mirror the fog toggle).

The determinism was the worry; the fog-of-war and host-roll work already paved
it. The remaining effort is authoring good events.

## Out of scope (follow-ups, not this plan)

- **AI uses monuments.** The AI effectively never builds or uses monuments
  today — already noted in `fog-of-war-plan.md` (requirement 7, Phase 3). It is
  unrelated to events and should not ride this branch; tracked separately.
- **Campaigns / challenge scenarios** — a separate 1.4.0b workstream.
