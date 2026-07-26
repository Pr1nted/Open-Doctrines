// Hello Panel, in TypeScript — the same mod again, on the same QuickJS engine.
//
// The runtime is identical to the JavaScript example; what TypeScript adds is
// that ../../types/gearbox.d.ts types the `gearbox` global, so getting an
// argument order wrong is a compile error instead of a mod that draws nonsense.
//
// Hooks must end up as globals for the host to find them, which is why this
// file has no import/export and is compiled as a script rather than a module.

let panel: GearboxPanel = 0;
let cursor = 0; // 0-based, as the ABI is

function mod_load(): number {
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

function mod_unload(): void {
  cursor = 0;
}

function mod_draw_panel(p: GearboxPanel, w: number, h: number): void {
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
