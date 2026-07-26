// Hello Panel, in AssemblyScript. Mirrors sdk/examples/hello-panel/mod.c.
//
// Shows the turn number, how many countries exist, and the selected country's
// name, province count and treasury, with a button to step through them.

import * as gb from "./gearbox";

let panel: u32 = 0;
let cursor: u32 = 0;

export function mod_load(): i32 {
  if (gb.isHeadless()) {
    // A training run has no renderer. Registering a panel would no-op anyway;
    // returning early makes the intent explicit.
    gb.log(gb.LOG_INFO, "hello-panel-as: headless, no UI");
    return 0;
  }
  panel = gb.panelRegister("Hello Panel (AS)", 280, 150);
  if (panel == 0) {
    // UI declared but revoked, or we hit the panel limit. Degrade, do not trap.
    gb.log(gb.LOG_WARN, "hello-panel-as: no panel, running quiet");
  }
  return 0;   // non-zero would refuse the load
}

export function mod_unload(): void {
  cursor = 0;
}

export function mod_draw_panel(p: u32, w: u32, h: u32): void {
  gb.drawRect(p, 0, 0, <i32>w, 1, 0x3C3C5AFF);
  gb.drawText(p, 8, 8, 0xFFFFFFFF, "Turn " + gb.turnNumber().toString());

  const count = gb.countryCount();
  gb.drawText(p, 8, 28, 0xB4B4C8FF, "Countries: " + count.toString());

  if (count == 0) {
    gb.drawText(p, 8, 52, 0x9696A0FF, "No world loaded");
    return;
  }
  if (cursor >= count) cursor = 0;

  const c = gb.countryAt(cursor);
  if (c != gb.GEARBOX_INVALID) {
    gb.drawText(p, 8, 52, 0xFFFFFFFF, gb.countryName(c));
    gb.drawText(p, 8, 72, 0xB4B4C8FF,
                "Provinces: " + gb.countryProvinceCount(c).toString());
    gb.drawText(p, 8, 92, 0xB4B4C8FF,
                "Treasury: " + (<i64>gb.countryTreasury(c)).toString());
  }

  if (gb.button(p, 8, 116, 120, 24, "Next country")) cursor++;
}
