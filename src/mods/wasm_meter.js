// Gas metering for the browser mod backend.
//
// WHY THIS EXISTS
//
// On desktop, WAMR enforces an instruction limit, so a mod stuck in an infinite
// loop is terminated rather than hanging the game. The browser's own WebAssembly
// engine exposes no such limit and cannot preempt a synchronous call, so without
// this a mod hook with `while (1) {}` would freeze the tab with no recovery --
// and the browser build is the one most casual players meet, on itch.io.
//
// WHAT IT DOES
//
// It rewrites the mod's wasm before instantiation to meter its own execution:
//
//   * appends one exported mutable i64 global, `__od_fuel`, that the host sets to
//     the hook's budget before each call;
//   * at every function entry and every `loop` header -- the only places control
//     flows BACKWARD, so the only places an unbounded loop or unbounded recursion
//     must pass through -- subtracts a cost from `__od_fuel` and traps
//     (`unreachable`) once it reaches zero.
//
// Straight-line code is not metered, because it cannot loop: its length is
// bounded by the module size. Loops and recursion, the actual denial-of-service
// vectors, both cross a metered point every iteration, so a runaway hook spends
// its budget and traps instead of hanging. The trap surfaces as a RuntimeError
// that the caller already treats as a failed hook.
//
// SAFETY: IT CAN ONLY ADD SAFETY, NEVER BREAK A MOD
//
// The injected sequence is stack-neutral and sits at an instruction boundary, so
// it cannot change what the mod computes -- only whether it is allowed to run on.
// The one place correctness could slip is decoding instruction boundaries, so:
//
//   * any opcode the decoder does not know (exotic SIMD, atomics) ABANDONS the
//     transform for that module -- meter() returns null and the caller runs the
//     ORIGINAL, unmetered, exactly as before this file existed;
//   * the caller validates the transformed bytes with WebAssembly.validate and
//     falls back to the original if they do not validate.
//
// So a module this file cannot meter is no worse off than it was; it is never
// miscompiled into something that runs differently.
//
// Tested in tests/wasm_meter_test.mjs against the real test mods under node:
// a fuel-exhausting hook traps, a cheap one runs, and every transform validates.

