# OpenDoctrines Modding — Gearbox API

## Status

**Implemented and running.** Phase 1 is done: the reader, the runtime, the
capability modules, the mod menu, and the AI-learning interlock.

| Piece | Where | Tests |
|---|---|---|
| `.odmod` reader, archive limits, manifest | `src/mods/ModPackage.{h,cpp}` | `ModArchiveTest` (48) |
| Runtime, capability enforcement, limits | `src/mods/ModRuntime.{h,cpp}` | `ModRuntimeTest` (41) |
| Host capabilities | `src/mods/ModHost.{h,cpp}` | `ModRuntimeTest` |
| States, grants, persistence | `src/mods/ModManager.{h,cpp}` | `ModManagerTest` (34) |
| ABI matches `sdk/abi.json` | `sdk/abi.json` | `ModAbiTest` (70) |
| Mod menu and panel rendering | `src/Game_Mods.cpp` | — |
| Documented example still compiles | `docs/gearbox-sdk.md` | `check_doc_examples.sh` |

```bash
tests/run_all.sh
```

That builds the fixture modules, runs all four suites, compiles the hello-world
straight out of the documentation, and validates every shipped example mod.
Without a wasm-capable clang the wasm cases skip themselves and say so, rather
than passing quietly.

### Keeping the SDKs honest

`sdk/abi.json` is the machine-readable ABI, and `ModAbiTest` asserts the host's
capability table matches it **in both directions** — a host function missing
from `abi.json` would be absent from every SDK, and an `abi.json` entry the host
lacks would fail to link for every modder.

That matters because the language bindings under `sdk/` mostly cannot be
compiled here: no Rust, Zig, TinyGo, or SwiftWasm toolchain is available. Only
C, C++ and AssemblyScript are verified end to end. Checking every binding
against the host is impossible; checking that `abi.json` still describes the
host is cheap, and it is the same guarantee one step removed.
`docs/gearbox-abi.md` is generated from the same file by
`tools/gen_abi_docs.py`, so the reference cannot drift either.

**Implemented capabilities:** `Core`, `UI`, `GameState.Read`, `GameProcess`,
`Assets`.
**Not implemented:** `GameState.Write`, `Neural`, `Diplomacy`, `Map`, `Storage`.
A mod requesting one is refused at load with a diagnostic naming the import,
rather than running without a capability it believes it holds.

**Signature verification is still not enforced.** `publicKey` is parsed and
format-checked; nothing verifies `signature.bin` yet.

### Known gap: no fuel metering on web

On desktop, WAMR enforces `limits.fuelPerTurn` and terminates a hook that
exceeds it — `ModRuntimeTest` pins that an infinite loop is stopped rather than
hanging the turn.

The browser exposes no equivalent instruction limit, so **on web that limit is
not enforced**, and a mod with an infinite loop in a hook will hang the tab.
Memory limits and capability enforcement work on both. This is a real gap, not a
detail.

The web backend compiles and links (`emcmake cmake -S . -B build-web`) but has
not yet been exercised in a browser.

This is separate from the map `ScriptEngine` (`docs/scripting.md`), which stays
as-is: that is a per-map DSL embedded in `.odmap` files. Gearbox mods are
game-wide, compiled, and sandboxed. The two may converge later; they do not
share an implementation today.

## Decisions already made

| Decision | Choice | Why |
|---|---|---|
| Runtime | Browser `WebAssembly` on web, WAMR embedded on desktop | Web builds are themselves WASM; the browser already has a fast sandbox, and using it makes runtime loading work on itch.io without a page reload. WAMR is small and designed for embedding. |
| Trust | Trust-on-first-use, key pinned per mod id | No registry to run. Catches a hijacked update, which is the realistic attack. Does **not** prevent impersonation at first install — see Security. |
| Language support | One C ABI, thin SDK bindings | Sixteen hand-written SDKs is sixteen things to keep in sync. The ABI is the product. |

## Container: `.odmod`

A ZIP archive. Extension `.odmod`. Layout:

```
MANIFEST.json        required, must be the first entry
mod.wasm             required, the module
thumbnail.png        optional, <= 512x512
data/                optional, mod-owned read-only assets
signature.bin        optional, detached signature (see Security)
```

`MANIFEST.json` must be the first archive entry so the loader can read and
validate it without decompressing the rest — a mod whose declared limits are
unacceptable is rejected before any bulk inflation happens.

