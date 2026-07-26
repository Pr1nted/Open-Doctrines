//! Hello Panel (Zig) — a complete OpenDoctrines mod.
//!
//! The Zig transcription of `sdk/examples/hello-panel/mod.c`. Same behaviour:
//! a panel with the turn number, how many countries are alive, and the name,
//! province count and treasury of whichever country you step to with the
//! button. Three capabilities: Core (log, env), UI (panel), GameState.Read.
//!
//! No `std` import, no allocator, no panic handler of our own. Everything the
//! mod needs is in this file — that is the point, a Tier 1 mod is a couple of
//! kilobytes. `std.fmt.bufPrint` would work here and does not allocate, but it
//! pulls the whole formatting machinery in and drags a panic handler along with
//! it, and the panic handler's signature is the single most version-sensitive
//! thing in Zig. Twenty lines of integer formatting avoids all of that.
//!
//! Build:  ./build.sh          (produces hello-panel-zig.odmod)

const gb = @import("gearbox");

var g_env: gb.Env = .{};
var g_panel: gb.Panel = 0;
var g_cursor: u32 = 0; // which country we are showing

// --- tiny formatting helpers, since there is no libc ----------------------

/// Writes the decimal form of `v` at the start of `out`, returns how many bytes
/// it used. `out` must have room for 20 digits.
fn u64ToStr(v_in: u64, out: []u8) usize {
    if (v_in == 0) {
        out[0] = '0';
        return 1;
    }
    var tmp: [20]u8 = undefined;
    var v = v_in;
    var n: usize = 0;
    while (v > 0) : (v /= 10) {
        tmp[n] = '0' + @as(u8, @intCast(v % 10));
        n += 1;
    }
    var i: usize = 0;
    while (i < n) : (i += 1) out[i] = tmp[n - 1 - i];
    return n;
}

fn append(dst: []u8, at: usize, src: []const u8) usize {
    @memcpy(dst[at..][0..src.len], src);
    return at + src.len;
}

// --- lifecycle ------------------------------------------------------------

export fn mod_load() i32 {
    g_env = gb.env();

    if (g_env.is_headless != 0) {
        // A training run has no renderer. Registering a panel would be a no-op
        // anyway, but skipping it makes the intent explicit.
        gb.log(.info, "hello-panel-zig: headless, no UI");
        return 0;
    }

    g_panel = gb.panelRegister("Hello Panel (Zig)", 280, 150);
    if (g_panel == 0) {
        // UI was declared but revoked, or we hit the panel limit. Not fatal:
        // degrade rather than trap.
        gb.log(.warn, "hello-panel-zig: no panel, running quiet");
    }
    return 0; // non-zero would refuse the load
}

export fn mod_unload() void {
    g_cursor = 0;
}

// --- the panel ------------------------------------------------------------

export fn mod_draw_panel(panel: gb.Panel, w: u32, h: u32) void {
    _ = h;
    var line: [256]u8 = undefined;

    gb.drawRect(panel, 0, 0, @intCast(w), 1, 0x3C3C5AFF);

    // Turn number
    var n = append(&line, 0, "Turn ");
    n += u64ToStr(gb.turnNumber(), line[n..]);
    gb.drawText(panel, 8, 8, 0xFFFFFFFF, line[0..n]);

    // Country count
    const count = gb.countryCount();
    n = append(&line, 0, "Countries: ");
    n += u64ToStr(count, line[n..]);
    gb.drawText(panel, 8, 28, 0xB4B4C8FF, line[0..n]);

    if (count == 0) {
        gb.drawText(panel, 8, 52, 0x9696A0FF, "No world loaded");
        return;
    }
    if (g_cursor >= count) g_cursor = 0;

    const c = gb.countryAt(g_cursor);
    if (c != gb.INVALID) {
        // Two-call sizing. We only ever show 64 bytes, so the sizing call is
        // not strictly needed — countryName truncates and tells us what it
        // wrote — but this is the pattern for anything you must fit exactly.
        var name: [64]u8 = undefined;
        const got = gb.countryName(c, &name); // a slice of `name`, never longer
        gb.drawText(panel, 8, 52, 0xFFFFFFFF, got);

        n = append(&line, 0, "Provinces: ");
        n += u64ToStr(gb.countryProvinceCount(c), line[n..]);
        gb.drawText(panel, 8, 72, 0xB4B4C8FF, line[0..n]);

        // Treasury comes back as f64; print the integer part. The clamp is not
        // paranoia: @intFromFloat on a value outside u64 is undefined
        // behaviour, and in a safety-checked build it is a trap that would
        // disable the mod over a cosmetic label.
        var t = gb.countryTreasury(c);
        const negative = t < 0;
        if (negative) t = -t;
        if (!(t < 1e18)) t = 1e18; // also catches NaN, which fails every compare

        n = append(&line, 0, "Treasury: ");
        if (negative) n = append(&line, n, "-");
        n += u64ToStr(@intFromFloat(t), line[n..]);
        gb.drawText(panel, 8, 92, 0xB4B4C8FF, line[0..n]);
    }

    if (gb.button(panel, 8, 116, 120, 24, "Next country")) g_cursor += 1;
}
