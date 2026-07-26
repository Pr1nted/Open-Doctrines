// Hello Panel, in JavaScript — the same mod as sdk/examples/hello-panel/mod.c.
//
// ModExamplesTest renders this alongside the other SDKs and fails the build if
// a single string differs, so the formatting below is deliberately exact.
//
// Hooks are plain globals: define mod_load, mod_draw_panel, mod_unload and the
// host calls them. `let` at top level is fine; the script is evaluated once in
// the global scope.

let panel = 0;
let cursor = 0; // 0-based, as the ABI is

function mod_load() {
  if (gearbox.env().isHeadless) {
    // A training run has no renderer, so registering a panel would be a no-op.
    gearbox.log(gearbox.INFO, "hello-panel: headless, no UI");
    return 0;
  }

  panel = gearbox.panelRegister("Hello Panel", 280, 150);
  if (panel === 0) {
    // UI was declared but revoked, or we hit the panel limit. Degrade rather
    // than throw.
    gearbox.log(gearbox.WARN, "hello-panel: no panel, running quiet");
  }
  return 0;
}

function mod_unload() {
  cursor = 0;
}

function mod_draw_panel(p, w, h) {
  gearbox.drawRect(p, 0, 0, w, 1, 0x3c3c5aff);

  gearbox.drawText(p, 8, 8, 0xffffffff, "Turn " + gearbox.turnNumber());

  const count = gearbox.countryCount();
  gearbox.drawText(p, 8, 28, 0xb4b4c8ff, "Countries: " + count);

  if (count === 0) {
    gearbox.drawText(p, 8, 52, 0x9696a0ff, "No world loaded");
    return;
  }
  if (cursor >= count) cursor = 0;

  const c = gearbox.countryAt(cursor);
  if (c !== null) {
    gearbox.drawText(p, 8, 52, 0xffffffff, gearbox.countryName(c));
    gearbox.drawText(p, 8, 72, 0xb4b4c8ff,
                     "Provinces: " + gearbox.countryProvinceCount(c));

    // Math.trunc, not Math.floor: the C reference casts to an unsigned
    // integer, so -50.25 prints as -50 rather than -51.
    gearbox.drawText(p, 8, 92, 0xb4b4c8ff,
                     "Treasury: " + Math.trunc(gearbox.countryTreasury(c)));
  }

  if (gearbox.button(p, 8, 116, 120, 24, "Next country")) {
    cursor++;
  }
}
