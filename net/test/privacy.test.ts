// The properties Part H of the design claims, asserted rather than assumed.
//
// Every test here corresponds to a sentence in PRIVACY.md. If one of them
// fails, the policy has become a lie, which is a worse bug than a crash.

import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import { clearKv, setupEnv } from "./helpers.js";
import {
    AUD_API, audRelay, issueSessionToken, resetKeyCache, verify, verifySessionToken,
} from "../src/auth/token.js";
import { issueJoinTicket, psidFor } from "../src/auth/ticket.js";
import type { TicketClaims } from "../src/auth/token.js";
import {
    createAccount, getAccount, identSubHash, publicAccount, type Account,
} from "../src/accounts/store.js";
import { exportAccount, confirmDeletion, issueDeleteConfirmation } from "../src/accounts/rights.js";

let env: Env;

async function anAccount(nick = "Vlad"): Promise<Account> {
    const created = await createAccount(
        env, "discord", await identSubHash(env, "discord", "subject-abc"), nick,
    );
    if (!created.account) throw new Error(`could not create account: ${created.nickError}`);
    return created.account;
}

beforeEach(async () => {
    env = await setupEnv();
    await clearKv(env);
});

describe("a join ticket cannot act as the player", () => {
    it("is refused by the account API, because its audience is not od-api", async () => {
        const account = await anAccount();
        const ticket = await issueJoinTicket(env, account, "SESSION1", "server-one", {
            presentBadges: true, nonce: "n1",
        });

        // This is the whole point of the two-token split: a hostile host that
        // somehow captured this holds something that cannot read, rename or
        // delete the account.
        expect(await verifySessionToken(env, ticket)).toBeNull();
    });

    it("is refused by a different session, because it is bound to one", async () => {
        const account = await anAccount();
        const ticket = await issueJoinTicket(env, account, "SESSION1", "server-one", {
            presentBadges: true, nonce: "n1",
        });

        expect(await verify(env, ticket, audRelay("SESSION1"))).not.toBeNull();
        expect(await verify(env, ticket, audRelay("SESSION2"))).toBeNull();
    });

    it("does not satisfy an audience that is a prefix of its own", async () => {
        const account = await anAccount();
        const ticket = await issueJoinTicket(env, account, "ABCD", "server-one", {
            presentBadges: true, nonce: "n1",
        });
        expect(await verify(env, ticket, audRelay("ABC"))).toBeNull();
    });

    it("expires within two minutes", async () => {
        const account = await anAccount();
        const now = Math.floor(Date.now() / 1000);
        const ticket = await issueJoinTicket(env, account, "S", "server-one", {
            presentBadges: true, nonce: "n1",
        }, now);

        expect(await verify(env, ticket, audRelay("S"), now + 119)).not.toBeNull();
        expect(await verify(env, ticket, audRelay("S"), now + 121)).toBeNull();
    });
});

describe("a session token cannot join a game", () => {
    it("is refused by the relay audience", async () => {
        const account = await anAccount();
        const session = await issueSessionToken(env, account.id);
        expect(await verify(env, session, audRelay("SESSION1"))).toBeNull();
        expect(await verify(env, session, AUD_API)).not.toBeNull();
    });
});

describe("a ticket tells a server as little as possible", () => {
    it("carries no account id and no provider identity", async () => {
        const account = await anAccount();
        const ticket = await issueJoinTicket(env, account, "S", "server-one", {
            presentBadges: true, nonce: "n1",
        });
        const claims = (await verify<TicketClaims>(env, ticket, audRelay("S")))!;

        const asText = JSON.stringify(claims);
        expect(asText).not.toContain(account.id);
        expect(asText).not.toContain(account.identities[0]!.subHash);
        expect(asText).not.toContain("discord");
        expect(claims).not.toHaveProperty("sub");
        expect(claims).not.toHaveProperty("email");
    });

    it("withholds badges when the player chose not to present them", async () => {
        const account = await anAccount();
        const badged = { ...account, badges: ["developer" as const] };

        const shown = await issueJoinTicket(env, badged, "S", "srv", {
            presentBadges: true, nonce: "n",
        });
        const hidden = await issueJoinTicket(env, badged, "S", "srv", {
            presentBadges: false, nonce: "n",
        });

        expect((await verify<TicketClaims>(env, shown, audRelay("S")))!.badges).toEqual(["developer"]);
        // A badge held by a handful of people is as correlating as a name, so
        // withholding it has to be the player's call, not just the server's.
        expect((await verify<TicketClaims>(env, hidden, audRelay("S")))!.badges).toEqual([]);
    });

    it("shows a per-server alias instead of the account nickname when asked", async () => {
        const account = await anAccount("Vlad");
        const ticket = await issueJoinTicket(env, account, "S", "srv", {
            alias: "Someone Else", presentBadges: false, nonce: "n",
        });
        const claims = (await verify<TicketClaims>(env, ticket, audRelay("S")))!;
        expect(claims.name).toBe("Someone Else");
        expect(claims.name).not.toBe(account.nick);
    });
});

