// Long-form turn storage: who may write what, and what anyone may read.
//
// This is the server half of TurnStoreKind::DurableObject (src/net/TurnStore.h).
// The URL shapes and the wire format come from the game's own client in
// src/net/TurnStore.cpp -- they are a contract being met, not a design being
// chosen here, so the tests are written against what that client actually
// sends.
//
// The asymmetry under test:
//
//   turn bundles   host writes once, and never again. Anyone reads, with no
//                  credential: a spectator following a tournament has none.
//   orders         one player writes their own, and reads their own. The host
//                  reads anyone's, because opening them resolves the turn.
//                  Nobody else does either -- and an authenticated attempt at
//                  somebody else's is recorded for the host to see.

import { env as testEnv, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import { clearKv, setupEnv } from "./helpers.js";
import { createAccount, identSubHash, type Account } from "../src/accounts/store.js";
import { issueJoinTicket, psidFor } from "../src/auth/ticket.js";
import { issueSessionToken } from "../src/auth/token.js";
import { issueSessionDescriptor } from "../src/lobby/session.js";
import type { SessionSettings } from "../src/lobby/LobbyDO.js";

let env: Env;

const ORIGIN = "https://od.test.invalid";
const SERVER = "server-one";

const SETTINGS: SessionSettings = {
    name: "Long game", listed: false, maxPlayers: 4,
    showBadges: true, longForm: true, requiredMods: [],
};

async function anAccount(nick: string, sub: string): Promise<Account> {
    const created = await createAccount(
        env, "discord", await identSubHash(env, "discord", sub), nick,
    );
    if (!created.account) throw new Error(String(created.nickError));
    return created.account;
}

/** Open a session owned by `owner`, the way the Worker's /session route does. */
async function openSession(code: string, owner: Account): Promise<void> {
    const stub = testEnv.LOBBY.get(testEnv.LOBBY.idFromName(code));
    const response = await stub.fetch(new Request("https://lobby/init", {
        method: "POST",
        body: JSON.stringify({
            descriptor: await issueSessionDescriptor(env, code, SERVER),
            settings: SETTINGS,
            hostPsid: await psidFor(env, owner.id, SERVER),
        }),
    }));
    expect(response.status).toBe(200);
}

/** Exactly what `wrapBlob` in TurnStore.cpp puts on the wire. */
function wrapBlob(turn: number, data: string): string {
    return JSON.stringify({ od: 1, turn, data });
}

function put(url: string, token: string | null, body: string): Promise<Response> {
    return SELF.fetch(`${ORIGIN}${url}`, {
        method: "PUT",
        ...(token ? { headers: { authorization: `Bearer ${token}` } } : {}),
        body,
    });
}

/** A read with no credential. Right for a turn bundle; refused for orders. */
function read(url: string): Promise<Response> {
    return SELF.fetch(`${ORIGIN}${url}`);
}

/** A read that says who is asking, which is what fetchOrders now sends. */
function readAs(url: string, token: string): Promise<Response> {
    return SELF.fetch(`${ORIGIN}${url}`, { headers: { authorization: `Bearer ${token}` } });
}

beforeEach(async () => {
    env = await setupEnv();
    await clearKv(env);
});

describe("publishing a turn", () => {
    it("is written by the host and readable by anyone, with the payload intact", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("AAAA-BBBB", host);
        const token = await issueSessionToken(env, host.id);

        const written = await put("/session/AAAA-BBBB/turn/1", token, wrapBlob(1, "aGVsbG8gd29ybGQ"));
        expect(written.status).toBe(200);

        // No bearer here, and that is the point: a spectator following a
        // tournament has no account and needs none.
        const got = await read("/session/AAAA-BBBB/turn/1");
        expect(got.status).toBe(200);
        expect(await got.json()).toEqual({ od: 1, turn: 1, data: "aGVsbG8gd29ybGQ" });
    });

    it("cannot be republished, even by the host that wrote it", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("AAAA-BBBC", host);
        const token = await issueSessionToken(env, host.id);

        expect((await put("/session/AAAA-BBBC/turn/2", token, wrapBlob(2, "Zmlyc3Q"))).status)
            .toBe(200);

        // A published turn is the record of what happened. Letting the host
        // revise it after the fact is the one thing immutability is for.
        const again = await put("/session/AAAA-BBBC/turn/2", token, wrapBlob(2, "c2Vjb25k"));
        expect(again.status).toBe(409);

        const got = await read("/session/AAAA-BBBC/turn/2");
        expect(await got.json()).toMatchObject({ data: "Zmlyc3Q" });
    });

    it("is refused to a player who is not the host", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("AAAA-BBBD", host);
        const player = await anAccount("Alice", "alice-sub");

        const response = await put(
            "/session/AAAA-BBBD/turn/1",
            await issueSessionToken(env, player.id),
            wrapBlob(1, "bm90bWluZQ"),
        );
        expect(response.status).toBe(403);
        expect((await read("/session/AAAA-BBBD/turn/1")).status).toBe(404);
    });

    it("is refused without a credential", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("AAAA-BBBE", host);

        expect((await put("/session/AAAA-BBBE/turn/1", null, wrapBlob(1, "bm9wZQ"))).status)
            .toBe(401);
    });

    it("is refused to a join ticket, whose audience is not the account API", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("AAAA-BBBF", host);

        // The host's OWN ticket for this session. It names the right psid, so
        // the only thing standing between it and a write is the audience check
        // -- which is exactly the split the two-token design exists to make.
        const ticket = await issueJoinTicket(env, host, "AAAA-BBBF", SERVER, {
            presentBadges: true, nonce: "n1",
        });

        expect((await put("/session/AAAA-BBBF/turn/1", ticket, wrapBlob(1, "dGlja2V0"))).status)
            .toBe(401);
    });
});

