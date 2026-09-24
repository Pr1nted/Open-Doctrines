// A stand-in for the account service, for testing connectivity offline.
//
// WHY THIS EXISTS
//
// Every piece of the join is tested on its own -- the handshake against RFC
// 6455's vectors, ticket verification against RFC 8032's, the lobby rules
// without a socket in sight. What none of them covers is the SEAM: a real
// NetHost and a real NetSession actually finding each other, exchanging a
// challenge, and agreeing about who joined.
//
// Testing that against the deployed Worker would need somebody's real account
// token and a network. This needs neither: it signs with a throwaway Ed25519
// key it generates at startup and publishes at the same well-known URL the real
// service uses, so the game cannot tell the difference and nothing here can
// touch a real account.
//
// IT IS NOT A SECURITY BOUNDARY AND MUST NEVER BE ONE. It performs no checks:
// it hands a ticket to anyone who asks. That is the point -- the thing under
// test is the game's verification, not this.
//
// Usage:
//     node tests/mock_issuer.mjs --port 8787 [--wrong-key] [--delay MS]
//                                    [--delay-host MS]
//
// `--wrong-key` publishes a key that does NOT match the one it signs with, so a
// test can assert the game REFUSES those tickets. Without a negative case, a
// verifier that returns true unconditionally would pass the whole suite.

import { createServer } from "node:http";
import { webcrypto } from "node:crypto";

const args = process.argv.slice(2);
const portArg = args.indexOf("--port");
const PORT = portArg >= 0 ? Number(args[portArg + 1]) : 8787;
const WRONG_KEY = args.includes("--wrong-key");
// Pads the ticket so its size can be varied independently of anything else.
const padArg = args.indexOf("--pad");
const PAD = padArg >= 0 ? Number(args[padArg + 1]) : 0;
// Delays the ticket reply, to imitate a real network. This is not cosmetic: a
// join where the client took more than a few milliseconds to get its ticket
// used to stall forever, because the client's single socket thread sat in a
// blocking read and everything queued to send waited behind it. Only latency
// exposes that, so a test needs to be able to introduce some.
const delayArg = args.indexOf("--delay");
const DELAY = delayArg >= 0 ? Number(args[delayArg + 1]) : 0;

// Delays what a HOST asks for -- opening a session and fetching the
// verification key -- rather than what a joiner asks for. On a real network
// those take a few hundred milliseconds, and the game draws its lobby for the
// whole of that window: the seconds in which the lobby used to be filled in
// from a worker thread while the screen was reading it.
const hostDelayArg = args.indexOf("--delay-host");
const HOST_DELAY = hostDelayArg >= 0 ? Number(args[hostDelayArg + 1]) : 0;
const slowHost = () =>
    HOST_DELAY ? new Promise((r) => setTimeout(r, HOST_DELAY)) : Promise.resolve();

const b64url = (bytes) =>
    Buffer.from(bytes).toString("base64")
        .replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");

// The key everything is signed with, and the key that gets published. Normally
// the same one; under --wrong-key deliberately not.
const signingPair = await webcrypto.subtle.generateKey("Ed25519", true, ["sign", "verify"]);
const publishedPair = WRONG_KEY
    ? await webcrypto.subtle.generateKey("Ed25519", true, ["sign", "verify"])
    : signingPair;
const publishedJwk = await webcrypto.subtle.exportKey("jwk", publishedPair.publicKey);

// `let`, and reassigned once the socket is actually bound -- see the listen
// call at the bottom. With --port 0 the OS picks the port, and it is not
// known until then; every token this service signs carries ISSUER as its
// `iss`, so getting this wrong does not fail loudly, it mints tickets the
// host cannot verify. The handlers read this at request time and the ready
// line is printed after the reassignment, so nothing can observe the
// placeholder.
let ISSUER = `http://localhost:${PORT}`;
// SHAPED LIKE A REAL ONE: four, a dash, four, from the unambiguous alphabet in
// src/net/RelayLink.cpp (no I, L, O, 0 or 1). It used to be "TESTCODE", which
// no relay URL could be built from -- so a relayed host could not be tested at
// all, and the whole relay path went uncovered.
const CODE = "TEST-GAME";

// code -> { host, players: Map<peerId, conn>, relayFrames } for the stand-in relay
const relayRooms = new Map();

