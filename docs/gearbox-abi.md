<!-- GENERATED FILE - do not edit by hand.
     Source: sdk/abi.json   Generator: tools/gen_abi_docs.py
     Regenerate with: python3 tools/gen_abi_docs.py -->

# Gearbox ABI Reference — v1.1

The complete wire contract between a mod and the host. Every SDK under
`sdk/` is a transcription of this; if an SDK disagrees with this page, the
SDK is wrong.

This page is generated from [`sdk/abi.json`](../sdk/abi.json), which
`tests/mod_abi_test.cpp` checks against the host's real capability table in
both directions. So the chain host → abi.json → this page is verified at
build time, not maintained by hand.

- **Imports** are functions the host provides and your mod calls.
- **Exports** are functions your mod provides and the host calls.
- Imports live in WASM modules named after the capability that grants them.
  You only receive imports for capabilities you declared in `MANIFEST.json`
  **and** the user granted. Importing anything else refuses the load.

Strings are `(ptr, len)` pairs of UTF-8 bytes in your linear memory. They
are **not** null-terminated. The host never keeps a pointer into your
memory after a call returns, and you must not keep one of the host's.

## Capability modules

| Module | Import namespace | Grants | Revocable | Status |
|---|---|---|---|---|
| `Core` | `gearbox:core` | Logging, environment introspection, abort, fuel budget | **no, always granted** | implemented |
| `GameState.Read` | `gearbox:gamestate.read` | Read the turn, countries, treasuries, provinces | yes | implemented |
| `UI` | `gearbox:ui` | Register panels and draw inside them | yes | implemented |
| `Assets` | `gearbox:assets` | Read your own data/ directory | yes | implemented |
| `GameProcess` | — | Turn lifecycle hooks. Grants exports, not imports. | yes | implemented |
| `GameState.Write` | `gearbox:gamestate.write` | Mutate the world. Implies GameState.Read. | yes | implemented |
| `Neural` | `gearbox:neural` | Observe AI features and rewards (observe-only: no import writes to the model) | yes | implemented |
| `Map` | `gearbox:map` | Province geometry and adjacency | yes | implemented |
| `Diplomacy` | `gearbox:diplomacy` | Read and propose diplomatic actions | yes | implemented |
| `Storage` | `gearbox:storage` | Persistent key-value store namespaced to your mod id | yes | implemented |
| `Audio` | `gearbox:audio` | Play and stop sounds from your own mod's assets | yes | implemented |
| `Net` | `gearbox:net` | Send and receive messages between copies of YOUR OWN mod | yes | implemented |
| `WasiStub` | `wasi_snapshot_preview1` | Minimal WASI shim so an interpreter-in-a-mod can boot. NOT a WASI implementation: no filesystem, deterministic randomness, no wall clock. | yes | implemented |
| `Military.Read` | `gearbox:military.read` | Read ships, armies, fortifications and ports | yes | implemented |
| `Military.Write` | `gearbox:military.write` | Queue army and ship orders through the turn resolver | yes | implemented |
| `Research.Read` | `gearbox:research.read` | Read the technology tree and what each country has researched | yes | implemented |
| `Research.Write` | `gearbox:research.write` | Set research funding | yes | implemented |
| `Politics.Read` | `gearbox:politics.read` | Read the political compass, policies, unrest and minorities | yes | implemented |
| `Politics.Write` | `gearbox:politics.write` | Enact and cancel policies through the game's own path | yes | implemented |
| `Economy.Read` | `gearbox:economy.read` | Read income, upkeep, industry and resources | yes | implemented |
| `Economy.Write` | `gearbox:economy.write` | Set province industry level | yes | implemented |
| `MapEditor` | `gearbox:mapeditor` | Read and write the open map editor project; inert outside the editor | yes | implemented |

Requesting a module marked *not implemented* means the imports do not
exist, so your mod is **refused at load** with a diagnostic naming the
import. That is deliberate: running you without a capability you believe
you hold would be worse than refusing.

`GameProcess` grants no imports. It gates whether the host *calls* your
`mod_pre_turn` / `mod_post_turn` exports.

## Imports

### `gearbox:core`

Requires the **Core** capability. Always granted; cannot be revoked.

#### `log`