## MANIFEST.json

```jsonc
{
  "schema": 1,                        // manifest schema, not the API version
  "id": "com.example.better-ai",      // reverse-DNS, immutable, the pinning key
  "name": "Better AI",
  "version": "1.2.0",                 // semver
  "description": "Retunes AI war scoring.",
  "authors": ["Jane Doe <jane@example.com>"],
  "gearbox": "1.0",                   // targeted API version, "Gearbox v1.0"
  "side": "client",                   // "client" | "server" | "both"; default "both"
  "modules": [                        // requested capabilities, least privilege
    "Core",
    "GameState.Read",
    "GameProcess"
  ],
  "dependencies": [
    { "id": "com.example.corelib", "version": ">=2.0.0 <3.0.0", "optional": false }
  ],
  "publicKey": "ed25519:BASE64...",   // author key, pinned on first install
  "limits": {                          // mod-declared, clamped by host maxima
    "memoryPages": 512,               // 64 KiB pages -> 32 MiB
    "fuelPerTurn": 5000000,           // per hook call
    "loadFuel": 500000000             // mod_load only; omit and the host decides
  }
}
```

Unknown top-level fields are ignored, so schema 1 mods keep loading. Unknown
*module names* are a hard error — silently dropping a capability a mod thinks
it has is worse than refusing to load.

Enforced field rules:

- `id` — 3–128 characters of `[a-z0-9._-]`, containing a dot, not starting or
  ending in one. **Lowercase only**: the id is both the `Storage` namespace and
  the trust-pinning key, and on a case-insensitive filesystem `com.a.Mod` and
  `com.a.mod` would collide as two identities sharing one store.
- `version` — semver `MAJOR.MINOR.PATCH`, optional `-prerelease` / `+build`.
- `modules` — `GameState.Write` implies `GameState.Read`; `Core` is added
  whether or not it is listed.
- `limits` — **clamped, not rejected**. A mod asking for more memory or fuel
  than the host allows is being optimistic, not hostile, and still loads with a
  warning. Ceilings are 1024 memory pages (64 MiB) and 100,000,000 fuel/turn.
- `publicKey` — must carry the `ed25519:` prefix if present. Not verified yet.
- `side` — optional, defaults to `"both"`. An unrecognised value is a warning
  and falls back to `"both"`, so a future release adding a side does not stop
  today's game loading your mod. See below.

### Which side a mod runs on

In multiplayer the **server is authoritative**. A client's copy of the world is
replaced by whatever the server sends at the end of each turn, so a write made
on a client does not survive and was never going to. That is what stops a
modified client from cheating, and it applies to your mod exactly as it applies
to everyone else's.

`side` says where your mod belongs:

| `side` | Where it runs | What it may do |
|---|---|---|
| `"client"` | Every client, and a host who is also playing | UI, overlays, convenience. **`GameState.Write` and `GameProcess` are masked off its grants for the whole session**, whatever the player granted in the Advanced panel. |
| `"server"` | The host only. Never instantiated on a client. | Everything it was granted. Clients do not need the file and are never asked for it. |
| `"both"` (default) | Both ends | Everything it was granted — but the server instance is authoritative and the client instance is presentation. Must be present on both ends at the same version and the same bytes. |

Almost every mod is `"client"`. Choose `"both"` only if the mod genuinely
changes how turns resolve *and* clients need a matching copy to display the
result sensibly.

At runtime, `gearbox_env_t.net_role` tells you which side you are on, and
`gearbox_is_client()` / `gearbox_is_server()` wrap it. In singleplayer both are
true, because there is one process and it is both.

### What mod matching does and does not prove

A server lists the `"both"`-side mods it runs, and a joining client is refused
if its own set does not match by id, version and SHA-256 of the `.odmod`.

**This is an integrity check, not an anti-tamper one**, and the game says so to
players in exactly those words. A client is a program on hardware its owner
controls; it can report any mod list it likes, and nothing in an open-source
game changes that. What the check is for is the failure that actually happens:
a wrong version, a truncated download, a mod someone edited and forgot to
rebuild.

Cheating is prevented by authority instead — state only flows server to client,
orders are re-attributed to the authenticated player's country, and a
client-side mod has the write capabilities absent rather than merely useless.
A client that lies about its mods desyncs its own display and gains nothing.