async function sign(claims) {
    const message = "od1." + b64url(new TextEncoder().encode(JSON.stringify(claims)));
    const sig = await webcrypto.subtle.sign(
        "Ed25519", signingPair.privateKey, new TextEncoder().encode(message));
    return message + "." + b64url(new Uint8Array(sig));
}

const now = () => Math.floor(Date.now() / 1000);

function json(res, body, status = 200) {
    const text = JSON.stringify(body);
    res.writeHead(status, {
        "content-type": "application/json",
        "content-length": Buffer.byteLength(text),
    });
    res.end(text);
}

function readBody(req) {
    return new Promise((resolve) => {
        let data = "";
        req.on("data", (c) => { data += c; });
        req.on("end", () => {
            try { resolve(JSON.parse(data || "{}")); } catch { resolve({}); }
        });
    });
}

// Each ticket gets its own name and pseudonym so a test can tell two joiners
// apart, and so "everyone ended up with the same seat" cannot pass by accident.
let issued = 0;

const server = createServer(async (req, res) => {
    const url = new URL(req.url, ISSUER);
    const path = url.pathname;

    // WHO IS ASKING.
    //
    // The real service derives a psid from the account behind the bearer. So
    // does this: a token of the form "dev-alice" is the player Alice, stably,
    // every time. That stability is the point -- a psid that changed per
    // request would make every rejoin look like a stranger, and seat memory
    // could never be tested at all.
    //
    // With no dev token the old behaviour is kept exactly: an incrementing
    // pseudonym per ticket, which is what the connectivity test expects.
    const bearer = String(req.headers["authorization"] || "")
        .replace(/^Bearer\s+/i, "");
    const devName = bearer.startsWith("dev-") ? bearer.slice(4) : "";
    const devPsid = devName ? `psid_dev_${devName}`.padEnd(24, "_") : "";
    const devLabel = devName
        ? devName.charAt(0).toUpperCase() + devName.slice(1)
        : "";

    // The verification key. This is the ONE endpoint that matters for whether
    // the game will believe anything below.
    if (req.method === "GET" && path === "/.well-known/od-keys.json") {
        await slowHost();
        return json(res, { keys: [publishedJwk], issuer: ISSUER });
    }

    // How many frames the stand-in relay has had from the host of `code`.
    // Only a test asks; the real service has no such thing.
    if (req.method === "GET" && path.startsWith("/relay-stats/")) {
        const room = relayRooms.get(path.slice("/relay-stats/".length));
        return json(res, { frames: room?.relayFrames ?? 0,
                           players: room ? room.players.size : 0 });
    }

    if (req.method === "POST" && path === "/server/register") {
        return json(res, { serverCredential: "mock-server-credential" });
    }

    // A host opening a session.
    if (req.method === "POST" && path === "/session") {
        await slowHost();
        return json(res, {
            code: CODE,
            hostPsid: devPsid || "psid_host_aaaaaaaaaaaa",
        });
    }

    // What a joiner asks about a session before minting a ticket. The
    // descriptor is opaque to the game; only we ever read it back.
    if (req.method === "GET" && path.startsWith("/session/")) {
        const code = path.slice("/session/".length);
        // A nonce too: a host opening THROUGH THE RELAY answers a challenge
        // here before it may connect, exactly as a joiner does.
        return json(res, { descriptor: `descriptor-for-${code}`, code,
                           nonce: "nonce-" + code });
    }

    // Mint a join ticket answering the host's challenge. No checks: the point
    // is to exercise the game's verification, not to perform any of our own.
    if (req.method === "POST" && path === "/ticket") {
        const body = await readBody(req);
        if (DELAY) await new Promise((r) => setTimeout(r, DELAY));
        const descriptor = String(body.descriptor || "");
        const code = descriptor.startsWith("descriptor-for-")
            ? descriptor.slice("descriptor-for-".length) : CODE;
        issued += 1;
        const ticket = await sign({
            iss: ISSUER + (PAD ? "/" + "x".repeat(PAD) : ""),
            aud: `od-relay:${code}`,
            psid: devPsid || `psid_player_${String(issued).padStart(4, "0")}______`,
            name: devLabel || `Tester${issued}`,
            badges: devName ? (devName === "alice" ? ["developer"] : [])
                            : (issued === 1 ? ["developer"] : []),
            nonce: String(body.nonce || ""),
            jti: `jti_${issued}_${now()}`,
            iat: now(),
            exp: now() + 120,
        });
        return json(res, { ticket });
    }

    if (req.method === "GET" && path === "/") {
        return json(res, {
            service: "mock-issuer",
            issuer: ISSUER,
            // Enough for the account screen to render. Nothing here can create
            // an account -- playtest clients arrive already signed in, with a
            // token written straight into account.json.
            providers: [{ id: "github", label: "GitHub", canCreate: true }],
            privacy: `${ISSUER}/privacy`,
            terms: `${ISSUER}/terms`,
            keys: `${ISSUER}/.well-known/od-keys.json`,
        });
    }

    // Who the bearer belongs to. The game asks this on startup and shows the
    // answer as the signed-in player.
    if (req.method === "GET" && path === "/account/me") {
        if (!devName) return json(res, { error: "no such session" }, 401);
        return json(res, {
            id: devPsid,
            nickname: devLabel,
            badges: devName === "alice" ? ["developer"] : [],
            providers: ["github"],
        });
    }

    if (req.method === "GET" && (path === "/privacy" || path === "/terms")) {
        res.writeHead(200, { "content-type": "text/markdown" });
        return res.end("# Local development issuer\n\nNot a real service.\n");
    }

    json(res, { error: "not_found" }, 404);
});

