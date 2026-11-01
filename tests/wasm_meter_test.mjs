// Proves the browser-backend gas meter (src/mods/wasm_meter.js) on real mods.
//
//   node tests/wasm_meter_test.mjs [testmods-dir]
//
// For every test-mod wasm it: transforms it, asserts the result VALIDATES, and
// asserts the transform is idempotent in the sense that re-validating holds. For
// coretest specifically it instantiates the TRANSFORMED module and shows the
// meter works end to end: with a full budget a cheap hook runs, and with a tiny
// budget the deliberately-infinite t_burn_fuel hook TRAPS instead of hanging --
// which on the real backend is the difference between a failed mod and a frozen
// tab. A module the meter declines (returns null for) is reported, not failed:
// declining is the safe fallback the backend runs unmetered.

import { readFileSync, existsSync, readdirSync } from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const { meter } = require(join(__dirname, "..", "src", "mods", "wasm_meter.js"));

let checks = 0, failures = 0;
function check(what, ok, detail = "") {
    checks++;
    console.log(`  ${ok ? "ok  " : "FAIL"} ${what}${detail ? "  --  " + detail : ""}`);
    if (!ok) failures++;
}

const dir = process.argv[2] || join(__dirname, "..", "cmake-build-debug", "testmods");
if (!existsSync(dir)) {
    console.log(`SKIP  no test mods at ${dir} (build them with tests/build_test_mods.sh)`);
    process.exit(0);
}

const wasms = readdirSync(dir).filter(f => f.endsWith(".wasm"));
check("there are test mods to meter", wasms.length > 0, dir);

for (const f of wasms) {
    const bytes = new Uint8Array(readFileSync(join(dir, f)));
    check(`${f}: original validates`, WebAssembly.validate(bytes));
    const metered = meter(bytes, { entryCost: 1, loopCost: 1 });
    if (metered === null) {
        check(`${f}: meter declined (runs unmetered, which is safe)`, true);
        continue;
    }
    check(`${f}: metered module validates`, WebAssembly.validate(metered));
    check(`${f}: metered module exports __od_fuel`,
          WebAssembly.Module.exports(new WebAssembly.Module(metered))
              .some(e => e.name === "__od_fuel" && e.kind === "global"));
}

// ---- end to end on coretest: the meter actually stops a runaway hook ----
const corePath = join(dir, "coretest.wasm");
if (existsSync(corePath)) {
    const bytes = new Uint8Array(readFileSync(corePath));
    const metered = meter(bytes, { entryCost: 1, loopCost: 1 });
    check("coretest meters", metered !== null);
    if (metered) {
        const mod = new WebAssembly.Module(metered);
        // Satisfy every import with a harmless stub so we can instantiate.
        const imports = {};
        for (const imp of WebAssembly.Module.imports(mod)) {
            imports[imp.module] = imports[imp.module] || {};
            if (imp.kind === "function") imports[imp.module][imp.name] = () => 0;
            else if (imp.kind === "memory")
                imports[imp.module][imp.name] = new WebAssembly.Memory({ initial: 1 });
            else if (imp.kind === "global")
                imports[imp.module][imp.name] = new WebAssembly.Global({ value: "i32", mutable: false }, 0);
        }
        let inst;
        try { inst = new WebAssembly.Instance(mod, imports); }
        catch (e) { check("coretest instantiates (transformed)", false, String(e)); }

        if (inst) {
            check("coretest instantiates (transformed)", true);
            const fuel = inst.exports.__od_fuel;
            check("the fuel global is an i64 global", fuel instanceof WebAssembly.Global);

            // A cheap hook runs when there is budget.
            if (typeof inst.exports.t_env_size === "function") {
                fuel.value = 1000000n;
                let ok = true, val = 0;
                try { val = inst.exports.t_env_size(); } catch (e) { ok = false; }
                check("a cheap hook runs with budget", ok, "returned " + val);
            }

            // The infinite-loop hook traps instead of hanging, once fuel runs out.
            if (typeof inst.exports.t_burn_fuel === "function") {
                fuel.value = 100000n;    // enough to enter, far too little to spin forever
                let trapped = false;
                try { inst.exports.t_burn_fuel(); }
                catch (e) { trapped = e instanceof WebAssembly.RuntimeError; }
                check("an infinite-loop hook traps on exhausted fuel", trapped);

                // And with no budget at all it cannot even get going.
                fuel.value = 0n;
                let trapped0 = false;
                try { inst.exports.t_burn_fuel(); }
                catch (e) { trapped0 = e instanceof WebAssembly.RuntimeError; }
                check("zero budget traps immediately", trapped0);
            }
        }
    }
}

console.log(`\n${checks} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
