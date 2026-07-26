//! # gearbox — Rust binding for the OpenDoctrines mod ABI, Gearbox v1.0
//!
//! `sdk/abi.json` is the contract; this crate is a transcription of it. If the
//! two ever disagree, `abi.json` wins and this crate is the bug. That file is
//! the one the host's own function table is pinned against, which is why a
//! binding written from it cannot silently drift from the host.
//!
//! Two directions, and they are not symmetric:
//!
//! * **Imports** — the host provides, you call. All 18 live in [`ffi`], with a
//!   safe wrapper for each in this module.
//! * **Exports** — you provide, the host calls. Rust cannot express these from
//!   a library crate; you write them in your own `cdylib`. See [`exports`] for
//!   the five prototypes to copy.
//!
//! ## Shape of a mod
//!
//! ```ignore
//! #![no_std]
//!
//! gearbox::abort_on_panic!("my-mod panicked");
//!
//! static PANEL: gearbox::ModCell<Option<gearbox::Panel>> = gearbox::ModCell::new(None);
//!
//! #[no_mangle]
//! pub extern "C" fn mod_load() -> i32 {
//!     let env = gearbox::Env::get();
//!     if !env.is_headless() {
//!         PANEL.set(gearbox::panel_register("My Mod", 240, 120));
//!     }
//!     0   // non-zero refuses the load
//! }
//! ```
//!
//! ## What will bite you
//!
//! * **Handles are per-hook.** [`Country`] and [`Province`] are opaque and only
//!   valid for the duration of the call that produced them. Rust's type system
//!   cannot catch a stale one for you — they are plain `u32` on the wire, so
//!   there is no lifetime to attach. Store a name or your own key instead.
//! * **Strings are borrowed `(ptr, len)`, never null-terminated.** `&str` maps
//!   onto that exactly, which is the one place this ABI is nicer in Rust than
//!   in C. The host does not keep your pointer past the call.
//! * **You will be granted less than you asked for.** A revoked capability does
//!   not unlink the import; it makes it return a neutral value — 0, `None`,
//!   nothing drawn. Handle that instead of assuming success.
//! * **`is_headless` is not hypothetical.** Self-play training runs thousands
//!   of turns with no renderer. Every UI import no-ops there but your logic
//!   still runs and still burns fuel.
//! * **Fuel is a limit, not a countdown.** [`fuel_budget`] returns the ceiling
//!   for the current hook and does not decrease as you run. Size your work
//!   against it up front and count your own iterations.

#![no_std]

// The extern block is GENERATED from sdk/abi.json by tools/gen_bindings.py.
// Adding a function to the ABI does not mean editing this crate; only the safe
// wrappers below, and only if the new function needs one.
#[path = "ffi_generated.rs"]
pub mod ffi;
pub mod fmt;

pub use fmt::Buf;

/// Gearbox API version this crate targets. Same major = compatible.
pub const GEARBOX_MAJOR: u32 = 1;
/// See [`GEARBOX_MAJOR`].
pub const GEARBOX_MINOR: u32 = 0;

/// The "no such entity" handle: `0xFFFFFFFF`.
///
/// The safe layer turns this into `None` at every boundary, so you should not
/// normally need it. It is here for round-tripping raw handles.
pub const INVALID: u32 = 0xFFFF_FFFF;

// ============================================================ interior state

/// A `Cell` you are allowed to put in a `static`.
///
/// Rust requires `static` items to be `Sync`, and `Cell` is not — for good
/// reason, on a machine with threads. A mod instance does not have threads: the
/// host instantiates your module once, drives it from one thread, and never
/// re-enters a hook while another is running. `Sync` is therefore vacuously
/// satisfiable here, and asserting it is the whole job of this type.
///
/// Restricted to `T: Copy` and to get/set, so there is never a live reference
/// into the cell for a re-entrant call to invalidate. That makes it sound even
/// if the host's threading model ever changes underneath you.
///
/// The alternative is `static mut` plus an `unsafe` block at every use, which
/// is the same guarantee with more syntax and a lint that grows teeth in
/// edition 2024.
pub struct ModCell<T>(core::cell::Cell<T>);

