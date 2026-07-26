//! gearbox.zig — OpenDoctrines mod ABI, Gearbox v1.0
//!
//! A transcription of `sdk/abi.json`. That file is the source of truth; if this
//! one disagrees with it, this one is a bug.
//!
//! Two directions:
//!   - IMPORTS (`raw.*`)  the host provides, your mod calls.
//!   - EXPORTS (`mod_*`)  your mod provides, the host calls. You write those.
//!
//! Imports live in WASM modules named `gearbox:<module>`, matching the
//! capability names in MANIFEST.json. You only receive imports for capabilities
//! you declared AND the user granted; an undeclared import is refused at load
//! time with a diagnostic, not at some later inconvenient moment.
//!
//! Only the imports you actually call are emitted. Zig analyses declarations
//! lazily and wasm-ld only keeps referenced undefined symbols, so merely
//! importing this file does not put `gearbox:assets` in your module. That is
//! what you want: an import you do not use would still have to be granted.
//!
//! Strings on the wire are (ptr, len) pairs of UTF-8 bytes in your linear
//! memory. They are NOT null-terminated. The host does not retain a pointer
//! into your memory after a call returns, and you must not retain one of its.
//!
//! Status: written against sdk/abi.json, not yet compiled.

pub const MAJOR: u32 = 1;
pub const MINOR: u32 = 0;

/// Returned by any call that yields a handle it could not produce.
pub const INVALID: u32 = 0xFFFF_FFFF;

/// Opaque handles. Valid only for the duration of the hook that produced them —
/// do not cache one across turns, it will be stale or reassigned. Store the
/// country's name or your own key instead.
pub const Country = u32;
pub const Province = u32;
pub const Panel = u32;

pub const LogLevel = enum(u32) {
    trace = 0,
    info = 1,
    warn = 2,
    /// `error` is a Zig keyword, so the ABI's ERROR is spelled `err` here.
    err = 3,
};

pub const Platform = enum(u8) {
    unknown = 0,
    windows = 1,
    macos = 2,
    linux = 3,
    web = 4,
    _,
};

/// Layout is ABI. Fields are only ever appended, never reordered or resized;
/// `size` is what lets an older mod stay safe against a newer host.
///
/// Every field defaults to 0 so you can write `var e: Env = .{};` — but use
/// `env()` below, which fills `size` for you before calling the host.
pub const Env = extern struct {
    /// You write @sizeOf(Env) here before calling `raw.env`.
    size: u32 = 0,
    gearbox_major: u32 = 0,
    gearbox_minor: u32 = 0,
    /// Packed: (major << 16) | (minor << 8) | patch.
    host_version: u32 = 0,
    /// A `Platform` value. Kept as u8 because the host may add platforms.
    platform: u8 = 0,
    /// 1 under Emscripten. Fuel is NOT enforced there.
    is_web: u8 = 0,
    /// 1 when there is no renderer. Every UI import silently no-ops.
    is_headless: u8 = 0,
    reserved0: u8 = 0,
    /// 0 when headless.
    screen_w: u32 = 0,
    screen_h: u32 = 0,
};

comptime {
    // The four u8 fields pack into one word; wasm32 uses 4-byte alignment for
    // u32, so this must come out at exactly the 28 bytes abi.json specifies.
    if (@sizeOf(Env) != 28) @compileError("Env must be 28 bytes to match the ABI");
}

// ============================================================== imports ====
//
// The wire ABI, one Zig declaration per entry in abi.json's "imports" array.
// Pointer parameters that the ABI allows to be null are `?[*]`; the rest are
// plain many-item pointers. Prefer the slice wrappers further down.

// The raw imports are GENERATED from sdk/abi.json by tools/gen_bindings.py.
// Adding a function to the ABI does not mean editing this file; only the
// ergonomic wrappers below, and only if the new function needs one.
pub const raw = @import("raw_generated.zig");


// ============================================================= wrappers ====
//
// Slices carry their own length, so the (ptr, len) split of the wire ABI is an
// implementation detail everywhere above this line. An empty slice is passed as
// a null pointer with length 0: the host treats a zero length as the empty
// string without reading memory, but a null pointer makes that explicit rather
// than handing it Zig's dangling-but-aligned pointer for `""`.

inline fn cptr(s: []const u8) ?[*]const u8 {
    return if (s.len == 0) null else s.ptr;
}

inline fn mptr(s: []u8) ?[*]u8 {
    return if (s.len == 0) null else s.ptr;
}

// ---- Core ----------------------------------------------------------------

/// Write a line to the game log and the mod menu's log view. Messages longer
/// than 2048 bytes are truncated by the host.
pub fn log(level: LogLevel, msg: []const u8) void {
    raw.log(@intFromEnum(level), cptr(msg), @intCast(msg.len));
}

/// Fill and return the environment. Sets `size` for you, which the host needs
/// in order to know how much of your struct it may write.
pub fn env() Env {
    var e: Env = .{ .size = @sizeOf(Env) };
    raw.env(@ptrCast(&e));
    return e;
}

