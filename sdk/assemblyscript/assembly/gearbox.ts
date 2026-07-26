// Gearbox v1.0 bindings for AssemblyScript.
//
// The raw imports are generated from sdk/abi.json, which is the source of
// truth, into raw_generated.ts. Everything in this file is convenience on
// top of them.
//
// AssemblyScript strings are UTF-16 and the ABI takes UTF-8 (ptr, len) pairs,
// so every string crossing the boundary is encoded first. That conversion is
// the only real friction in this binding.

// The raw imports are GENERATED from sdk/abi.json by
// tools/gen_bindings.py. Adding a function to the ABI does not mean
// editing this file; only the ergonomic wrappers below.
import {
  _log,
  _env,
  _abort,
  _fuelBudget,
  _turnNumber,
  _countryCount,
  _countryAt,
  _countryName,
  _countryTreasury,
  _countryProvinceCount,
  _provincePopulation,
  _provinceOwner,
  _panelRegister,
  _drawRect,
  _drawText,
  _button,
  _assetSize,
  _assetRead,
} from "./raw_generated";
// ----------------------------------------------------------- raw imports --

// ------------------------------------------------------------- constants --

export const GEARBOX_MAJOR: u32 = 1;
export const GEARBOX_MINOR: u32 = 0;
export const GEARBOX_INVALID: u32 = 0xFFFFFFFF;

export const LOG_TRACE: i32 = 0;
export const LOG_INFO: i32 = 1;
export const LOG_WARN: i32 = 2;
export const LOG_ERROR: i32 = 3;

export const PLATFORM_UNKNOWN: u32 = 0;
export const PLATFORM_WINDOWS: u32 = 1;
export const PLATFORM_MACOS: u32 = 2;
export const PLATFORM_LINUX: u32 = 3;
export const PLATFORM_WEB: u32 = 4;

// ------------------------------------------------------------------ Core --

export function log(level: i32, msg: string): void {
  const b = String.UTF8.encode(msg);
  _log(level, changetype<usize>(b), b.byteLength);
}

/** Unrecoverable. Traps out of the current call and disables the mod. */
export function abort_(msg: string): void {
  const b = String.UTF8.encode(msg);
  _abort(changetype<usize>(b), b.byteLength);
}

/** The instruction limit for this hook, NOT a live countdown. */
export function fuelBudget(): u64 { return _fuelBudget(); }

// gearbox_env_t, 28 bytes. Offsets are ABI; see env_struct in sdk/abi.json.
let envBuf: ArrayBuffer | null = null;

function envPtr(): usize {
  if (envBuf == null) {
    const b = new ArrayBuffer(28);
    // The host writes at most as many bytes as we declare here, so an older mod
    // stays safe against a newer host. Write it before every call.
    envBuf = b;
  }
  const p = changetype<usize>(envBuf!);
  store<u32>(p, 28);
  _env(p);
  return p;
}

export function gearboxMajor(): u32 { return load<u32>(envPtr(), 4); }
export function gearboxMinor(): u32 { return load<u32>(envPtr(), 8); }
export function hostVersion(): u32  { return load<u32>(envPtr(), 12); }
export function platform(): u32     { return <u32>load<u8>(envPtr(), 16); }
export function isWeb(): bool       { return load<u8>(envPtr(), 17) != 0; }
export function isHeadless(): bool  { return load<u8>(envPtr(), 18) != 0; }
export function screenWidth(): u32  { return load<u32>(envPtr(), 20); }
export function screenHeight(): u32 { return load<u32>(envPtr(), 24); }

// -------------------------------------------------------- GameState.Read --

export function turnNumber(): u32 { return _turnNumber(); }
export function countryCount(): u32 { return _countryCount(); }
export function countryAt(index: u32): u32 { return _countryAt(index); }
export function countryTreasury(country: u32): f64 { return _countryTreasury(country); }
export function countryProvinceCount(country: u32): u32 { return _countryProvinceCount(country); }
export function provincePopulation(province: u32): i64 { return _provincePopulation(province); }
export function provinceOwner(province: u32): u32 { return _provinceOwner(province); }

/** Two-call sizing, handled for you: ask for the length, then fill. */
export function countryName(country: u32): string {
  const need = _countryName(country, 0, 0);
  if (need == 0) return "";
  const buf = new ArrayBuffer(<i32>need);
  _countryName(country, changetype<usize>(buf), need);
  return String.UTF8.decode(buf);
}

// -------------------------------------------------------------------- UI --

export function panelRegister(title: string, minW: u32, minH: u32): u32 {
  const b = String.UTF8.encode(title);
  return _panelRegister(changetype<usize>(b), b.byteLength, minW, minH);
}

export function drawRect(panel: u32, x: i32, y: i32, w: i32, h: i32, rgba: u32): void {
  _drawRect(panel, x, y, w, h, rgba);
}

export function drawText(panel: u32, x: i32, y: i32, rgba: u32, text: string): void {
  const b = String.UTF8.encode(text);
  _drawText(panel, x, y, rgba, changetype<usize>(b), b.byteLength);
}

/** Returns true on the frame the button is clicked. */
export function button(panel: u32, x: i32, y: i32, w: i32, h: i32, label: string): bool {
  const b = String.UTF8.encode(label);
  return _button(panel, x, y, w, h, changetype<usize>(b), b.byteLength) != 0;
}

// ---------------------------------------------------------------- Assets --

export function assetSize(name: string): u32 {
  const b = String.UTF8.encode(name);
  return _assetSize(changetype<usize>(b), b.byteLength);
}

/** Reads one of your own data/ files. Returns an empty buffer if absent. */
export function assetRead(name: string): ArrayBuffer {
  const nb = String.UTF8.encode(name);
  const need = _assetSize(changetype<usize>(nb), nb.byteLength);
  if (need == 0) return new ArrayBuffer(0);
  const out = new ArrayBuffer(<i32>need);
  _assetRead(changetype<usize>(nb), nb.byteLength, changetype<usize>(out), need);
  return out;
}