// SAFETY: a mod instance is single-threaded; see the type docs.
unsafe impl<T> Sync for ModCell<T> {}

impl<T> ModCell<T> {
    /// `const`, so it can initialise a `static`.
    ///
    /// Deliberately in the unbounded impl: a trait bound on a `const fn` was
    /// unstable until Rust 1.61, and `new` does not need `Copy` anyway.
    pub const fn new(value: T) -> Self {
        ModCell(core::cell::Cell::new(value))
    }
}

impl<T: Copy> ModCell<T> {
    pub fn get(&self) -> T {
        self.0.get()
    }
    pub fn set(&self, value: T) {
        self.0.set(value)
    }
    /// Set and return the previous value.
    pub fn replace(&self, value: T) -> T {
        self.0.replace(value)
    }
}

// ======================================================================= Core

/// Log severity. Matches `enums.log_level` in `abi.json`.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Level {
    Trace = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
}

/// Host platform. Matches `enums.platform` in `abi.json`.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Platform {
    Unknown = 0,
    Windows = 1,
    Macos = 2,
    Linux = 3,
    Web = 4,
}

impl Platform {
    /// Anything the host reports that this crate does not know about becomes
    /// `Unknown`. A newer host adding a platform must not be a trap.
    pub const fn from_raw(v: u8) -> Platform {
        match v {
            1 => Platform::Windows,
            2 => Platform::Macos,
            3 => Platform::Linux,
            4 => Platform::Web,
            _ => Platform::Unknown,
        }
    }
}

/// The environment struct, `gearbox_env_t`.
///
/// **The layout is ABI.** Fields are only ever appended, never reordered or
/// resized, which is what lets an older mod read a newer host's struct safely:
/// you write your own `size` into it, and the host writes at most that many
/// bytes. `#[repr(C)]` is what makes Rust lay this out the way the host reads
/// it — without it the compiler is free to reorder fields.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Env {
    /// You write `Env::SIZE` here; [`Env::get`] does it for you.
    pub size: u32,
    pub gearbox_major: u32,
    pub gearbox_minor: u32,
    /// Packed `(major << 16) | (minor << 8) | patch`.
    pub host_version: u32,
    /// A [`Platform`] discriminant; use [`Env::platform`].
    pub platform: u8,
    /// 1 under Emscripten. Fuel is **not** enforced there.
    pub is_web: u8,
    /// 1 when there is no renderer. Every UI import no-ops.
    pub is_headless: u8,
    pub reserved0: u8,
    /// 0 when headless.
    pub screen_w: u32,
    /// 0 when headless.
    pub screen_h: u32,
}

// `abi.json` says 28 bytes on wasm32. If this ever fails, the struct above
// drifted from the ABI and every field after the break is being read from the
// wrong offset — better a build error than a garbage `is_headless`.
const _: () = {
    assert!(core::mem::size_of::<Env>() == 28);
    assert!(core::mem::align_of::<Env>() == 4);
};

impl Env {
    /// `sizeof(gearbox_env_t)` as this crate knows it.
    pub const SIZE: u32 = 28;

    const fn zeroed() -> Env {
        Env {
            size: 0,
            gearbox_major: 0,
            gearbox_minor: 0,
            host_version: 0,
            platform: 0,
            is_web: 0,
            is_headless: 0,
            reserved0: 0,
            screen_w: 0,
            screen_h: 0,
        }
    }

    /// Ask the host to fill an `Env`.
    ///
    /// Cheap enough to call whenever you need it, but the values do not change
    /// within a session, so `mod_load` is the natural place.
    pub fn get() -> Env {
        let mut e = Env::zeroed();
        e.size = Env::SIZE;
        // SAFETY: `e` is a live, correctly-sized, correctly-aligned Env in our
        // own linear memory, and we told the host how many bytes it may write.
        unsafe { ffi::env(&mut e as *mut Env as *mut u8) };
        e
    }

    pub fn is_headless(&self) -> bool {
        self.is_headless != 0
    }

    pub fn is_web(&self) -> bool {
        self.is_web != 0
    }

    pub fn platform(&self) -> Platform {
        Platform::from_raw(self.platform)
    }

