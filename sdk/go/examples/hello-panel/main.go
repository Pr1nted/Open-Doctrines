// Hello Panel (Go) — a complete OpenDoctrines mod, built with TinyGo.
//
// The Go transcription of sdk/examples/hello-panel/mod.c. Same behaviour: a
// panel with the turn number, how many countries are alive, and the name,
// province count and treasury of whichever country you step to with the button.
// Three capabilities: Core (log, env), UI (panel), GameState.Read.
//
// Two things it deliberately does not do:
//
//   - It does not import fmt or strconv. fmt drags reflection in and costs tens
//     of kilobytes in a module that is otherwise mostly TinyGo runtime; the
//     twenty lines of integer formatting below cost nothing.
//
//   - It does not allocate after mod_load. Every buffer is a package-level
//     array. mod_draw_panel runs once per frame per panel under a per-call fuel
//     budget, and a Go mod that produces garbage every frame pays for it twice:
//     once in the allocation and again when the collector scans.
//
// Every package-level variable here is its zero value on purpose. TinyGo runs
// package initialisers from the module's entry point, and the Gearbox host
// never calls one — it calls mod_load and the other mod_* exports directly. So
// anything that needs computing gets computed in mod_load. See README.md.
//
// Build:  ./build.sh          (produces hello-panel-go.odmod)
package main

import gearbox "github.com/Pr1nted/Open-Doctrines/sdk/go"

var (
	gEnv    gearbox.Env
	gPanel  gearbox.Panel
	gCursor uint32 // which country we are showing

	// Reused every frame; see the note above about not allocating.
	gLine [256]byte
	gName [64]byte
)

// TinyGo builds a package main, so main must exist. The host never calls it:
// there is no entry point in a Gearbox mod, only the mod_* exports.
func main() {}

// --- tiny formatting helpers ----------------------------------------------

func appendStr(dst []byte, at int, s string) int {
	return at + copy(dst[at:], s)
}

func appendUint(dst []byte, at int, v uint64) int {
	if v == 0 {
		dst[at] = '0'
		return at + 1
	}
	var tmp [20]byte
	n := 0
	for v > 0 {
		tmp[n] = byte('0' + v%10)
		n++
		v /= 10
	}
	for i := n - 1; i >= 0; i-- {
		dst[at] = tmp[i]
		at++
	}
	return at
}

// --- lifecycle -------------------------------------------------------------

//go:wasmexport mod_load
func modLoad() int32 {
	gEnv = gearbox.Environment()

	if gEnv.IsHeadless != 0 {
		// A training run has no renderer. Registering a panel would be a no-op
		// anyway, but skipping it makes the intent explicit.
		gearbox.Log(gearbox.LogInfo, "hello-panel-go: headless, no UI")
		return 0
	}

	gPanel = gearbox.PanelRegister("Hello Panel (Go)", 280, 150)
	if gPanel == 0 {
		// UI was declared but revoked, or we hit the panel limit. Not fatal:
		// degrade rather than trap.
		gearbox.Log(gearbox.LogWarn, "hello-panel-go: no panel, running quiet")
	}
	return 0 // non-zero would refuse the load
}

//go:wasmexport mod_unload
func modUnload() {
	gCursor = 0
}

// --- the panel -------------------------------------------------------------

// height goes unused: we draw a fixed layout and the host clips anything that
// does not fit. An unused parameter is legal Go, so there is no (void)h here.
//
//go:wasmexport mod_draw_panel
func modDrawPanel(panel, width, height uint32) {
	p := gearbox.Panel(panel)
	line := gLine[:]

	gearbox.DrawRect(p, 0, 0, int32(width), 1, 0x3C3C5AFF)

	// Turn number
	n := appendStr(line, 0, "Turn ")
	n = appendUint(line, n, uint64(gearbox.TurnNumber()))
	gearbox.DrawTextBytes(p, 8, 8, 0xFFFFFFFF, line[:n])

	// Country count
	count := gearbox.CountryCount()
	n = appendStr(line, 0, "Countries: ")
	n = appendUint(line, n, uint64(count))
	gearbox.DrawTextBytes(p, 8, 28, 0xB4B4C8FF, line[:n])

	if count == 0 {
		gearbox.DrawText(p, 8, 52, 0x9696A0FF, "No world loaded")
		return
	}
	if gCursor >= count {
		gCursor = 0
	}

	c := gearbox.CountryAt(gCursor)
	if uint32(c) != gearbox.Invalid {
		// Two-call sizing. CountryNameInto truncates to the buffer and tells us
		// what it wrote, so the sizing call is not needed when a fixed 64 bytes
		// is acceptable — CountryNameLen is there for when it is not.
		name := gearbox.CountryNameInto(c, gName[:])
		gearbox.DrawTextBytes(p, 8, 52, 0xFFFFFFFF, name)

		n = appendStr(line, 0, "Provinces: ")
		n = appendUint(line, n, uint64(gearbox.CountryProvinceCount(c)))
		gearbox.DrawTextBytes(p, 8, 72, 0xB4B4C8FF, line[:n])

		// Treasury comes back as float64; print the integer part. The clamp
		// matters: converting a float outside uint64's range is undefined in
		// Go, so a negative or absurd balance would print nonsense rather than
		// fail loudly. Written as !(t < 1e18) so NaN, which loses every
		// comparison, clamps too.
		t := gearbox.CountryTreasury(c)
		negative := t < 0
		if negative {
			t = -t
		}
		if !(t < 1e18) {
			t = 1e18
		}

		n = appendStr(line, 0, "Treasury: ")
		if negative {
			n = appendStr(line, n, "-")
		}
		n = appendUint(line, n, uint64(t))
		gearbox.DrawTextBytes(p, 8, 92, 0xB4B4C8FF, line[:n])
	}

	if gearbox.Button(p, 8, 116, 120, 24, "Next country") {
		gCursor++
	}
}