describe("submitting orders", () => {
    it("is written by the player it belongs to, and may be revised", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("CCCC-DDDD", host);
        const alice = await anAccount("Alice", "alice-sub");
        const token = await issueSessionToken(env, alice.id);
        const psid = await psidFor(env, alice.id, SERVER);

        expect((await put(`/session/CCCC-DDDD/orders/1/${psid}`, token, wrapBlob(1, "Zmlyc3Q"))).status)
            .toBe(200);

        // Changing your mind before the turn resolves is ordinary play, and
        // only this player can do it.
        expect((await put(`/session/CCCC-DDDD/orders/1/${psid}`, token, wrapBlob(1, "c2Vjb25k"))).status)
            .toBe(200);

        const got = await readAs(`/session/CCCC-DDDD/orders/1/${psid}`, token);
        expect(await got.json()).toMatchObject({ data: "c2Vjb25k" });
    });

    it("cannot be written on another player's behalf", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("CCCC-DDDE", host);
        const alice = await anAccount("Alice", "alice-sub");
        const bob = await anAccount("Bob", "bob-sub");

        const alicePsid = await psidFor(env, alice.id, SERVER);

        // Bob, holding a perfectly valid credential of his own, aiming at
        // Alice's slot. On a public bucket this is the attack the seal has to
        // absorb; here it simply does not happen.
        const response = await put(
            `/session/CCCC-DDDE/orders/1/${alicePsid}`,
            await issueSessionToken(env, bob.id),
            wrapBlob(1, "Zm9yZ2Vk"),
        );
        expect(response.status).toBe(403);
        expect((await readAs(`/session/CCCC-DDDE/orders/1/${alicePsid}`,
                             await issueSessionToken(env, alice.id))).status).toBe(404);
    });

    it("is refused to the host, who is not the player either", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("CCCC-DDDF", host);
        const alice = await anAccount("Alice", "alice-sub");
        const alicePsid = await psidFor(env, alice.id, SERVER);

        const response = await put(
            `/session/CCCC-DDDF/orders/1/${alicePsid}`,
            await issueSessionToken(env, host.id),
            wrapBlob(1, "aG9zdA"),
        );
        expect(response.status).toBe(403);
    });
});