### API versioning

`gearbox` is `MAJOR.MINOR`. Same major = compatible; the host provides every
minor at or below its own. A mod targeting a newer minor than the host loads
with a warning and any missing imports trap on first call. A mod targeting a
different major is refused.

## Capability modules

Permissions, in the OS sense. A mod receives imports **only** for the modules it
declared and the user granted. Anything else is simply absent from its import
object, so calling it is a link-time failure, not a runtime check that can be
bypassed. Users can revoke individual modules per mod in the mod menu's
**Advanced** panel.

| Module | Grants | Notes |
|---|---|---|
| `Core` | Logging, API/host version query, environment introspection, abort | Always granted. Cannot be revoked. |
| `GameState.Read` | Read countries, provinces, armies, treasuries, relations, turn number | Read-only. Safe default for analysis/UI mods. |
| `GameState.Write` | Mutate the above | Dangerous. Implies `GameState.Read`. |
| `GameProcess` | Turn lifecycle hooks: pre-turn, post-turn, per-country | The hook surface, not the data — pair with a `GameState.*` module. |
| `Neural` | Read AI model metadata, observe decisions, supply feature/reward overrides | Can corrupt a trained model. Warn prominently on grant. |
| `UI` | Register panels, menu entries, draw within an allotted region | Cannot draw outside its region or capture global input. |
| `Map` | Province geometry, adjacency, pixel lookups | Read-only. Large data; access is by handle, not bulk copy. |
| `Diplomacy` | Read/propose diplomatic actions and ceasefire terms | Separated from `GameState.Write` because it is the most abusable surface. |
| `Assets` | Read files under the mod's own `data/` | Read-only, scoped to the mod. No path escape, no other mod's data. |
| `Storage` | Persistent key-value store, namespaced to the mod id | Quota enforced. On web this is IndexedDB. |

Deliberately absent: any filesystem, network, process, or clock-with-identity
capability. There is no module that grants them, so they cannot be requested.

## Security

**Filesystem.** WASM has no ambient authority. WASI's `fd_*` and `path_*`
imports are **not linked**. A mod cannot open, delete, or enumerate a file — not
because we check, but because the capability does not exist in its instance.
`Assets` and `Storage` are the only data paths and both are host-mediated and
mod-scoped. This is the main reason for choosing WASM over Lua: with Lua the
default is ambient access and you spend forever taking things away.

**Memory.** Instantiated with a declared maximum (`limits.memoryPages`, clamped
to a host ceiling). Growth past it traps the module rather than the process.

**Execution time.** Fuel metering (native) / periodic yield checks (web). A mod
exceeding `fuelPerTurn` is suspended and disabled with a diagnostic, so an
infinite loop in a hook cannot hang the turn.

`mod_load` is metered separately, against `limits.loadFuel` (500,000,000 by
default), because starting an interpreter costs far more than any single turn:
a Ruby mod burns ~87M instructions and a Python one ~130M before running a line
of script. The trade is deliberate and worth stating — a mod that spins in
`mod_load` now stalls for up to that many instructions, on the order of seconds,
instead of being cut off at the per-turn budget. That is bounded, happens once,
and happens at the moment the user clicked "enable"; the per-turn budget, which
is what protects every subsequent turn, is unchanged.

**Archive limits**, enforced while streaming, before full extraction:

| Limit | Value |
|---|---|
| Total decompressed size | 256 MiB |
| Compression ratio, whole archive | 100:1 |
| Compression ratio, single entry | 200:1 |
| Entry count | 4096 |
| Path depth | 16 |
| `MANIFEST.json` | 256 KiB |
| `mod.wasm` | 64 MiB |

All of these are checked against the ZIP central directory *before* anything is
inflated, so a bomb is refused on its declared sizes and never costs memory.
Those declared sizes are attacker-controlled, which is fine — they are the upper
bound we then allocate against. An entry that inflates to more than it declared
fails extraction rather than growing a buffer; one that declares more than it
produces was already rejected. Neither path can inflate more than was screened.

Entry names are rejected if absolute, drive-qualified, containing `..`, `.`, a
backslash, a control character, or invalid UTF-8. Overlong UTF-8 sequences are
rejected specifically: two encodings of the same character are how a traversal
slips past a byte comparison.

