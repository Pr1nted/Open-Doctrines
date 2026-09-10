// How many accounts exist, and why it is counted rather than remembered.
//
// The obvious build is a stored counter bumped on sign-up. KV has no atomic
// increment, so two sign-ups landing together read the same number and write
// the same number back — the total drifts downward, permanently, and nothing
// in the system can tell. These tests pin the counting behaviour instead,
// including the one case where the answer is deliberately not the whole truth.

import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import type { Account } from "../src/accounts/store.js";
import { countAccounts } from "../src/accounts/store.js";
import { clearKv, setupEnv } from "./helpers.js";

let env: Env;

async function makeAccount(id: string): Promise<void> {
    const account = {
        id, nick: id, nickNorm: id.toLowerCase(), created: 1, nickChangedAt: 0,
        badges: [] as Account["badges"], identities: [],
    } as Account;
    await env.OD_ACCOUNTS.put(`acct:${id}`, JSON.stringify(account));
}

beforeEach(async () => {
    env = await setupEnv();
    await clearKv(env);
});

describe("counting the accounts", () => {
    it("says nought when nobody has signed up", async () => {
        expect(await countAccounts(env)).toEqual({ accounts: 0, exact: true });
    });

    it("counts every account", async () => {
        for (let i = 0; i < 7; i++) await makeAccount(`a${i}`);
        expect(await countAccounts(env)).toEqual({ accounts: 7, exact: true });
    });

    it("COUNTS ONLY ACCOUNTS, not the other things that live in the same store", async () => {
        await makeAccount("real-one");
        // The namespace is shared: nicknames, identities, tombstones, usage rows
        // and the announcement board all sit beside the accounts. A prefix that
        // caught any of them would inflate the figure and look plausible doing it.
        await env.OD_ACCOUNTS.put("nick:real-one", "real-one");
        await env.OD_ACCOUNTS.put("tomb:someone", "1");
        await env.OD_ACCOUNTS.put("ident:discord:abc", "real-one");
        await env.OD_ACCOUNTS.put("usage:2026-09-10:x", "1");
        await env.OD_ACCOUNTS.put("stat:2026-09-10:y", "1");
        await env.OD_ACCOUNTS.put("announcements", "[]");
        expect(await countAccounts(env)).toEqual({ accounts: 1, exact: true });
    });

    it("reports a capped count as a floor, never as the total", async () => {
        for (let i = 0; i < 5; i++) await makeAccount(`a${i}`);
        const capped = await countAccounts(env, 3);
        expect(capped.exact).toBe(false);
        expect(capped.accounts).toBeGreaterThanOrEqual(3);
    });

    it("is exact when the count sits under the cap", async () => {
        for (let i = 0; i < 3; i++) await makeAccount(`a${i}`);
        expect(await countAccounts(env, 100)).toEqual({ accounts: 3, exact: true });
    });
});
