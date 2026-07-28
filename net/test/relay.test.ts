// The relay's own guarantees: who it lets in, what it tells the host about
// them, and what it refuses to carry.

import { env as testEnv } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import { clearKv, setupEnv } from "./helpers.js";
import { createAccount, identSubHash, type Account } from "../src/accounts/store.js";
import { issueJoinTicket, psidFor } from "../src/auth/ticket.js";
import { issueSessionDescriptor } from "../src/lobby/session.js";
import { FromHost, ToHost, type PeerIdentity, type SessionSettings } from "../src/lobby/LobbyDO.js";

let env: Env;

const SETTINGS: SessionSettings = {
    name: "Test game", listed: false, maxPlayers: 4,
    showBadges: true, requiredMods: [],
};

function lobby(code: string) {
    return testEnv.LOBBY.get(testEnv.LOBBY.idFromName(code));
}

/**
 * Open a session owned by `owner`.
 *
 * The owner's pseudonym is recorded at creation, which is what pins the host
 * slot to one account -- everyone invited knows the join code, so "first to
 * connect as host wins" would be no protection.
 */
async function openSession(
    code: string, owner: Account, serverId = "server-one", settings = SETTINGS,
) {
    const descriptor = await issueSessionDescriptor(env, code, serverId);
    const response = await lobby(code).fetch(new Request("https://lobby/init", {
        method: "POST",
        body: JSON.stringify({
            descriptor, settings, hostPsid: await psidFor(env, owner.id, serverId),
        }),
    }));
    expect(response.status).toBe(200);
    return { descriptor };
}

async function info(code: string): Promise<{ descriptor: string; nonce: string; players: number }> {
    const response = await lobby(code).fetch(new Request("https://lobby/info"));
    return response.json();
}

type Frame =
    | { type: "text"; text: string }
    | { type: "binary"; bytes: Uint8Array }
    | { type: "close"; code: number };

/**
 * A socket with every frame queued from the moment it opens.
 *
 * Attaching a listener only when a test is ready to read would race: the relay
 * can dispatch a frame during any `await` in between, and a dropped frame
 * looks exactly like a relay that never sent one. Buffering from connect makes
 * the assertions about what WAS sent rather than about timing.
 */
interface Peer {
    socket: WebSocket;
    next(timeoutMs?: number): Promise<Frame>;
}

function wrap(socket: WebSocket): Peer {
    const queued: Array<Promise<Frame>> = [];
    let waiting: ((frame: Promise<Frame>) => void) | null = null;

    const push = (frame: Promise<Frame>) => {
        if (waiting) { const w = waiting; waiting = null; w(frame); }
        else queued.push(frame);
    };

    socket.addEventListener("message", (event) => {
        const data = event.data as unknown;
        if (typeof data === "string") {
            push(Promise.resolve({ type: "text", text: data }));
        } else if (data instanceof ArrayBuffer) {
            push(Promise.resolve({ type: "binary", bytes: new Uint8Array(data) }));
        } else {
            // A Blob, which is what this runtime hands back for binary frames.
            // `new Uint8Array(blob)` yields an EMPTY array rather than
            // throwing, so getting this wrong looks exactly like a relay that
            // sent nothing -- worth handling explicitly rather than assuming.
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
                // A "nothing arrived" result, so a test that asserts silence
                // finishes instead of hanging.
                setTimeout(() => {
                    if (waiting === settle) { waiting = null; resolve({ type: "close", code: -1 }); }
                }, timeoutMs);
            });
        },
    };
}

async function connect(code: string, role: "host" | "player" | "spectator"): Promise<Peer> {
    const response = await lobby(code).fetch(new Request(`https://lobby/ws?role=${role}`, {
        headers: { Upgrade: "websocket" },
    }));
    expect(response.status).toBe(101);
    const socket = response.webSocket!;
    socket.accept();
    return wrap(socket);
}

async function anAccount(nick: string, sub: string): Promise<Account> {
    const created = await createAccount(
        env, "discord", await identSubHash(env, "discord", sub), nick,
    );
    if (!created.account) throw new Error(String(created.nickError));
    return created.account;
}

/** Join and complete the handshake, returning the socket. */
async function join(
    code: string, account: Account, role: "host" | "player", serverId = "server-one",
): Promise<Peer> {
    const { nonce } = await info(code);
    const ticket = await issueJoinTicket(env, account, code, serverId, {
        presentBadges: true, nonce,
    });
    const peer = await connect(code, role);
    peer.socket.send(JSON.stringify({ ticket }));
    return peer;
}