**Nothing is extracted to disk at all.** `mod.wasm` is held in memory; `data/`
assets stay compressed and are inflated individually on request through the
`Assets` capability, so a mod shipping 200 MiB of art costs its zipped size
until something asks for a file. There is no staging directory to escape from,
which is a stronger property than sanitising paths into one.

**Signatures.** `signature.bin` is an ed25519 signature over the canonical bytes
of every entry except itself. On first install the key in `publicKey` is pinned
to the mod `id`. Later versions of that id must verify against the pinned key,
or the user gets an explicit warning naming the mismatch.

Be clear-eyed about what this buys: it proves the file was produced by whoever
holds that private key and has not been altered since. It does **not** prove
identity on first install — anyone can generate a key and claim any name. It
defends against a compromised distribution channel pushing a malicious update,
which is the realistic threat. A signed mod is not a safe mod; capability grants
are the actual security boundary.

## Environment introspection (`Core`)

Mods need to know where they are running — a UI mod should not assume a
desktop window, and a mod touching `Storage` should know quota behaviour
differs on web.

```c
typedef struct {
    uint32_t gearbox_major, gearbox_minor;
    uint32_t host_version;      // packed game version
    uint8_t  platform;          // 0 unknown, 1 windows, 2 macos, 3 linux, 4 web
    uint8_t  is_web;            // 1 when running under Emscripten
    uint8_t  is_headless;       // 1 during --train-ai
    uint8_t  net_role;          // 0 standalone, 1 client, 2 server, 3 host+player
    uint32_t screen_w, screen_h;
} gearbox_env_t;

void gearbox_core_env(gearbox_env_t* out);   // import: gearbox:core/env

int gearbox_is_client(const gearbox_env_t*);       // has a local player
int gearbox_is_server(const gearbox_env_t*);       // owns the world
int gearbox_is_multiplayer(const gearbox_env_t*);
```

`is_headless` matters: self-play training runs thousands of turns with no
renderer, and a `UI` mod must no-op there rather than trap.

`net_role` occupies the byte that was `reserved` before multiplayer existed.
The layout is unchanged — still 28 bytes, still ten fields — so a mod built
against the older struct is byte-compatible and reads `0`, which is
`STANDALONE`: the right answer for the only game it could have been running.

## SDK tiers

One C ABI (`gearbox.h`) is the reference. Everything else binds to it.

**Tier 1 — compiles to compact WASM directly.** C, C++, Rust, Zig, TinyGo,
AssemblyScript, C# (NativeAOT-LLVM). Shipped and supported first.

**Tier 2 — requires an interpreter inside the mod.** Python, Ruby, Lua, Java,
JavaScript/TypeScript. These *work* — JS via Javy/QuickJS, Python via
CPython-on-WASM, Lua via wasmoon — but each mod carries a multi-megabyte
runtime and starts slower. Supported as templates, documented with the cost
stated plainly, not presented as equivalent to Tier 1.

This is worth being blunt about in the modder docs: "we support 16 languages"
is misleading if five of them mean a 10 MB mod that starts in 400 ms.

## Phase 1 scope — delivered

1. ~~`.odmod` reader with all archive limits, manifest parse and validation,
   clear diagnostics for every rejection reason.~~
2. ~~Runtime abstraction over WAMR (desktop) and browser WebAssembly, with
   memory and fuel limits applied at instantiation.~~
3. ~~`Core`: log, env, abort, fuel budget.~~
4. ~~`UI` end to end — a mod registers a panel and draws in it, clipped to a
   rectangle the host owns.~~
5. ~~Mod menu: list, enable/disable, delete, reload one mod, reload the
   modloader behind a loading screen, Advanced per-module permissions,
   add-from-computer via file picker and drag-and-drop.~~

Beyond the original scope: `GameState.Read`, `GameProcess` and `Assets` are
implemented too, and `odmod-check` ships as a validator for modders.

Still deferred: signature verification (format reserved, not enforced),
dependency resolution (parsed, not acted on), `Neural`, `GameState.Write`,
`Diplomacy`, `Map`, `Storage`, web persistence.

## Duplicate mod ids

The `id` is the identity: it keys the persisted enable/grant state, the pinned
signing key, and the `Storage` namespace. If two files in the mods directory
declare the same id, the first wins and the second is refused with a diagnostic
naming the file that won — rather than both loading and silently sharing all
three.