```wat
(import "gearbox:core" "log" (func $x (param i32 i32 i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `level` | `i32` | see [log_level](#enums) |
| `msg` | `i32` | pointer into your linear memory |
| `msg_len` | `i32` | byte length |

**Returns** nothing.

Write a line to the game log and the mod menu's log view. Messages longer than 2048 bytes are truncated. An out-of-bounds (ptr,len) is refused and logged as an error against your mod rather than read.

#### `env`

```wat
(import "gearbox:core" "env" (func $x (param i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `out` | `i32` | pointer into your linear memory |

**Returns** nothing.

Fill a gearbox_env_t. Write your own sizeof into out->size FIRST; the host writes at most that many bytes, so an older mod stays safe against a newer host. If size is 0 or larger than the host's struct, the host uses its own size.

#### `abort`

```wat
(import "gearbox:core" "abort" (func $x (param i32 i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `msg` | `i32` | pointer into your linear memory |
| `msg_len` | `i32` | byte length |

**Returns** nothing.

**Does not return.** The call traps out of your mod.

Unrecoverable error. Traps out of the current call, disables the mod, and shows the message to the user. Prefer returning an error from a hook where you can.

#### `fuel_budget`

```wat
(import "gearbox:core" "fuel_budget" (func $x (result i64)))
```

**Returns** `i64`.

The instruction budget for the current hook, or 0xFFFFFFFFFFFFFFFF when unmetered. This is the LIMIT, not a live countdown: it does not decrease as you run. Use it to size your work up front and count your own iterations.

### `gearbox:gamestate.read`

Requires the **GameState.Read** capability.

#### `turn_number`

```wat
(import "gearbox:gamestate.read" "turn_number" (func $x (result i32)))
```

**Returns** `i32`.

The current turn. 0 when no world is loaded.

#### `country_count`

```wat
(import "gearbox:gamestate.read" "country_count" (func $x (result i32)))
```

**Returns** `i32`.

How many countries exist. 0 when no world is loaded. Rebel factions are not included.

#### `country_at`

```wat
(import "gearbox:gamestate.read" "country_at" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |

**Returns** `i32` — opaque country handle.

The country at index in [0, country_count). Returns GEARBOX_INVALID (0xFFFFFFFF) if out of range. Ordering is stable within a turn but not across turns.

#### `country_name`

```wat
(import "gearbox:gamestate.read" "country_name" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | opaque country handle |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

Two-call sizing. Writes at most cap bytes of UTF-8 and returns the FULL length. Call with cap 0 to size, then again to fill. A return greater than cap means truncation, not failure. Returns 0 for an unknown country.

#### `country_treasury`

```wat
(import "gearbox:gamestate.read" "country_treasury" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | opaque country handle |

**Returns** `f64`.

Treasury balance. 0 for an unknown country.

#### `country_province_count`

```wat
(import "gearbox:gamestate.read" "country_province_count" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | opaque country handle |

**Returns** `i32`.

How many provinces the country owns. 0 for an unknown country.

#### `province_population`

```wat
(import "gearbox:gamestate.read" "province_population" (func $x (param i32) (result i64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |

**Returns** `i64`.

Population of a province. 0 for an unknown province.

#### `province_owner`

```wat
(import "gearbox:gamestate.read" "province_owner" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |

**Returns** `i32` — opaque country handle.

Owning country, or GEARBOX_INVALID if unowned or unknown.

### `gearbox:ui`

Requires the **UI** capability.

#### `panel_register`

```wat
(import "gearbox:ui" "panel_register" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `title` | `i32` | pointer into your linear memory |
| `title_len` | `i32` | byte length |
| `min_w` | `i32` | — |
| `min_h` | `i32` | — |

**Returns** `i32` — opaque panel handle.

Register a panel and return its handle. Returns 0 (invalid) when headless, when UI was revoked, or when you already hold 8 panels. Titles are truncated to 64 bytes. Call this from mod_load, not from your draw hook.

#### `draw_rect`

```wat
(import "gearbox:ui" "draw_rect" (func $x (param i32 i32 i32 i32 i32 i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |
| `x` | `i32` | — |
| `y` | `i32` | — |
| `w` | `i32` | — |
| `h` | `i32` | — |
| `rgba` | `i32` | `0xRRGGBBAA` |

**Returns** nothing.

Filled rectangle in panel-relative coordinates. Colour is 0xRRGGBBAA. Coordinates outside the panel are clipped by the host; they cannot escape it.

#### `draw_text`

```wat
(import "gearbox:ui" "draw_text" (func $x (param i32 i32 i32 i32 i32 i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |
| `x` | `i32` | — |
| `y` | `i32` | — |
| `rgba` | `i32` | `0xRRGGBBAA` |
| `text` | `i32` | pointer into your linear memory |
| `text_len` | `i32` | byte length |

**Returns** nothing.

UTF-8 text in panel-relative coordinates. Truncated to 512 bytes per call.

#### `button`

```wat
(import "gearbox:ui" "button" (func $x (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |
| `x` | `i32` | — |
| `y` | `i32` | — |
| `w` | `i32` | — |
| `h` | `i32` | — |
| `label` | `i32` | pointer into your linear memory |
| `label_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Immediate-mode button: draws it and returns 1 on the frame it is clicked. One click activates one button -- the host consumes it, so overlapping rects do not all fire. Label truncated to 64 bytes.

#### `draw_line`

```wat
(import "gearbox:ui" "draw_line" (func $x (param i32 i32 i32 i32 i32 f64 i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |
| `x1` | `i32` | — |
| `y1` | `i32` | — |
| `x2` | `i32` | — |
| `y2` | `i32` | — |
| `thickness` | `f64` | — |
| `rgba` | `i32` | — |

**Returns** nothing.

Queue a line from (x1,y1) to (x2,y2) in panel-relative pixels. Thickness is clamped to 0.25..64. Clipped to your panel like every other command.

#### `draw_circle`

```wat
(import "gearbox:ui" "draw_circle" (func $x (param i32 i32 i32 f64 i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |
| `cx` | `i32` | — |
| `cy` | `i32` | — |
| `radius` | `f64` | — |
| `rgba` | `i32` | — |

**Returns** nothing.

Queue a filled circle centred at (cx,cy), panel-relative. Radius is clamped to 0..4096.

#### `draw_image`

```wat
(import "gearbox:ui" "draw_image" (func $x (param i32 i32 i32 i32 i32 i32 i32 i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |
| `x` | `i32` | — |
| `y` | `i32` | — |
| `w` | `i32` | — |
| `h` | `i32` | — |
| `name` | `i32` | pointer into your linear memory |
| `name_len` | `i32` | byte length |
| `tint` | `i32` | — |

**Returns** nothing.

Queue an image from YOUR OWN package -- `name` is a path inside your .odmod, resolved exactly as gearbox:assets/read resolves it, so you cannot name a file on disk, a game asset, or another mod's art. Pass w or h as 0 to use the image's own size. tint 0xFFFFFFFF draws it unmodified. Decoded once and cached; a name that fails to decode draws nothing and does not retry. PNG, JPG, BMP, TGA and GIF are recognised by extension. This is the call that makes a real reskin possible.

#### `draw_text_sized`

```wat
(import "gearbox:ui" "draw_text_sized" (func $x (param i32 i32 i32 i32 i32 i32 i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |
| `x` | `i32` | — |
| `y` | `i32` | — |
| `size` | `i32` | — |
| `rgba` | `i32` | — |
| `text` | `i32` | pointer into your linear memory |
| `text_len` | `i32` | byte length |

**Returns** nothing.

Like draw_text but with a type size, clamped to 6..96. draw_text remains 14pt, unchanged, so v1.0 mods look exactly as they did.

#### `measure_text`

```wat
(import "gearbox:ui" "measure_text" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `text` | `i32` | pointer into your linear memory |
| `text_len` | `i32` | byte length |
| `size` | `i32` | — |

**Returns** `i32`.

Width in pixels of `text` at `size`, measured with the font the game will actually draw. Centring, right-alignment and wrapping all need this before the text is queued.

#### `panel_width`

```wat
(import "gearbox:ui" "panel_width" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |

**Returns** `i32`.

The width the host assigned your panel this frame, in pixels. Lay out against this rather than against min_w -- the host may have given you more.

#### `panel_height`

```wat
(import "gearbox:ui" "panel_height" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |

**Returns** `i32`.

The height the host assigned your panel this frame, in pixels.

#### `panel_set_visible`

```wat
(import "gearbox:ui" "panel_set_visible" (func $x (param i32 i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |
| `visible` | `i32` | 0 or 1 |

**Returns** nothing.

Show or hide one of your panels. A hidden panel is not drawn and receives no input, but keeps its handle and its registration.

#### `mouse_x`

```wat
(import "gearbox:ui" "mouse_x" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |

**Returns** `f64`.

Cursor X, panel-relative, or 0 when the cursor is not over your panel. You cannot observe the pointer outside your own box.

#### `mouse_y`

```wat
(import "gearbox:ui" "mouse_y" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |

**Returns** `f64`.

Cursor Y, panel-relative, or 0 when the cursor is not over your panel.

#### `mouse_inside`

```wat
(import "gearbox:ui" "mouse_inside" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |

**Returns** `i32` — 0 or 1.

Whether the cursor is over your panel this frame.

#### `theme_accent`

```wat
(import "gearbox:ui" "theme_accent" (func $x (result i32)))
```

**Returns** `i32`.

The PLAYER's accent colour as 0x00RRGGBB -- not another mod's override. Build your palette around this and you harmonise with what they chose.

#### `set_theme_accent`

```wat
(import "gearbox:ui" "set_theme_accent" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `rgb` | `i32` | — |

**Returns** `i32` — 0 or 1.

Restyle the whole interface. The accent is read at over a hundred sites -- every heading, highlight, selection and button -- so this is the cheapest full reskin there is. It is NOT persisted: the game's settings file keeps the player's own colour, and the override is dropped the moment no mod is running, so it cannot outlive uninstalling you.

### `gearbox:assets`

Requires the **Assets** capability.

#### `size`

```wat
(import "gearbox:assets" "size" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `name` | `i32` | pointer into your linear memory |
| `name_len` | `i32` | byte length |

**Returns** `i32` — byte length.

Byte size of one of your own data/ files, or 0 if there is no such asset. Names are relative to data/ and use '/' separators: data/flags/fr.png is "flags/fr.png".

#### `read`

```wat
(import "gearbox:assets" "read" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `name` | `i32` | pointer into your linear memory |
| `name_len` | `i32` | byte length |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

Two-call sizing, like country_name. Writes at most cap bytes and returns the asset's full size. The name is looked up in your package's entry list, never resolved as a filesystem path.

### `gearbox:audio`

Requires the **Audio** capability.

#### `play`

```wat
(import "gearbox:audio" "play" (func $x (param i32 i32 f32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `path` | `i32` | pointer into your linear memory |
| `path_len` | `i32` | byte length |
| `volume` | `f32` | — |

**Returns** `i32`.

Play a sound from your own mod's assets. `path` is relative to your mod root; a path outside it is refused rather than resolved. Volume is 0..1 and is multiplied by the player's own effects setting, so a mod cannot be louder than they allowed. Returns a handle, or 0 if it could not be played.

#### `stop`

```wat
(import "gearbox:audio" "stop" (func $x (param i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `handle` | `i32` | — |

**Returns** nothing.

Stop a sound this mod started. A handle belonging to another mod, or one that already finished, does nothing.

#### `set_volume`

```wat
(import "gearbox:audio" "set_volume" (func $x (param i32 f32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `handle` | `i32` | — |
| `volume` | `f32` | — |

**Returns** nothing.

Change the volume of a playing sound, 0..1, again scaled by the player's setting.

#### `is_playing`

```wat
(import "gearbox:audio" "is_playing" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `handle` | `i32` | — |

**Returns** `i32` — 0 or 1.

Whether that handle is still making sound.

### `gearbox:net`

Requires the **Net** capability.

#### `send`

```wat
(import "gearbox:net" "send" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `peer` | `i32` | — |
| `data` | `i32` | pointer into your linear memory |
| `data_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Send a message to the same mod running on another peer. `peer` is a peer id -- the value `recv` reported in `from_peer` -- and -1 broadcasts to every other peer, the host included. There is no fixed id for the host: a host that plays holds an ordinary seat, and a dedicated one holds none. The host stamps your mod id on the message, so you cannot send as another mod, and it never carries game traffic: orders, deltas and chat do not travel here. Messages larger than 8192 bytes are refused. Returns 0 if this is not a network game, or the message was too large.

#### `recv`

```wat
(import "gearbox:net" "recv" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `out` | `i32` | pointer into your linear memory |
| `out_len` | `i32` | byte length |
| `from_peer` | `i32` | pointer into your linear memory |

**Returns** `i32` — byte length.

Take the next message addressed to this mod, writing it into `out` and the sender's peer id into `from_peer`. Returns the number of bytes written, or 0 when the queue is empty. A message longer than `out_len` is truncated rather than dropped, so a small buffer loses data instead of stalling the queue.

#### `peer_count`

```wat
(import "gearbox:net" "peer_count" (func $x (result i32)))
```

**Returns** `i32`.

How many players this session has, a playing host included. 0 when this is not a network game, which is how a mod tells the difference. Spectators are not counted.

#### `self_peer`

```wat
(import "gearbox:net" "self_peer" (func $x (result i32)))
```

**Returns** `i32`.

This machine's own peer id. 0 means this is not a network game, or this is a dedicated host holding no seat -- a host that plays has an ordinary peer id like anyone else, so do not use this to tell host from client. `is_host` is that question.

#### `is_host`

```wat
(import "gearbox:net" "is_host" (func $x (result i32)))
```

**Returns** `i32` — 0 or 1.

Whether this copy is the authoritative one. A mod that computes anything the game depends on must do it here and send the result, not compute it separately on each machine.

#### `peer_at`

```wat
(import "gearbox:net" "peer_at" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |

**Returns** `i32`.

The peer id at `index` in 0..peer_count-1, or 0xFFFFFFFF past the end. This is the id net/send takes.

#### `peer_name`

```wat
(import "gearbox:net" "peer_name" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

That peer's display name -- deliberately NOT their account id or issuer. A mod has no business correlating players across sessions. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `max_message_bytes`

```wat
(import "gearbox:net" "max_message_bytes" (func $x (result i32)))
```

**Returns** `i32`.

The largest payload net/send will accept. Chunk against this rather than discovering the limit by having a message dropped.

### `wasi_snapshot_preview1`

Requires the **WasiStub** capability.

#### `fd_write`

```wat
(import "wasi_snapshot_preview1" "fd_write" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `iovs` | `i32` | — |
| `iovs_len` | `i32` | — |
| `nwritten` | `i32` | — |

**Returns** `i32`.

Writes to fd 1 or 2 only, and the bytes go to the mod log where the user can see them. Any other fd returns EBADF. This is how print() in an interpreted mod reaches you.

#### `proc_exit`

```wat
(import "wasi_snapshot_preview1" "proc_exit" (func $x (param i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `code` | `i32` | — |

**Returns** nothing.

Traps the mod. A runtime calling exit() must not return into code that believes the process is gone.

#### `random_get`

```wat
(import "wasi_snapshot_preview1" "random_get" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `buf` | `i32` | — |
| `buf_len` | `i32` | — |

**Returns** `i32`.

DETERMINISTIC pseudo-random bytes, seeded from your mod id -- not OS entropy. Real entropy is a machine fingerprint and would make self-play and save replay irreproducible. Two mods get different streams; one mod gets the same stream every run. Do not use for anything security-relevant.

#### `clock_time_get`

```wat
(import "wasi_snapshot_preview1" "clock_time_get" (func $x (param i32 i64 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `clock_id` | `i32` | — |
| `precision` | `i64` | — |
| `time` | `i32` | — |

**Returns** `i32`.

Returns the turn number expressed as nanoseconds, not the wall clock. Monotonic and coarse. A mod cannot learn the real date, the timezone, or how long anything took -- all of which are fingerprints.

#### `environ_sizes_get`

```wat
(import "wasi_snapshot_preview1" "environ_sizes_get" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `count` | `i32` | — |
| `buf_size` | `i32` | — |

**Returns** `i32`.

Always reports zero variables. There is no environment.

#### `environ_get`

```wat
(import "wasi_snapshot_preview1" "environ_get" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `environ` | `i32` | — |
| `buf` | `i32` | — |

**Returns** `i32`.

No-op; there is no environment to write.

#### `args_sizes_get`

```wat
(import "wasi_snapshot_preview1" "args_sizes_get" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `count` | `i32` | — |
| `buf_size` | `i32` | — |

**Returns** `i32`.

Always reports zero arguments. Reports success rather than failing, because runtimes commonly abort at startup if argv cannot be read.

#### `args_get`

```wat
(import "wasi_snapshot_preview1" "args_get" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `argv` | `i32` | — |
| `buf` | `i32` | — |

**Returns** `i32`.

No-op; there are no arguments.

#### `fd_close`

```wat
(import "wasi_snapshot_preview1" "fd_close" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |

**Returns** `i32`.

EBADF. There are no real descriptors.

#### `fd_fdstat_get`

```wat
(import "wasi_snapshot_preview1" "fd_fdstat_get" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `stat` | `i32` | — |

**Returns** `i32`.

ENOTCAPABLE.

#### `fd_prestat_get`

```wat
(import "wasi_snapshot_preview1" "fd_prestat_get" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `prestat` | `i32` | — |

**Returns** `i32`.

ENOTCAPABLE. No preopened directories, so no filesystem root exists to walk.

#### `fd_prestat_dir_name`

```wat
(import "wasi_snapshot_preview1" "fd_prestat_dir_name" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `path` | `i32` | — |
| `path_len` | `i32` | — |

**Returns** `i32`.

ENOTCAPABLE.

#### `fd_read`

```wat
(import "wasi_snapshot_preview1" "fd_read" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `iovs` | `i32` | — |
| `iovs_len` | `i32` | — |
| `nread` | `i32` | — |

**Returns** `i32`.

ENOTCAPABLE. There is no stdin and no file to read.

#### `fd_seek`

```wat
(import "wasi_snapshot_preview1" "fd_seek" (func $x (param i32 i64 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `offset` | `i64` | — |
| `whence` | `i32` | — |
| `newoffset` | `i32` | — |

**Returns** `i32`.

EBADF.

#### `path_open`

```wat
(import "wasi_snapshot_preview1" "path_open" (func $x (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `dirfd` | `i32` | — |
| `dirflags` | `i32` | — |
| `path` | `i32` | — |
| `path_len` | `i32` | — |
| `oflags` | `i32` | — |
| `fs_rights_base` | `i64` | — |
| `fs_rights_inheriting` | `i64` | — |
| `fdflags` | `i32` | — |
| `fd` | `i32` | — |

**Returns** `i32`.

ENOTCAPABLE, always. Opening a file is refused rather than stubbed -- this is the single import that would turn the shim into the hole the sandbox exists to prevent.

#### `clock_res_get`

```wat
(import "wasi_snapshot_preview1" "clock_res_get" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `id` | `i32` | — |
| `out` | `i32` | — |

**Returns** `i32`.

Reports a fixed, coarse resolution matching clock_time_get's one-tick-per-turn granularity. Claiming anything finer would be a lie a runtime might act on.

#### `sched_yield`

```wat
(import "wasi_snapshot_preview1" "sched_yield" (func $x (result i32)))
```

**Returns** `i32`.

Succeeds and does nothing. A mod is single-threaded; there is nothing to yield to.

#### `fd_advise`

```wat
(import "wasi_snapshot_preview1" "fd_advise" (func $x (param i32 i64 i64 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `offset` | `i64` | — |
| `len` | `i64` | — |
| `advice` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). There is no filesystem.

#### `fd_allocate`

```wat
(import "wasi_snapshot_preview1" "fd_allocate" (func $x (param i32 i64 i64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `offset` | `i64` | — |
| `len` | `i64` | — |

**Returns** `i32`.

Refused (EBADF). There is no filesystem.

#### `fd_datasync`

```wat
(import "wasi_snapshot_preview1" "fd_datasync" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). There is no filesystem.

#### `fd_sync`

```wat
(import "wasi_snapshot_preview1" "fd_sync" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). There is no filesystem.

#### `fd_fdstat_set_flags`

```wat
(import "wasi_snapshot_preview1" "fd_fdstat_set_flags" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `flags` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). There is no filesystem.

#### `fd_filestat_get`

```wat
(import "wasi_snapshot_preview1" "fd_filestat_get" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `out` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). File metadata would describe a filesystem the mod cannot reach.

#### `fd_tell`

```wat
(import "wasi_snapshot_preview1" "fd_tell" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `out` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). There is no filesystem.

#### `fd_renumber`

```wat
(import "wasi_snapshot_preview1" "fd_renumber" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `from` | `i32` | — |
| `to` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). There are no descriptors to renumber.

#### `fd_filestat_set_size`

```wat
(import "wasi_snapshot_preview1" "fd_filestat_set_size" (func $x (param i32 i64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `size` | `i64` | — |

**Returns** `i32`.

Refused (EBADF). There is no filesystem.

#### `fd_filestat_set_times`

```wat
(import "wasi_snapshot_preview1" "fd_filestat_set_times" (func $x (param i32 i64 i64 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `atim` | `i64` | — |
| `mtim` | `i64` | — |
| `fst_flags` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). There is no filesystem.

#### `fd_pread`

```wat
(import "wasi_snapshot_preview1" "fd_pread" (func $x (param i32 i32 i32 i64 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `iovs` | `i32` | — |
| `iovs_len` | `i32` | — |
| `offset` | `i64` | — |
| `nread` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). Reading is not provided; assets come from the Assets module instead.

#### `fd_pwrite`

```wat
(import "wasi_snapshot_preview1" "fd_pwrite" (func $x (param i32 i32 i32 i64 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `iovs` | `i32` | — |
| `iovs_len` | `i32` | — |
| `offset` | `i64` | — |
| `nwritten` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). A mod cannot write files; use the host log.

#### `fd_readdir`

```wat
(import "wasi_snapshot_preview1" "fd_readdir" (func $x (param i32 i32 i32 i64 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `buf` | `i32` | — |
| `buf_len` | `i32` | — |
| `cookie` | `i64` | — |
| `bufused` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). Listing a directory would reveal a filesystem the sandbox denies.

#### `path_create_directory`

```wat
(import "wasi_snapshot_preview1" "path_create_directory" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `path` | `i32` | — |
| `path_len` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). There is no filesystem, by design.

#### `path_remove_directory`

```wat
(import "wasi_snapshot_preview1" "path_remove_directory" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `path` | `i32` | — |
| `path_len` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). There is no filesystem, by design.

#### `path_unlink_file`

```wat
(import "wasi_snapshot_preview1" "path_unlink_file" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `path` | `i32` | — |
| `path_len` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). There is no filesystem, by design.

#### `path_filestat_get`

```wat
(import "wasi_snapshot_preview1" "path_filestat_get" (func $x (param i32 i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `flags` | `i32` | — |
| `path` | `i32` | — |
| `path_len` | `i32` | — |
| `out` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). Answering would tell a mod whether a path exists on your machine.

#### `path_symlink`

```wat
(import "wasi_snapshot_preview1" "path_symlink" (func $x (param i32 i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `old_path` | `i32` | — |
| `old_path_len` | `i32` | — |
| `fd` | `i32` | — |
| `new_path` | `i32` | — |
| `new_path_len` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). There is no filesystem, by design.

#### `path_readlink`

```wat
(import "wasi_snapshot_preview1" "path_readlink" (func $x (param i32 i32 i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `path` | `i32` | — |
| `path_len` | `i32` | — |
| `buf` | `i32` | — |
| `buf_len` | `i32` | — |
| `bufused` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). There is no filesystem, by design.

#### `path_rename`

```wat
(import "wasi_snapshot_preview1" "path_rename" (func $x (param i32 i32 i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `old_path` | `i32` | — |
| `old_path_len` | `i32` | — |
| `new_fd` | `i32` | — |
| `new_path` | `i32` | — |
| `new_path_len` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). There is no filesystem, by design.

#### `path_link`

```wat
(import "wasi_snapshot_preview1" "path_link" (func $x (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `old_fd` | `i32` | — |
| `old_flags` | `i32` | — |
| `old_path` | `i32` | — |
| `old_path_len` | `i32` | — |
| `new_fd` | `i32` | — |
| `new_path` | `i32` | — |
| `new_path_len` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). There is no filesystem, by design.

#### `path_filestat_set_times`

```wat
(import "wasi_snapshot_preview1" "path_filestat_set_times" (func $x (param i32 i32 i32 i32 i64 i64 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `flags` | `i32` | — |
| `path` | `i32` | — |
| `path_len` | `i32` | — |
| `atim` | `i64` | — |
| `mtim` | `i64` | — |
| `fst_flags` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). There is no filesystem, by design.

#### `poll_oneoff`

```wat
(import "wasi_snapshot_preview1" "poll_oneoff" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `in` | `i32` | — |
| `out` | `i32` | — |
| `nsubscriptions` | `i32` | — |
| `nevents` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). There is nothing to poll: no files, no sockets, no timers a mod may wait on.

#### `sock_accept`

```wat
(import "wasi_snapshot_preview1" "sock_accept" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `flags` | `i32` | — |
| `out_fd` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). A mod has no network access, which is much of the point of the sandbox.

#### `sock_recv`

```wat
(import "wasi_snapshot_preview1" "sock_recv" (func $x (param i32 i32 i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `ri_data` | `i32` | — |
| `ri_data_len` | `i32` | — |
| `ri_flags` | `i32` | — |
| `ro_datalen` | `i32` | — |
| `ro_flags` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). A mod has no network access.

#### `sock_send`

```wat
(import "wasi_snapshot_preview1" "sock_send" (func $x (param i32 i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `si_data` | `i32` | — |
| `si_data_len` | `i32` | — |
| `si_flags` | `i32` | — |
| `so_datalen` | `i32` | — |

**Returns** `i32`.

Refused (ENOTCAPABLE). A mod has no network access.

#### `sock_shutdown`

```wat
(import "wasi_snapshot_preview1" "sock_shutdown" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `fd` | `i32` | — |
| `how` | `i32` | — |

**Returns** `i32`.

Refused (EBADF). A mod has no network access.

### `gearbox:storage`

Requires the **Storage** capability.

#### `get`

```wat
(import "gearbox:storage" "get" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `key` | `i32` | pointer into your linear memory |
| `key_len` | `i32` | byte length |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

Reads one of your own keys. Two-call sizing: returns the full value length and writes at most cap bytes. Returns GEARBOX_INVALID if the key is absent -- which is NOT the same as a zero-length value, so you can tell 'never stored' from 'stored empty'. Values are arbitrary bytes, not text.

#### `set`

```wat
(import "gearbox:storage" "set" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `key` | `i32` | pointer into your linear memory |
| `key_len` | `i32` | byte length |
| `value` | `i32` | pointer into your linear memory |
| `value_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Stores bytes under one of your own keys. Returns 1 on success, 0 if a quota was exceeded (256 keys, 128-byte keys, 16 KiB per value, 64 KiB total per mod) -- the reason is written to your log. Not written to disk immediately: the store is flushed at turn boundaries and on unload, because a mod may call this from a draw hook.

#### `remove`

```wat
(import "gearbox:storage" "remove" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `key` | `i32` | pointer into your linear memory |
| `key_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Deletes one of your own keys. Returns 1 if it existed, 0 if it did not.

### `gearbox:map`

Requires the **Map** capability.

#### `width`

```wat
(import "gearbox:map" "width" (func $x (result i32)))
```

**Returns** `i32`.

Width of the province map in pixels. 0 when no world is loaded.

#### `height`

```wat
(import "gearbox:map" "height" (func $x (result i32)))
```

**Returns** `i32`.

Height of the province map in pixels. 0 when no world is loaded.

#### `province_count`

```wat
(import "gearbox:map" "province_count" (func $x (result i32)))
```

**Returns** `i32`.

How many provinces the loaded map has. 0 when no world is loaded.

#### `province_at`

```wat
(import "gearbox:map" "province_at" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |

**Returns** `i32` — opaque province handle.

Province handle at an index in [0, province_count). Returns GEARBOX_INVALID if out of range. The order is stable across runs, unlike the game's internal storage, so an index is safe to remember within a session.

#### `province_name`

```wat
(import "gearbox:map" "province_name" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The province's name. Two-call sizing: returns the full length and writes at most cap bytes. Empty for an unknown province.

#### `province_center_x`

```wat
(import "gearbox:map" "province_center_x" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |

**Returns** `f64`.

X pixel coordinate of the province's centre. 0 for an unknown province.

#### `province_center_y`

```wat
(import "gearbox:map" "province_center_y" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |

**Returns** `f64`.

Y pixel coordinate of the province's centre. 0 for an unknown province.

#### `province_is_land`

```wat
(import "gearbox:map" "province_is_land" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |

**Returns** `i32` — 0 or 1.

1 if the province is land, 0 if it is sea or unknown. Sampled at the province centre.

#### `province_neighbor_count`

```wat
(import "gearbox:map" "province_neighbor_count" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |

**Returns** `i32`.

How many provinces border this one. 0 for an unknown province.

#### `province_neighbor_at`

```wat
(import "gearbox:map" "province_neighbor_at" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |
| `index` | `i32` | — |

**Returns** `i32` — opaque province handle.

The bordering province at an index in [0, province_neighbor_count). GEARBOX_INVALID if out of range. Adjacency is computed once when the map loads, so walking it is cheap.

#### `province_is_coastal`

```wat
(import "gearbox:map" "province_is_coastal" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i32` — 0 or 1.

Whether a province touches water. Ports, embarking and naval bombardment all require it.

#### `sea_route_exists`

```wat
(import "gearbox:map" "sea_route_exists" (func $x (param f64 f64 f64 f64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `from_lon` | `f64` | — |
| `from_lat` | `f64` | — |
| `to_lon` | `f64` | — |
| `to_lat` | `f64` | — |

**Returns** `i32` — 0 or 1.

Whether a fleet could get from one point to another by sea, using the game's own navigation grid. You cannot compute this from province neighbours: those describe LAND adjacency.

#### `point_is_land`

```wat
(import "gearbox:map" "point_is_land" (func $x (param f64 f64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `lon` | `f64` | — |
| `lat` | `f64` | — |

**Returns** `i32` — 0 or 1.

Whether a world coordinate is land. Ordering a ship onto land is not an error -- the resolver clamps it -- but knowing first is cheaper.

### `gearbox:diplomacy`

Requires the **Diplomacy** capability.

#### `at_war`

```wat
(import "gearbox:diplomacy" "at_war" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `a` | `i32` | opaque country handle |
| `b` | `i32` | opaque country handle |

**Returns** `i32` — 0 or 1.

1 if the two countries are at war. Relations are symmetric, so the argument order does not matter. 0 for unknown countries or for a country with itself.

#### `allied`

```wat
(import "gearbox:diplomacy" "allied" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `a` | `i32` | opaque country handle |
| `b` | `i32` | opaque country handle |

**Returns** `i32` — 0 or 1.

1 if the two countries are allied.

#### `non_aggression`

```wat
(import "gearbox:diplomacy" "non_aggression" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `a` | `i32` | opaque country handle |
| `b` | `i32` | opaque country handle |

**Returns** `i32` — 0 or 1.

1 if the two countries have a non-aggression pact.

#### `guaranteed`

```wat
(import "gearbox:diplomacy" "guaranteed" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `a` | `i32` | opaque country handle |
| `b` | `i32` | opaque country handle |

**Returns** `i32` — 0 or 1.

1 if the first country guarantees the second.

#### `propose_war`

```wat
(import "gearbox:diplomacy" "propose_war" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `attacker` | `i32` | opaque country handle |
| `defender` | `i32` | opaque country handle |

**Returns** `i32` — 0 or 1.

PROPOSES a declaration of war, and returns 1 only if the game accepted it. It is routed through the same code path any other actor uses, so guarantee chains and war consequences follow exactly as normal -- a mod cannot produce a diplomatic state the game itself could not reach. Refused (0) if either country is unknown, they are the same country, or they are already at war. Either outcome is written to your mod log, so a player can see after the fact that a mod started a war.

### `gearbox:gamestate.write`

Requires the **GameState.Write** capability.

#### `set_country_treasury`

```wat
(import "gearbox:gamestate.write" "set_country_treasury" (func $x (param i32 f64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | opaque country handle |
| `value` | `f64` | — |

**Returns** `i32` — 0 or 1.

Sets a country's treasury outright. Returns 1 on success, 0 if the country is unknown or the value is not finite and within +/-1e12 -- NaN or infinity would silently poison every later calculation, so they are refused rather than stored.

#### `add_country_treasury`

```wat
(import "gearbox:gamestate.write" "add_country_treasury" (func $x (param i32 f64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | opaque country handle |
| `delta` | `f64` | — |

**Returns** `i32` — 0 or 1.

Adds to a country's treasury. Usually what you want instead of set: it composes with whatever the economy did this turn. Refused (0) if the result would leave the sane range.

#### `set_province_owner`

```wat
(import "gearbox:gamestate.write" "set_province_owner" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |
| `country` | `i32` | opaque country handle |

**Returns** `i32` — 0 or 1.

Transfers a province to another country. Routed through the same code the game's own ceasefires use, so the province, the ownership lookup, the per-pixel country map and both countries' pixel lists all stay consistent -- and the previous owner's troops in that province are disbanded, as they are for any other transfer. Returns 0 if either handle is unknown or the country already owns it. Always written to your mod log: territory changing hands is the most consequential thing a mod can do.

#### `set_province_population`

```wat
(import "gearbox:gamestate.write" "set_province_population" (func $x (param i32 i64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | opaque province handle |
| `value` | `i64` | — |

**Returns** `i32` — 0 or 1.

Sets a province's population. Returns 0 if the province is unknown, the value is negative, or it exceeds 100 billion. The game stores population in two places -- a map and a dense array used by the population texture -- and this updates both, which is why it exists as an import rather than being something a mod could do by other means.

### `gearbox:neural`

Requires the **Neural** capability.

#### `feature_count`

```wat
(import "gearbox:neural" "feature_count" (func $x (result i32)))
```

**Returns** `i32`.

How many floats are in the AI's feature vector. 0 when there is no AI or no world.

#### `features`

```wat
(import "gearbox:neural" "features" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | opaque country handle |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

Copies the feature vector the AI would see for a country, as 32-bit floats. Two-call sizing, but note cap counts FLOATS and the buffer must therefore be cap*4 bytes. This is a snapshot: writing to your copy does not affect the AI.

#### `reward_count`

```wat
(import "gearbox:neural" "reward_count" (func $x (result i32)))
```

**Returns** `i32`.

How many reward channels the AI tracks (economy, politics, war, navy).

#### `reward_mean`

```wat
(import "gearbox:neural" "reward_mean" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |

**Returns** `f64`.

The running mean reward for one channel, indexed in [0, reward_count). 0 if out of range. OBSERVE ONLY: this capability has no import that writes to the model, the optimiser state or the reward history, which is deliberate -- a trained model is hours of work and a mod that could quietly retrain it is not something a user can meaningfully consent to.

#### `module_count`

```wat
(import "gearbox:neural" "module_count" (func $x (result i32)))
```

**Returns** `i32`.

How many decision modules the AI has. Each acts independently every turn.

#### `module_name`

```wat
(import "gearbox:neural" "module_name" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `module` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The module's name: "economy", "politics", "war", "navy". Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `action_count`

```wat
(import "gearbox:neural" "action_count" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `module` | `i32` | — |

**Returns** `i32`.

How many actions that module can choose between.

#### `action_name`

```wat
(import "gearbox:neural" "action_name" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `module` | `i32` | — |
| `action` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The action's name, e.g. "reinforce", "embark", "propose_alliance". THE FEATURE VECTOR IS DELIBERATELY NOT NAMED: its 143 slots are an implementation detail that has changed before and will again, and a mod written against those names would break silently. What the AI CAN DO is stable enough to build an advisor or a decision log against. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `country_is_ai`

```wat
(import "gearbox:neural" "country_is_ai" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `i32` — 0 or 1.

Whether a country is played by the AI rather than by the local player.

#### `update_count`

```wat
(import "gearbox:neural" "update_count" (func $x (result i64)))
```

**Returns** `i64`.

Gradient updates the loaded model has been through -- roughly, how much training it has seen.

#### `model_loaded`

```wat
(import "gearbox:neural" "model_loaded" (func $x (result i32)))
```

**Returns** `i32` — 0 or 1.

Whether an AI model is loaded at all. False in a game with no AI players.

### `gearbox:military.read`

Requires the **Military.Read** capability.

#### `ship_count`

```wat
(import "gearbox:military.read" "ship_count" (func $x (result i32)))
```

**Returns** `i32`.

How many ships exist in the world, across all owners.

#### `ship_at`

```wat
(import "gearbox:military.read" "ship_at" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |

**Returns** `i32`.

The ship id at `index` in 0..ship_count-1, or 0xFFFFFFFF past the end. Ids are stable within a turn and not across turns -- do not store one.

#### `ship_exists`

```wat
(import "gearbox:military.read" "ship_exists" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |

**Returns** `i32` — 0 or 1.

Whether a ship id is still live. Check this before acting on an id you read earlier in the same turn; ships sink.

#### `ship_owner`

```wat
(import "gearbox:military.read" "ship_owner" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |

**Returns** `i32`.

The country that owns a ship, or 0xFFFFFFFF for an id that does not exist.

#### `ship_type`

```wat
(import "gearbox:military.read" "ship_type" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The hull type as a lowercase string: "transport", "destroyer", "battleship", "carrier", "submarine". Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `ship_lon`

```wat
(import "gearbox:military.read" "ship_lon" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |

**Returns** `f64`.

Longitude in degrees, -180..180. Ships live in world coordinates, not provinces.

#### `ship_lat`

```wat
(import "gearbox:military.read" "ship_lat" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |

**Returns** `f64`.

Latitude in degrees, -90..90.

#### `ship_health`

```wat
(import "gearbox:military.read" "ship_health" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |

**Returns** `i32`.

Hull integrity, 0..100. A ship at 0 has already sunk and will not appear.

#### `ship_crew`

```wat
(import "gearbox:military.read" "ship_crew" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |

**Returns** `i32`.

Crew aboard. For a transport this includes the embarked army, which is why a sunk transport costs so much more than its hull.

#### `ship_range`

```wat
(import "gearbox:military.read" "ship_range" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |

**Returns** `f64`.

How far this hull may move in one turn, in degrees. The resolver clamps any order beyond it, so read this before ordering a move rather than discovering the clamp afterwards.

#### `army_stack_count`

```wat
(import "gearbox:military.read" "army_stack_count" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i32`.

How many distinct owners have troops in a province. Usually 1; more than one means a contested or garrisoned province.

#### `army_stack_owner`

```wat
(import "gearbox:military.read" "army_stack_owner" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `index` | `i32` | — |

**Returns** `i32`.

The country owning stack `index` in a province, or 0xFFFFFFFF past the end.

#### `army_stack_size`

```wat
(import "gearbox:military.read" "army_stack_size" (func $x (param i32 i32) (result i64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `index` | `i32` | — |

**Returns** `i64`.

How many troops are in that stack.

#### `country_army`

```wat
(import "gearbox:military.read" "country_army" (func $x (param i32) (result i64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `i64`.

A country's total troops everywhere, which is the number its own army screen shows.

#### `province_fortification`

```wat
(import "gearbox:military.read" "province_fortification" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i32`.

Fortification level, 0..5. Multiplies the defender's strength.

#### `province_port_level`

```wat
(import "gearbox:military.read" "province_port_level" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i32`.

Port level, 0..3. 0 means no port, so no embarking and no ship repair.

### `gearbox:military.write`

Requires the **Military.Write** capability.

#### `order_army_move`

```wat
(import "gearbox:military.write" "order_army_move" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `from` | `i32` | — |
| `to` | `i32` | — |
| `percent` | `i32` | — |

**Returns** `i32` — 0 or 1.

Move `percent` (0..100) of the troops in `from` into the adjacent province `to`. Into an enemy province this is an attack; into your own or an ally's it is a transfer. Non-adjacent moves are refused. QUEUES AN ORDER; it does not move anything. It lands in the same queue the player's own click writes to and is validated by the same resolver at end of turn, so a mod cannot teleport, cheat range, or attack across an ocean. Returns 0 if the order is rejected outright.

#### `order_ship_move`

```wat
(import "gearbox:military.write" "order_ship_move" (func $x (param i32 f64 f64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |
| `lon` | `f64` | — |
| `lat` | `f64` | — |

**Returns** `i32` — 0 or 1.

Sail a ship toward (lon,lat). The resolver routes around land and clamps to ship_range, so a destination on land or beyond range moves the ship as far as it legally can rather than failing. QUEUES AN ORDER; it does not move anything. It lands in the same queue the player's own click writes to and is validated by the same resolver at end of turn, so a mod cannot teleport, cheat range, or attack across an ocean. Returns 0 if the order is rejected outright.

#### `order_ship_engage`

```wat
(import "gearbox:military.write" "order_ship_engage" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |
| `target` | `i32` | — |

**Returns** `i32` — 0 or 1.

Attack another ship. Requires that you are at war with its owner and that it is within range; both are checked by the resolver. QUEUES AN ORDER; it does not move anything. It lands in the same queue the player's own click writes to and is validated by the same resolver at end of turn, so a mod cannot teleport, cheat range, or attack across an ocean. Returns 0 if the order is rejected outright.

#### `order_ship_bombard`

```wat
(import "gearbox:military.write" "order_ship_bombard" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `ship` | `i32` | — |
| `province` | `i32` | — |
| `ammo` | `i32` | pointer into your linear memory |
| `ammo_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Bombard a coastal province. `ammo` names the shell type; pass an empty string for the default. QUEUES AN ORDER; it does not move anything. It lands in the same queue the player's own click writes to and is validated by the same resolver at end of turn, so a mod cannot teleport, cheat range, or attack across an ocean. Returns 0 if the order is rejected outright.

### `gearbox:research.read`

Requires the **Research.Read** capability.

#### `node_count`

```wat
(import "gearbox:research.read" "node_count" (func $x (result i32)))
```

**Returns** `i32`.

How many technologies exist in the tree.

#### `node_id`

```wat
(import "gearbox:research.read" "node_id" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The stable string id of technology `index`, which is what country_has_researched takes. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `node_name`

```wat
(import "gearbox:research.read" "node_name" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The technology's display name, which is localised and NOT stable -- never match on it. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `node_category`

```wat
(import "gearbox:research.read" "node_category" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

Which branch of the tree it sits in. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `node_cost`

```wat
(import "gearbox:research.read" "node_cost" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |

**Returns** `i32`.

Research points required.

#### `country_has_researched`

```wat
(import "gearbox:research.read" "country_has_researched" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |
| `node_id` | `i32` | pointer into your linear memory |
| `node_id_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Whether a country has completed a technology. Takes the id from node_id, not the display name.

#### `country_funding`

```wat
(import "gearbox:research.read" "country_funding" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `f64`.

Research funding as A SHARE OF INCOME, 0..1 -- not an absolute sum. That is how the game stores it and how its own economy screen presents it.

### `gearbox:research.write`

Requires the **Research.Write** capability.

#### `set_country_funding`

```wat
(import "gearbox:research.write" "set_country_funding" (func $x (param i32 f64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |
| `share` | `f64` | — |

**Returns** `i32` — 0 or 1.

Set research funding as a share of income. Clamped to 0..1; a value in 'points per turn' is not a quantity this game has.

### `gearbox:politics.read`

Requires the **Politics.Read** capability.

#### `country_compass_econ`

```wat
(import "gearbox:politics.read" "country_compass_econ" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `f64`.

Economic axis of the political compass, -100 (planned) to 100 (market).

#### `country_compass_social`

```wat
(import "gearbox:politics.read" "country_compass_social" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `f64`.

Social axis, -100 (authoritarian) to 100 (libertarian).

#### `province_unrest`

```wat
(import "gearbox:politics.read" "province_unrest" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `f64`.

This province's chance of rebelling, as the game itself computes it.

#### `policy_count`

```wat
(import "gearbox:politics.read" "policy_count" (func $x (result i32)))
```

**Returns** `i32`.

How many policies exist.

#### `policy_id`

```wat
(import "gearbox:politics.read" "policy_id" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The stable string id of policy `index`. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `policy_name`

```wat
(import "gearbox:politics.read" "policy_name" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The policy's display name; localised, not stable, do not match on it. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `country_has_policy`

```wat
(import "gearbox:politics.read" "country_has_policy" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |
| `policy_id` | `i32` | pointer into your linear memory |
| `policy_id_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Whether a country currently has a policy active or implementing.

#### `province_minority_count`

```wat
(import "gearbox:politics.read" "province_minority_count" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i32`.

How many named minority groups live in a province.

#### `province_minority_name`

```wat
(import "gearbox:politics.read" "province_minority_name" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `index` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The minority's name. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `province_minority_share`

```wat
(import "gearbox:politics.read" "province_minority_share" (func $x (param i32 i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `index` | `i32` | — |

**Returns** `f64`.

That minority's share of the province's population, 0..1.

### `gearbox:politics.write`

Requires the **Politics.Write** capability.

#### `set_country_policy`

```wat
(import "gearbox:politics.write" "set_country_policy" (func $x (param i32 i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |
| `policy_id` | `i32` | pointer into your linear memory |
| `policy_id_len` | `i32` | byte length |
| `enabled` | `i32` | 0 or 1 |

**Returns** `i32` — 0 or 1.

Enact or cancel a policy. GOES THROUGH THE GAME'S OWN enactPolicy, so the cost, the prerequisites and the per-turn enactment cap all still apply -- a country cannot end up running policies it could never have afforded. Returns 1 if the policy is already in the requested state.

### `gearbox:economy.read`

Requires the **Economy.Read** capability.

#### `country_income_gross`

```wat
(import "gearbox:economy.read" "country_income_gross" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `f64`.

Income per turn before upkeep.

#### `country_income_net`

```wat
(import "gearbox:economy.read" "country_income_net" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `f64`.

Income per turn after army and navy upkeep. Negative means the treasury is draining.

#### `country_army_upkeep`

```wat
(import "gearbox:economy.read" "country_army_upkeep" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `f64`.

What the standing army costs per turn.

#### `country_navy_upkeep`

```wat
(import "gearbox:economy.read" "country_navy_upkeep" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `f64`.

What the fleet costs per turn. Ships a country is not using still cost this, which is what makes scrapping a real decision.

#### `country_is_bankrupt`

```wat
(import "gearbox:economy.read" "country_is_bankrupt" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `country` | `i32` | — |

**Returns** `i32` — 0 or 1.

Whether a country is currently bankrupt.

#### `province_industry_level`

```wat
(import "gearbox:economy.read" "province_industry_level" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i32`.

Industry level, 0..10.

#### `province_industry_specialization`

```wat
(import "gearbox:economy.read" "province_industry_specialization" (func $x (param i32 i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

What this province's industry specialises in, or an empty string for none. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `province_resource`

```wat
(import "gearbox:economy.read" "province_resource" (func $x (param i32 i32 i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `which` | `i32` | pointer into your linear memory |
| `which_len` | `i32` | byte length |

**Returns** `f64`.

How much of a resource a province holds, 0..100. `which` is one of "oil", "gold", "rubber", "gemstones", "metal"; anything else reads 0.

### `gearbox:economy.write`

Requires the **Economy.Write** capability.

#### `set_province_industry_level`

```wat
(import "gearbox:economy.write" "set_province_industry_level" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `level` | `i32` | — |

**Returns** `i32` — 0 or 1.

Set a province's industry level, clamped to 0..10. This writes the built level directly and does not charge for it -- it is a scenario-authoring tool, not a build order.

### `gearbox:mapeditor`

Requires the **MapEditor** capability.

#### `editor_active`

```wat
(import "gearbox:mapeditor" "editor_active" (func $x (result i32)))
```

**Returns** `i32` — 0 or 1.

Whether the map editor is open with a project loaded. EVERY OTHER CALL IN THIS MODULE returns 0 or an empty string when this is 0, including from inside a running game: the data behind them is an editor project, and a game does not have one. Check this first.

#### `editor_province_count`

```wat
(import "gearbox:mapeditor" "editor_province_count" (func $x (result i32)))
```

**Returns** `i32`.

How many provinces the open project has. Returns a neutral value unless the map editor is open with a project loaded -- see mapeditor/active.

#### `editor_province_at`

```wat
(import "gearbox:mapeditor" "editor_province_at" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `index` | `i32` | — |

**Returns** `i32`.

The province id at `index`, in ascending id order, or 0xFFFFFFFF past the end. Returns a neutral value unless the map editor is open with a project loaded -- see mapeditor/active.

#### `editor_province_population`

```wat
(import "gearbox:mapeditor" "editor_province_population" (func $x (param i32) (result i64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i64`.

Population. Returns a neutral value unless the map editor is open with a project loaded -- see mapeditor/active.

#### `editor_province_industry_level`

```wat
(import "gearbox:mapeditor" "editor_province_industry_level" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i32`.

Industry level, 0..10. Returns a neutral value unless the map editor is open with a project loaded -- see mapeditor/active.

#### `editor_province_fortification`

```wat
(import "gearbox:mapeditor" "editor_province_fortification" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i32`.

Fortification, 0..5. Returns a neutral value unless the map editor is open with a project loaded -- see mapeditor/active.

#### `editor_province_port_level`

```wat
(import "gearbox:mapeditor" "editor_province_port_level" (func $x (param i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `i32`.

Port level, 0..3. Returns a neutral value unless the map editor is open with a project loaded -- see mapeditor/active.

#### `editor_province_resource`

```wat
(import "gearbox:mapeditor" "editor_province_resource" (func $x (param i32 i32 i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `which` | `i32` | pointer into your linear memory |
| `which_len` | `i32` | byte length |

**Returns** `f64`.

Resource amount, 0..100. `which` is "oil", "gold", "rubber", "gemstones" or "metal". Returns a neutral value unless the map editor is open with a project loaded -- see mapeditor/active.

#### `editor_province_compass_econ`

```wat
(import "gearbox:mapeditor" "editor_province_compass_econ" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `f64`.

Province economic compass, -100..100. Returns a neutral value unless the map editor is open with a project loaded -- see mapeditor/active.

#### `editor_province_compass_social`

```wat
(import "gearbox:mapeditor" "editor_province_compass_social" (func $x (param i32) (result f64)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |

**Returns** `f64`.

Province social compass, -100..100. Returns a neutral value unless the map editor is open with a project loaded -- see mapeditor/active.

#### `editor_set_province_population`

```wat
(import "gearbox:mapeditor" "editor_set_province_population" (func $x (param i32 i64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `value` | `i64` | — |

**Returns** `i32` — 0 or 1.

Set population, clamped to 0..2e9. Writes the SAME per-province data the editor's own tools write, so it saves, exports and shows up in the unsaved-changes prompt like any other edit. A province the project does not have is refused rather than created: data without a shape on the province bitmap exports a map the game cannot load.

#### `editor_set_province_industry_level`

```wat
(import "gearbox:mapeditor" "editor_set_province_industry_level" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `level` | `i32` | — |

**Returns** `i32` — 0 or 1.

Set industry level, clamped to 0..10. Writes the SAME per-province data the editor's own tools write, so it saves, exports and shows up in the unsaved-changes prompt like any other edit. A province the project does not have is refused rather than created: data without a shape on the province bitmap exports a map the game cannot load.

#### `editor_set_province_fortification`

```wat
(import "gearbox:mapeditor" "editor_set_province_fortification" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `level` | `i32` | — |

**Returns** `i32` — 0 or 1.

Set fortification, clamped to 0..5. Writes the SAME per-province data the editor's own tools write, so it saves, exports and shows up in the unsaved-changes prompt like any other edit. A province the project does not have is refused rather than created: data without a shape on the province bitmap exports a map the game cannot load.

#### `editor_set_province_port_level`

```wat
(import "gearbox:mapeditor" "editor_set_province_port_level" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `level` | `i32` | — |

**Returns** `i32` — 0 or 1.

Set port level, clamped to 0..3. Writes the SAME per-province data the editor's own tools write, so it saves, exports and shows up in the unsaved-changes prompt like any other edit. A province the project does not have is refused rather than created: data without a shape on the province bitmap exports a map the game cannot load.

#### `editor_set_province_resource`

```wat
(import "gearbox:mapeditor" "editor_set_province_resource" (func $x (param i32 i32 i32 f64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `which` | `i32` | pointer into your linear memory |
| `which_len` | `i32` | byte length |
| `amount` | `f64` | — |

**Returns** `i32` — 0 or 1.

Set a resource amount, clamped to 0..100. An unrecognised name is refused rather than silently mapped onto oil. Writes the SAME per-province data the editor's own tools write, so it saves, exports and shows up in the unsaved-changes prompt like any other edit. A province the project does not have is refused rather than created: data without a shape on the province bitmap exports a map the game cannot load.

#### `editor_set_province_compass`

```wat
(import "gearbox:mapeditor" "editor_set_province_compass" (func $x (param i32 f64 f64) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `province` | `i32` | — |
| `econ` | `f64` | — |
| `social` | `f64` | — |

**Returns** `i32` — 0 or 1.

Set both compass axes, each clamped to -100..100. Writes the SAME per-province data the editor's own tools write, so it saves, exports and shows up in the unsaved-changes prompt like any other edit. A province the project does not have is refused rather than created: data without a shape on the province bitmap exports a map the game cannot load.

#### `editor_map_name`

```wat
(import "gearbox:mapeditor" "editor_map_name" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `buf` | `i32` | pointer into your linear memory |
| `cap` | `i32` | byte length |

**Returns** `i32` — byte length.

The project's map name. Two-call sizing: call with cap 0 to learn the length, allocate, call again. Returns the full length either way; the copy is truncated to cap.

#### `editor_set_map_name`

```wat
(import "gearbox:mapeditor" "editor_set_map_name" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `name` | `i32` | pointer into your linear memory |
| `name_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Rename the map. Refused if empty or over 96 bytes.

#### `editor_set_author`

```wat
(import "gearbox:mapeditor" "editor_set_author" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `author` | `i32` | pointer into your linear memory |
| `author_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Set the author recorded in the exported .odmap. Up to 96 bytes.

#### `editor_set_license`

```wat
(import "gearbox:mapeditor" "editor_set_license" (func $x (param i32 i32) (result i32)))
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `license` | `i32` | pointer into your linear memory |
| `license_len` | `i32` | byte length |

**Returns** `i32` — 0 or 1.

Set the licence recorded in the exported .odmap. Up to 96 bytes.

## Exports

Only `mod_load` is mandatory. A missing optional export is simply not
called — it is not an error.

### `mod_load`

```wat
(func (export "mod_load") (result i32) ...)
```

**Required:** yes.  **Capability:** none

Called once when your mod is enabled, before anything else. Return 0 to accept the load; non-zero refuses it and the value is shown to the user. There is no autorun, so this runs every session the user enables you -- never assume prior state.

### `mod_unload`

```wat
(func (export "mod_unload") ...)
```

**Required:** no.  **Capability:** none

Called when disabled, reloaded, or at shutdown. Release what you hold. Your state does not survive a reload and there is no hook to serialise it.

### `mod_pre_turn`

```wat
(func (export "mod_pre_turn") (param i32) ...)
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `turn` | `i32` | — |

**Required:** no.  **Capability:** `GameProcess`

Called before the host processes a turn. Only invoked if GameProcess is granted.

### `mod_post_turn`

```wat
(func (export "mod_post_turn") (param i32) ...)
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `turn` | `i32` | — |

**Required:** no.  **Capability:** `GameProcess`

Called after the host processes a turn. Only invoked if GameProcess is granted.

### `mod_draw_panel`

```wat
(func (export "mod_draw_panel") (param i32 i32 i32) ...)
```

| Parameter | Wire type | Meaning |
|---|---|---|
| `panel` | `i32` | opaque panel handle |
| `width` | `i32` | — |
| `height` | `i32` | — |

**Required:** no.  **Capability:** `UI`

Called once per frame per visible panel you registered. Never called when headless. Re-issue all your draw calls every frame; the command list is cleared between frames.

## The `env` struct

`28` bytes on wasm32. **Layout is part of the ABI:**
fields are only ever appended, never reordered or resized.

Write your own struct size into `size` *before* calling `env`. The host
writes at most that many bytes, so a mod built against an older, smaller
struct is safe against a newer host that has appended fields.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `size` | `u32` | You write sizeof(gearbox_env_t) here before calling env |
| 4 | `gearbox_major` | `u32` | — |
| 8 | `gearbox_minor` | `u32` | — |
| 12 | `host_version` | `u32` | (major<<16)|(minor<<8)|patch |
| 16 | `platform` | `u8` | enum:platform |
| 17 | `is_web` | `u8` | 1 under Emscripten. Fuel is NOT enforced there. |
| 18 | `is_headless` | `u8` | 1 when there is no renderer. UI imports no-op. |
| 19 | `net_role` | `u8` | enum:net_role. 0 in singleplayer, which is what an older mod reading this byte as reserved already saw. |
| 20 | `screen_w` | `u32` | 0 when headless |
| 24 | `screen_h` | `u32` | 0 when headless |

## Enums

**`log_level`** — `TRACE` = 0, `INFO` = 1, `WARN` = 2, `ERROR` = 3

**`platform`** — `UNKNOWN` = 0, `WINDOWS` = 1, `MACOS` = 2, `LINUX` = 3, `WEB` = 4

**`net_role`** — `STANDALONE` = 0, `CLIENT` = 1, `SERVER` = 2, `HOST_PLAYER` = 3

## Constants

- `GEARBOX_INVALID` = `0xFFFFFFFF`
- `GEARBOX_MAJOR` = `1`
- `GEARBOX_MINOR` = `1`

## Writing a binding for a language we do not ship

If your language compiles to wasm32 and can declare imports with an
explicit module name, you can bind to this in an afternoon. You need three
things:

1. **Declare the imports.** Whatever your language's syntax is for
   "external function in wasm module X named Y". The module name contains
   a colon (`gearbox:core`), which some toolchains handle awkwardly — check
   that early.
2. **Export `mod_load`** with the exact name, returning `i32`.
3. **Get a pointer and a length out of a string.** Everything else follows.

At the wire level there is nothing else to it. The raw WAT in
[`sdk/wat/`](../sdk/wat) shows exactly what the bytes look like, and
`sdk/abi.json` is machine-readable if you would rather generate the
binding than write it.

Two things that trip people up:

- **Do not import anything else.** A toolchain that emits `env.abort`,
  `wasi_snapshot_preview1.fd_write`, or similar will be refused at load —
  the diagnostic names the offending import. Freestanding/no-std flags are
  usually what you need.
- **Your allocator, if any, is yours.** The host never allocates in your
  memory and never frees anything you pass it.