    /// `host_version` unpacked into `(major, minor, patch)`.
    pub fn host_version_parts(&self) -> (u32, u32, u32) {
        (
            (self.host_version >> 16) & 0xFFFF,
            (self.host_version >> 8) & 0xFF,
            self.host_version & 0xFF,
        )
    }
}

/// Write a line to the game log and the mod menu's log view.
///
/// Truncated at 2048 bytes by the host. An out-of-bounds `(ptr, len)` is
/// refused and logged against your mod rather than read — you cannot produce
/// one from a `&str`, which is the point of taking one.
pub fn log(level: Level, msg: &str) {
    // SAFETY: a &str is by construction an in-bounds (ptr, len) pair into our
    // own linear memory, and the host does not retain it past this call.
    unsafe { ffi::log(level as u32, msg.as_ptr(), msg.len() as u32) }
}

pub fn trace(msg: &str) {
    log(Level::Trace, msg)
}
pub fn info(msg: &str) {
    log(Level::Info, msg)
}
pub fn warn(msg: &str) {
    log(Level::Warn, msg)
}
pub fn error(msg: &str) {
    log(Level::Error, msg)
}

/// Unrecoverable error: traps out of the current call, disables the mod, and
/// shows `msg` to the user.
///
/// Prefer returning non-zero from `mod_load`, or just drawing nothing. Aborting
/// is for states you genuinely cannot continue from — the user loses the mod
/// for the session.
//
// The `unused_unsafe` allow is on the function, not on the block below, because
// attributes on a tail expression are still unstable. It is belt and braces:
// `core::arch::wasm32::unreachable` is safe in current `core`, but it has not
// always been, and a warning is not worth a version check.
#[allow(unused_unsafe)]
pub fn abort(msg: &str) -> ! {
    // SAFETY: see `log`.
    unsafe { ffi::abort(msg.as_ptr(), msg.len() as u32) };

    // The desktop host raises a WASM exception inside that call, so control
    // never comes back here. The web host does not — it records the message and
    // returns, because there is no interpreter to unwind. So this is not
    // decoration: on web it is the thing that actually stops us, and it is why
    // the import is declared as returning `()` and the `!` is added here.
    //
    // `unreachable` emits the WASM `unreachable` instruction, which traps. That
    // is the right ending: `loop {}` would also type as `!` but would hang the
    // browser tab, and `unreachable_unchecked` would be undefined behaviour on
    // the one path where the call really does return.
    unsafe { core::arch::wasm32::unreachable() }
}

/// The instruction budget for the current hook.
///
/// This is the **limit**, not a live countdown: it does not decrease as you
/// run. The interpreter enforces it and terminates you at zero, but exposes no
/// running counter, and inventing one would mean lying to you. Use it to size
/// work up front — "budget is 100k, so I walk at most N provinces" — and count
/// your own iterations if you need finer control.
///
/// Returns [`UNMETERED`] when there is no limit, which is the case on web.
pub fn fuel_budget() -> u64 {
    // SAFETY: no arguments, no memory touched.
    unsafe { ffi::fuel_budget() }
}

/// The value [`fuel_budget`] returns when the hook is not metered.
pub const UNMETERED: u64 = u64::MAX;

/// Whether the current hook is unmetered. True on web, where the browser has no
/// instruction limit to impose — so nothing stops a runaway loop but the tab
/// freezing. Bound your own loops regardless.
pub fn is_unmetered() -> bool {
    fuel_budget() == UNMETERED
}

// =========================================================== GameState.Read

/// An opaque country handle. **Valid only for the hook that produced it.**
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Country(u32);

/// An opaque province handle. **Valid only for the hook that produced it.**
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Province(u32);

impl Country {
    /// Wrap a raw handle. `INVALID` is accepted; every getter returns a neutral
    /// value for an unknown country rather than trapping.
    pub const fn from_raw(raw: u32) -> Country {
        Country(raw)
    }
    pub const fn raw(self) -> u32 {
        self.0
    }
    pub const fn is_valid(self) -> bool {
        self.0 != INVALID
    }

