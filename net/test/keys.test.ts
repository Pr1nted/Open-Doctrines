// Publish keys.
//
// Almost everything worth testing here is a NEGATIVE. A publish key is a
// credential handed to a build server and pasted into a CI settings page, which
// is to say a credential that will eventually leak. What decides whether that
// is an incident or an inconvenience is the list of things it cannot do — so
// that list is what this file pins, one assertion each.

import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import type { Account } from "../src/accounts/store.js";
import {
    KEY_PREFIX, MAX_KEYS, list, looksLikeKey, mint, publicKey, publisher, revoke,
} from "../src/mods/keys.js";
import { authenticate } from "../src/http.js";
import { issueSessionToken } from "../src/auth/token.js";
import { clearKv, setupEnv } from "./helpers.js";

let env: Env;

async function makeAccount(id: string, nick: string): Promise<Account> {
    const account = {
        id, nick, nickNorm: nick.toLowerCase(), created: 1, nickChangedAt: 0,
        badges: [], identities: [],
    } as unknown as Account;
    await env.OD_ACCOUNTS.put(`acct:${id}`, JSON.stringify(account));
    return account;
}

const withBearer = (token: string) =>
    new Request("https://test.invalid/mods", {
        method: "POST", headers: { authorization: `Bearer ${token}` },
    });

async function mintFor(account: Account, label = "CI") {
    const r = await mint(env, account, label);
    if ("ok" in r && r.ok === false) throw new Error(r.message);
    return r as { token: string; key: { hash: string; label: string } };
}

beforeAll(async () => { env = await setupEnv(); });
beforeEach(async () => { await clearKv(env); });

describe("a key publishes", () => {
    it("resolves to the account that minted it", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const { token } = await mintFor(jane);

        const who = await publisher(withBearer(token), env);
        expect(who?.id).toBe("acct-jane");
    });

    it("is recognisable by its prefix, without touching storage", () => {
        expect(looksLikeKey(KEY_PREFIX + "abc")).toBe(true);
        expect(looksLikeKey("od1.something.else")).toBe(false);
        expect(looksLikeKey(null)).toBe(false);
    });
});

describe("what a publish key CANNOT do", () => {
    it("cannot act as an account credential", async () => {
        // THE ONE THAT MATTERS. auth/token.ts says the session token "is the
        // only thing that can call /account/*". If authenticate() ever returned
        // an account for a publish key, every route gated on it -- nickname,
        // deletion, join tickets, the whole of /moderation -- would accept a
        // credential that lives in a CI settings page.
        const jane = await makeAccount("acct-jane", "Jane");
        const { token } = await mintFor(jane);

        expect(await authenticate(withBearer(token), env)).toBeNull();
    });

    it("is not confusable with a session token in either direction", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const session = await issueSessionToken(env, jane.id);
        const { token } = await mintFor(jane);

        // A session token is not a publish key...
        expect(looksLikeKey(session)).toBe(false);
        expect(await publisher(withBearer(session), env)).toBeNull();
        // ...and a publish key is not a session token.
        expect(await authenticate(withBearer(token), env)).toBeNull();
    });

    it("stops working the moment it is revoked", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const { token, key } = await mintFor(jane);
        expect(await publisher(withBearer(token), env)).not.toBeNull();

        expect(await revoke(env, jane.id, key.hash)).toBe(true);
        expect(await publisher(withBearer(token), env)).toBeNull();
    });

    it("cannot be revoked by somebody else", async () => {
        // The hash IS the storage key, so without an ownership check anyone who
        // learned one could stop another author's pipeline.
        const jane = await makeAccount("acct-jane", "Jane");
        const bob = await makeAccount("acct-bob", "Bob");
        const { token, key } = await mintFor(jane);

        expect(await revoke(env, bob.id, key.hash)).toBe(false);
        expect(await publisher(withBearer(token), env)).not.toBeNull();
    });
});

describe("what is stored", () => {
    it("never stores the key itself", async () => {
        // A copy of the database must not let anybody publish as anyone.
        const jane = await makeAccount("acct-jane", "Jane");
        const { token } = await mintFor(jane);

        const all = await env.OD_ACCOUNTS.list({ prefix: "pkg:key" });
        expect(all.keys.length).toBeGreaterThan(0);
        for (const k of all.keys) {
            expect(k.name).not.toContain(token);
            const raw = (await env.OD_ACCOUNTS.get(k.name)) ?? "";
            expect(raw).not.toContain(token);
        }
    });

    it("shows an author their keys without anything reversible in them", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const { token } = await mintFor(jane, "GitHub Actions");

        const shown = (await list(env, jane.id)).map(publicKey);
        expect(shown).toHaveLength(1);
        expect(shown[0]!.label).toBe("GitHub Actions");
        expect(JSON.stringify(shown)).not.toContain(token);
    });

    it("keeps one author's keys out of another's list", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const bob = await makeAccount("acct-bob", "Bob");
        await mintFor(jane);
        expect(await list(env, bob.id)).toHaveLength(0);
    });
});

describe("limits", () => {
    it("caps how many an account may hold", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        for (let i = 0; i < MAX_KEYS; i++) await mintFor(jane, `key ${i}`);

        const over = await mint(env, jane, "one too many");
        expect("ok" in over && over.ok === false).toBe(true);
        expect((over as { code: string }).code).toBe("too_many_keys");
    });

    it("gives an unnamed key a name rather than an empty one", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const { key } = await mintFor(jane, "");
        expect(key.label).toBe("unnamed");
    });
});