/// Unrecoverable error: traps out of the current call, disables the mod and
/// shows `msg` to the user. Prefer returning an error from a hook where you
/// can — a refused `mod_load` is a much better experience than a dead mod.
pub fn abortWith(msg: []const u8) noreturn {
    raw.abort(cptr(msg), @intCast(msg.len));
}

/// The instruction budget for the current hook, or `max(u64)` when unmetered.
///
/// This is the LIMIT, not a live countdown — it does not decrease as you run.
/// Size your work against it up front and count your own iterations.
pub fn fuelBudget() u64 {
    return raw.fuel_budget();
}

// ---- GameState.Read ------------------------------------------------------

pub fn turnNumber() u32 {
    return raw.turn_number();
}

pub fn countryCount() u32 {
    return raw.country_count();
}

/// `index` in [0, countryCount()). Returns INVALID if out of range. Ordering is
/// stable within a turn but not across turns.
pub fn countryAt(index: u32) Country {
    return raw.country_at(index);
}

/// The full byte length of the country's name, without writing anything.
/// This is the sizing half of the two-call pattern.
pub fn countryNameLen(c: Country) u32 {
    return raw.country_name(c, null, 0);
}

/// Fill `buf` and return the bytes actually written, which is
/// `@min(full length, buf.len)`. A name longer than `buf` is truncated, which
/// is not an error; use `countryNameLen` first if you need to know.
pub fn countryName(c: Country, buf: []u8) []u8 {
    const need = raw.country_name(c, mptr(buf), @intCast(buf.len));
    return buf[0..@min(@as(usize, need), buf.len)];
}

pub fn countryTreasury(c: Country) f64 {
    return raw.country_treasury(c);
}

pub fn countryProvinceCount(c: Country) u32 {
    return raw.country_province_count(c);
}

pub fn provincePopulation(p: Province) i64 {
    return raw.province_population(p);
}

/// Owning country, or INVALID if unowned or unknown.
pub fn provinceOwner(p: Province) Country {
    return raw.province_owner(p);
}

// ---- UI ------------------------------------------------------------------

/// Register a panel and return its handle. Returns 0 when headless, when UI was
/// revoked, or when you already hold 8 panels — none of which is fatal, so
/// degrade rather than trap. Titles are truncated to 64 bytes.
///
/// Call this from `mod_load`, not from your draw hook.
pub fn panelRegister(title: []const u8, min_w: u32, min_h: u32) Panel {
    return raw.panel_register(cptr(title), @intCast(title.len), min_w, min_h);
}

/// Filled rectangle, panel-relative. Colour is 0xRRGGBBAA. Coordinates outside
/// the panel are clipped by the host; they cannot escape it.
pub fn drawRect(panel: Panel, x: i32, y: i32, w: i32, h: i32, rgba: u32) void {
    raw.draw_rect(panel, x, y, w, h, rgba);
}

/// UTF-8 text, panel-relative. Truncated to 512 bytes per call.
pub fn drawText(panel: Panel, x: i32, y: i32, rgba: u32, text: []const u8) void {
    raw.draw_text(panel, x, y, rgba, cptr(text), @intCast(text.len));
}

/// Immediate-mode button: draws it and returns true on the frame it is clicked.
/// One click activates one button — the host consumes it, so overlapping rects
/// do not all fire. Label truncated to 64 bytes.
pub fn button(panel: Panel, x: i32, y: i32, w: i32, h: i32, label: []const u8) bool {
    return raw.button(panel, x, y, w, h, cptr(label), @intCast(label.len)) != 0;
}

// ---- Assets --------------------------------------------------------------

/// Byte size of one of your own `data/` files, or 0 if there is no such asset.
/// Names are relative to `data/` and use '/' separators: `data/flags/fr.png` is
/// `"flags/fr.png"`. The name is looked up in your package's entry list, never
/// resolved as a filesystem path.
pub fn assetSize(name: []const u8) u32 {
    return raw.size(cptr(name), @intCast(name.len));
}

/// Fill `buf` and return the bytes actually written, which is
/// `@min(asset size, buf.len)`. Two-call sizing, like `countryName`: call
/// `assetSize` first when you need the whole file.
pub fn assetRead(name: []const u8, buf: []u8) []u8 {
    const need = raw.read(cptr(name), @intCast(name.len), mptr(buf), @intCast(buf.len));
    return buf[0..@min(@as(usize, need), buf.len)];
}

// ============================================================== exports ====
//
// You write these; there is nothing here to call. Declare them in your root
// source file and pass `--export=<name>` to the linker for each one.
//
//   export fn mod_load() i32                                    required
//   export fn mod_unload() void                                 optional
//   export fn mod_pre_turn(turn: u32) void                      needs GameProcess
//   export fn mod_post_turn(turn: u32) void                     needs GameProcess
//   export fn mod_draw_panel(panel: Panel, w: u32, h: u32) void needs UI
//
// `mod_load` returns 0 to accept the load; non-zero refuses it and the value is
// shown to the user. There is no autorun, so it runs every session the user
// enables you — never assume prior state.