    /// See [`country_name`].
    pub fn name(self, buf: &mut [u8]) -> &str {
        country_name(self, buf)
    }
    /// See [`country_name_len`].
    pub fn name_len(self) -> usize {
        country_name_len(self)
    }
    /// See [`country_treasury`].
    pub fn treasury(self) -> f64 {
        country_treasury(self)
    }
    /// See [`country_province_count`].
    pub fn province_count(self) -> u32 {
        country_province_count(self)
    }
}

impl Province {
    pub const fn from_raw(raw: u32) -> Province {
        Province(raw)
    }
    pub const fn raw(self) -> u32 {
        self.0
    }
    pub const fn is_valid(self) -> bool {
        self.0 != INVALID
    }

    /// See [`province_population`].
    pub fn population(self) -> i64 {
        province_population(self)
    }
    /// See [`province_owner`].
    pub fn owner(self) -> Option<Country> {
        province_owner(self)
    }
}

/// The result of a two-call-sizing fill.
///
/// `written < full` means the host had more to give than your buffer could
/// hold. That is truncation, **not** failure — the data you got is good, there
/// is just more of it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Fill {
    /// Bytes actually placed in your buffer.
    pub written: usize,
    /// Full length the host has, regardless of your capacity.
    pub full: usize,
}

impl Fill {
    pub fn truncated(self) -> bool {
        self.full > self.written
    }
}

/// The current turn. 0 when no world is loaded — a panel is reachable from the
/// main menu, so this is a normal state and not an error.
pub fn turn_number() -> u32 {
    unsafe { ffi::turn_number() }
}

/// How many countries exist. 0 when no world is loaded. Rebel factions are not
/// included.
pub fn country_count() -> u32 {
    unsafe { ffi::country_count() }
}

/// The country at `index` in `[0, country_count())`, or `None` if out of range.
///
/// Ordering is stable within a turn but not across turns, so an index is not an
/// identity either.
pub fn country_at(index: u32) -> Option<Country> {
    let raw = unsafe { ffi::country_at(index) };
    if raw == INVALID {
        None
    } else {
        Some(Country(raw))
    }
}

/// Iterator over every country this turn.
///
/// Re-reads `country_count()` once at creation. Do not hold it across a hook
/// boundary; the handles it yields stop meaning anything.
pub fn countries() -> Countries {
    Countries {
        index: 0,
        count: country_count(),
    }
}

/// See [`countries`].
pub struct Countries {
    index: u32,
    count: u32,
}

impl Iterator for Countries {
    type Item = Country;
    fn next(&mut self) -> Option<Country> {
        while self.index < self.count {
            let i = self.index;
            self.index += 1;
            if let Some(c) = country_at(i) {
                return Some(c);
            }
        }
        None
    }
}

/// The full byte length of a country's name, without writing anything.
///
/// This is the sizing half of the two-call protocol: `cap = 0`, null buffer.
/// Returns 0 for an unknown country.
pub fn country_name_len(c: Country) -> usize {
    unsafe { ffi::country_name(c.0, core::ptr::null_mut(), 0) as usize }
}

/// Fill `buf` with a country's name and report what happened.
///
/// The filling half of the two-call protocol. Prefer [`country_name`] unless
/// you need to know about truncation.
pub fn country_name_into(c: Country, buf: &mut [u8]) -> Fill {
    let cap = buf.len();
    let full = unsafe { ffi::country_name(c.0, buf.as_mut_ptr(), cap as u32) } as usize;
    Fill {
        written: if full < cap { full } else { cap },
        full,
    }
}

/// A country's name, into a buffer you own.
///
/// ```ignore
/// let mut scratch = [0u8; 64];
/// let name = gearbox::country_name(c, &mut scratch);
/// ```
///
/// If the name is longer than `buf`, you get the prefix that fits, cut back to
/// a UTF-8 character boundary — the host truncates by bytes and does not know
/// or care where a codepoint ends, so half a character is a real possibility
/// and dropping it is better than drawing a replacement glyph.
///
/// Empty string for an unknown country.
pub fn country_name(c: Country, buf: &mut [u8]) -> &str {
    let n = country_name_into(c, buf).written;
    // Two statements rather than a match on `from_utf8`'s Result: the first
    // borrow ends at the semicolon, so the returned &str is the only live
    // borrow of `buf` and the lifetime works out without fighting the checker.
    let valid = match core::str::from_utf8(buf.get(..n).unwrap_or(&[])) {
        Ok(_) => n,
        Err(e) => e.valid_up_to(),
    };
    core::str::from_utf8(buf.get(..valid).unwrap_or(&[])).unwrap_or("")
}

