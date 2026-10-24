// The audit behind "Link expired": verify() stays opaque to the player, but the
// server must be able to tell WHY a sign-in link failed -- genuine expiry, a
// mangled URL, or a signature problem look identical to a user and must not to
// us. These lock the reasons so the audit log can be trusted.

import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import { clearKv, setupEnv } from "./helpers.js";
import {
    AUTHREQ_TTL, issueAuthRequest, verifyAuthRequestExplain,
} from "../src/auth/device.js";
import { AUD_API, verify, verifyExplain } from "../src/auth/token.js";

let env: Env;

beforeEach(async () => {
    env = await setupEnv();
    await clearKv(env);
});

function aLink(now?: number): Promise<string> {
    return issueAuthRequest(env, "rid-xyz", "discord", "login", undefined, now);
}

describe("a failed sign-in link is explained to the server, not the player", () => {
    it("a fresh link verifies", async () => {
        const r = await verifyAuthRequestExplain(env, await aLink());
        expect(r.ok).toBe(true);
    });

    it("an old link reports expiry, by how long, and the TTL it carried", async () => {
        const issuedAt = Math.floor(Date.now() / 1000) - AUTHREQ_TTL - 120;
        const r = await verifyAuthRequestExplain(env, await aLink(issuedAt));
        expect(r.ok).toBe(false);
        if (!r.ok) {
            expect(r.reason).toBe("expired");
            expect(r.expiredBy ?? 0).toBeGreaterThanOrEqual(110);
            expect(r.ttl).toBe(AUTHREQ_TTL);
        }
    });

    it("a tampered signature is bad_signature, not expired", async () => {
        const parts = (await aLink()).split(".");
        const sig = parts[2]!;
        const flipped = (sig[0] === "A" ? "B" : "A") + sig.slice(1);
        const r = await verifyAuthRequestExplain(env, `${parts[0]}.${parts[1]}.${flipped}`);
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.reason).toBe("bad_signature");
    });

    it("a truncated token is malformed", async () => {
        const r = await verifyAuthRequestExplain(env, "od1.only-two-parts");
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.reason).toBe("malformed");
    });

    it("the right token for the wrong audience is wrong_audience", async () => {
        const r = await verifyExplain(env, await aLink(), AUD_API);
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.reason).toBe("wrong_audience");
    });

    it("the opaque verify() still returns null for every one of these", async () => {
        // The player must never learn which check failed; only the audit does.
        const parts = (await aLink(Math.floor(Date.now() / 1000) - AUTHREQ_TTL - 10)).split(".");
        expect(await verify(env, parts.join("."), "od-authreq")).toBeNull();      // expired
        expect(await verify(env, "od1.only-two-parts", "od-authreq")).toBeNull(); // malformed
    });
});