// READING somebody else's orders, which is the attack the seal was believed to
// cover and did not.
//
// One seal key is generated per SESSION and handed to every player (Host.cpp
// sends the same config.sealKey to each peer); the URL is built from the join
// code, the turn and a psid the roster publishes; and turnOpen() binds the psid
// as associated data rather than treating it as a secret. So while reads were
// unauthenticated, every input to "fetch and open a rival's orders before the
// turn resolves" was in the hands of anybody who had joined the game -- with a
// stock client, over plain HTTP GET.
describe("reading orders", () => {
    it("is refused without a credential, where a turn bundle is not", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("KKKK-MMMM", host);
        const alice = await anAccount("Alice", "alice-sub");
        const alicePsid = await psidFor(env, alice.id, SERVER);
        await put(`/session/KKKK-MMMM/orders/1/${alicePsid}`,
                  await issueSessionToken(env, alice.id), wrapBlob(1, "b3JkZXJz"));

        expect((await read(`/session/KKKK-MMMM/orders/1/${alicePsid}`)).status).toBe(401);
    });

    it("is refused to a rival holding a valid credential of their own", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("KKKK-MMMN", host);
        const alice = await anAccount("Alice", "alice-sub");
        const bob = await anAccount("Bob", "bob-sub");
        const alicePsid = await psidFor(env, alice.id, SERVER);
        await put(`/session/KKKK-MMMN/orders/1/${alicePsid}`,
                  await issueSessionToken(env, alice.id), wrapBlob(1, "c2VjcmV0"));

        // Bob is a legitimate player in this very session. That is the point:
        // he holds the session's seal key, so ciphertext would have been
        // plaintext to him.
        const peek = await readAs(`/session/KKKK-MMMN/orders/1/${alicePsid}`,
                                  await issueSessionToken(env, bob.id));
        expect(peek.status).toBe(403);
        expect(await peek.text()).not.toContain("c2VjcmV0");
    });

    it("is allowed to the player themselves and to the host who must open it", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("KKKK-MMMP", host);
        const alice = await anAccount("Alice", "alice-sub");
        const alicePsid = await psidFor(env, alice.id, SERVER);
        await put(`/session/KKKK-MMMP/orders/1/${alicePsid}`,
                  await issueSessionToken(env, alice.id), wrapBlob(1, "bWluZQ"));

        expect((await readAs(`/session/KKKK-MMMP/orders/1/${alicePsid}`,
                             await issueSessionToken(env, alice.id))).status).toBe(200);
        // The host opens every player's orders to resolve the turn; refusing
        // this would substitute the AI for everybody, every turn.
        expect((await readAs(`/session/KKKK-MMMP/orders/1/${alicePsid}`,
                             await issueSessionToken(env, host.id))).status).toBe(200);
    });

    it("tells the host who tried, and tells nobody else", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("KKKK-MMMQ", host);
        const alice = await anAccount("Alice", "alice-sub");
        const bob = await anAccount("Bob", "bob-sub");
        const alicePsid = await psidFor(env, alice.id, SERVER);
        const bobPsid = await psidFor(env, bob.id, SERVER);
        await put(`/session/KKKK-MMMQ/orders/3/${alicePsid}`,
                  await issueSessionToken(env, alice.id), wrapBlob(3, "b3Vycw"));

        await readAs(`/session/KKKK-MMMQ/orders/3/${alicePsid}`,
                     await issueSessionToken(env, bob.id));

        const seen = await readAs("/session/KKKK-MMMQ/peeks",
                                  await issueSessionToken(env, host.id));
        expect(seen.status).toBe(200);
        expect(await seen.json()).toMatchObject({
            peeks: [{ turn: 3, who: bobPsid, target: alicePsid }],
        });

        // The accused does not get to find out that it registered -- that is
        // the one fact that would turn a refusal into a tuning signal. Same
        // rule NetPlayerReport already follows.
        expect((await readAs("/session/KKKK-MMMQ/peeks",
                             await issueSessionToken(env, bob.id))).status).toBe(403);
        expect((await read("/session/KKKK-MMMQ/peeks")).status).toBe(401);
    });

    it("records nothing when a player reads their own", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("KKKK-MMMR", host);
        const alice = await anAccount("Alice", "alice-sub");
        const alicePsid = await psidFor(env, alice.id, SERVER);
        await put(`/session/KKKK-MMMR/orders/1/${alicePsid}`,
                  await issueSessionToken(env, alice.id), wrapBlob(1, "bWluZQ"));

        await readAs(`/session/KKKK-MMMR/orders/1/${alicePsid}`,
                     await issueSessionToken(env, alice.id));
        await readAs(`/session/KKKK-MMMR/orders/1/${alicePsid}`,
                     await issueSessionToken(env, host.id));

        const seen = await readAs("/session/KKKK-MMMR/peeks",
                                  await issueSessionToken(env, host.id));
        expect(await seen.json()).toEqual({ peeks: [] });
    });
});

