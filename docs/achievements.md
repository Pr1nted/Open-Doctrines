# Achievements

There are 109 of them, defined in `tools/achievements/catalog.json`. Every other
copy of the catalog is generated from that file by `tools/gen_achievements.py`:

| Copy | What uses it |
|---|---|
| `src/achievements/AchievementCatalog.gen.h` | Compiled into the game, so editing a shipped file cannot change what an achievement needs. |
| `net/src/achievements/catalog.gen.ts` | The Worker's tier and clock table. |
| `<Unifico>/src/gen/catalog.gen.h` | The launcher's copy. |
| `docs/steam/achievements.md`, `.csv`, `achievements-loc.vdf` | What to type into Steamworks, plus the localisation upload. |
| `docs/steam/achievements/*.png` | 256×256 icons for Steam, achieved and locked. |
| `data/icons/achievements.png` | The 64 px atlas the game and the launcher draw. |

To regenerate everything:

```bash
python3 tools/gen_achievements.py --icons --unifico ../Unifico
```

`tests/run_all.sh` runs `--check`, which fails if any copy has drifted from the
catalog. Names and descriptions are translated like any other string: the i18n
extractor reads the catalog, so `tools/i18n_extract.py` puts them in `en.json`.

## Earned is not granted

The game shows an achievement in one of two states:

- **Earned:** the game saw it happen. This is recorded in
  `data/achievements/progress.json` along with the counters that led to it.
  That file is plain JSON on the player's disk, so it proves nothing, and
  nothing treats it as proof. An earned achievement is only a **claim**.
- **Granted:** the account service accepted the claim and signed it. A grant is
  `oda1.<payload>.<Ed25519 signature>`, naming the account and the achievement.
  It is kept in `data/achievements/grants.json` and **verified against the
  public keys compiled into the build every time it is read**. This is the only
  state that counts:
  - in the collection;
  - on Steam;
  - in the launcher;
  - in a `.odstate`.

What each kind of tampering achieves:

| What someone does | What happens |
|---|---|
| Edits `grants.json` | The signatures do not verify, so the entries are dropped on load. |
| Hand-builds a `.odstate` | Its grants are only **offered** (`grants.import.json`) and verified one by one. An archive's progress never replaces an install's own. Any other file under `achievements/` is refused. See `OdState::load`. |
| Edits `progress.json` | This produces claims, and claims still have to get past the service. |
| Runs a modified client | It can send any claim it likes, so what stops it is time (next section). |

## What the service checks (`net/src/achievements/claim.ts`)

The game is open source and runs on the player's machine. **No server can prove
that a single-player game happened the way a claim says.** What the service does
instead is make a forged collection cost about as much as an earned one, using
the one thing a forger cannot fake from their side: real time.

1. **Account clock.** Each achievement has a `minHours`: the earliest point,
   measured from the account's first play session, at which it is plausible.
   The service records that first session itself, so the client cannot
   backdate it.
2. **Session clock.** Achievements about one long sitting have a
   `minSessionMinutes`. The play ticket carries the service's own start time.
3. **Budget.** A token bucket per account allows at most 12 grants in a burst,
   refilling one every four minutes. However it is collected, the catalog takes
   hours of wall clock.
4. **Tiers.** "Superpower" needs "Regional Power" first.

Rule tables that do not match a known release (the `od_t4` seal) are marked
**modified rules** on the grant. They are never refused: a modded game is a
real game.

Each account's state lives in a Durable Object (`AchievementsDO`), not in KV,
because KV writes are the budget account creation runs on. Deleting an account
wipes its grants.

## Keys

Grants have their own keypair, separate from the session-token key. The session
key is rotated whenever a credential leaks. A grant should still verify in ten
years. To create the keypair, once per deployment:

```bash
net/setup-achievement-key.sh
```

It sets the Worker secrets and prints the public key. That value goes into the
`OD_ACHIEVEMENT_KEYS` repository variable on both `Pr1nted/Open-Doctrines` and
`Pr1nted/Unifico`. The release workflows pass it to CMake as
`-DOD_ACHIEVEMENT_KEYS`.

`OD_ACHIEVEMENT_KEYS` is a list. If the key ever has to change, **prepend** the
new key and keep the old one. A build with no keys still counts and claims
achievements, but shows them all as waiting.

## Steam

`src/platform/SteamBridge.cpp` loads `steam_api` from beside the executable at
run time. No build links the SDK, so non-Steam builds carry nothing.

It only ever sets **granted** achievements, by their API names
(`ACH_<ID>`). That keeps the Steam collection exactly as meaningful as the one
in the game.

For the Steam depot:

- ship `libsteam_api` / `steam_api64.dll` from the Steamworks SDK
  (`packaging/steam`);
- create the achievements in Steamworks from `docs/steam/achievements.md`.

Steamworks has no bulk import, so the second step is done by hand.

## When the tracker runs

Only when a person is playing. `Game::achievementsLive()` excludes:

- AI training and evaluation, `--simulate`, `--screenshots` tours;
- benches and the agent door;
- the dedicated server, chat-plays, the tutorial and its walk;
- timelapse export and `--llm-letter`.

Most state is measured once a turn (`Game::achTurnSnapshot`) and compared with
the previous turn. That way every writer of that state is caught, including a
multiplayer client applying the host's delta. Actions the player takes are
counted where they happen (`achNote`).

## Launcher statistics

These are separate from achievements and opt-in. See `net/PRIVACY.md`
("Launcher statistics") and `net/src/analytics/ga.ts`.
