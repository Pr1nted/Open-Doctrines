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

  // ---- Content (only when built with -DGBX_WITH_CONTENT=1) ----
  /**
   * Add or replace one entry in a catalogue. `kind` is one of the CONTENT_*
   * constants, `mode` is CONTENT_HOLLOW or CONTENT_PERSIST, and `definition`
   * is the SAME JSON the game's own data file uses — a doctrine goes through
   * the parser data/policies.json goes through. `"aiVisible": true` inside it
   * opts the entry into the AI's options.
   *
   * False for a malformed id, an unreadable definition, or an id another mod
   * already owns: catalogue ids are global, because a country records the
   * doctrine it holds by id. Re-adding your own updates it.
   */
  contentAdd(kind: number, id: string, definition: string, mode: number): boolean;
  /** Remove one of your own entries. False if it was not yours. */
  contentRemove(kind: number, id: string): boolean;
  /** How many entries of this kind you have added. */
  contentCount(kind: number): number;
  /** The id of your entry at an index within a kind, sorted. */
  contentIdAt(kind: number, index: number): string;
  /** Which mod owns an id, or "" — including another mod's content. */
  contentOwnerOf(kind: number, id: string): string;

  /** Catalogue ids, as the ABI numbers them. Only ever appended to. */
  readonly CONTENT_DOCTRINE: 0;
  readonly CONTENT_RESEARCH: 1;
  readonly CONTENT_TROOP_TYPE: 2;
  readonly CONTENT_ARTILLERY: 3;
  readonly CONTENT_DISTRICT_LAW: 4;
  /** Redeclared on every load. */
  readonly CONTENT_HOLLOW: 0;
  /** Written into the save, so it outlives the mod that added it. */
  readonly CONTENT_PERSIST: 1;
}

declare const gearbox: Gearbox;

/** Routed to the host log; there is no stdout a player can see. */
declare const console: {
  log(...args: unknown[]): void;
  info(...args: unknown[]): void;
  warn(...args: unknown[]): void;
  error(...args: unknown[]): void;
};
