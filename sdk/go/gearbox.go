// Package gearbox is the TinyGo binding for the OpenDoctrines mod ABI,
// Gearbox v1.0.
//
// It is a transcription of sdk/abi.json. That file is the source of truth; if
// this package disagrees with it, this package is the bug.
//
// Two directions:
//
//	IMPORTS (the raw* functions)  the host provides, your mod calls.
//	EXPORTS (mod_*)               your mod provides, the host calls.
//
// Imports live in WASM modules named "gearbox:<module>", matching the
// capability names in MANIFEST.json. You only receive imports for capabilities
// you declared AND the user granted; an undeclared import is refused at load
// time with a diagnostic, not later at an inconvenient moment.
//
// Strings on the wire are (ptr, len) pairs of UTF-8 bytes in your linear
// memory. They are NOT null-terminated. The host does not retain a pointer into
// your memory after a call returns, and you must not retain one of its.
//
// # Requires TinyGo
//
// Standard Go cannot build a mod. GOOS=js needs the JavaScript glue this host
// does not provide, and GOOS=wasip1 emits WASI imports the host deliberately
// does not link. See README.md.
//
// Status: written against sdk/abi.json, not yet compiled.
package gearbox

import "unsafe"

const (
	Major = 1
	Minor = 0
)

// Invalid is returned by any call that yields a handle it could not produce.
// Note that it is 0xFFFFFFFF, not zero — a zero handle is a real one.
const Invalid uint32 = 0xFFFFFFFF

// Opaque handles. Valid only for the duration of the hook that produced them —
// do not cache one across turns, it will be stale or reassigned. Store the
// country's name or your own key instead.
type (
	Country  uint32
	Province uint32
	Panel    uint32
)

type LogLevel uint32

const (
	LogTrace LogLevel = 0
	LogInfo  LogLevel = 1
	LogWarn  LogLevel = 2
	LogError LogLevel = 3
)

type Platform uint8

const (
	PlatformUnknown Platform = 0
	PlatformWindows Platform = 1
	PlatformMacOS   Platform = 2
	PlatformLinux   Platform = 3
	PlatformWeb     Platform = 4
)

// Env mirrors gearbox_env_t. The layout is part of the ABI: fields are only
// ever appended, never reordered or resized, and Size is what lets an older mod
// stay safe against a newer host.
//
// Use Environment() rather than filling this yourself; it sets Size, which the
// host needs in order to know how much of your struct it may write.
type Env struct {
	Size         uint32
	GearboxMajor uint32
	GearboxMinor uint32
	// HostVersion is packed: (major << 16) | (minor << 8) | patch.
	HostVersion uint32
	// Platform is a Platform value. Kept as uint8 because the host may add
	// platforms this binding has never heard of.
	Platform uint8
	// IsWeb is 1 under Emscripten. Fuel is NOT enforced there.
	IsWeb uint8
	// IsHeadless is 1 when there is no renderer. Every UI import no-ops.
	IsHeadless uint8
	Reserved0  uint8
	// ScreenW and ScreenH are 0 when headless.
	ScreenW uint32
	ScreenH uint32
}

// The four uint8 fields pack into one word and wasm32 aligns uint32 to 4, so
// this must come out at exactly the 28 bytes abi.json specifies. Either of
// these constants goes negative — and a negative uintptr constant is a compile
// error — if the size is anything else.
const _ = 28 - unsafe.Sizeof(Env{})
const _ = unsafe.Sizeof(Env{}) - 28

// ============================================================== imports ====
//
// One declaration per entry in abi.json's "imports" array. These are the wire
// ABI; prefer the wrappers below, which take Go strings and slices.
//
// The raw imports are GENERATED into raw_generated.go from sdk/abi.json by
// tools/gen_bindings.py, and live in this same package. Adding a function to
// the ABI therefore does not mean editing this file.
//
// (The module name really does contain a colon. //go:wasmimport takes the
// module and the field name as two space-separated words, and "gearbox:core"
// is one word, so it is legal even though it looks unusual.)
//
// Everything below is the ergonomic layer: Go strings to (ptr,len), two-call
// sizing, and turning INVALID into a bool-ok return. That part stays
// hand-written -- it is what makes this read as Go.

