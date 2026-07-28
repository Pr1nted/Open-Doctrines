import { env } from "cloudflare:test";
import type { Env } from "../src/env.js";
import { resetKeyCache } from "../src/auth/token.js";
import { resetBlocklistCache } from "../src/accounts/nickname.js";

/**
 * Give the test isolate a real Ed25519 keypair and real HMAC keys.
 *
 * Generated rather than hardcoded so that a committed test key can never
 * become a deployed one -- the failure mode where a repository's example
 * signing key ends up in production is common enough to design against.
 */
export async function setupEnv(): Promise<Env> {
    const pair = await crypto.subtle.generateKey(
        "Ed25519", true, ["sign", "verify"],
    ) as CryptoKeyPair;

    const e = env as unknown as Record<string, unknown>;
    e.ISSUER = "https://test.invalid";
    e.ED25519_PRIVATE_KEY = JSON.stringify(await crypto.subtle.exportKey("jwk", pair.privateKey));
    e.ED25519_PUBLIC_JWK = JSON.stringify(await crypto.subtle.exportKey("jwk", pair.publicKey));
    e.IDENT_KEY = "test-ident-key";
    e.PAIRWISE_KEY = "test-pairwise-key";
    e.ADMIN_SECRET = "test-admin-secret";

    resetKeyCache();
    resetBlocklistCache();
    return env as unknown as Env;
}

/** Wipe every key so one test's account cannot be seen by the next. */
export async function clearKv(e: Env): Promise<void> {
    const listed = await e.OD_ACCOUNTS.list();
    for (const key of listed.keys) await e.OD_ACCOUNTS.delete(key.name);
}