// ─────────────────────────────────────────────────────────────────────────────
// A RELAY, ENOUGH OF ONE TO TEST AGAINST
//
// Browser players reach a host through the account service's relay, and until
// now nothing exercised that path at all -- which is where three separate bugs
// were found by reading rather than by failing: the host fanned a broadcast out
// as one frame per player and tripped the relay's own rate limit, it checked
// its snapshot against the wrong ceiling, and a refusal from the relay never
// reached the player.
//
// This is not the real relay (net/src/lobby/LobbyDO.ts). It seats whoever
// asks, keeps no state worth the name, and enforces nothing. What it does is
// speak the same wire format in both directions, so the game's relay code can
// be driven end to end on one machine.
const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const ToHost = { Data: 1, PeerJoined: 2, PeerLeft: 3 };
const FromHost = { ToPeer: 1, Broadcast: 2, Kick: 3, Ban: 4 };

/** One socket, framed. Handles fragments and control frames; masks nothing. */
function wsWrap(socket) {
    const self = { socket, buf: Buffer.alloc(0), asm: null, asmOp: 0, onText: null, onBinary: null, onClose: null };
    socket.on("data", (chunk) => {
        self.buf = Buffer.concat([self.buf, chunk]);
        for (;;) {
            if (self.buf.length < 2) return;
            const b0 = self.buf[0], b1 = self.buf[1];
            const fin = (b0 & 0x80) !== 0, op = b0 & 0x0f, masked = (b1 & 0x80) !== 0;
            let len = b1 & 0x7f, at = 2;
            if (len === 126) { if (self.buf.length < 4) return; len = self.buf.readUInt16BE(2); at = 4; }
            else if (len === 127) { if (self.buf.length < 10) return; len = Number(self.buf.readBigUInt64BE(2)); at = 10; }
            const maskAt = at;
            if (masked) at += 4;
            if (self.buf.length < at + len) return;
            let payload = self.buf.subarray(at, at + len);
            if (masked) {
                const m = self.buf.subarray(maskAt, maskAt + 4);
                payload = Buffer.from(payload);
                for (let i = 0; i < payload.length; i++) payload[i] ^= m[i & 3];
            } else payload = Buffer.from(payload);
            self.buf = self.buf.subarray(at + len);

            if (op === 0x8) { self.onClose?.(); socket.end(); return; }
            if (op === 0x9) { wsSend(self, 0xA, payload); continue; }
            if (op === 0xA) continue;
            if (op === 0x0) self.asm = Buffer.concat([self.asm ?? Buffer.alloc(0), payload]);
            else { self.asm = payload; self.asmOp = op; }
            if (!fin) continue;
            const whole = self.asm; self.asm = null;
            if (self.asmOp === 0x1) self.onText?.(whole.toString("utf8"));
            else self.onBinary?.(whole);
        }
    });
    socket.on("error", () => self.onClose?.());
    socket.on("close", () => self.onClose?.());
    return self;
}