// ============================================================= wrappers ====
//
// Go strings and slices carry their own length, so the (ptr, len) split of the
// wire ABI stops at this line.
//
// An empty string or slice is passed as a nil pointer with length 0. The host
// treats a zero length as empty without reading memory either way, but the
// pointer behind a zero-length Go slice is not guaranteed to be anything, and
// handing that to a host that bounds-checks pointers is a bad habit.

func strPtr(s string) unsafe.Pointer {
	if len(s) == 0 {
		return nil
	}
	return unsafe.Pointer(unsafe.StringData(s))
}

func bufPtr(b []byte) unsafe.Pointer {
	if len(b) == 0 {
		return nil
	}
	return unsafe.Pointer(unsafe.SliceData(b))
}

// ---- Core -----------------------------------------------------------------

// Log writes a line to the game log and the mod menu's log view. Messages
// longer than 2048 bytes are truncated by the host.
func Log(level LogLevel, msg string) {
	rawLog(uint32(level), strPtr(msg), uint32(len(msg)))
}

// Environment fills and returns the environment, setting Size for you.
func Environment() Env {
	e := Env{Size: uint32(unsafe.Sizeof(Env{}))}
	rawEnv(unsafe.Pointer(&e))
	return e
}

// Abort reports an unrecoverable error: it traps out of the current call,
// disables the mod and shows msg to the user. It does not return — the trap
// happens inside the host, so nothing after the call runs, even though Go has
// no way to say so in the signature.
//
// Prefer returning non-zero from mod_load where you can. A refused load is a
// much better experience than a mod that dies mid-turn.
func Abort(msg string) {
	rawAbort(strPtr(msg), uint32(len(msg)))
}

// FuelBudget returns the instruction budget for the current hook, or
// 0xFFFFFFFFFFFFFFFF when unmetered.
//
// This is the LIMIT, not a live countdown — it does not decrease as you run.
// Size your work against it up front and count your own iterations.
func FuelBudget() uint64 {
	return rawFuelBudget()
}

// ---- GameState.Read -------------------------------------------------------

// TurnNumber returns the current turn, or 0 when no world is loaded.
func TurnNumber() uint32 { return rawTurnNumber() }

// CountryCount returns how many countries exist, or 0 when no world is loaded.
// Rebel factions are not included.
func CountryCount() uint32 { return rawCountryCount() }

// CountryAt returns the country at index in [0, CountryCount()), or Invalid if
// out of range. Ordering is stable within a turn but not across turns.
func CountryAt(index uint32) Country { return Country(rawCountryAt(index)) }

// CountryNameLen returns the full byte length of the country's name without
// writing anything. This is the sizing half of the two-call pattern.
func CountryNameLen(c Country) uint32 {
	return rawCountryName(uint32(c), nil, 0)
}

// CountryNameInto fills buf and returns the sub-slice actually written, which
// is never longer than buf. A name longer than buf is truncated, which is not
// an error; call CountryNameLen first if you need to know.
//
// This is the allocation-free form. Use it in mod_draw_panel, which runs every
// frame.
func CountryNameInto(c Country, buf []byte) []byte {
	n := rawCountryName(uint32(c), bufPtr(buf), uint32(len(buf)))
	if int(n) > len(buf) {
		n = uint32(len(buf))
	}
	return buf[:n]
}

// CountryName returns the country's name as a Go string. Convenient, and it
// allocates twice — once for the buffer, once for the string. Fine in mod_load;
// in a per-frame hook use CountryNameInto with a buffer you keep.
func CountryName(c Country) string {
	n := CountryNameLen(c)
	if n == 0 {
		return ""
	}
	buf := make([]byte, n)
	return string(CountryNameInto(c, buf))
}

// CountryTreasury returns the treasury balance, or 0 for an unknown country.
func CountryTreasury(c Country) float64 { return rawCountryTreasury(uint32(c)) }

// CountryProvinceCount returns how many provinces the country owns, or 0 for an
// unknown country.
func CountryProvinceCount(c Country) uint32 { return rawCountryProvinceCount(uint32(c)) }

// ProvincePopulation returns the population of a province, or 0 for an unknown
// province.
func ProvincePopulation(p Province) int64 { return rawProvincePopulation(uint32(p)) }