/** Create a host account and open a session it owns. */
async function hosted(
    code: string, serverId = "server-one", settings = SETTINGS,
): Promise<Account> {
    const owner = await anAccount("Hosty", "host-sub");
    await openSession(code, owner, serverId, settings);
    return owner;
}

beforeEach(async () => {
    env = await setupEnv();
    await clearKv(env);
});

describe("session setup", () => {
    it("hands out a descriptor and a fresh nonce each time", async () => {
        await hosted("AAAA-0001");
        const first = await info("AAAA-0001");
        const second = await info("AAAA-0001");

        expect(first.descriptor).toBe(second.descriptor);
        // A reused nonce would let a ticket be minted now and spent later.
        expect(first.nonce).not.toBe(second.nonce);
    });

    it("refuses to be initialised twice", async () => {
        await hosted("AAAA-0002");
        const descriptor = await issueSessionDescriptor(env, "AAAA-0002", "other-server");
        const again = await lobby("AAAA-0002").fetch(new Request("https://lobby/init", {
            method: "POST",
            body: JSON.stringify({ descriptor, settings: SETTINGS, hostPsid: "someone-else" }),
        }));
        expect(again.status).toBe(409);
    });
});

describe("ticket verification", () => {
    it("accepts a valid ticket and reports the peer id", async () => {
        const owner = await hosted("BBBB-0001");
        const host = await join("BBBB-0001", owner, "host");

        const frame = await host.next();
        expect(frame.type).toBe("text");
        expect(JSON.parse((frame as { text: string }).text))
            .toMatchObject({ ok: true, role: "host" });
    });

    it("refuses a ticket minted for a different session", async () => {
        await hosted("CCCC-0001");
        const account = await anAccount("Player", "p-sub");

        const { nonce } = await info("CCCC-0001");
        // Minted for another session, presented at this one.
        const wrong = await issueJoinTicket(env, account, "CCCC-0002", "server-one", {
            presentBadges: true, nonce,
        });
        const peer = await connect("CCCC-0001", "player");
        peer.socket.send(JSON.stringify({ ticket: wrong }));

        expect(await peer.next()).toMatchObject({ type: "close", code: 4401 });
    });

    it("refuses a ticket whose nonce this lobby never issued", async () => {
        await hosted("CCCC-0003");
        const account = await anAccount("Player", "p-sub");

        const forged = await issueJoinTicket(env, account, "CCCC-0003", "server-one", {
            presentBadges: true, nonce: "a-nonce-nobody-issued",
        });
        const peer = await connect("CCCC-0003", "player");
        peer.socket.send(JSON.stringify({ ticket: forged }));

        expect(await peer.next()).toMatchObject({ type: "close", code: 4401 });
    });

    it("refuses the same ticket twice, because the jti is burned", async () => {
        const owner = await hosted("DDDD-0001");
        const player = await anAccount("Player", "p-sub");
        await join("DDDD-0001", owner, "host");

        const { nonce } = await info("DDDD-0001");
        const ticket = await issueJoinTicket(env, player, "DDDD-0001", "server-one", {
            presentBadges: true, nonce,
        });

        const first = await connect("DDDD-0001", "player");
        first.socket.send(JSON.stringify({ ticket }));
        expect((await first.next()).type).toBe("text");

        // Replay. Even at the session it was minted for, and even inside its
        // 120-second life, a ticket is good exactly once.
        const second = await connect("DDDD-0001", "player");
        second.socket.send(JSON.stringify({ ticket }));
        expect(await second.next()).toMatchObject({ type: "close", code: 4401 });
    });

    it("disconnects anyone who sends binary before authenticating", async () => {
        await hosted("DDDD-0002");
        const peer = await connect("DDDD-0002", "player");
        peer.socket.send(new Uint8Array([1, 2, 3]));
        expect((await peer.next()).type).toBe("close");
    });
});

describe("the host slot", () => {
    it("belongs to the account that opened the session, not to whoever asks first", async () => {
        await hosted("GGGG-0001");
        // A player who knows the join code -- which is everyone invited --
        // tries to take the host slot before the real host connects.
        const impostor = await join("GGGG-0001", await anAccount("Sneaky", "s-sub"), "host");
        expect(await impostor.next()).toMatchObject({ type: "close", code: 4403 });
    });

    it("still admits that account as an ordinary player", async () => {
        const owner = await hosted("GGGG-0002");
        await join("GGGG-0002", owner, "host");

        const player = await join("GGGG-0002", await anAccount("Sneaky", "s-sub"), "player");
        expect((await player.next()).type).toBe("text");
    });
});