// A Durable Object outlives a test -- `clearKv` wipes KV and nothing else --
// so each of these opens its own session rather than sharing one.
describe("what the store refuses to hold", () => {
    async function hostOf(code: string): Promise<string> {
        const host = await anAccount("Hosty", "host-sub");
        await openSession(code, host);
        return issueSessionToken(env, host.id);
    }

    it("a blob whose body disagrees with its URL about the turn", async () => {
        const token = await hostOf("EEEE-FFFA");
        // A client that contradicts itself has a bug, and storing the blob
        // anyway would hide it until someone read back the wrong turn.
        expect((await put("/session/EEEE-FFFA/turn/5", token, wrapBlob(4, "bWlzbWF0Y2g"))).status)
            .toBe(400);
    });

    it("a data field that is not base64url", async () => {
        const token = await hostOf("EEEE-FFFB");
        // Otherwise this is a place to park arbitrary JSON, which is not what
        // a turn store is.
        expect((await put("/session/EEEE-FFFB/turn/6", token,
                          JSON.stringify({ od: 1, turn: 6, data: { nested: "object" } }))).status)
            .toBe(400);
        expect((await put("/session/EEEE-FFFB/turn/6", token, wrapBlob(6, "not/valid+chars="))).status)
            .toBe(400);
    });

    it("a blob without the format marker", async () => {
        const token = await hostOf("EEEE-FFFC");
        expect((await put("/session/EEEE-FFFC/turn/7", token,
                          JSON.stringify({ turn: 7, data: "bm9tYXJrZXI" }))).status)
            .toBe(400);
    });

    it("anything larger than the cap", async () => {
        const token = await hostOf("EEEE-FFFD");
        // The game's own HttpRequest caps a response at 1 MB, so a blob it
        // could write but never read back would be a trap rather than a
        // feature.
        const huge = "A".repeat(600 * 1024);
        expect((await put("/session/EEEE-FFFD/turn/8", token, wrapBlob(8, huge))).status)
            .toBe(400);
    });
});

describe("the routes themselves", () => {
    it("never reach the lobby for a code the service could not have issued", async () => {
        // `not_found` is the router falling through, having touched no
        // binding; `no_session` comes from inside the Durable Object, so
        // seeing it means the object was reached.
        const bad = await read("/session/ABCD-EFG0/turn/1");
        expect(bad.status).toBe(404);
        expect(await bad.json()).toMatchObject({ error: "not_found" });

        const wellFormed = await read("/session/ABCD-EFGH/turn/1");
        expect(await wellFormed.json()).toMatchObject({ error: "no_session" });
    });

    it("leave nothing behind when a well-formed code names no session", async () => {
        // The shape check makes a malformed guess free, but a well-formed one
        // still reaches the object -- and reaching it creates it, durably,
        // with no alarm that would ever reclaim it. Probing must not be a way
        // to accumulate empty Durable Objects.
        expect((await read("/session/JJJJ-KKKK/turn/1")).status).toBe(404);

        // The assertion is that the session can still be opened afterwards.
        // A probe that left `meta` rows behind would make /init believe the
        // session already existed and refuse it with 409 -- and one that wiped
        // the tables without putting them back would fail on `no such table`,
        // which is how the missing `ensureSchema` was found.
        const host = await anAccount("Hosty", "host-sub");
        await openSession("JJJJ-KKKK", host);
    });

    it("answer a browser, which the web build needs and a Durable Object does not do", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("GGGG-HHHH", host);
        await put("/session/GGGG-HHHH/turn/1", await issueSessionToken(env, host.id),
                  wrapBlob(1, "Y29ycw"));

        // The DO's own response carries no CORS headers at all, so without the
        // Worker adding them the web build discards every one of these.
        const got = await read("/session/GGGG-HHHH/turn/1");
        expect(got.headers.get("access-control-allow-origin")).toBe("*");
        expect(got.headers.get("access-control-allow-methods")).toContain("PUT");
    });

    it("let the edge cache a published turn, but never a set of orders", async () => {
        const host = await anAccount("Hosty", "host-sub");
        await openSession("GGGG-HHHJ", host);
        const alice = await anAccount("Alice", "alice-sub");
        const alicePsid = await psidFor(env, alice.id, SERVER);

        await put("/session/GGGG-HHHJ/turn/1", await issueSessionToken(env, host.id),
                  wrapBlob(1, "dHVybg"));
        await put(`/session/GGGG-HHHJ/orders/1/${alicePsid}`,
                  await issueSessionToken(env, alice.id), wrapBlob(1, "b3JkZXJz"));

        // A turn never changes, so spectators re-reading one should not cost a
        // DO request each time. Orders can still be revised.
        expect((await read("/session/GGGG-HHHJ/turn/1")).headers.get("cache-control"))
            .toContain("immutable");
        expect((await readAs(`/session/GGGG-HHHJ/orders/1/${alicePsid}`,
                             await issueSessionToken(env, alice.id))).headers.get("cache-control"))
            .toBe("no-store");
    });
});
