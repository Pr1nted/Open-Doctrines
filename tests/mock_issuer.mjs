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

const ISSUER = `http://localhost:${PORT}`;
const CODE = "TESTCODE";

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

    // The verification key. This is the ONE endpoint that matters for whether
    // the game will believe anything below.
    if (req.method === "GET" && path === "/.well-known/od-keys.json") {
        return json(res, { keys: [publishedJwk], issuer: ISSUER });
    }

    if (req.method === "POST" && path === "/server/register") {
        return json(res, { serverCredential: "mock-server-credential" });
    }

    // A host opening a session.
    if (req.method === "POST" && path === "/session") {
        return json(res, { code: CODE, hostPsid: "psid_host_aaaaaaaaaaaa" });
    }

    // What a joiner asks about a session before minting a ticket. The
    // descriptor is opaque to the game; only we ever read it back.
    if (req.method === "GET" && path.startsWith("/session/")) {
        const code = path.slice("/session/".length);
        return json(res, { descriptor: `descriptor-for-${code}`, code });
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
            psid: `psid_player_${String(issued).padStart(4, "0")}______`,
            name: `Tester${issued}`,
            badges: issued === 1 ? ["developer"] : [],
            nonce: String(body.nonce || ""),
            jti: `jti_${issued}_${now()}`,
            iat: now(),
            exp: now() + 120,
        });
        return json(res, { ticket });
    }

    if (req.method === "GET" && path === "/") {
        return json(res, { service: "mock-issuer", issuer: ISSUER });
    }

    json(res, { error: "not_found" }, 404);
});

server.listen(PORT, "127.0.0.1", () => {
    // The test harness waits for this line before starting the game side.
    console.log(`mock-issuer ready on ${ISSUER}${WRONG_KEY ? " (publishing a WRONG key)" : ""}`);
});
