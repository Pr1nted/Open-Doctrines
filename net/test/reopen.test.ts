// Reopening a session under the code it already has.
//
// A dedicated host that restarts used to be handed a new join code, which broke
// every player's saved server entry and orphaned the long-form turn store kept
// under the old one. And the descriptor expires after a day, so a host running
// a month-long tournament stopped admitting anybody on day two. `reopen` is the
// answer to both, and what is under test is that it answers them without
// handing a code to anybody but the server that opened it.

import { env as testEnv, runInDurableObject, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import { clearKv, setupEnv } from "./helpers.js";
import { createAccount, identSubHash, type Account } from "../src/accounts/store.js";
import { psidFor } from "../src/auth/ticket.js";
import { issueSessionToken } from "../src/auth/token.js";
import {
    issueServerCredential, issueSessionDescriptor, SESSION_TTL, verifyServerCredential,
    verifySessionDescriptor,
} from "../src/lobby/session.js";
import { FromHost, type SessionSettings } from "../src/lobby/LobbyDO.js";

let env: Env;

const ORIGIN = "https://od.test.invalid";

interface Opened {
    code: string;
    descriptor: string;
    wsUrl: string;
    hostPsid: string;
    issuer: string;
    settings: SessionSettings;
    reopened?: boolean;
    reopenRefused?: string;
}

/** A host: an account, a server it registered, and a token to act with. */
interface Host { account: Account; token: string; credential: string; srv: string; }

function lobby(code: string) {
    return testEnv.LOBBY.get(testEnv.LOBBY.idFromName(code));
}

async function anAccount(nick: string, sub: string): Promise<Account> {
    const created = await createAccount(
        env, "discord", await identSubHash(env, "discord", sub), nick,
    );
    if (!created.account) throw new Error(String(created.nickError));
    return created.account;
}

async function aHost(nick: string, sub: string): Promise<Host> {
    const account = await anAccount(nick, sub);
    const credential = await issueServerCredential(env, account.id);
    const claims = await verifyServerCredential(env, credential);
    return {
        account, credential, srv: claims!.srv,
        token: await issueSessionToken(env, account.id),
    };
}

/** POST /session exactly as Host.cpp makes it, plus `reopen` when given. */
async function open(
    host: Host, settings: Partial<SessionSettings> = {}, reopen?: unknown,
): Promise<Opened> {
    const response = await SELF.fetch(`${ORIGIN}/session`, {
        method: "POST",
        headers: { authorization: `Bearer ${host.token}`, "content-type": "application/json" },
        body: JSON.stringify({
            serverCredential: host.credential,
            settings: { name: "Tournament", maxPlayers: 8, ...settings },
            ...(reopen === undefined ? {} : { reopen }),
        }),
    });
    expect(response.status).toBe(200);
    return response.json();
}

async function info(code: string): Promise<{
    descriptor: string; nonce: string; name: string; maxPlayers: number;
}> {
    const response = await SELF.fetch(`${ORIGIN}/session/${code}`);
    expect(response.status).toBe(200);
    return response.json();
}

function wrapBlob(turn: number, data: string): string {
    return JSON.stringify({ od: 1, turn, data });
}

type Frame =
    | { type: "text"; text: string }
    | { type: "binary"; bytes: Uint8Array }
    | { type: "close"; code: number };

interface Peer { socket: WebSocket; next(timeoutMs?: number): Promise<Frame>; }

/** Every frame queued from the moment the socket opens; see relay.test.ts. */
function wrap(socket: WebSocket): Peer {
    const queued: Array<Promise<Frame>> = [];
    let waiting: ((frame: Promise<Frame>) => void) | null = null;
    const push = (frame: Promise<Frame>) => {
        if (waiting) { const w = waiting; waiting = null; w(frame); }
        else queued.push(frame);
    };
    socket.addEventListener("message", (event) => {
        const data = event.data as unknown;
        if (typeof data === "string") push(Promise.resolve({ type: "text", text: data }));
        else if (data instanceof ArrayBuffer) {
            push(Promise.resolve({ type: "binary", bytes: new Uint8Array(data) }));
        } else {
            push((data as Blob).arrayBuffer()
                .then((buffer) => ({ type: "binary", bytes: new Uint8Array(buffer) }) as Frame));
        }
    });
    socket.addEventListener("close", (event) => push(
        Promise.resolve({ type: "close", code: event.code }),
    ));
    return {
        socket,
        next(timeoutMs = 2000) {
            const queuedFrame = queued.shift();
            if (queuedFrame) return queuedFrame;
            return new Promise<Frame>((resolve) => {
                const settle = (frame: Promise<Frame>) => { void frame.then(resolve); };
                waiting = settle;
                setTimeout(() => {
                    if (waiting === settle) { waiting = null; resolve({ type: "close", code: -1 }); }
                }, timeoutMs);
            });
        },
    };
}

/**
 * Join the way a real client does: read the descriptor from GET /session,
 * trade it for a ticket at /ticket, present the ticket to the relay. Going
 * through /ticket is the point -- it is what refuses an expired descriptor.
 */
async function join(code: string, account: Account, role: "host" | "player"): Promise<Peer> {
    const { descriptor, nonce } = await info(code);
    const minted = await SELF.fetch(`${ORIGIN}/ticket`, {
        method: "POST",
        headers: {
            authorization: `Bearer ${await issueSessionToken(env, account.id)}`,
            "content-type": "application/json",
        },
        body: JSON.stringify({ descriptor, nonce }),
    });
    expect(minted.status).toBe(200);
    const { ticket } = await minted.json<{ ticket: string }>();

    const response = await lobby(code).fetch(new Request(`https://lobby/ws?role=${role}`, {
        headers: { Upgrade: "websocket" },
    }));
    expect(response.status).toBe(101);
    const socket = response.webSocket!;
    socket.accept();
    const peer = wrap(socket);
    socket.send(JSON.stringify({ ticket }));
    return peer;
}

/** Run the alarm now, optionally winding the object's state first. */
async function inLobby(code: string, fn: (instance: any) => Promise<void> | void): Promise<void> {
    const run = runInDurableObject as unknown as (
        stub: unknown, fn: (instance: any) => Promise<void>) => Promise<void>;
    await run(lobby(code), async (instance: any) => { await fn(instance); });
}

beforeEach(async () => {
    env = await setupEnv();
    await clearKv(env);
});

describe("reopening a session", () => {
    it("keeps the code and renews a descriptor that was about to expire", async () => {
        const host = await aHost("Hosty", "host-sub");

        // A session opened twenty-three hours ago, one hour from refusing joins.
        const code = "RRRR-AAAA";
        const old = Math.floor(Date.now() / 1000) - 23 * 60 * 60;
        const init = await lobby(code).fetch(new Request("https://lobby/init", {
            method: "POST",
            body: JSON.stringify({
                descriptor: await issueSessionDescriptor(env, code, host.srv, old),
                settings: { name: "Old", listed: false, maxPlayers: 8, showBadges: true,
                            longForm: true, requiredMods: [] },
                hostPsid: await psidFor(env, host.account.id, host.srv),
                srv: host.srv,
            }),
        }));
        expect(init.status).toBe(200);

        const renewed = await open(host, { longForm: true }, code);
        expect(renewed).toMatchObject({
            code, reopened: true, wsUrl: `wss://test.invalid/session/${code}/ws`,
            hostPsid: await psidFor(env, host.account.id, host.srv),
            issuer: "https://test.invalid",
        });
        expect(renewed).not.toHaveProperty("reopenRefused");

        const claims = await verifySessionDescriptor(env, renewed.descriptor);
        expect(claims).toMatchObject({ sid: code, srv: host.srv });
        expect(claims!.exp).toBeGreaterThan(old + SESSION_TTL);

        // And it is the one players are now handed, so their tickets are
        // minted against a descriptor with a day left on it.
        expect((await info(code)).descriptor).toBe(renewed.descriptor);
    });

    it("replaces the settings", async () => {
        const host = await aHost("Hosty", "host-sub");
        const first = await open(host, { name: "Week one", maxPlayers: 8 });

        await open(host, { name: "Week two", maxPlayers: 12 }, first.code);

        expect(await info(first.code)).toMatchObject({ name: "Week two", maxPlayers: 12 });
    });

    it("keeps every turn and every order the session has stored", async () => {
        const host = await aHost("Hosty", "host-sub");
        const first = await open(host, { longForm: true });

        const put = await SELF.fetch(`${ORIGIN}/session/${first.code}/turn/1`, {
            method: "PUT", headers: { authorization: `Bearer ${host.token}` },
            body: wrapBlob(1, "dHVybi1vbmU"),
        });
        expect(put.status).toBe(200);

        await open(host, { longForm: true }, first.code);

        const got = await SELF.fetch(`${ORIGIN}/session/${first.code}/turn/1`);
        expect(got.status).toBe(200);
        expect(await got.json()).toMatchObject({ data: "dHVybi1vbmU" });

        // And the host can still publish the next one: the store still knows
        // who the host is.
        const next = await SELF.fetch(`${ORIGIN}/session/${first.code}/turn/2`, {
            method: "PUT", headers: { authorization: `Bearer ${host.token}` },
            body: wrapBlob(2, "dHVybi10d28"),
        });
        expect(next.status).toBe(200);
    });

    it("disconnects nobody, so a running host can renew under its players", async () => {
        const host = await aHost("Hosty", "host-sub");
        const first = await open(host);

        const hostPeer = await join(first.code, host.account, "host");
        expect(await hostPeer.next()).toMatchObject({ type: "text" });
        const player = await join(first.code, await anAccount("Alice", "a-sub"), "player");
        expect(await player.next()).toMatchObject({ type: "text" });
        expect((await hostPeer.next()).type).toBe("binary");     // PeerJoined

        // The twelve-hourly renewal the C++ host makes while a game is live.
        await open(host, {}, first.code);

        expect(await player.next(300)).toMatchObject({ type: "close", code: -1 });
        expect(await hostPeer.next(300)).toMatchObject({ type: "close", code: -1 });

        const payload = new TextEncoder().encode("still-here");
        const framed = new Uint8Array(3 + payload.length);
        framed[0] = FromHost.Broadcast;
        framed.set(payload, 3);
        hostPeer.socket.send(framed);

        const got = await player.next();
        expect(got.type).toBe("binary");
        expect(new TextDecoder().decode((got as { bytes: Uint8Array }).bytes)).toBe("still-here");
    });

    it("lets the same account back into the host slot after a restart", async () => {
        const host = await aHost("Hosty", "host-sub");
        const first = await open(host);

        const before = await join(first.code, host.account, "host");
        expect(await before.next()).toMatchObject({ type: "text" });
        before.socket.close();            // the dedicated server goes down
        await new Promise((r) => setTimeout(r, 50));

        const reopened = await open(host, {}, first.code);
        expect(reopened.code).toBe(first.code);
        expect(reopened.hostPsid).toBe(first.hostPsid);

        // Back on the relay through the ordinary path, against the renewed
        // descriptor, and recognised as the host rather than refused with 4403.
        const after = await join(first.code, host.account, "host");
        const frame = await after.next();
        expect(frame.type).toBe("text");
        expect(JSON.parse((frame as { text: string }).text)).toMatchObject({ ok: true, role: "host" });
    });

    it("restarts the host's grace under the settings it was reopened with", async () => {
        const host = await aHost("Hosty", "host-sub");
        const first = await open(host);              // rapid

        const hostPeer = await join(first.code, host.account, "host");
        expect(await hostPeer.next()).toMatchObject({ type: "text" });
        hostPeer.socket.close();
        await new Promise((r) => setTimeout(r, 50));

        // Reopened as long-form: the ninety seconds it left with must not be
        // the deadline it is held to now.
        await open(host, { longForm: true }, first.code);

        await inLobby(first.code, (instance) => {
            expect(Number(instance.get("hostGraceMs"))).toBe(90 * 24 * 60 * 60 * 1000);
        });
        // Wound past the rapid grace, the session is still there.
        await inLobby(first.code, async (instance) => {
            instance.set("hostGoneAt", String(Date.now() - 10 * 60 * 1000));
            await instance.alarm();
        });
        expect((await SELF.fetch(`${ORIGIN}/session/${first.code}`)).status).toBe(200);
    });

    it("reopens a session opened before the server id was stored", async () => {
        const host = await aHost("Hosty", "host-sub");
        const code = "RRRR-BBBB";
        // /init as it was called before `srv` was part of the body. The
        // descriptor still names the server, and that is what is checked.
        await lobby(code).fetch(new Request("https://lobby/init", {
            method: "POST",
            body: JSON.stringify({
                descriptor: await issueSessionDescriptor(env, code, host.srv),
                settings: { name: "Old", listed: false, maxPlayers: 8, showBadges: true,
                            requiredMods: [] },
                hostPsid: await psidFor(env, host.account.id, host.srv),
            }),
        }));
        await inLobby(code, (instance) => { instance.sql.exec("DELETE FROM meta WHERE k = 'srv'"); });

        expect(await open(host, {}, code)).toMatchObject({ code, reopened: true });
    });

    it("is not asked for when the field is absent, null or empty", async () => {
        const host = await aHost("Hosty", "host-sub");
        for (const reopen of [undefined, null, ""]) {
            const opened = await open(host, {}, reopen);
            expect(opened).not.toHaveProperty("reopened");
            expect(opened).not.toHaveProperty("reopenRefused");
        }
    });
});

// ── NOBODY ELSE'S SERVER GETS THE CODE ──
//
// A join code is public to everyone invited, so knowing one proves nothing.
// If reopen took a code on that basis, any operator could take over a running
// tournament's code -- and with it the players' saved entries -- by asking.
describe("a reopen that is refused", () => {
    it("issues another account's server a fresh code and leaves the session alone", async () => {
        const owner = await aHost("Hosty", "host-sub");
        const first = await open(owner, { name: "Owner's game" });

        const squatter = await aHost("Squat", "squat-sub");
        const attempt = await open(squatter, { name: "Mine now" }, first.code);

        expect(attempt.code).not.toBe(first.code);
        expect(attempt).toMatchObject({ reopened: false, reopenRefused: "not_your_session" });
        // A working session all the same, for the server that asked.
        expect((await verifySessionDescriptor(env, attempt.descriptor))!.srv).toBe(squatter.srv);

        expect(await info(first.code)).toMatchObject({
            descriptor: first.descriptor, name: "Owner's game",
        });
    });

    it("refuses the same account under a different server credential", async () => {
        // Registering again mints a new srv, and with it every player's
        // pseudonym. A session belongs to a server, not to an account.
        const host = await aHost("Hosty", "host-sub");
        const first = await open(host);

        const second = await issueServerCredential(env, host.account.id);
        const attempt = await open({
            ...host, credential: second, srv: (await verifyServerCredential(env, second))!.srv,
        }, {}, first.code);

        expect(attempt).toMatchObject({ reopened: false, reopenRefused: "not_your_session" });
        expect(attempt.code).not.toBe(first.code);
    });

    it("issues a fresh code for a session that no longer exists", async () => {
        const host = await aHost("Hosty", "host-sub");
        const attempt = await open(host, {}, "RRRR-CCCC");

        expect(attempt).toMatchObject({ reopened: false, reopenRefused: "no_session" });
        expect(attempt.code).not.toBe("RRRR-CCCC");
        // The probe left nothing behind.
        expect((await SELF.fetch(`${ORIGIN}/session/RRRR-CCCC`)).status).toBe(404);
    });

    it("never reaches the lobby for a code that could not have been issued", async () => {
        const host = await aHost("Hosty", "host-sub");
        for (const bad of ["ABCD-EFG0", "not a code", 42]) {
            expect(await open(host, {}, bad))
                .toMatchObject({ reopened: false, reopenRefused: "bad_code" });
        }
    });

    it("still requires the credential's owner", async () => {
        const owner = await aHost("Hosty", "host-sub");
        const first = await open(owner);
        const other = await aHost("Other", "other-sub");

        // Someone else's token with the owner's credential: refused outright,
        // exactly as a plain open is.
        const response = await SELF.fetch(`${ORIGIN}/session`, {
            method: "POST",
            headers: { authorization: `Bearer ${other.token}`, "content-type": "application/json" },
            body: JSON.stringify({ serverCredential: owner.credential, reopen: first.code }),
        });
        expect(response.status).toBe(403);
    });
});

// ── LONG-FORM OUTLIVES ITS HOST ──
//
// The whole of long-form is a host that is away between turns. Held to the
// rapid ninety seconds, a tournament would be deleted when the host closed
// their laptop.
describe("a long-form session whose host has gone", () => {
    it("keeps its players, its code and its turns past the rapid grace", async () => {
        const host = await aHost("Hosty", "host-sub");
        const first = await open(host, { longForm: true });
        expect(first.settings.longForm).toBe(true);

        const put = await SELF.fetch(`${ORIGIN}/session/${first.code}/turn/1`, {
            method: "PUT", headers: { authorization: `Bearer ${host.token}` },
            body: wrapBlob(1, "a2VlcA"),
        });
        expect(put.status).toBe(200);

        const hostPeer = await join(first.code, host.account, "host");
        expect(await hostPeer.next()).toMatchObject({ type: "text" });
        const player = await join(first.code, await anAccount("Alice", "a-sub"), "player");
        expect(await player.next()).toMatchObject({ type: "text" });
        hostPeer.socket.close();
        await new Promise((r) => setTimeout(r, 50));

        await inLobby(first.code, async (instance) => {
            expect(Number(instance.get("hostGraceMs"))).toBe(90 * 24 * 60 * 60 * 1000);
            // Ten minutes gone: a rapid game would have ended long ago.
            instance.set("hostGoneAt", String(Date.now() - 10 * 60 * 1000));
            await instance.alarm();
        });

        expect(await player.next(300)).toMatchObject({ type: "close", code: -1 });
        expect((await SELF.fetch(`${ORIGIN}/session/${first.code}`)).status).toBe(200);
        expect((await SELF.fetch(`${ORIGIN}/session/${first.code}/turn/1`)).status).toBe(200);
    });

    it("still keeps the session and its storage once the idle grace has run out", async () => {
        const host = await aHost("Hosty", "host-sub");
        const first = await open(host, { longForm: true });
        await SELF.fetch(`${ORIGIN}/session/${first.code}/turn/1`, {
            method: "PUT", headers: { authorization: `Bearer ${host.token}` },
            body: wrapBlob(1, "a2VlcA"),
        });

        const hostPeer = await join(first.code, host.account, "host");
        expect(await hostPeer.next()).toMatchObject({ type: "text" });
        const player = await join(first.code, await anAccount("Alice", "a-sub"), "player");
        expect(await player.next()).toMatchObject({ type: "text" });
        hostPeer.socket.close();
        await new Promise((r) => setTimeout(r, 50));

        await inLobby(first.code, async (instance) => {
            instance.set("hostGoneAt", String(Date.now() - 91 * 24 * 60 * 60 * 1000));
            await instance.alarm();
        });

        // Whoever was waiting is let go -- there is no host to be connected
        // to -- but the tournament is still there for the host to reopen.
        expect(await player.next()).toMatchObject({ type: "close", code: 4404 });
        expect((await SELF.fetch(`${ORIGIN}/session/${first.code}/turn/1`)).status).toBe(200);
        expect(await open(host, { longForm: true }, first.code))
            .toMatchObject({ code: first.code, reopened: true });
    });
});
