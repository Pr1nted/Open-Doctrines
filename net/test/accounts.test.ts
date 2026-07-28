import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import { clearKv, setupEnv } from "./helpers.js";
import {
    accountForIdentity, changeNickname, createAccount, deleteAccount, getAccount,
    identSubHash, identityIsBanned, linkIdentity, NICK_COOLDOWN_SECONDS,
    publicAccount, setBanned, unlinkIdentity, type Account,
} from "../src/accounts/store.js";
import { checkAccountAge } from "../src/accounts/policy.js";
import { PROVIDERS } from "../src/auth/providers.js";
import { setBadge } from "../src/accounts/badges.js";

let env: Env;

type Provider = "discord" | "github";

async function make(nick: string, provider: Provider = "discord", sub = "sub-1"): Promise<Account> {
    const created = await createAccount(env, provider, await identSubHash(env, provider, sub), nick);
    if (!created.account) throw new Error(String(created.nickError));
    return created.account;
}

/** Every store call takes a hashed subject; this is the only way to make one. */
const hashed = (provider: Provider, sub: string) => identSubHash(env, provider, sub);

beforeEach(async () => {
    env = await setupEnv();
    await clearKv(env);
});

describe("creation", () => {
    it("writes exactly three keys, which is the free-tier budget it was designed to", async () => {
        await make("Vlad");
        const listed = await env.OD_ACCOUNTS.list();
        expect(listed.keys.map((k) => k.name.split(":")[0]).sort())
            .toEqual(["acct", "ident", "nick"]);
    });

    it("finds the account again from the same provider identity", async () => {
        const account = await make("Vlad");
        // The raw subject is hashed once, here, and the store only ever deals
        // in the hash -- so nothing downstream can accidentally persist the
        // provider's own user id.
        const hash = await hashed("discord", "sub-1");
        const found = await accountForIdentity(env, "discord", hash);
        expect(found?.id).toBe(account.id);
        expect(found?.identities[0]!.subHash).toBe(hash);
    });

    it("refuses a nickname that is already taken, in its matching form", async () => {
        await make("Vlad");
        const clash = await createAccount(env, "github", await hashed("github", "sub-2"), "V_l_a_d");
        expect(clash.account).toBeNull();
        expect(clash.nickError).toBe("taken");
    });
});

describe("nickname changes", () => {
    it("are rate limited to one change a week", async () => {
        const account = await make("Vlad");
        const now = Math.floor(Date.now() / 1000);

        const first = await changeNickname(env, account, "Vladimir", now);
        expect(first.ok).toBe(true);

        const second = await changeNickname(env, (first as { account: Account }).account, "Vladislav", now + 10);
        expect(second.ok).toBe(false);
        expect(second).toMatchObject({ reason: "cooldown" });

        const later = await changeNickname(
            env, (first as { account: Account }).account, "Vladislav",
            now + NICK_COOLDOWN_SECONDS + 1,
        );
        expect(later.ok).toBe(true);
    });

    it("charges the cooldown for a restyle too", async () => {
        // "V-l-a-d" normalizes to "vlad", so this is the same name restyled.
        // It used to be exempt, which meant the cooldown never engaged and a
        // player could spend the service's whole daily KV write budget.
        const account = await make("Vlad");
        const now = Math.floor(Date.now() / 1000);

        const first = await changeNickname(env, account, "V-l-a-d", now);
        expect(first.ok).toBe(true);
        expect((first as { account: Account }).account.nick).toBe("V-l-a-d");

        const again = await changeNickname(
            env, (first as { account: Account }).account, "V_l_a_d", now + 5);
        expect(again.ok).toBe(false);
        expect(again).toMatchObject({ reason: "cooldown" });
    });

    it("costs no write at all when the name is unchanged", async () => {
        const account = await make("Vlad");
        const before = await env.OD_ACCOUNTS.get(`acct:${account.id}`);
        const result = await changeNickname(env, account, "Vlad");
        expect(result.ok).toBe(true);
        // Same bytes back, and crucially no cooldown spent on a no-op.
        expect(await env.OD_ACCOUNTS.get(`acct:${account.id}`)).toBe(before);
        expect((result as { account: Account }).account.nickChangedAt)
            .toBe(account.nickChangedAt);
    });

    it("a restyle does not disturb the uniqueness key", async () => {
        const account = await make("Vlad");
        await changeNickname(env, account, "V-l-a-d");
        expect(await env.OD_ACCOUNTS.get("nick:vlad")).toBe(account.id);
    });

    it("releases the old name immediately, since the account is still findable", async () => {
        const account = await make("Vlad");
        await changeNickname(env, account, "Vladimir");
        expect(await env.OD_ACCOUNTS.get("nick:vlad")).toBeNull();
        expect(await env.OD_ACCOUNTS.get("nick:vladimir")).toBe(account.id);
    });
});

