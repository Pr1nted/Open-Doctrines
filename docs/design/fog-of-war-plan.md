# Fog of war — implementation plan

Working design for 1.3.1b. Ten phases; Phase 1 (the fog model) is the
foundation and every later phase builds on it. Line numbers are as of
2026-10-10 and will drift — search for the function names.

## Requirements (from the owner)

1. Per-world option, chosen by the host/creator from the map's "i" popup in the
   New World browser, recorded in the save, never flippable afterwards.
2. You see your own provinces and their neighbours. Unknown provinces are
   darkened; sea not visible from your coasts or ships is darkened. Ships have
   radar. Allies and guarantors share their own country plus what they see.
3. Research entries that exist only in fog worlds and improve visibility.
4. Spying, unique to fog worlds, and counter-espionage against it.
5. Country profiles can hide more in fog worlds.
6. The AI plays under the same limited information.
7. The AI can use monuments (today it effectively never does — see Phase 3).
8. LLM advisors / correspondents only know what their country can see.
9. File snooping must not reveal hidden state: real protection in multiplayer
   (the server only sends what a seat can see); in single-player the save's
   hidden state is sealed with odseal — deters casual snooping only.
10. Rendering stays cheap on mobile web.

## What the code already gives us

- **Per-world flag precedent:** `m_goodsEconomy` — written unconditionally in
  `saveStateJson` (Game_UI.cpp), read in `loadStateJsonBody`.
