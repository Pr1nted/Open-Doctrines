import type { Env as OdEnv } from "../src/env.js";

// `cloudflare:test` types its `env` as the global `Cloudflare.Env`, so that is
// what has to know about our bindings. Without this, tests see an empty bag and
// `env.LOBBY` does not typecheck even though it exists at runtime.
declare global {
    namespace Cloudflare {
        interface Env extends OdEnv {}
    }
}

export {};