function wsSend(conn, opcode, payload) {
    const n = payload.length;
    const head = n < 126 ? Buffer.from([0x80 | opcode, n])
        : n <= 0xffff ? Buffer.concat([Buffer.from([0x80 | opcode, 126]), (() => { const b = Buffer.alloc(2); b.writeUInt16BE(n); return b; })()])
        : Buffer.concat([Buffer.from([0x80 | opcode, 127]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(n)); return b; })()]);
    try { conn.socket.write(Buffer.concat([head, payload])); } catch { /* gone */ }
}


server.on("upgrade", async (req, socket) => {
    const url = new URL(req.url, "http://localhost");
    const m = url.pathname.match(/^\/session\/([A-Za-z0-9-]+)\/ws$/);
    const key = req.headers["sec-websocket-key"];
    if (!m || !key) { socket.destroy(); return; }
    const role = url.searchParams.get("role") === "host" ? "host" : "player";
    const code = m[1];

    const digest = await webcrypto.subtle.digest("SHA-1", Buffer.from(key + WS_GUID));
    socket.write("HTTP/1.1 101 Switching Protocols\r\n" +
                 "Upgrade: websocket\r\nConnection: Upgrade\r\n" +
                 "Sec-WebSocket-Accept: " + Buffer.from(digest).toString("base64") + "\r\n\r\n");

    const conn = wsWrap(socket);
    const room = relayRooms.get(code) ?? { host: null, players: new Map(), nextPeer: 1 };
    relayRooms.set(code, room);

    conn.onText = (text) => {
        // The HELLO. Anything is accepted: this stands in for the relay's
        // authentication, and what is under test is everything after it.
        if (role === "host") {
            room.host = conn;
            // peerId 0 for the host: parseHelloReply wants the field present
            // whichever side is asking.
            wsSend(conn, 0x1, Buffer.from(JSON.stringify({ ok: true, peerId: 0, role: "host" })));
            return;
        }
        const peerId = room.nextPeer++;
        conn.peerId = peerId;
        room.players.set(peerId, conn);
        wsSend(conn, 0x1, Buffer.from(JSON.stringify({ ok: true, peerId, role: "player" })));
        if (room.host) {
            const head = Buffer.from([ToHost.PeerJoined, peerId & 0xff, (peerId >> 8) & 0xff]);
            wsSend(room.host, 0x2, Buffer.concat([head, Buffer.from(JSON.stringify({ psid: "psid-relay-" + peerId, name: "Relayed " + peerId, issuer: ISSUER }))]));
        }
    };

    conn.onBinary = (data) => {
        if (role === "host") {
            if (data.length < 3) return;
            const kind = data[0], target = data[1] | (data[2] << 8);
            const payload = data.subarray(3);
            room.relayFrames = (room.relayFrames ?? 0) + 1;
            if (kind === FromHost.Broadcast) {
                for (const p of room.players.values()) wsSend(p, 0x2, payload);
            } else if (kind === FromHost.ToPeer) {
                const p = room.players.get(target);
                if (p) wsSend(p, 0x2, payload);
            } else if (kind === FromHost.Kick || kind === FromHost.Ban) {
                const p = room.players.get(target);
                if (p) { room.players.delete(target); p.socket.end(); }
            }
            return;
        }
        if (!room.host) return;
        const id = conn.peerId ?? 0;
        const head = Buffer.from([ToHost.Data, id & 0xff, (id >> 8) & 0xff]);
        wsSend(room.host, 0x2, Buffer.concat([head, data]));
    };

    conn.onClose = () => {
        if (role === "host") { if (room.host === conn) room.host = null; return; }
        const id = conn.peerId ?? 0;
        if (room.players.delete(id) && room.host) {
            const head = Buffer.from([ToHost.PeerLeft, id & 0xff, (id >> 8) & 0xff]);
            wsSend(room.host, 0x2, head);
        }
    };
});

server.listen(PORT, "127.0.0.1", () => {
    // PORT may be 0, meaning "whatever is free" -- which is the whole point:
    // the process that BINDS the port is now the one that chooses it, so there
    // is no window between picking and binding for anything else to take it.
    ISSUER = `http://localhost:${server.address().port}`;
    // The test harness waits for this line before starting the game side, and
    // reads the port back out of it.
    console.log(`mock-issuer ready on ${ISSUER}${WRONG_KEY ? " (publishing a WRONG key)" : ""}`);
});
