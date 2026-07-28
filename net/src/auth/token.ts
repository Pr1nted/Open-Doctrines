// Ed25519-signed tokens, and the split between the two kinds that everything
// else in the privacy design rests on.
//
//   SESSION TOKEN   aud "od-api".  The account credential. It NEVER leaves the
//                   client -- no relay, no game server and no other player ever
//                   sees one. It is the only thing that can call /account/*.
//
//   JOIN TICKET     aud "od-relay:<sessionId>".  Minted per join, 120 seconds,
//                   single use. Carries a per-server pseudonym and a display
//                   name, and NO account id, NO provider identity, NO email.
//
// The audience check is what makes the split real: a ticket presented to the
// account API fails, because `od-relay:...` is not `od-api`. So even a game
// host who somehow captured a ticket holds something that cannot act as the
// player anywhere, expires in two minutes, and is bound to one session it has
// already been spent on.
//
// Why our own compact format rather than JWT: a JWT header carries `alg`, and
// every historic JWT vulnerability is downstream of a verifier trusting it.
// Here the algorithm is not a field. It is Ed25519 because that is what the
// code does, and there is no way to say otherwise.

import type { Env } from "../env.js";
import { b64urlDecode, b64urlEncode, b64urlJson, fromUtf8, utf8 } from "../util/encoding.js";

export const TOKEN_PREFIX = "od1";

/** Audience of the account-API credential. */
export const AUD_API = "od-api";

/** Audience of a join ticket for one relay session. */
export function audRelay(sessionId: string): string {
    return `od-relay:${sessionId}`;
}

export const SESSION_TOKEN_TTL = 12 * 60 * 60;

// Two minutes. Long enough to survive a slow connect and a clock a little out
// of step; short enough that a captured ticket is worthless before anyone
// could act on it.
export const TICKET_TTL = 120;

export interface SessionClaims {
    iss: string;
    aud: typeof AUD_API;
    sub: string;          // account id
    iat: number;
    exp: number;
}

export interface TicketClaims {
    iss: string;
    aud: string;          // od-relay:<sessionId>
    psid: string;         // pairwise pseudonym; see ticket.ts
    name: string;         // display name, global nickname or a per-server alias
    badges: string[];     // may be empty because the PLAYER chose to withhold them
    nonce: string;        // the relay's challenge, echoed back
    jti: string;          // burned by the relay on first use
    iat: number;
    exp: number;
}

// Workers name the curve "Ed25519"; older runtimes and some test harnesses
// only know "NODE-ED25519". Try the standard name and fall back, rather than
// pinning to whichever one happens to work today.
type EdAlgorithm = string | { name: string; namedCurve: string };

const ED_ALGORITHMS: EdAlgorithm[] = [
    "Ed25519",
    // Older workerd. It needs the object form -- given the bare string it
    // fails with 'Missing field "namedCurve"', so a string fallback here
    // would never actually work.
    { name: "NODE-ED25519", namedCurve: "NODE-ED25519" },
];

// The algorithm identifier travels with the key, because sign() and verify()
// must be handed the same one the import accepted -- mixing them fails.
interface EdKey { key: CryptoKey; algorithm: EdAlgorithm }

/**
 * Reduce a JWK to the fields that define the key.
 *
 * THIS IS NOT TIDINESS. Node's `exportKey` stamps `alg: "Ed25519"` onto an
 * Ed25519 JWK; workerd rejects exactly that with
 *
 *     JSON Web Key Algorithm parameter "alg" ("Ed25519") does not match
 *     requested Ed25519 curve.
 *
 * Since the documented way to generate these keys is `node -e ...`, importing
 * one verbatim fails on every deployment -- and it fails by returning null
 * from verify(), so the visible symptom is "that sign-in expired" for every
 * user forever, with nothing in any log to say why.
 *
 * `key_ops` and `ext` are dropped for the same class of reason: they are
 * assertions made by whichever runtime exported the key, and they can conflict
 * with the usage being requested here. What actually defines an Ed25519 key is
 * kty, crv, x and (privately) d.
 */
function canonicalJwk(raw: string): JsonWebKey {
    const parsed = JSON.parse(raw) as JsonWebKey;
    const jwk: JsonWebKey = { kty: parsed.kty, crv: parsed.crv, x: parsed.x };
    if (parsed.d !== undefined) jwk.d = parsed.d;
    return jwk;
}

async function importKey(jwk: string, usage: "sign" | "verify"): Promise<EdKey> {
    const parsed = canonicalJwk(jwk);
    let lastError: unknown;
    for (const algorithm of ED_ALGORITHMS) {
        try {
            const key = await crypto.subtle.importKey(
                "jwk", parsed, algorithm as unknown as string, false, [usage],
            );
            return { key, algorithm };
        } catch (e) { lastError = e; }
    }
    throw lastError instanceof Error ? lastError : new Error("Ed25519 unavailable");
}

// Cached per isolate: importKey on every request is wasted work on a hot path.
let signingKey: Promise<EdKey> | null = null;
let verifyingKey: Promise<EdKey> | null = null;

function privateKey(env: Env): Promise<EdKey> {
    return (signingKey ??= importKey(env.ED25519_PRIVATE_KEY, "sign"));
}

function publicKey(env: Env): Promise<EdKey> {
    return (verifyingKey ??= importKey(env.ED25519_PUBLIC_JWK, "verify"));
}

/** Test seam: drop the isolate-level key cache. */
export function resetKeyCache(): void {
    signingKey = null;
    verifyingKey = null;
}

export async function sign(env: Env, claims: object): Promise<string> {
    const message = `${TOKEN_PREFIX}.${b64urlJson(claims)}`;
    const { key, algorithm } = await privateKey(env);
    const sig = await crypto.subtle.sign(algorithm as unknown as string, key, utf8(message));
    return `${message}.${b64urlEncode(new Uint8Array(sig))}`;
}

// Returns null for every failure -- malformed, wrong signature, wrong
// audience, expired. Callers must not distinguish these to the client: telling
// an attacker WHICH check failed turns one oracle into four.
export async function verify<T extends { aud: string; exp: number; iss: string }>(
    env: Env, token: string, expectedAud: string, now = Math.floor(Date.now() / 1000),
): Promise<T | null> {
    const parts = token.split(".");
    if (parts.length !== 3 || parts[0] !== TOKEN_PREFIX) return null;

    let ok: boolean;
    let claims: T;
    try {
        const { key, algorithm } = await publicKey(env);
        ok = await crypto.subtle.verify(
            algorithm as unknown as string, key,
            b64urlDecode(parts[2]!), utf8(`${parts[0]}.${parts[1]}`),
        );
        claims = JSON.parse(fromUtf8(b64urlDecode(parts[1]!))) as T;
    } catch {
        return null;
    }
    if (!ok) return null;

    if (claims.iss !== env.ISSUER) return null;
    // Exact match, never a prefix or `startsWith`. A ticket for
    // "od-relay:ABC" must not satisfy a check for "od-relay:AB".
    if (claims.aud !== expectedAud) return null;
    if (typeof claims.exp !== "number" || claims.exp <= now) return null;
    return claims;
}

export async function issueSessionToken(
    env: Env, accountId: string, now = Math.floor(Date.now() / 1000),
): Promise<string> {
    const claims: SessionClaims = {
        iss: env.ISSUER,
        aud: AUD_API,
        sub: accountId,
        iat: now,
        exp: now + SESSION_TOKEN_TTL,
    };
    return sign(env, claims);
}

export function verifySessionToken(env: Env, token: string): Promise<SessionClaims | null> {
    return verify<SessionClaims>(env, token, AUD_API);
}