## The runtime, concretely

WAMR is fetched at configure time (pinned to `WAMR-2.4.5`) and built
interpreter-only with **`WAMR_BUILD_LIBC_BUILTIN=0` and
`WAMR_BUILD_LIBC_WASI=0`**. Those two options are the sandbox: WASI's `fd_*` and
`path_*` imports are never linked, so a mod cannot open, delete or enumerate a
file because the capability does not exist in its instance.

The whole thing is optional. `-DOD_ENABLE_MODS=OFF`, or a machine that cannot
fetch WAMR, still builds a working game; the mod menu then reports the runtime
as unavailable and every mod as Failed, rather than the build breaking.

WAMR registers host functions globally per import-module name, which cannot
express per-instance grants. Enforcement therefore happens at instantiation:
every import a module declares is checked against the granted mask before it
runs. The effect is what this document promises — a mod never reaches a
capability it was not given — and the diagnostic names the missing module, which
a bare link error could not.

## Lifecycle rules

**Mods are loaded from the mod menu and nowhere else.** Not from a save, not
from a map, not from a command-line flag, not by another mod. This is a
security property, not a UI convenience: it guarantees that arriving at a mod
instantiation always required a deliberate human action in a screen that shows
what capabilities are being granted. A `.odmod` sitting in the data directory
does nothing until someone opens the menu and enables it.

A consequence worth stating for modders: there is no autorun, so a mod cannot
arrange to be present before the menu exists. Anything a mod needs at startup
must be re-established through its load hook every session.

**Web persistence is deliberately not implemented.** Mods loaded in a browser
live only for that page session and are lost on refresh. Since loading is
hot — no reload, no restart — re-adding a mod costs one interaction, which is
a better trade than an IndexedDB layer that can desync from the mod list.
Modders must not assume `Storage` survives a refresh on web; the `Core` `env`
struct exposes `is_web` precisely so a mod can adapt.

## Mods and AI learning are mutually exclusive

In-game AI learning (`Experimental` → `AI Learning`, off by default) mutates
`data/ai/model.bin` as you play. Mods can change what the AI observes and how
turns resolve. Together they silently poison a trained model, and the damage is
invisible until the model is already degraded.

The two are therefore never active at once:

- Enabling a mod while AI Learning is on → warn, and require the user to either
  cancel the load or turn AI Learning off. The warning names the model file, so
  it is clear what is at risk.
- AI Learning cannot be switched on while any mod is enabled. The Experimental
  toggle is disabled with an explanation rather than silently ignored.

This also settles turn determinism: self-play training and save replay both
assume deterministic turns, and since learning and mods can never overlap, a
`GameProcess` mod can never perturb a training run.

Headless `--train-ai` loads no mods at all — it never opens the mod menu, which
is the only load path.

## Mod states and deferred activation

A mod is never instantiated into a game that is already running. Adding one
mid-game registers it but does not start it: the game world would gain a
participant halfway through, and any turn-order or determinism guarantee would
be gone. Activation is always an explicit user action.

| State | Meaning | Shown as |
|---|---|---|
| `Disabled` | Present, not running | Normal |
| `Active` | Instantiated, hooks live | Normal |
| `PendingReload` | Enabled or newly added while a game was in progress; not instantiated | **Accented highlight** + "Needs reload to take effect" |
| `Failed` | Refused at load — bad manifest, link error, limit exceeded, trap in `mod_load` | Error styling + the diagnostic |

`PendingReload` is also the state a running mod enters when its file is replaced
on disk, so the flow is the same whether the mod is new or updated.

### Reload

Reload tears down every mod instance and re-instantiates the enabled set behind
a loading screen. It is permitted during a game — forbidding it would make
modding useless on web, where the process cannot be restarted — with one hard
constraint:

**Reload only happens between turns, never during turn processing.** A request
made mid-turn is queued and runs once the turn completes.

Because `mod_load` always runs fresh and no mod may assume prior state, reload
needs no cooperation from the mod and no state migration.

### Consequence for the ABI

There is **no state-serialisation hook**, deliberately. Mod state does not
survive a reload; `mod_unload` releases resources, `mod_load` rebuilds from
nothing. Mods needing persistence use `Storage` (and must handle it being
session-scoped on web).

## Open questions

None blocking Phase 1.
