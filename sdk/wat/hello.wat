;; hello.wat — a complete OpenDoctrines mod, hand-written in WebAssembly text.
;;
;; This is the canonical reference for anyone writing a Gearbox binding for a
;; language the SDK does not cover. Everything a binding has to get right is
;; visible here with nothing generating it: the import declarations, the export
;; declarations, and the way a (ptr, len) string is just an offset and a byte
;; count into linear memory.
;;
;; If your language can (a) declare a function import from a named module,
;; (b) export a function under an exact name, and (c) put bytes in linear
;; memory and tell you their offset, it can write a Gearbox mod. That is the
;; whole requirement. Nothing else in this file is essential.
;;
;; Build:  ./build.sh          (produces hello-panel-wat.odmod)
;;
;; The ABI it implements is sdk/abi.json. Where this file and abi.json
;; disagree, abi.json wins and this file is a bug.

(module

  ;; ==================================================================
  ;; IMPORTS — what the host gives you
  ;; ==================================================================
  ;;
  ;; An import is a triple: (module name, field name, type). The module name
  ;; is the capability: "gearbox:core", "gearbox:ui", "gearbox:gamestate.read",
  ;; "gearbox:assets". You get an import ONLY if your MANIFEST.json declared
  ;; that capability and the user granted it. Importing anything else — any
  ;; other module name, a WASI function, an "env" symbol your linker invented
  ;; for an undefined reference — refuses the load with a diagnostic naming it.
  ;; That is a link-time rejection, not a runtime one: there is no way to
  ;; discover at runtime that you were denied, because you never instantiate.
  ;;
  ;; In the binary format each of these becomes an entry in the import section
  ;; (section id 2). The first one below encodes as:
  ;;
  ;;     0c 67 65 61 72 62 6f 78 3a 63 6f 72 65   ; len 12, "gearbox:core"
  ;;     03 6c 6f 67                              ; len  3, "log"
  ;;     00 00                                    ; kind 0x00 = func, typeidx 0
  ;;
  ;; Both names are length-prefixed UTF-8 with no terminator, so the colon in
  ;; "gearbox:core" is an ordinary byte and needs no escaping anywhere. If you
  ;; are writing a binding by hand-emitting bytes, that is the entire format.
  ;;
  ;; Imported functions occupy the LOW function indices, in declaration order,
  ;; before any function you define. The three below are funcidx 0, 1, 2, so
  ;; the first function defined in this module is funcidx 3. Getting this
  ;; wrong is the classic hand-written-wasm bug; using symbolic names ($log)
  ;; instead of raw indices avoids it entirely.

  ;; gearbox:core log — (i32 level, i32 msg_ptr, i32 msg_len) -> ()
  ;; Capability: Core (always granted, cannot be revoked).
  ;; level: 0 TRACE, 1 INFO, 2 WARN, 3 ERROR. Truncated at 2048 bytes.
  (import "gearbox:core" "log"
    (func $log (param i32 i32 i32)))

  ;; gearbox:ui panel_register — (i32 title_ptr, i32 title_len,
  ;;                              i32 min_w, i32 min_h) -> i32 panel
  ;; Capability: UI. Returns 0 when headless, when UI was revoked, or when you
  ;; already hold 8 panels. 0 is not a valid panel — check it.
  (import "gearbox:ui" "panel_register"
    (func $panel_register (param i32 i32 i32 i32) (result i32)))

  ;; gearbox:ui draw_text — (i32 panel, i32 x, i32 y, i32 rgba,
  ;;                         i32 text_ptr, i32 text_len) -> ()
  ;; Capability: UI. Panel-relative coordinates, colour is 0xRRGGBBAA,
  ;; truncated at 512 bytes per call.
  (import "gearbox:ui" "draw_text"
    (func $draw_text (param i32 i32 i32 i32 i32 i32)))

  ;; ==================================================================
  ;; MEMORY
  ;; ==================================================================
  ;;
  ;; One page = 64 KiB, which is far more than the strings below need, and one
  ;; is the minimum a module can declare while still having a memory at all.
  ;;
  ;; Export it as "memory". The desktop backend (WAMR) reaches your linear
  ;; memory through its own instance handle and does not need the export, but
  ;; the web backend reads `instance.exports.memory` and silently fails every
  ;; string read without it (src/mods/ModRuntime.cpp, od_mods_mem_read). Export
  ;; it and your mod behaves the same in both places. clang's wasm-ld exports
  ;; memory by default, which is why C mods never hit this; if you hand-write a
  ;; module, or your toolchain passes --no-export-memory, you will.
  ;;
  ;; Declare no maximum. The host applies limits.memoryPages from your manifest
  ;; as the ceiling at instantiation; a maximum declared here that is lower
  ;; than the manifest's just fights with it.
  (memory (export "memory") 1)

  ;; ==================================================================
  ;; DATA — where (ptr, len) strings actually come from
  ;; ==================================================================
  ;;
  ;; A Gearbox string is two i32s: a byte offset into this memory, and a byte
  ;; count. Not null-terminated, and the host does not keep the pointer after
  ;; the call returns. So a string constant is nothing more than bytes at a
  ;; known offset plus a number you wrote down.
  ;;
  ;; Each (data (i32.const N) "...") is an ACTIVE segment: the engine copies
  ;; those bytes to offset N during instantiation, before any export runs.
  ;; Active segments are core wasm and always initialised. (Passive segments,
  ;; the `memory.init` kind, need bulk-memory — the desktop host enables it,
  ;; but there is no reason to use them for constants.)
  ;;
  ;; Offsets are spaced out to 16-byte boundaries purely so this table stays
  ;; readable when a string changes length. Nothing requires alignment; a
  ;; string may start at any byte.
  ;;
  ;;   offset  len  content
  ;;   ------  ---  ---------------------------------
  ;;        0    9  "Hello WAT"                       panel title
  ;;       16   23  "hello-panel-wat: loaded"         INFO on load
  ;;       48   25  "hello-panel-wat: no panel"       WARN when UI unavailable
  ;;       80   25  "Hand-written WebAssembly."       first drawn line
  ;;      112   21  "No compiler involved."           second drawn line
  ;;
  ;; When you see (i32.const 80) (i32.const 25) passed to $draw_text below,
  ;; that pair IS the string. There is no string type, no length header, no
  ;; allocation and nothing to free.
  (data (i32.const 0)   "Hello WAT")
  (data (i32.const 16)  "hello-panel-wat: loaded")
  (data (i32.const 48)  "hello-panel-wat: no panel")
  (data (i32.const 80)  "Hand-written WebAssembly.")
  (data (i32.const 112) "No compiler involved.")

  ;; ==================================================================
  ;; STATE
  ;; ==================================================================
  ;;
  ;; A mutable global, the wasm equivalent of a static variable. It could just
  ;; as well live at a fixed address in linear memory; a global is cheaper and
  ;; cannot be scribbled on by a stray store.
  ;;
  ;; Zero means "no panel". Nothing persists across a reload: mod_load runs
  ;; again from scratch every time the user enables this mod.
  (global $panel (mut i32) (i32.const 0))

  ;; ==================================================================
  ;; EXPORTS — what the host calls on you
  ;; ==================================================================
  ;;
  ;; An export is a (name, kind, index) triple in the export section
  ;; (section id 7). "mod_load" encodes as:
  ;;
  ;;     08 6d 6f 64 5f 6c 6f 61 64   ; len 8, "mod_load"
  ;;     00 03                        ; kind 0x00 = func, funcidx 3
  ;;
  ;; The host looks the name up verbatim. There is no name mangling, no
  ;; underscore prefix, and no leading "_": if your toolchain decorates
  ;; symbols, the exported name still has to come out exactly "mod_load".
  ;;
  ;; Only mod_load is required. Optional exports are found by name and simply
  ;; not called if absent — mod_unload, mod_pre_turn, mod_post_turn (needs
  ;; GameProcess) and mod_draw_panel (needs UI). Exporting a hook you have no
  ;; capability for is harmless; it never fires.
  ;;
  ;; Nothing else of yours runs. The host calls your exports and nothing more:
  ;; there is no init hook of its own. Note the one exception, which matters if
  ;; you are porting a language with static constructors — a wasm `start`
  ;; function does run on both backends (the browser as part of instantiation,
  ;; WAMR in its post-instantiate step), whereas the linker-generated
  ;; `__wasm_call_ctors` is called by WAMR on desktop and NOT by the browser
  ;; backend. Do not put anything load-bearing in ctors. This module has
  ;; neither, which is why it behaves identically in both.

  ;; mod_load — () -> i32
  ;; Return 0 to accept the load. Non-zero refuses it and the value is shown to
  ;; the user, so use it for "I cannot run here", not for "something was
  ;; missing but I coped".
  (func (export "mod_load") (result i32)
    (local $p i32)

    ;; log(INFO, "hello-panel-wat: loaded")
    ;; level 1 = INFO; then the (ptr, len) pair straight out of the table above.
    (call $log
      (i32.const 1)
      (i32.const 16)
      (i32.const 23))

    ;; panel_register("Hello WAT", 240, 120) -> panel handle
    (local.set $p
      (call $panel_register
        (i32.const 0)     ;; title ptr
        (i32.const 9)     ;; title len
        (i32.const 240)   ;; min width
        (i32.const 120))) ;; min height
    (global.set $panel (local.get $p))

    ;; A zero handle means headless, revoked, or over the panel limit. Degrade,
    ;; do not trap: a mod that dies because UI was revoked is a bug in the mod.
    (if (i32.eqz (local.get $p))
      (then
        (call $log
          (i32.const 2)     ;; WARN
          (i32.const 48)
          (i32.const 25))))

    (i32.const 0))          ;; accept the load

  ;; mod_draw_panel — (i32 panel, i32 width, i32 height) -> ()
  ;; Called once per frame per visible panel. Never called when headless.
  ;;
  ;; The command list is cleared between frames, so re-issue every draw call
  ;; every frame. There is no retained scene to update.
  ;;
  ;; $width and $height are the panel's current size; this mod ignores them,
  ;; which is fine — the host clips anything you draw to the panel, so
  ;; out-of-range coordinates are cropped rather than escaping it.
  ;;
  ;; Use the panel handle the host passes in, not the one you stored. They are
  ;; the same today, but the argument is authoritative and a handle you do not
  ;; own is ignored anyway.
  (func (export "mod_draw_panel") (param $panel i32) (param $w i32) (param $h i32)

    ;; draw_text(panel, 8, 8, 0xFFFFFFFF, "Hand-written WebAssembly.")
    (call $draw_text
      (local.get $panel)
      (i32.const 8)             ;; x, panel-relative
      (i32.const 8)             ;; y, panel-relative
      (i32.const 0xFFFFFFFF)    ;; rgba: opaque white
      (i32.const 80)            ;; text ptr
      (i32.const 25))           ;; text len

    ;; draw_text(panel, 8, 28, 0xB4B4C8FF, "No compiler involved.")
    (call $draw_text
      (local.get $panel)
      (i32.const 8)
      (i32.const 28)
      (i32.const 0xB4B4C8FF)    ;; rgba: muted grey, opaque
      (i32.const 112)
      (i32.const 21)))
)
