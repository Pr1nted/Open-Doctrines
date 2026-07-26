/* Gearbox ABI type declarations for TypeScript mods.
 *
 * Point tsconfig.json at this file and the `gearbox` global is typed, so a
 * mistyped call is a compile error rather than a mod that draws nonsense in the
 * game. This is the whole reason to prefer TypeScript here: the runtime is the
 * same QuickJS either way.
 *
 * Kept by hand in step with sdk/abi.json. tools/check_bindings.py lints that
 * every import name appears somewhere in this directory.
 */

/** Opaque handle to a host-managed panel. */
declare type GearboxPanel = number;

/** Opaque handle to a country. Only valid for the hook that produced it. */
declare type GearboxCountry = number;

/** Opaque handle to a province. Only valid for the hook that produced it. */
declare type GearboxProvince = number;

declare interface GearboxEnv {
  gearboxMajor: number;
  gearboxMinor: number;
  /** Packed (major<<16)|(minor<<8)|patch. */
  hostVersion: number;
  /** 0 unknown, 1 windows, 2 macos, 3 linux, 4 web. */
  platform: number;
  /** True under Emscripten. Storage is NOT persistent. */
  isWeb: boolean;
  /** True when there is no renderer. Every UI call is a no-op. */
  isHeadless: boolean;
  screenW: number;
  screenH: number;
}

declare interface Gearbox {
  readonly TRACE: 0;
  readonly INFO: 1;
  readonly WARN: 2;
  readonly ERROR: 3;

  // ---- Core (always granted) ----
  log(level: number, message: string): void;
  env(): GearboxEnv;
  /** Disables the mod and shows `message`. Does not return. */
  abort(message: string): never;
  /** The budget for the current hook, or Infinity when unmetered. This is the
   *  LIMIT, not a countdown: it does not fall as you run. */
  fuelBudget(): number;

  // ---- GameState.Read ----
  turnNumber(): number;
  countryCount(): number;
  /** 0-based. Returns null when out of range. */
  countryAt(index: number): GearboxCountry | null;
  countryName(country: GearboxCountry): string;
  countryTreasury(country: GearboxCountry): number;
  countryProvinceCount(country: GearboxCountry): number;
  provincePopulation(province: GearboxProvince): number;
  provinceOwner(province: GearboxProvince): GearboxCountry | null;

  // ---- UI (silently no-ops when env().isHeadless) ----
  panelRegister(title: string, minW?: number, minH?: number): GearboxPanel;
  drawText(panel: GearboxPanel, x: number, y: number, rgba: number, text: string): void;
  drawRect(panel: GearboxPanel, x: number, y: number, w: number, h: number, rgba: number): void;
  /** True on the frame the button is released inside its bounds. */
  button(panel: GearboxPanel, x: number, y: number, w: number, h: number, label: string): boolean;

  // ---- Assets (only when built with -DGBX_WITH_ASSETS=1) ----
  assetSize(name: string): number;
  assetRead(name: string): Uint8Array | null;
}

declare const gearbox: Gearbox;

/** Routed to the host log; there is no stdout a player can see. */
declare const console: {
  log(...args: unknown[]): void;
  info(...args: unknown[]): void;
  warn(...args: unknown[]): void;
  error(...args: unknown[]): void;
};