// ProvinceOwner returns the owning country, or Invalid if unowned or unknown.
func ProvinceOwner(p Province) Country { return Country(rawProvinceOwner(uint32(p))) }

// ---- UI -------------------------------------------------------------------

// PanelRegister registers a panel and returns its handle. It returns 0 when
// headless, when UI was revoked, or when you already hold 8 panels — none of
// which is fatal, so degrade rather than trap. Titles are truncated to 64
// bytes.
//
// Call this from mod_load, not from your draw hook.
func PanelRegister(title string, minW, minH uint32) Panel {
	return Panel(rawPanelRegister(strPtr(title), uint32(len(title)), minW, minH))
}

// DrawRect draws a filled rectangle in panel-relative coordinates. Colour is
// 0xRRGGBBAA. Coordinates outside the panel are clipped by the host; they
// cannot escape it.
func DrawRect(panel Panel, x, y, w, h int32, rgba uint32) {
	rawDrawRect(uint32(panel), x, y, w, h, rgba)
}

// DrawText draws UTF-8 text in panel-relative coordinates. Truncated to 512
// bytes per call.
func DrawText(panel Panel, x, y int32, rgba uint32, text string) {
	rawDrawText(uint32(panel), x, y, rgba, strPtr(text), uint32(len(text)))
}

// DrawTextBytes is DrawText for a byte slice, so a formatted line built in a
// reusable buffer does not have to be copied into a string first.
func DrawTextBytes(panel Panel, x, y int32, rgba uint32, text []byte) {
	rawDrawText(uint32(panel), x, y, rgba, bufPtr(text), uint32(len(text)))
}

// Button draws an immediate-mode button and reports whether it was clicked on
// this frame. One click activates one button — the host consumes it, so
// overlapping rects do not all fire. Label truncated to 64 bytes.
func Button(panel Panel, x, y, w, h int32, label string) bool {
	return rawButton(uint32(panel), x, y, w, h, strPtr(label), uint32(len(label))) != 0
}

// ---- Assets ---------------------------------------------------------------

// AssetSize returns the byte size of one of your own data/ files, or 0 if there
// is no such asset. Names are relative to data/ and use '/' separators:
// data/flags/fr.png is "flags/fr.png". The name is looked up in your package's
// entry list, never resolved as a filesystem path.
func AssetSize(name string) uint32 {
	return rawAssetSize(strPtr(name), uint32(len(name)))
}

// AssetReadInto fills buf and returns the sub-slice actually written, never
// longer than buf. Two-call sizing, like CountryNameInto: call AssetSize first
// when you need the whole file.
func AssetReadInto(name string, buf []byte) []byte {
	n := rawAssetRead(strPtr(name), uint32(len(name)), bufPtr(buf), uint32(len(buf)))
	if int(n) > len(buf) {
		n = uint32(len(buf))
	}
	return buf[:n]
}

// AssetRead returns a whole asset as a fresh slice, or nil if there is no such
// asset. It allocates the asset's full size — check AssetSize first if that
// might be large, because your ceiling is limits.memoryPages in the manifest
// and exceeding it traps the mod.
func AssetRead(name string) []byte {
	n := AssetSize(name)
	if n == 0 {
		return nil
	}
	return AssetReadInto(name, make([]byte, n))
}

// ============================================================== exports ====
//
// You write these; there is nothing here to call. Put them in your package main
// with //go:wasmexport (TinyGo 0.35.0+) or //export (older TinyGo):
//
//	//go:wasmexport mod_load
//	func modLoad() int32                                  required
//
//	//go:wasmexport mod_unload
//	func modUnload()                                      optional
//
//	//go:wasmexport mod_pre_turn
//	func modPreTurn(turn uint32)                          needs GameProcess
//
//	//go:wasmexport mod_post_turn
//	func modPostTurn(turn uint32)                         needs GameProcess
//
//	//go:wasmexport mod_draw_panel
//	func modDrawPanel(panel, width, height uint32)        needs UI
//
// mod_load returns 0 to accept the load; non-zero refuses it and the value is
// shown to the user. There is no autorun, so it runs every session the user
// enables you — never assume prior state.