- **Map "i" popup:** inline in `Game::drawMapBrowser` (Game_Browsers.cpp
  ~1779-1943; "i" hit-test ~2695; activation ~2713; name dialog ~1630-1683,
  confirm via Enter ~2391 / click ~2427 → `startNewGameWithName`
  (Game_Loading.cpp ~3690). Parallel copy `startNewGame` (~3586); other callers
  Game_AITrain.cpp, tutorial (Game_Comms.cpp), MP joiner (Game_Multiplayer.cpp).
- **Save metadata** is hand-written in FIVE places in SaveManager.cpp
  (createSave, appendTurn, updateLastPlayed, updatePlayerCountry, writeState),
  parsed in `parseMetadataJson`. A key missed by one writer is silently dropped.
  The binary turn delta has no version field (extended via trailer extra_flags).
- **Renderer:** the political layer is `kPoliticalFrag{Es,330}` (MapRenderer.cpp)
  driven by a per-province owners/rows table plus one colour row per owner,
  built in `Game::generatePoliticalTexture` (Game_Loading.cpp). Land fog = extra
  colour rows, no new texture, no shader change. Sea is province id 0.
- **AI observation:** `AISystem::buildFeatures`, `buildRelational`,
  `updateWorld`, `sampleTrend`, all fed by `refreshStats` (TRUE per-country
  stats for everyone), plus `buildTargetFeatures`, `buildAttackFeatures`, the
  request slots in `decideDiplomacy`. Fog precedents exist: relational r[3]
  treasury fog, r[6] weariness fogged to world mean. Existing leaks:
  `buildTargetFeatures` out[7] (true treasury), out[8] (true weariness).
  `NeuralNet::deserialize` widens a first layer and zeroes new input columns —
  appended features are safe for the shipped model until a retrain.
- **Monuments:** `monumentReflex` ships on but is gated on
  `hasResearched(odmon::unlockNode(k))`. The AI research executor only targets
  categories {buildings, army, navy}; "monuments" nodes are reached only by the
  cheapest-node fallback or extra research slots (2+ groups). Net: almost no AI
  ever unlocks a monument, so the reflex never fires.
- **LLM:** every fact about another country flows through `llmRelativeStrength`,
  `describeSituation`, `answerAdvisorTool` (`strength_of`, `profile_of`), and
  the lookup precompute in `askAdvisor` (Game_Llm.cpp).
- **Multiplayer:** `mpResolveTurn` broadcasts one full delta to everyone;
  `mpBuildSnapshot` sends full `saveStateJson` (minus mail, WITH simRng) plus
  every delta 1..N, cached for all joiners. Existing leak: every delta carries
  the host seat's research state and clients write it into their own research.
  Client capabilities are negotiated by a `caps` string in HELLO ("snapz").
- **odseal** exports no cipher today (od_s7, od_v3, od_fz, od_k9, od_t4).
  Precedent for a new export: od_t4 behind `OD_HAS_T4`; prebuilts rebuilt by
  `.github/workflows/odseal-prebuilts.yml` for all 13 platforms.

## Data model

New pure module `src/Fog.h/.cpp` (namespace `odfog`, no raylib, no Game,
unit-tested), game glue in `src/Game_Fog.cpp`, `bool m_fogOfWar` and
`std::unordered_map<int, CountryFog> m_fog` in Game.h.

```cpp
struct SightCircle { float x, y, r; };        // map px, x wraps
struct ProvinceIntel {                         // ~16 B
  uint16_t owner;      // last-known (seeded from turn-0 owners: the atlas is public)
  uint16_t seenTurn;   // 0xFFFF = never observed
  uint32_t garrison, population;
  uint8_t industry, fort, port, flags;
};
struct CountryFog {
  std::vector<uint64_t> visible;        // province bitset, this turn
  std::vector<uint64_t> shipsVisible;   // m_ships index bitset
  std::vector<SightCircle> seaSight;    // coasts + radar (renderer AND rules)
  std::vector<ProvinceIntel> memory;    // dense per province, lazy
};
```

`Game::recomputeFog()` per country c, in sorted cid order:
1. Own provinces + `m_provinceNeighbors` expanded `1 + landSightRings(c)` rings.
2. Coastal provinces add a `SightCircle(coastSightPx)`; each ship adds
   `shipRadarPx(ship)` (beside `shipMaxRangePx`; base boat 120 / destroyer 260 /
   carrier 400 px, scaled by research). A province whose centre is inside a
   circle is visible (radar spots coastal land).
3. A Signals Directorate monument adds its radius in province steps.
4. Sharing: union the BASE sets of allies and both directions of a guarantee.
   One hop, not transitive; spy grants are not shared.
5. Active spy grants add all of the target's provinces.
6. Ships visible if a partner owns them or they lie in c's / a partner's circles.
7. Memory: for each visible pid write truth into `memory[pid]`, seenTurn = turn.

Runs at the end of `processTurn` (after `expelStrandedArmies`, before the delta
is built), after a load completes, and on MP clients from the fog payload.
Randomness (spying) uses `g_simRng`.

Accessors used everywhere instead of raw state: `fogOn()`,
`provinceVisibleTo(cid,pid)`, `shipVisibleTo(cid,idx)`, `knownOwner`,
`knownGarrison`, `intel`, `sharesIntel(a,b)`, `intelGrant(a,b,level)`,
`displayOwners()` (== truth when fog off / spectator / MP client).

Memory ~6 MB RAM; saved sparsely (~100-300 KB).

## Phases (each ships on its own, in order)

**Phase 0 — groundwork, no behaviour change.**
Collapse the five SaveManager metadata writers into one
`metadataJson(const SaveMetadata&)`; add `fogOfWar`, `format` (3), `sealSalt`.
`struct WorldOptions { bool fogOfWar = false; }` threaded through
`startNewGameWithName` / `startNewGame`; cleared next to `m_tutorialMode = false`
and in `unloadGameData`; set in `LOAD_CREATE_SAVE`. `j["fogOfWar"]` written
unconditionally (goodsEconomy pattern). `MapEntry::fogOfWar`; "Fog of War:
Off/On" row in the "i" popup under "Scripted Events"; read-only "cannot be
changed later" line in the name dialog; badge in the world browser. Toggle
hidden behind an Experimental switch until Phases 2, 4 and 7 land. Training
hooks `OD_FOG` / `OD_FOG_FRAC`; server config key `fogOfWar`.

**Phase 1 — the fog model, invisible to players.**
`Fog.h/.cpp` (rings, x-wrapped circles, sharing union, memory update, sparse
binary codec "FOGM"+version), `Game_Fog.cpp`, `recomputeFog()` hook,
`fog/memory.bin` via `appendTurn` extraFiles next to `rebels.json`, load after
`replaySaveTurns`. `tests/fog_test.cpp` in `tests/run_all.sh`. Debug overlay.

**Phase 2 — single-player presentation and UI gating.**
Land fog via darkened/desaturated colour rows keyed (owner, fogged). Sea fog:
`setSeaFog(mask,w,h)` + `uniform sampler2D seaFog; uniform float fogOn;` in both
political shaders, darkening province-id-0 texels; GRAYSCALE mask at index/8
(512 KB desktop, 128 KB web), bilinear, one repaint per turn; globe gets it via
`buildSurface`. Hover tooltip uses display owners. `rebuildGradientField`,
`computeCountryLabels`, population/relations/resource textures read display
owners / hatch never-seen provinces. `drawCountryPanel`: last-known values with
"as of turn N". `drawInner`: battles, icons, armies, ships only when visible;
last-seen ships fade for 3 turns. Order-log marks filtered. Ceasefire map uses
display owners; notifications about hidden provinces suppressed. Timelapse,
history scrub and rewind disabled for seated players in fog worlds (they replay
the truth). Country profile: fog-only disclosure bits DISCLOSE_POPULATION /
INCOME / ARMY; graphs, treasury, doctrines, district laws hidden without
own/shared/grant/disclosure ("Unknown — no intelligence (last seen turn N)").

**Phase 3 — the AI researches monuments (all worlds; RULES change).**
`monumentResearchReflex(cid)` beside `monumentReflex`: when the reflex's own
affordability gates pass but no `mon_*` is researched, arm the cheapest
available "monuments" node in an idle extra slot, else group 1 only when idle
and the net did not arm research this turn. `OD_MONUMENT_RESEARCH` (default on),
bump RULES, bench (OD_ACT_HIST "[MONUMENT] built by the reflex: N").

**Phase 4 — the AI under fog, plus LLM gating.** See "AI observation" below.
Game_Llm.cpp evaluated through the speaker's view: `llmRelativeStrength` counts
`knownOwner`; `describeSituation` proximity uses known owners; `strength_of` /
`profile_of` answer "You cannot judge…" without intel; wars, treaties, claims
stay public. Game_Agent.cpp position text scoped to the seat.

**Phase 5 — fog-only research.** `ResearchNode::fogOnly` + effects
`radarRangePct, coastSightPct, landSightRings, spyCostPct, spySuccessPct,
counterIntelPct`. APPEND at the end of `buildResearchNodes` (node indices are
saved). Category "intelligence": intel_coastwatch, intel_radar1/2, intel_recon,
intel_aerial, intel_service, intel_codebreak, intel_counter1/2.
`isNodeAvailableFor` false when `fogOnly && !m_fogOfWar`; tab shown only in fog
worlds. AI `intelResearchReflex` (fog worlds, at war or bordering a stronger
power).

**Phase 6 — spying and counter-espionage.** `SpyOp {spy, target,
kind(Network|Dossier), launchTurn, turns}`, `m_intelGrants[spy][target]`,
`m_counterIntel[cid]` (share of income, like pacification; new
`CountryIncomeSnapshot::intelCost`). Launch cost base × sqrt(target provinces)
× (1 − spyCostPct). `processSpyOps()` after `processDiplomaticRequests`,
p = clamp(0.6 + spySuccess − counter(target), 0.1, 0.95). Success grants
(Network: provinces; Dossier: stats + profile). Failure notifies the target
(named above a counter threshold), costs credibility, feeds LLM disposition.
UI: Intelligence section in the country profile; counter-intel slider on the
economy screen. MP orders key "spyOps". AI `intelReflex`.

**Phase 7 — sealed single-player saves (odseal).** New exports
`od_p2` (seal) / `od_p3` (open) — ChaCha20-Poly1305 + key derivation inside the
archive, caller-supplied 12-byte nonce. `OD_HAS_P2` define. Rebuild prebuilts
for all 13 platforms via the workflow. SaveManager `sealEntry`/`openEntry`
choke point ("ODSL", version, nonce, ciphertext, tag); readers auto-detect.
Sealed: turns/t_*.dat, turns/s_*.json, state.json, rebels.json, rebellion/*,
fog/*. Treasuries move out of metadata into sealed state. Embedded map stored
as fog/map.odmap so pre-fog builds fail cleanly.

**Phase 8 — multiplayer authority.** Keep protocol 2; add capability "fog"
(`kNetClientCaps = "snapz,fog"`); a fog session rejects a HELLO without it
("This game uses fog of war; update to join."). `NetSessionInfo` trailing
`uint8 fogOfWar`. New `NetMsg::FogTurn = 85`, one frame per peer per turn:
`{turn, delta (fogDeltaFor(cid): truth vs that seat's mirror, visible only,
own research only), vis, intel, turnOrders}`; fog peers get no `Delta` /
`TurnOrders`. Per-country snapshots with a whitelist `saveStateJsonFor(cid)`
(no simRng, no foreign weariness/treasury/credibility) and one synthetic delta
at turn N instead of 1..N. Spectators get a delayed true feed
(`fogSpectatorDelay`, default 5; 0 = no spectators). Long-form + fog refused
for now (8b: per-seat keys and blobs). Host-side `FogMirror` per human seat,
persisted for dedicated-server restarts.

**Phase 9 — retrain, bench, release.** Mixed curriculum `OD_FOG_FRAC=0.5`,
one model for both world types (f[143] tells them apart). Bench non-fog seats
(must not regress beyond noise) and fog seats separately; new model tag.
`docs/fog-of-war.md`; update docs/multiplayer.md and AGENTS.md. Toggle leaves
Experimental once Phases 2, 4, 7 are in; MP fog needs Phase 8.

## AI observation

`const CountryStat& AISystem::seen(int observer, int other)` returns truth when
fog is off, observer == other, sharesIntel, or an active Dossier grant;
otherwise a per-turn cached estimate from memory: province count of known
owners; last-known army/industry/fort for seen provinces plus a prior for
never-seen ones (median army per province; world median industry); ships seen
within K turns; ports static; claims and pacts public; frontiers from known
owners. Used for every read about another country: buildFeatures f[21], f[22],
f[40], f[52-54]; buildRelational r[0-2] and the neighbour sort;
buildTargetFeatures (fog the out[7]/out[8] leaks); buildAttackFeatures 9, 11;
decideDiplomacy 81-84, 88; updateWorld per observer. Reflexes reading past
visibility go through the Game accessors (amphibiousReflex, shipDestination,
navy engage, bombardAvailable, campaignReflex, findWarTarget) — ~250 raw reads
of m_provinceArmies / m_ships / m_provinceIndustry / m_stats to sort into own vs
other. The scripted behaviour-cloning teacher must use the same lens.

Mask strategy: reject zero-fill (0 means "no army" — the AI would see
undefended neighbours everywhere). Use last-known values with a prior, plus
appended staleness channels via the widen-and-zero path: FEATURE_COUNT
143→147 (fog world, mean frontier staleness, share never seen, active grants),
REL_FEATURES 8→9 (staleness), TARGET_FEATURES 12→13. ARCH stays 8; RULES bumps.
Non-fog worlds must be bit-identical (gate on `fogOn()`; verify with the
decision hash and `tests/determinism_check.sh`). Fog worlds play the shipped
model on shifted inputs — a degradation, not a crash — until the retrain.

## Risks and tests

1. Leaks through ~250 AI read sites and unaudited UI paths → `OD_FOG_LEAK_PROBE`:
   hash country X's inputs, scale every state X cannot see, rebuild, assert the
   hash is unchanged.
2. Determinism: memory is history and must round-trip exactly
   (determinism_check, campaign_det, campaign_restart).
3. Metadata drift → do the Phase 0 single-writer refactor first.
4. Node index stability → fog nodes only at the end.
5. Mobile budget → no new map-size textures; sea mask 128 KB on web.
6. Relay rate limits → one FogTurn per peer per turn.
7. AI weaker in fog worlds until the retrain — label it.
8. Eliminated cids in last-known owners → render grey "unknown".
9. Mods: a GameState.Read mod sees through fog in single-player.

Honest limit of the seal: it stops unzip, text editors and read_odsv.py; it does
not stop a program calling od_p3 against the shipped archive, a debugger, a
memory scan, or a rebuilt open-source client. Real protection is multiplayer
only.

Tests: tests/fog_test.cpp (rings, x-wrap, non-transitive sharing, codec,
two-way guarantees); FogTurn round-trip + fail-closed in net_protocol_test and
fuzz_parsers_test; spyOps order validation; odseal seal/open/tamper/wrong-salt;
fog save round-trip → identical memory and next-turn decisions; a sealed entry
refused by a build without P2; country_profile_test disclosure bits; playtest
--verify with a fog session asserting a client holds no armies for provinces it
cannot see; benches for monuments and fog/non-fog ratings.
