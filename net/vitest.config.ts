import { defineConfig } from "vitest/config";
import { cloudflareTest } from "@cloudflare/vitest-pool-workers";

// Tests run inside workerd, not Node, so `crypto.subtle` behaves exactly as it
// will in production -- which matters here because Ed25519 support is the one
// thing that differs between runtimes.
export default defineConfig({
    plugins: [
        cloudflareTest({
            main: "./src/index.ts",
            wrangler: { configPath: "./wrangler.toml" },
        }),
    ],
});