describe("pairwise pseudonyms", () => {
    it("are stable for one server, so a host can recognise a returning player", async () => {
        const account = await anAccount();
        const first = await psidFor(env, account.id, "server-one");
        const second = await psidFor(env, account.id, "server-one");
        expect(first).toBe(second);
    });

    it("are unrelated across servers, so two hosts cannot compare notes", async () => {
        const account = await anAccount();
        const onA = await psidFor(env, account.id, "server-one");
        const onB = await psidFor(env, account.id, "server-two");
        expect(onA).not.toBe(onB);
    });

    it("differ between players on the same server", async () => {
        const a = await anAccount("PlayerOne");
        const created = await createAccount(
            env, "github", await identSubHash(env, "github", "subject-def"), "PlayerTwo",
        );
        const b = created.account!;
        expect(await psidFor(env, a.id, "srv")).not.toBe(await psidFor(env, b.id, "srv"));
    });

    it("cannot be reversed to the account id without the key", async () => {
        const account = await anAccount();
        const psid = await psidFor(env, account.id, "srv");
        expect(psid).not.toContain(account.id);
        expect(account.id).not.toContain(psid);
    });
});

describe("what leaves the service", () => {
    it("never includes the hashed provider subject in a public account view", async () => {
        const account = await anAccount();
        const view = JSON.stringify(publicAccount(account));
        expect(view).not.toContain(account.identities[0]!.subHash);
        // Which providers are linked is fine; which USER at that provider is not.
        expect(view).toContain("discord");
    });

    it("has no email field anywhere in an account", async () => {
        const account = await anAccount();
        expect(JSON.stringify(account).toLowerCase()).not.toContain("email");
        expect(JSON.stringify(exportAccount(env, account)).toLowerCase())
            .not.toContain('"email"');
    });
});

describe("data rights", () => {
    it("export includes every field actually stored, including the hashes", async () => {
        const account = await anAccount();
        const dump = exportAccount(env, account) as {
            account: { linkedIdentities: Array<{ subjectHash: string }> };
        };

        // Article 15 is "what do you hold", not "what do you normally show".
        expect(dump.account.linkedIdentities[0]!.subjectHash)
            .toBe(account.identities[0]!.subHash);

        const keys = Object.keys(dump.account);
        for (const expected of [
            "id", "nickname", "createdAt", "badges", "linkedIdentities",
            "nicknameLastChangedAt", "nicknameMatchForm",
        ]) {
            expect(keys).toContain(expected);
        }
    });

    it("deletion removes every key except the nickname tombstone", async () => {
        const account = await anAccount("Deletable");
        const before = await env.OD_ACCOUNTS.list();
        expect(before.keys.length).toBe(3);      // acct:, ident:, nick:

        const confirmation = await issueDeleteConfirmation(env, account);
        expect((await confirmDeletion(env, account, confirmation)).ok).toBe(true);

        const after = await env.OD_ACCOUNTS.list();
        const names = after.keys.map((k) => k.name);
        expect(names).toEqual([`tomb:${account.nickNorm}`]);
        expect(await getAccount(env, account.id)).toBeNull();
    });

    it("the tombstone holds nothing that points back at the deleted account", async () => {
        const account = await anAccount("Deletable");
        await confirmDeletion(env, account, await issueDeleteConfirmation(env, account));

        const value = await env.OD_ACCOUNTS.get(`tomb:${account.nickNorm}`);
        expect(value).toBeTruthy();
        expect(value).not.toContain(account.id);
        expect(value).not.toContain(account.identities[0]!.subHash);
        // Just a timestamp.
        expect(Number(value)).toBeGreaterThan(0);
    });

    it("a confirmation minted for one account cannot delete another", async () => {
        const victim = await anAccount("Victim");
        const attackerCreated = await createAccount(
            env, "github", await identSubHash(env, "github", "attacker"), "Attacker",
        );
        const attacker = attackerCreated.account!;

        const forAttacker = await issueDeleteConfirmation(env, attacker);
        const result = await confirmDeletion(env, victim, forAttacker);

        expect(result.ok).toBe(false);
        expect(await getAccount(env, victim.id)).not.toBeNull();
    });
});

describe("keys generated outside workerd", () => {
    it("import even though Node stamps alg:\"Ed25519\" onto the JWK", async () => {
        // The documented way to generate these keys is `node -e ...` (see
        // net/README.md). Node's exportKey adds `alg`, `key_ops` and `ext`,
        // and workerd rejects the `alg` outright:
        //
        //   JSON Web Key Algorithm parameter "alg" ("Ed25519") does not match
        //   requested Ed25519 curve.
        //
        // That failure surfaces as verify() returning null, which every caller
        // reports as "that sign-in expired" -- so a deployment with such a key
        // would let nobody log in, ever, with nothing in any log saying why.
        const pair = await crypto.subtle.generateKey(
            "Ed25519", true, ["sign", "verify"],
        ) as CryptoKeyPair;

        const nodeShaped = (jwk: JsonWebKey, ops: string[]) => JSON.stringify({
            key_ops: ops, ext: true, alg: "Ed25519",
            crv: jwk.crv, ...(jwk.d ? { d: jwk.d } : {}), x: jwk.x, kty: jwk.kty,
        });

        const e = env as unknown as Record<string, unknown>;
        e.ED25519_PRIVATE_KEY = nodeShaped(
            await crypto.subtle.exportKey("jwk", pair.privateKey) as JsonWebKey, ["sign"]);
        e.ED25519_PUBLIC_JWK = nodeShaped(
            await crypto.subtle.exportKey("jwk", pair.publicKey) as JsonWebKey, ["verify"]);
        resetKeyCache();

        const account = await anAccount();
        const token = await issueSessionToken(env, account.id);
        expect(await verifySessionToken(env, token)).not.toBeNull();
    });
});