(function (root) {
    "use strict";

    // ---- LEB128 ----
    function readU(buf, p) {
        let result = 0, shift = 0, byte;
        do {
            byte = buf[p.i++];
            result |= (byte & 0x7f) << shift;
            shift += 7;
        } while (byte & 0x80);
        return result >>> 0;
    }
    // Signed LEB, returned as a Number (fine for the s32/s33 we read here; we do
    // not need the VALUE of i64.const, only to skip it).
    function readS(buf, p) {
        let result = 0, shift = 0, byte;
        do {
            byte = buf[p.i++];
            result |= (byte & 0x7f) << shift;
            shift += 7;
        } while (byte & 0x80);
        if (shift < 32 && (byte & 0x40)) result |= (-1 << shift);
        return result;
    }
    function skipU(buf, p) { while (buf[p.i++] & 0x80) {} }
    function skipS(buf, p) { while (buf[p.i++] & 0x80) {} }

    function writeU(arr, value) {
        value >>>= 0;
        do {
            let byte = value & 0x7f;
            value >>>= 7;
            if (value !== 0) byte |= 0x80;
            arr.push(byte);
        } while (value !== 0);
    }
    // ---- instruction boundary decoder ----
    //
    // Walks a function body's instruction stream, calling onLoop(afterHeaderPos)
    // for each `loop` and returning the END position (one past `end`). Returns
    // null if it meets an opcode it does not understand, so the caller abandons.

    // Skip a blocktype: 0x40 (void), a single valtype byte, or a signed LEB type
    // index.
    function skipBlockType(buf, p) {
        const b = buf[p.i];
        if (b === 0x40 || (b >= 0x7b && b <= 0x7f) || b === 0x70 || b === 0x6f) {
            p.i++;              // void, or a numeric/ref valtype
        } else {
            skipS(buf, p);      // s33 type index
        }
    }

    // Returns false on an unknown opcode.
    function decodeInstr(buf, p, injectLoops) {
        const op = buf[p.i++];
        switch (op) {
            case 0x00: case 0x01: // unreachable, nop
                return true;
            case 0x02: case 0x03: case 0x04: { // block, loop, if
                skipBlockType(buf, p);
                if (op === 0x03 && injectLoops) injectLoops.push(p.i);
                return true;
            }
            case 0x05: // else
            case 0x0b: // end
                return true;
            case 0x0c: case 0x0d: // br, br_if
                skipU(buf, p); return true;
            case 0x0e: { // br_table
                const n = readU(buf, p);
                for (let k = 0; k < n; k++) skipU(buf, p);
                skipU(buf, p); // default
                return true;
            }
            case 0x0f: // return
                return true;
            case 0x10: // call
                skipU(buf, p); return true;
            case 0x11: // call_indirect
                skipU(buf, p); skipU(buf, p); return true;
            case 0x1a: case 0x1b: // drop, select
                return true;
            case 0x1c: { // select t*
                const n = readU(buf, p);
                for (let k = 0; k < n; k++) p.i++; // valtypes
                return true;
            }
            case 0x20: case 0x21: case 0x22: // local.get/set/tee
            case 0x23: case 0x24: // global.get/set
            case 0x25: case 0x26: // table.get/set
                skipU(buf, p); return true;
            case 0x3f: case 0x40: // memory.size/grow
                p.i++; return true; // reserved 0x00
            case 0x41: // i32.const
                skipS(buf, p); return true;
            case 0x42: // i64.const
                skipS(buf, p); return true;
            case 0x43: // f32.const
                p.i += 4; return true;
            case 0x44: // f64.const
                p.i += 8; return true;
            case 0xd0: // ref.null t
                p.i++; return true;
            case 0xd1: // ref.is_null
                return true;
            case 0xd2: // ref.func
                skipU(buf, p); return true;
            case 0xfc: { // bulk-memory / saturating truncation
                const sub = readU(buf, p);
                if (sub <= 7) return true;                 // sat trunc: no imm
                if (sub === 8) { skipU(buf, p); p.i++; return true; } // memory.init
                if (sub === 9) { skipU(buf, p); return true; }        // data.drop
                if (sub === 10) { p.i += 2; return true; }            // memory.copy
                if (sub === 11) { p.i++; return true; }               // memory.fill
                if (sub === 12) { skipU(buf, p); skipU(buf, p); return true; } // table.init
                if (sub === 13) { skipU(buf, p); return true; }       // elem.drop
                if (sub === 14) { skipU(buf, p); skipU(buf, p); return true; } // table.copy
                if (sub >= 15 && sub <= 17) { skipU(buf, p); return true; } // table.grow/size/fill
                return false;
            }
            default:
                // Memory load/store 0x28..0x3e carry a memarg (align + offset).
                if (op >= 0x28 && op <= 0x3e) { skipU(buf, p); skipU(buf, p); return true; }
                // Every numeric / comparison / conversion / sign-extension op in
                // 0x45..0xc4 is immediate-free.
                if (op >= 0x45 && op <= 0xc4) return true;
                // 0xfd (SIMD) and 0xfe (atomics) and anything else: abandon, so
                // the module runs unmetered rather than being miscompiled.
                return false;
        }
    }

    // ---- the fuel prelude, emitted as raw bytes ----
    //
    //   global.get G ; i64.const COST ; i64.sub ; global.set G ;
    //   global.get G ; i64.const 0 ; i64.le_s ; if ; unreachable ; end
    //
    // Stack-neutral: it pushes and consumes its own operands and leaves the value
    // stack as it found it.
    function prelude(globalIndex, cost) {
        const out = [];
        out.push(0x23); writeU(out, globalIndex);     // global.get G
        out.push(0x42); writeSLEB(out, cost);         // i64.const COST
        out.push(0x7d);                               // i64.sub
        out.push(0x24); writeU(out, globalIndex);     // global.set G
        out.push(0x23); writeU(out, globalIndex);     // global.get G
        out.push(0x42); out.push(0x00);               // i64.const 0
        out.push(0x57);                               // i64.le_s (0x57; 0x59 is ge_s)
        out.push(0x04); out.push(0x40);               // if (void)
        out.push(0x00);                               // unreachable
        out.push(0x0b);                               // end
        return out;
    }
    // Signed LEB for a small non-negative Number cost.
    function writeSLEB(arr, value) {
        let more = true;
        while (more) {
            let byte = value & 0x7f;
            value >>= 7;
            if ((value === 0 && (byte & 0x40) === 0) ||
                (value === -1 && (byte & 0x40) !== 0)) more = false;
            else byte |= 0x80;
            arr.push(byte);
        }
    }

    // ---- section framing ----
    function parseSections(buf) {
        if (buf.length < 8 ||
            buf[0] !== 0x00 || buf[1] !== 0x61 || buf[2] !== 0x73 || buf[3] !== 0x6d)
            return null;                 // not a wasm module
        const sections = [];
        const p = { i: 8 };
        while (p.i < buf.length) {
            const id = buf[p.i++];
            const size = readU(buf, p);
            const start = p.i;
            sections.push({ id, start, end: start + size });
            p.i = start + size;
        }
        return sections;
    }

    // Count imported globals (they take the low global indices).
    function countImportedGlobals(buf, sec) {
        if (!sec) return 0;
        const p = { i: sec.start };
        const n = readU(buf, p);
        let globals = 0;
        for (let k = 0; k < n; k++) {
            skipName(buf, p); skipName(buf, p);     // module, field
            const kind = buf[p.i++];
            if (kind === 0x00) skipU(buf, p);        // func: typeidx
            else if (kind === 0x01) { p.i++; skipLimits(buf, p); } // table
            else if (kind === 0x02) skipLimits(buf, p);           // memory
            else if (kind === 0x03) { p.i++; p.i++; globals++; }  // global: valtype, mut
            else return -1;
        }
        return globals;
    }
    function skipName(buf, p) { const n = readU(buf, p); p.i += n; }
    function skipLimits(buf, p) { const f = buf[p.i++]; skipU(buf, p); if (f & 0x01) skipU(buf, p); }

    function countDefinedGlobals(buf, sec) {
        if (!sec) return 0;
        const p = { i: sec.start };
        return readU(buf, p);
    }

    // ---- the transform ----
    //
    // Returns a Uint8Array, or null to run the original unmetered.
    function meter(wasm, opts) {
        opts = opts || {};
        const entryCost = opts.entryCost || 1;
        const loopCost = opts.loopCost || 1;
        try {
            const buf = wasm;
            const sections = parseSections(buf);
            if (!sections) return null;

            const byId = {};
            for (const s of sections) if (!(s.id in byId)) byId[s.id] = s;

            // Section ids (NOT the order they appear in): 2 import, 6 global,
            // 7 export, 10 code. Global precedes Export precedes Code.
            const importGlobals = countImportedGlobals(buf, byId[2]);
            if (importGlobals < 0) return null;
            const definedGlobals = countDefinedGlobals(buf, byId[6] /* global */);
            const fuelGlobal = importGlobals + definedGlobals;

            const codeSec = byId[10];
            if (!codeSec) return null;  // nothing executable to meter

            // Rebuild the code section with preludes injected.
            const newCode = rewriteCode(buf, codeSec, fuelGlobal, entryCost, loopCost);
            if (!newCode) return null;

            // Rebuild the global section with one appended i64 mutable global
            // initialised to 0 (the host sets it before every call).
            const newGlobals = appendGlobal(buf, byId[6]);

            // Rebuild the export section with `__od_fuel` -> fuelGlobal.
            const newExports = appendExport(buf, byId[7], fuelGlobal);

            return assemble(buf, sections, {
                6: newGlobals,     // global
                7: newExports,     // export
                10: newCode,       // code
            });
        } catch (e) {
            return null;           // never let a transform bug break a mod
        }
    }

    // Reassemble in canonical section order, substituting the replacement
    // payloads and copying every other section byte for byte. Custom sections
    // (id 0) are kept, up front; they carry no semantics the engine runs. A
    // replacement for a section the module lacked (a module with no global or
    // export section) is thereby INSERTED in the right place, which is why the
    // rebuild is always ordered rather than in file order.
    function assemble(buf, sections, replacements) {
        const out = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];
        const byId = {};
        const customs = [];
        for (const s of sections) {
            if (s.id === 0) customs.push(s);
            else byId[s.id] = s;
        }
        const emit = (id, payload) => {
            out.push(id); writeU(out, payload.length);
            for (const b of payload) out.push(b);
        };
        const copy = (s) => {
            out.push(s.id); writeU(out, s.end - s.start);
            for (let k = s.start; k < s.end; k++) out.push(buf[k]);
        };
        for (const s of customs) copy(s);
        // Canonical section order. DataCount (12) sits between Element (9) and
        // Code (10), not at the numeric end -- emitting by numeric id would put
        // it after Code and the module would not validate.
        const order = [1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 10, 11];
        for (const id of order) {
            if (id in replacements) emit(id, replacements[id]);
            else if (byId[id]) copy(byId[id]);
        }
        return new Uint8Array(out);
    }

    function rewriteCode(buf, sec, fuelGlobal, entryCost, loopCost) {
        const p = { i: sec.start };
        const count = readU(buf, p);
        const bodies = [];
        for (let f = 0; f < count; f++) {
            const bodySize = readU(buf, p);
            const bodyStart = p.i;
            const bodyEnd = bodyStart + bodySize;
            // locals declarations, then the instruction stream.
            const lp = { i: bodyStart };
            const localDecls = readU(buf, lp);
            for (let d = 0; d < localDecls; d++) { skipU(buf, lp); lp.i++; }
            const codeStart = lp.i;

            // Decode the instruction stream, collecting loop-header positions.
            const loops = [];
            const dp = { i: codeStart };
            let depth = 1;
            while (dp.i < bodyEnd && depth > 0) {
                const op = buf[dp.i];
                if (!decodeInstr(buf, dp, loops)) return null;
                if (op === 0x0b) depth--;             // end
                else if (op === 0x02 || op === 0x03 || op === 0x04) depth++;
            }
            if (dp.i !== bodyEnd) return null;        // decode did not line up

            // Build the new body: locals + entry prelude + instr stream with a
            // loop prelude inserted after each loop header.
            const head = Array.from(buf.subarray(bodyStart, codeStart)); // locals
            const entry = prelude(fuelGlobal, entryCost);
            const instr = [];
            let cur = codeStart;
            const sortedLoops = loops.slice().sort((a, b) => a - b);
            for (const pos of sortedLoops) {
                for (let k = cur; k < pos; k++) instr.push(buf[k]);
                for (const b of prelude(fuelGlobal, loopCost)) instr.push(b);
                cur = pos;
            }
            for (let k = cur; k < bodyEnd; k++) instr.push(buf[k]);

            const newBody = head.concat(entry, instr);
            bodies.push(newBody);
            p.i = bodyEnd;
        }

        const out = [];
        writeU(out, count);
        for (const b of bodies) { writeU(out, b.length); for (const x of b) out.push(x); }
        return out;
    }

    function appendGlobal(buf, sec) {
        const existing = [];
        let count = 0;
        if (sec) {
            const p = { i: sec.start };
            count = readU(buf, p);
            for (let k = p.i; k < sec.end; k++) existing.push(buf[k]);
        }
        const prefix = [];
        writeU(prefix, count + 1);
        // i64 (0x7e) mutable (0x01), init: i64.const 0 ; end
        const g = [0x7e, 0x01, 0x42, 0x00, 0x0b];
        return prefix.concat(existing, g);
    }

    function appendExport(buf, sec, fuelGlobal) {
        const out = [];
        let count = 0;
        if (sec) {
            const p = { i: sec.start };
            count = readU(buf, p);
            for (let k = p.i; k < sec.end; k++) out.push(buf[k]);
        }
        const name = "__od_fuel";
        const nameBytes = [];
        for (let k = 0; k < name.length; k++) nameBytes.push(name.charCodeAt(k));
        const exp = [];
        writeU(exp, nameBytes.length);
        for (const b of nameBytes) exp.push(b);
        exp.push(0x03);                 // kind: global
        writeU(exp, fuelGlobal);
        const prefix = [];
        writeU(prefix, count + 1);
        return prefix.concat(out, exp);
    }

    const api = { meter };
    if (typeof module !== "undefined" && module.exports) module.exports = api;
    if (root) root.__odWasmMeter = api;
    return api;
})(typeof globalThis !== "undefined" ? globalThis : this);