/// Treasury balance. 0.0 for an unknown country.
pub fn country_treasury(c: Country) -> f64 {
    unsafe { ffi::country_treasury(c.0) }
}

/// How many provinces the country owns. 0 for an unknown country.
pub fn country_province_count(c: Country) -> u32 {
    unsafe { ffi::country_province_count(c.0) }
}

/// Population of a province. 0 for an unknown province.
pub fn province_population(p: Province) -> i64 {
    unsafe { ffi::province_population(p.0) }
}

/// Owning country, or `None` if unowned or unknown.
pub fn province_owner(p: Province) -> Option<Country> {
    let raw = unsafe { ffi::province_owner(p.0) };
    if raw == INVALID {
        None
    } else {
        Some(Country(raw))
    }
}

// ========================================================================= UI

/// An `0xRRGGBBAA` colour.
///
/// A newtype rather than a bare `u32` because [`Panel::text`] takes the colour
/// in the middle of its argument list, where a stray coordinate would compile
/// fine and draw something baffling.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Color(pub u32);

impl Color {
    pub const fn rgba(r: u8, g: u8, b: u8, a: u8) -> Color {
        Color(((r as u32) << 24) | ((g as u32) << 16) | ((b as u32) << 8) | a as u32)
    }
    pub const WHITE: Color = Color(0xFFFF_FFFF);
    pub const BLACK: Color = Color(0x0000_00FF);
    pub const TRANSPARENT: Color = Color(0x0000_0000);
}

/// A host-managed rectangle you may draw inside, and nowhere else.
///
/// You cannot draw outside it, read the framebuffer, or capture global input.
/// All coordinates are panel-relative and the host clips them; an out-of-range
/// rectangle is a no-op, not an escape.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Panel(u32);

impl Panel {
    /// Wrap the handle `mod_draw_panel` hands you.
    pub const fn from_raw(raw: u32) -> Panel {
        Panel(raw)
    }
    pub const fn raw(self) -> u32 {
        self.0
    }

    /// Filled rectangle.
    pub fn rect(self, x: i32, y: i32, w: i32, h: i32, color: Color) {
        unsafe { ffi::draw_rect(self.0, x, y, w, h, color.0) }
    }

    /// UTF-8 text, truncated at 512 bytes by the host.
    pub fn text(self, x: i32, y: i32, color: Color, s: &str) {
        unsafe { ffi::draw_text(self.0, x, y, color.0, s.as_ptr(), s.len() as u32) }
    }

    /// Immediate-mode button: draws it and returns `true` on the frame it is
    /// clicked. The host consumes a click, so overlapping buttons do not all
    /// fire from one press. Label truncated at 64 bytes.
    pub fn button(self, x: i32, y: i32, w: i32, h: i32, label: &str) -> bool {
        unsafe { ffi::button(self.0, x, y, w, h, label.as_ptr(), label.len() as u32) != 0 }
    }
}

/// Register a panel. Call this from `mod_load`, not from your draw hook.
///
/// `None` means no panel, which happens when the game is headless, when the
/// user revoked UI in Advanced, or when you already hold 8 panels. **None of
/// those is an error.** Degrade — log it and run quiet — rather than refusing
/// the load or aborting.
///
/// The title is truncated to 64 bytes.
pub fn panel_register(title: &str, min_w: u32, min_h: u32) -> Option<Panel> {
    let raw = unsafe { ffi::panel_register(title.as_ptr(), title.len() as u32, min_w, min_h) };
    if raw == 0 {
        None
    } else {
        Some(Panel(raw))
    }
}

// ===================================================================== Assets

/// Byte size of one of your own `data/` files, or 0 if there is no such asset.
///
/// Names are relative to `data/` and use `/` separators: `data/flags/fr.png` is
/// `"flags/fr.png"`. The name is looked up in your package's entry list, never
/// resolved as a filesystem path — there is no other mod's data to reach and no
/// filesystem to escape into.
pub fn asset_size(name: &str) -> usize {
    unsafe { ffi::asset_size(name.as_ptr(), name.len() as u32) as usize }
}

