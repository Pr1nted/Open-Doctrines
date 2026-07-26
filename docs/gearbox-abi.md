<!-- GENERATED FILE - do not edit by hand.
     Source: sdk/abi.json   Generator: tools/gen_abi_docs.py
     Regenerate with: python3 tools/gen_abi_docs.py -->

# Gearbox ABI Reference — v1.0

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
| `WasiStub` | `wasi_snapshot_preview1` | Minimal WASI shim so an interpreter-in-a-mod can boot. NOT a WASI implementation: no filesystem, deterministic randomness, no wall clock. | yes | implemented |

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
| 19 | `reserved0` | `u8` | — |
| 20 | `screen_w` | `u32` | 0 when headless |
| 24 | `screen_h` | `u32` | 0 when headless |

## Enums

**`log_level`** — `TRACE` = 0, `INFO` = 1, `WARN` = 2, `ERROR` = 3

**`platform`** — `UNKNOWN` = 0, `WINDOWS` = 1, `MACOS` = 2, `LINUX` = 3, `WEB` = 4

## Constants

- `GEARBOX_INVALID` = `0xFFFFFFFF`
- `GEARBOX_MAJOR` = `1`
- `GEARBOX_MINOR` = `0`

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