describe("linking", () => {
    it("attaches a second provider to the same account", async () => {
        const account = await make("Vlad");
        const linked = await linkIdentity(env, account, "github", await hashed("github", "gh-1"));
        expect(linked.ok).toBe(true);

        const viaGithub = await accountForIdentity(env, "github", await hashed("github", "gh-1"));
        const viaDiscord = await accountForIdentity(env, "discord", await hashed("discord", "sub-1"));
        expect(viaGithub?.id).toBe(account.id);
        expect(viaDiscord?.id).toBe(account.id);
    });

    it("refuses to move an identity that belongs to someone else", async () => {
        const mine = await make("Vlad");
        const theirs = await createAccount(env, "github", await hashed("github", "gh-1"), "Someone");

        const stolen = await linkIdentity(env, mine, "github", await hashed("github", "gh-1"));
        expect(stolen.ok).toBe(false);
        expect(stolen).toMatchObject({ reason: "linked_elsewhere" });
        // Still theirs.
        expect((await accountForIdentity(env, "github", await hashed("github", "gh-1")))?.id)
            .toBe(theirs.account!.id);
    });

    it("refuses to unlink the last sign-in method", async () => {
        const account = await make("Vlad");
        const result = await unlinkIdentity(env, account, "discord");
        expect(result.ok).toBe(false);
        expect(result).toMatchObject({ reason: "last_identity" });
    });

    it("unlinks once another remains, and the freed identity can be reused", async () => {
        const account = await make("Vlad");
        const linked = await linkIdentity(env, account, "github", await hashed("github", "gh-1"));
        const after = await unlinkIdentity(env, (linked as { account: Account }).account, "discord");

        expect(after.ok).toBe(true);
        expect(await accountForIdentity(env, "discord", await hashed("discord", "sub-1"))).toBeNull();
        expect((await accountForIdentity(env, "github", await hashed("github", "gh-1")))?.id).toBe(account.id);
    });
});

describe("badges", () => {
    it("grants and revokes", async () => {
        const account = await make("Vlad");
        const granted = await setBadge(env, account.id, "developer", true);
        expect(granted.ok && granted.account.badges).toEqual(["developer"]);

        const revoked = await setBadge(env, account.id, "developer", false);
        expect(revoked.ok && revoked.account.badges).toEqual([]);
    });

    it("rejects an unknown badge rather than inventing one", async () => {
        const account = await make("Vlad");
        const result = await setBadge(env, account.id, "administrator", true);
        expect(result.ok).toBe(false);
        expect(result).toMatchObject({ reason: "bad_badge" });
    });
});

