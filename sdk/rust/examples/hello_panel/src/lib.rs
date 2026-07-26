//! Hello Panel — a complete OpenDoctrines mod, in Rust.
//!
//! Same mod as `sdk/examples/hello-panel/mod.c`, line for line where it can be:
//! a panel showing the turn number, how many countries are alive, and the name,
//! province count and treasury of whichever country you step to with the
//! button. Three capabilities — Core (log, env), UI (the panel), GameState.Read
//! (the world).
//!
//! `no_std`, no allocator, no startup code. The whole mod is this file plus the
//! `gearbox` crate, whose wrappers compile down to the bare import calls.
//!
//! Build: `./build.sh` from `sdk/rust/` — produces `hello-panel-rust.odmod`.

#![no_std]

use gearbox::{Buf, Color, Env, ModCell, Panel};

// A `no_std` artifact must define exactly one panic handler and the compiler
// will not link without it. Reaching it means a bug in this file — an index out
// of range, an arithmetic overflow in a debug build — so it routes to
// gearbox_abort, which disables the mod and tells the user why. A bare WASM
// trap with no message would be the alternative.
gearbox::abort_on_panic!("hello-panel: panicked");

// State. `ModCell` instead of `static mut` so the accesses are ordinary safe
// code; see its docs for why a mod may assert `Sync`.
//
// Note what is *not* here: no country handle. Handles are valid only for the
// hook that produced them, so we keep an index and re-resolve it every frame,
// exactly as the C version does.
//
// PANEL is stored but never read: `mod_draw_panel` is handed the handle, so it
// does not need to look ours up. Keeping it is still worth it — it is how you
// know whether you have a panel at all, which matters the moment the mod grows
// a turn hook that wants to skip work when there is nothing to draw into.
static PANEL: ModCell<Option<Panel>> = ModCell::new(None);
static CURSOR: ModCell<u32> = ModCell::new(0);

const HEADER: Color = Color(0x3C3C_5AFF);
const BRIGHT: Color = Color(0xFFFF_FFFF);
const DIM: Color = Color(0xB4B4_C8FF);
const FAINT: Color = Color(0x9696_A0FF);

// --- lifecycle -------------------------------------------------------------

#[no_mangle]
pub extern "C" fn mod_load() -> i32 {
    let env = Env::get();

    if env.is_headless() {
        // A training run has no renderer. Registering a panel would be a no-op
        // anyway, but skipping it makes the intent explicit.
        gearbox::info("hello-panel: headless, no UI");
        return 0;
    }

    match gearbox::panel_register("Hello Panel (Rust)", 280, 150) {
        Some(p) => PANEL.set(Some(p)),
        // UI was declared but revoked, or we hit the eight-panel limit. Not
        // fatal: degrade rather than trap.
        None => gearbox::warn("hello-panel: no panel, running quiet"),
    }

    0 // non-zero would refuse the load, and the value is shown to the user
}

#[no_mangle]
pub extern "C" fn mod_unload() {
    CURSOR.set(0);
    PANEL.set(None);
}

// --- the panel -------------------------------------------------------------

#[no_mangle]
pub extern "C" fn mod_draw_panel(panel: u32, width: u32, height: u32) {
    let _ = height;
    // The handle comes from the host and is good for this call only.
    let panel = Panel::from_raw(panel);

    panel.rect(0, 0, width as i32, 1, HEADER);

    // One scratch buffer, cleared between uses. Stack is a declared resource
    // here (limits.memoryPages in MANIFEST.json), so reuse beats several.
    let mut line = Buf::<128>::new();

    line.push_str("Turn ").push_u64(gearbox::turn_number() as u64);
    panel.text(8, 8, BRIGHT, line.as_str());

    let count = gearbox::country_count();
    line.clear().push_str("Countries: ").push_u64(count as u64);
    panel.text(8, 28, DIM, line.as_str());

    if count == 0 {
        // Reachable from the main menu, and not an error.
        panel.text(8, 52, FAINT, "No world loaded");
        return;
    }

    // Country order is stable within a turn but not across turns, so the cursor
    // can point past the end after a collapse. Wrap instead of clamping.
    let mut cursor = CURSOR.get();
    if cursor >= count {
        cursor = 0;
        CURSOR.set(0);
    }

    if let Some(c) = gearbox::country_at(cursor) {
        // Two-call sizing: `name_len` asks how big it is, then the fill takes
        // what fits. Here we skip the sizing call — 64 bytes is enough for any
        // country name in the base game and truncation is not an error — but
        // the length is one call away if you want to branch on it.
        let mut scratch = [0u8; 64];
        let name = c.name(&mut scratch);
        panel.text(8, 52, BRIGHT, name);

        line.clear()
            .push_str("Provinces: ")
            .push_u64(c.province_count() as u64);
        panel.text(8, 72, DIM, line.as_str());

        // Treasury is an f64. `push_f64_trunc` prints the integer part, like
        // the C example; `push_f64_fixed(t, 2)` would give you the cents.
        line.clear().push_str("Treasury: ").push_f64_trunc(c.treasury());
        panel.text(8, 92, DIM, line.as_str());
    }

    // Immediate mode: the button is drawn and polled by the same call, and
    // returns true only on the frame it is clicked.
    if panel.button(8, 116, 120, 24, "Next country") {
        CURSOR.set(cursor + 1);
    }
}

// mod_pre_turn / mod_post_turn are deliberately absent: they need the
// GameProcess capability, this mod does not declare it, and exporting hooks you
// cannot be called for is noise. See `gearbox::exports` for their prototypes.
