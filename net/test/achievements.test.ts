// Achievements: a claim is a request, and only a signed grant is an
// achievement. Each property the game relies on is pinned here.

import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import { clearKv, setupEnv } from "./helpers.js";
import { createAccount, identSubHash, type Account } from "../src/accounts/store.js";
import { issueSessionToken } from "../src/auth/token.js";
import {
    achievementsClaim, achievementsMine, achievementsSession, wipeAchievements,
} from "../src/achievements/claim.js";
import { resetGrantKeyCache, verifyGrant } from "../src/achievements/grant.js";
import { BUDGET_CAPACITY } from "../src/achievements/AchievementsDO.js";
import { CATALOG } from "../src/achievements/catalog.gen.js";

let env: Env;
let account: Account;
let token: string;

async function withGrantKeys(e: Env): Promise<void> {
    const pair = await crypto.subtle.generateKey("Ed25519", true, ["sign", "verify"]) as CryptoKeyPair;
    const r = e as unknown as Record<string, unknown>;
    r.ACHIEVEMENT_PRIVATE_KEY = JSON.stringify(await crypto.subtle.exportKey("jwk", pair.privateKey));
    r.ACHIEVEMENT_PUBLIC_JWK = JSON.stringify(await crypto.subtle.exportKey("jwk", pair.publicKey));
    r.ACHIEVEMENT_KNOWN_SEALS = "";
    resetGrantKeyCache();
}

function post(path: string, body: unknown, bearer = token): Request {
    return new Request(`https://test.invalid${path}`, {
        method: "POST",
        headers: { authorization: `Bearer ${bearer}`, "content-type": "application/json" },
        body: JSON.stringify(body),
    });
}

async function startPlay(): Promise<string> {
    const r = await achievementsSession(post("/achievements/session", { seal: "abc", build: "test" }), env);
    expect(r.status).toBe(200);
    return ((await r.json()) as { play: string }).play;
}

async function claim(play: string, ids: string[]): Promise<any[]> {
    const r = await achievementsClaim(post("/achievements/claim", {
        play, claims: ids.map((ach) => ({ ach, ev: "{}" })),
    }), env);
    expect(r.status).toBe(200);
    return ((await r.json()) as { decisions: any[] }).decisions;
}

const immediate = CATALOG.filter((c) => c.minHours === 0 && c.minSessionMinutes === 0 && c.requires.length === 0);

beforeEach(async () => {
    env = await setupEnv();
    await withGrantKeys(env);
    await clearKv(env);
    const created = await createAccount(env, "discord", await identSubHash(env, "discord", "s1"), "Claimant");
    account = created.account!;
    token = await issueSessionToken(env, account.id);
    await wipeAchievements(env, account.id);
});

describe("a claim becomes a grant only through the service", () => {
    it("signs a grant the verifier accepts, naming the account and the achievement", async () => {
        const play = await startPlay();
        const [d] = await claim(play, ["first_steps"]);
        expect(typeof d.token).toBe("string");
        const g = await verifyGrant(env, d.token);
        expect(g?.sub).toBe(account.id);
        expect(g?.ach).toBe("first_steps");
    });

    it("refuses a grant whose payload was edited", async () => {
        const play = await startPlay();
        const [d] = await claim(play, ["first_steps"]);
        const [prefix, payload, sig] = (d.token as string).split(".");
        const edited = JSON.parse(atob(payload!.replace(/-/g, "+").replace(/_/g, "/")));
        edited.ach = "world_share_50";
        const forged = `${prefix}.${btoa(JSON.stringify(edited)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "")}.${sig}`;
        expect(await verifyGrant(env, forged)).toBeNull();
    });

    it("refuses a claim with no play session", async () => {
        const r = await achievementsClaim(post("/achievements/claim", { play: "nope", claims: [{ ach: "first_steps" }] }), env);
        expect(r.status).toBe(401);
    });

    it("refuses somebody else's play session", async () => {
        const play = await startPlay();
        const other = await createAccount(env, "discord", await identSubHash(env, "discord", "s2"), "Other");
        const otherToken = await issueSessionToken(env, other.account!.id);
        const r = await achievementsClaim(post("/achievements/claim", { play, claims: [{ ach: "first_steps" }] }, otherToken), env);
        expect(r.status).toBe(401);
    });

    it("refuses an achievement the catalog does not have", async () => {
        const play = await startPlay();
        const [d] = await claim(play, ["made_up_one"]);
        expect(d.refused).toBe("unknown");
    });
});

describe("time is what a forger cannot fake", () => {
    it("defers an achievement whose account clock has not run", async () => {
        const play = await startPlay();
        const decisions = await claim(play, ["first_steps", "end_turn_1", "end_turn_100"]);
        const late = decisions.find((d) => d.ach === "end_turn_100");
        expect(late.token).toBeUndefined();
        expect(late.retryAfter).toBeGreaterThan(0);
    });

    it("refuses a tier whose predecessor is not held", async () => {
        const play = await startPlay();
        const [d] = await claim(play, ["warmonger"]);
        expect(d.token).toBeUndefined();
        expect(String(d.refused ?? "")).toMatch(/^requires:/);
    });

    it("grants at most a budget's worth in one burst", async () => {
        expect(immediate.length).toBeGreaterThan(BUDGET_CAPACITY);
        const play = await startPlay();
        const decisions = await claim(play, immediate.slice(0, 24).map((c) => c.id));
        const granted = decisions.filter((d) => d.token).length;
        expect(granted).toBe(BUDGET_CAPACITY);
        expect(decisions.filter((d) => d.retryAfter).length).toBeGreaterThan(0);
    });

    it("is idempotent: claiming again returns the same grant and spends nothing", async () => {
        const play = await startPlay();
        const [a] = await claim(play, ["first_steps"]);
        const [b] = await claim(play, ["first_steps"]);
        expect(b.token).toBe(a.token);
    });
});

describe("the collection travels", () => {
    it("lists every grant the account holds", async () => {
        const play = await startPlay();
        await claim(play, ["first_steps", "end_turn_1"]);
        const r = await achievementsMine(new Request("https://test.invalid/achievements/mine", {
            headers: { authorization: `Bearer ${token}` },
        }), env);
        const body = await r.json() as { grants: string[] };
        expect(body.grants.length).toBe(2);
    });

    it("is gone when the account is", async () => {
        const play = await startPlay();
        await claim(play, ["first_steps"]);
        await wipeAchievements(env, account.id);
        const r = await achievementsMine(new Request("https://test.invalid/achievements/mine", {
            headers: { authorization: `Bearer ${token}` },
        }), env);
        expect(((await r.json()) as { grants: string[] }).grants.length).toBe(0);
    });
});
