// The primitives everything else is built from: keyed hashing, random ids and
// a constant-time compare.

import { b64urlEncode, utf8 } from "./encoding.js";

// Cached per isolate. importKey is not free and these keys are used on every
// login and every join; re-importing per request was measurable.
const hmacKeys = new Map<string, Promise<CryptoKey>>();

function hmacKey(secret: string): Promise<CryptoKey> {
    let k = hmacKeys.get(secret);
    if (!k) {
        k = crypto.subtle.importKey(
            "raw", utf8(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"],
        );
        hmacKeys.set(secret, k);
    }
    return k;
}

export async function hmac(secret: string, message: string): Promise<Uint8Array> {
    const sig = await crypto.subtle.sign("HMAC", await hmacKey(secret), utf8(message));
    return new Uint8Array(sig);
}

// A keyed hash rendered for storage or transport. `chars` truncates it: 22
// base64url characters is 132 bits, which is far past any birthday concern for
// the identifier spaces here and keeps KV keys short.
export async function hmacId(secret: string, message: string, chars = 43): Promise<string> {
    return b64urlEncode(await hmac(secret, message)).slice(0, chars);
}

export async function sha256(bytes: Uint8Array): Promise<Uint8Array> {
    return new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
}

// 256 bits of CSPRNG, base64url. Used for account ids, device codes, jti and
// nonces -- everything unguessable.
export function randomId(chars = 43): string {
    const b = new Uint8Array(32);
    crypto.getRandomValues(b);
    return b64urlEncode(b).slice(0, chars);
}

// The code a human reads off a screen and types into a browser. Deliberately
// short, and deliberately drawn from an alphabet with no 0/O/1/I/L, because it
// is transcribed by hand and a device code that is one glyph from another
// device code is a support problem, not a security one -- the security comes
// from the 256-bit device_code that never leaves the client.
const HUMAN_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

export function humanCode(len = 8): string {
    const b = new Uint8Array(len);
    crypto.getRandomValues(b);
    let out = "";
    for (let i = 0; i < len; i++) {
        if (i === len / 2) out += "-";
        out += HUMAN_ALPHABET[b[i]! % HUMAN_ALPHABET.length];
    }
    return out;
}

// Constant time in the length of `a`. Length inequality returns early, which
// leaks only the length -- and every secret compared here is fixed-length.
export function timingSafeEqual(a: string, b: string): boolean {
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
    return diff === 0;
}