describe("account age gate", () => {
    const DAY = 86_400_000;
    const now = 1_800_000_000_000;

    it("refuses an account younger than the minimum", () => {
        const v = checkAccountAge(now - 3 * DAY, now, 30);
        expect(v.ok).toBe(false);
        expect(v).toMatchObject({ reason: "too_new", daysRemaining: 27 });
    });

    it("accepts one at exactly the minimum", () => {
        expect(checkAccountAge(now - 30 * DAY, now, 30).ok).toBe(true);
    });

    it("accepts one older than the minimum", () => {
        expect(checkAccountAge(now - 400 * DAY, now, 30).ok).toBe(true);
    });

    it("passes when the provider does not say, rather than refusing everyone", () => {
        // Google exposes no creation date under `openid`. Treating unknown as a
        // failure would turn an anti-alt measure into "nobody may use Google".
        expect(checkAccountAge(null, now, 30).ok).toBe(true);
    });

    it("treats a future creation date as unknown, not as infinitely old", () => {
        // A clock skew or a hostile value must not read as "very old", which is
        // the interpretation an evader would want.
        expect(checkAccountAge(now + 10 * DAY, now, 30).ok).toBe(true);
    });

    it("is disabled by a zero minimum", () => {
        expect(checkAccountAge(now, now, 0).ok).toBe(true);
    });

    it("derives a Discord account's age from its snowflake id", () => {
        // The top 42 bits are ms since 2015-01-01, so no extra scope or request.
        const created = Date.UTC(2020, 0, 1);
        const snowflake = String((BigInt(created - 1420070400000) << 22n) | 5n);
        const got = PROVIDERS.discord.accountCreatedAt({ id: snowflake });
        expect(got).toBe(created);
    });

    it("reads a GitHub account's age from created_at", () => {
        expect(PROVIDERS.github.accountCreatedAt({ created_at: "2015-06-01T00:00:00Z" }))
            .toBe(Date.parse("2015-06-01T00:00:00Z"));
    });

    it("returns null for Google, which cannot be gated", () => {
        expect(PROVIDERS.google.accountCreatedAt({ sub: "x" })).toBeNull();
    });

    it("returns null rather than throwing on a malformed provider reply", () => {
        expect(PROVIDERS.discord.accountCreatedAt({ id: "not-a-snowflake" })).toBeNull();
        expect(PROVIDERS.github.accountCreatedAt({ created_at: 12345 })).toBeNull();
        expect(PROVIDERS.github.accountCreatedAt({})).toBeNull();
    });
});

describe("bans", () => {
    it("marks and clears an account", async () => {
        const account = await make("Vlad");
        const banned = await setBanned(env, account, true, "cheating");
        expect(banned.banned?.reason).toBe("cheating");

        const cleared = await setBanned(env, banned, false, "");
        expect(cleared.banned).toBeUndefined();
    });

    it("tells the player, so joins do not just silently fail", async () => {
        const banned = await setBanned(env, await make("Vlad"), true, "cheating");
        expect(publicAccount(banned)).toHaveProperty("banned");
    });

    it("blocks the provider identity when a banned account is deleted", async () => {
        // Otherwise "delete my account, sign up again" clears a ban in two
        // clicks and the ban list is decorative.
        const account = await make("Vlad");
        const banned = await setBanned(env, account, true, "cheating");
        await deleteAccount(env, banned);

        expect(await identityIsBanned(env, "discord", await hashed("discord", "sub-1")))
            .toBe(true);

        const retry = await createAccount(
            env, "discord", await hashed("discord", "sub-1"), "FreshStart",
        );
        expect(retry.account).toBeNull();
        expect(retry.nickError).toBe("banned");
    });

    it("does not block the identity when an unbanned account is deleted", async () => {
        // Deleting is a right, not an admission. Leaving normally must not cost
        // you the ability to come back.
        const account = await make("Vlad");
        await deleteAccount(env, account);
        expect(await identityIsBanned(env, "discord", await hashed("discord", "sub-1")))
            .toBe(false);
    });

    it("still lets a banned account delete itself", async () => {
        // Erasure is a legal right and is not forfeited by a ban.
        const banned = await setBanned(env, await make("Vlad"), true, "cheating");
        await deleteAccount(env, banned);
        expect(await getAccount(env, banned.id)).toBeNull();
    });
});

describe("link-only providers", () => {
    it("marks Google as unable to create an account", () => {
        // It cannot be age-gated, so allowing it to create accounts would be a
        // door straight past the gate.
        expect(PROVIDERS.google.canCreateAccount).toBe(false);
    });

    it("leaves the gateable providers able to create", () => {
        expect(PROVIDERS.github.canCreateAccount).toBe(true);
        expect(PROVIDERS.discord.canCreateAccount).toBe(true);
    });

    it("keeps at least one provider able to create accounts", () => {
        // If every provider became link-only, nobody could ever sign up and the
        // service would be closed without anyone deciding to close it.
        expect(Object.values(PROVIDERS).some((p) => p.canCreateAccount)).toBe(true);
    });

    it("still lets a link-only provider be linked to an existing account", async () => {
        const account = await make("Vlad");
        const linked = await linkIdentity(
            env, account, "google" as never, await identSubHash(env, "google", "g-1"),
        );
        expect(linked.ok).toBe(true);
    });
});