describe("what the host is told", () => {
    it("receives a pseudonym and a name, and never the ticket", async () => {
        const owner = await hosted("EEEE-0001");
        const playerAccount = await anAccount("Player", "p-sub");

        const host = await join("EEEE-0001", owner, "host");
        expect((await host.next()).type).toBe("text");        // its own ok frame

        await join("EEEE-0001", playerAccount, "player");

        const frame = await host.next();
        expect(frame.type).toBe("binary");
        const bytes = (frame as { bytes: Uint8Array }).bytes;
        expect(bytes[0]).toBe(ToHost.PeerJoined);

        const identity = JSON.parse(new TextDecoder().decode(bytes.subarray(3))) as PeerIdentity;
        expect(identity.name).toBe("Player");
        expect(identity.psid).toBe(await psidFor(env, playerAccount.id, "server-one"));

        // The things a hostile host must not end up holding.
        const asText = JSON.stringify(identity);
        expect(asText).not.toContain(playerAccount.id);
        expect(asText).not.toContain(playerAccount.identities[0]!.subHash);
        expect(identity).not.toHaveProperty("ticket");
    });

    it("hides badges entirely when the server has them switched off", async () => {
        const owner = await hosted("EEEE-0002", "server-one", { ...SETTINGS, showBadges: false });
        const badged = { ...await anAccount("Player", "p-sub"), badges: ["developer" as const] };

        const host = await join("EEEE-0002", owner, "host");
        await host.next();
        await join("EEEE-0002", badged, "player");

        const frame = await host.next();
        expect(frame.type).toBe("binary");
        const bytes = (frame as { bytes: Uint8Array }).bytes;
        const identity = JSON.parse(new TextDecoder().decode(bytes.subarray(3))) as PeerIdentity;
        expect(identity.badges).toEqual([]);
    });
});

describe("routing", () => {
    it("lets the host broadcast to every player", async () => {
        const owner = await hosted("FFFF-0001");
        const host = await join("FFFF-0001", owner, "host");
        await host.next();

        const player = await join("FFFF-0001", await anAccount("Player", "p-sub"), "player");
        await player.next();                     // ok frame
        await host.next();                       // PeerJoined

        const payload = new TextEncoder().encode("turn-delta");
        const framed = new Uint8Array(3 + payload.length);
        framed[0] = FromHost.Broadcast;
        framed.set(payload, 3);
        host.socket.send(framed);

        const got = await player.next();
        expect(got.type).toBe("binary");
        expect(new TextDecoder().decode((got as { bytes: Uint8Array }).bytes)).toBe("turn-delta");
    });

    it("gives a player no way to reach another player", async () => {
        const owner = await hosted("FFFF-0002");
        const host = await join("FFFF-0002", owner, "host");
        await host.next();

        const alice = await join("FFFF-0002", await anAccount("Alice", "a-sub"), "player");
        await alice.next();
        await host.next();

        const bob = await join("FFFF-0002", await anAccount("Bob", "b-sub"), "player");
        await bob.next();
        await host.next();

        // Alice sends something shaped exactly like a host broadcast. There is
        // no peer-to-peer path, so it can only ever reach the host -- which is
        // what makes the server authoritative in practice and not just on
        // paper.
        alice.socket.send(new Uint8Array([FromHost.Broadcast, 0, 0, 42]));

        const hostFrame = await host.next();
        expect(hostFrame.type).toBe("binary");
        expect((hostFrame as { bytes: Uint8Array }).bytes[0]).toBe(ToHost.Data);

        // Bob heard nothing.
        expect(await bob.next(300)).toMatchObject({ type: "close", code: -1 });
    });

    it("turns players away once the session is full", async () => {
        const owner = await hosted("FFFF-0004", "server-one", { ...SETTINGS, maxPlayers: 1 });
        const host = await join("FFFF-0004", owner, "host");
        await host.next();

        const first = await join("FFFF-0004", await anAccount("Alice", "a-sub"), "player");
        expect((await first.next()).type).toBe("text");

        const second = await join("FFFF-0004", await anAccount("Bob", "b-sub"), "player");
        expect(await second.next()).toMatchObject({ type: "close", code: 4409 });
    });
});