/// Read one of your own assets into `buf`.
///
/// Two-call sizing, same contract as [`country_name_into`]: writes at most
/// `buf.len()` bytes and reports the asset's full size. `full == 0` means the
/// asset does not exist (or Assets was revoked); `written < full` means your
/// buffer was too small.
///
/// Without an allocator the pattern is: [`asset_size`] first, refuse or chunk
/// if it does not fit your fixed buffer, then read.
pub fn asset_read(name: &str, buf: &mut [u8]) -> Fill {
    let cap = buf.len();
    let full = unsafe {
        ffi::asset_read(
            name.as_ptr(),
            name.len() as u32,
            buf.as_mut_ptr(),
            cap as u32,
        )
    } as usize;
    Fill {
        written: if full < cap { full } else { cap },
        full,
    }
}

// ==================================================================== Exports

/// The five functions the host calls on you.
///
/// This module contains no code. A library crate cannot define these — a
/// `#[no_mangle]` symbol has to live in the artifact that gets linked, so they
/// belong in your own `cdylib`. Copy the prototypes:
///
/// ```ignore
/// /// Required. Called once when the mod is enabled, before anything else.
/// /// Return 0 to accept the load; non-zero refuses it and the value is shown
/// /// to the user. There is no autorun, so this runs every session the user
/// /// enables you — never assume prior state.
/// #[no_mangle]
/// pub extern "C" fn mod_load() -> i32 { 0 }
///
/// /// Optional. Called when disabled, reloaded, or at shutdown. Your state does
/// /// not survive a reload and there is no hook to serialise it.
/// #[no_mangle]
/// pub extern "C" fn mod_unload() {}
///
/// /// Optional, GameProcess. Before the host processes a turn.
/// #[no_mangle]
/// pub extern "C" fn mod_pre_turn(turn: u32) { let _ = turn; }
///
/// /// Optional, GameProcess. After the host processes a turn.
/// #[no_mangle]
/// pub extern "C" fn mod_post_turn(turn: u32) { let _ = turn; }
///
/// /// Optional, UI. Once per frame per visible panel you registered. Never
/// /// called when headless. Re-issue every draw call every frame; the command
/// /// list is cleared between frames.
/// #[no_mangle]
/// pub extern "C" fn mod_draw_panel(panel: u32, width: u32, height: u32) {
///     let _ = (panel, width, height);
/// }
/// ```
///
/// Three things must all be true or the host will not find them:
///
/// * `#[no_mangle]`, so the symbol keeps the name the host looks up.
/// * `pub extern "C"`, so the calling convention is the C ABI the host uses.
///   Rust's default `extern "Rust"` ABI is explicitly unstable — same
///   compiler, different version, different layout.
/// * built as `crate-type = ["cdylib"]`, so the functions are actually placed
///   in the module's export section. An `rlib` exports nothing.
///
/// Exporting a hook you have no capability for is harmless: the host simply
/// never calls it.
pub mod exports {}

/// Define a `#[panic_handler]` that reports through [`abort`].
///
/// A `no_std` binary must define exactly one panic handler and the compiler
/// will not accept the crate without it. It cannot live in this library —
/// `#[panic_handler]` is a global item and having one in an `rlib` would fix
/// the choice for every mod that ever links it — so it comes as a macro you
/// invoke once, at the top level of your own crate:
///
/// ```ignore
/// gearbox::abort_on_panic!("my-mod panicked");
/// ```
///
/// The message is a fixed string. Rendering the real panic message would mean
/// pulling in `core::fmt` and its argument machinery for a path that, by
/// definition, only runs when your mod is already broken.
///
/// Reaching this is a bug: aborting disables the mod for the session and shows
/// the user an error. It is still much better than the alternative, which is a
/// bare WASM trap with no explanation attached.
#[macro_export]
macro_rules! abort_on_panic {
    ($msg:expr) => {
        #[panic_handler]
        fn gearbox_panic(_info: &::core::panic::PanicInfo) -> ! {
            $crate::abort($msg)
        }
    };
}
