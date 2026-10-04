// Achievement grants: the only thing that makes an achievement mean anything.
//
// WHAT A GRANT IS
//
// A grant is a statement by this service, signed with a key nobody else holds:
// "account <sub> earned achievement <ach> at <iat>". The game and the launcher
// verify it offline against a public key baked into their builds, so a grant
// can travel anywhere -- inside a .odstate, across a reinstall, onto a second
// machine -- and still be checked without calling home.
//
// What the game writes locally when a player earns something is NOT a grant.
// It is a claim, and a claim is only a request: it becomes an achievement when
// claim.ts has looked at it and this file has signed it. Editing a file, or
// hand-building a .odstate, therefore produces at most a claim -- and a claim
// still has to get past the clock and the budget in claim.ts.
//
// A KEY OF ITS OWN
//
// Not the session-token key. That one is rotated whenever a credential leaks
// (net/rotate-signing-key.sh), and rotating it is meant to invalidate every
// token in existence. An achievement is the opposite kind of thing: it should
// still verify in ten years. Sharing a key would force a choice between
// "a leak cannot be revoked" and "rotating erases every achievement ever
// earned", so the two are separate secrets with separate lifetimes. When this
// key does have to change, the old public key stays in the game's list
// (src/achievements/AchievementKeys.h) so grants already issued keep verifying.
//
// FORMAT
//
//   oda1.<base64url(JSON claims)>.<base64url(Ed25519 signature)>
//
// The signature covers "oda1.<payload>" exactly as transmitted, so a verifier
// never re-serialises JSON (the source of half of all signature bugs). The
// algorithm is not a field, for the reason given in auth/token.ts.

import type { Env } from "../env.js";
import { b64urlDecode, b64urlEncode, b64urlJson, fromUtf8, utf8 } from "../util/encoding.js";

export const GRANT_PREFIX = "oda1";
export const AUD_GRANT = "od-achievement";

/** Audience of the play-session clock ticket. See claim.ts. */
export const AUD_PLAY = "od-play";

/** A play ticket is good for a long sitting, not for ever. */
export const PLAY_TTL = 36 * 60 * 60;

export interface GrantClaims {
    iss: string;
    aud: typeof AUD_GRANT;
    sub: string;          // account id
    ach: string;          // achievement id, from the catalog
    iat: number;          // when it was granted, unix seconds
    /**
     * Earned while the client's rule tables did not match a known release
     * (od_t4, see third_party/odseal/odseal.h). Not a refusal: a modded game is
     * a legitimate game. The mark is shown, so a player comparing collections
     * can tell a stock achievement from one earned under different rules.
     */
    mod?: 1;
    /** SHA-256 of the evidence the client sent, hex, for audit. */
    ev?: string;
}

export interface PlayClaims {
    iss: string;
    aud: typeof AUD_PLAY;
    sub: string;
    iat: number;
    exp: number;
    seal: string;         // od_t4 word as the client reported it, hex
    build: string;        // client version string
}

type EdAlgorithm = string | { name: string; namedCurve: string };
const ED_ALGORITHMS: EdAlgorithm[] = [
    "Ed25519",
    { name: "NODE-ED25519", namedCurve: "NODE-ED25519" },
];
interface EdKey { key: CryptoKey; algorithm: EdAlgorithm }

/** Same canonicalisation as auth/token.ts, for the same workerd reason. */
function canonicalJwk(raw: string): JsonWebKey {
    const parsed = JSON.parse(raw) as JsonWebKey;
    const jwk: JsonWebKey = { kty: parsed.kty, crv: parsed.crv, x: parsed.x };
    if (parsed.d !== undefined) jwk.d = parsed.d;
    return jwk;
}

async function importKey(jwk: string, usage: "sign" | "verify"): Promise<EdKey> {
    let lastError: unknown;
    for (const algorithm of ED_ALGORITHMS) {
        try {
            const key = await crypto.subtle.importKey(
                "jwk", canonicalJwk(jwk), algorithm as unknown as string, false, [usage]);
            return { key, algorithm };
        } catch (e) { lastError = e; }
    }
    throw lastError instanceof Error ? lastError : new Error("Ed25519 unavailable");
}

let grantKey: Promise<EdKey> | null = null;
let grantPub: Promise<EdKey> | null = null;

/** Test seam. */
export function resetGrantKeyCache(): void { grantKey = null; grantPub = null; }

/** Whether this deployment can sign grants at all. A fork without the secret
 *  still runs; it simply has no achievements service. */
export function grantsConfigured(env: Env): boolean {
    return !!env.ACHIEVEMENT_PRIVATE_KEY && !!env.ACHIEVEMENT_PUBLIC_JWK;
}

export async function signGrant(env: Env, claims: GrantClaims): Promise<string> {
    const message = `${GRANT_PREFIX}.${b64urlJson(claims)}`;
    const { key, algorithm } = await (grantKey ??= importKey(env.ACHIEVEMENT_PRIVATE_KEY!, "sign"));
    const sig = await crypto.subtle.sign(algorithm as unknown as string, key, utf8(message));
    return `${message}.${b64urlEncode(new Uint8Array(sig))}`;
}

/** null for every failure, undistinguished, as in auth/token.ts. */
export async function verifyGrant(env: Env, token: string): Promise<GrantClaims | null> {
    const parts = token.split(".");
    if (parts.length !== 3 || parts[0] !== GRANT_PREFIX) return null;
    try {
        const { key, algorithm } = await (grantPub ??= importKey(env.ACHIEVEMENT_PUBLIC_JWK!, "verify"));
        const ok = await crypto.subtle.verify(algorithm as unknown as string, key,
            b64urlDecode(parts[2]!), utf8(`${parts[0]}.${parts[1]}`));
        if (!ok) return null;
        const claims = JSON.parse(fromUtf8(b64urlDecode(parts[1]!))) as GrantClaims;
        if (claims.iss !== env.ISSUER || claims.aud !== AUD_GRANT) return null;
        return claims;
    } catch {
        return null;
    }
}

/** The raw 32-byte public key, base64url -- what the C++ verifier takes. */
export function grantPublicKeyRaw(env: Env): string {
    const jwk = JSON.parse(env.ACHIEVEMENT_PUBLIC_JWK!) as JsonWebKey;
    return jwk.x ?? "";
}
