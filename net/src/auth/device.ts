// The device flow: how a game that is not a web browser gets a login.
//
// The client makes a secret, opens a browser at a URL derived from it, and
// polls until the browser half finishes. It is RFC 8628 in shape, with two
// deliberate departures:
//
// 1. NO SEPARATE user_code. RFC 8628 has the user type a short code because it
//    assumes the device cannot open a browser -- a TV, a console. Our client
//    can, so it uses the "verification_uri_complete" form and the player never
//    types anything.
//
// 2. NO SERVER STATE UNTIL THE LOGIN SUCCEEDS. The usual implementation writes
//    a pending record on /auth/device, another for the user code, and another
//    for the OAuth state: three KV writes before the player has even seen the
//    consent screen. Against a 1,000 writes/day free tier that would cap us at
//    ~300 login ATTEMPTS a day, most of which people abandon. So instead:
//
//      request_id  = SHA-256(device_secret)   -- public, safe to put in a URL
//      auth request = a token WE signed        -- carries request_id, no storage
//      pkce verifier = HMAC(IDENT_KEY, request_id) -- recomputed, never stored
//
//    Exactly one KV write happens, at the end, when there is a result to hand
//    back. An abandoned login costs nothing.
//
// WHY request_id IS A HASH OF THE SECRET
//
// The browser URL is the one part of this that leaks -- history, screen
// sharing, a shoulder. It carries request_id. Polling requires device_secret.
// Since request_id is its hash, seeing the URL does not let anyone collect the
// result.
//
// WHAT THIS STILL DOES NOT STOP, and how we surface it: someone who can read
// the victim's verification URL can run their own login against that
// request_id, and the victim's client would receive a session for the
// ATTACKER'S account. That is inherent to the device flow. We do not pretend
// otherwise -- the browser shows which account was signed in, and the client
// shows the nickname it received and asks the player to confirm it is theirs.

import type { Env } from "../env.js";
import { b64urlEncode } from "../util/encoding.js";
import { hmacId, sha256 } from "../util/crypto.js";
import { utf8 } from "../util/encoding.js";
import { sign, verify } from "./token.js";
import type { ProviderId } from "./providers.js";

/** Audience of the short-lived token that authorises one browser round trip. */
export const AUD_AUTHREQ = "od-authreq";

/** Audience of the token that lets a brand-new user pick a nickname and commit. */
export const AUD_SIGNUP = "od-signup";

export const AUTHREQ_TTL = 10 * 60;
export const SIGNUP_TTL = 10 * 60;

/** How long a completed result waits in KV for the client to collect it. */
export const RESULT_TTL = 300;

export type AuthPurpose = "login" | "link";

export interface AuthRequestClaims {
    iss: string;
    aud: typeof AUD_AUTHREQ;
    rid: string;               // request id = base64url(SHA-256(device_secret))
    provider: ProviderId;
    purpose: AuthPurpose;
    /** For purpose "link": the account the new identity attaches to. */
    link?: string;
    iat: number;
    exp: number;
}

export interface SignupClaims {
    iss: string;
    aud: typeof AUD_SIGNUP;
    provider: ProviderId;
    subHash: string;
    /** A name to prefill, from the provider. The player may replace it. */
    suggested?: string;
    iat: number;
    exp: number;
}

export async function requestIdFor(deviceSecret: string): Promise<string> {
    return b64urlEncode(await sha256(utf8(deviceSecret)));
}

export function issueAuthRequest(
    env: Env, rid: string, provider: ProviderId, purpose: AuthPurpose, link?: string,
    now = Math.floor(Date.now() / 1000),
): Promise<string> {
    const claims: AuthRequestClaims = {
        iss: env.ISSUER, aud: AUD_AUTHREQ, rid, provider, purpose,
        ...(link ? { link } : {}),
        iat: now, exp: now + AUTHREQ_TTL,
    };
    return sign(env, claims);
}

export function verifyAuthRequest(env: Env, token: string): Promise<AuthRequestClaims | null> {
    return verify<AuthRequestClaims>(env, token, AUD_AUTHREQ);
}

export function issueSignupTicket(
    env: Env, provider: ProviderId, subHash: string, suggested: string | null,
    now = Math.floor(Date.now() / 1000),
): Promise<string> {
    const claims: SignupClaims = {
        iss: env.ISSUER, aud: AUD_SIGNUP, provider, subHash,
        ...(suggested ? { suggested } : {}),
        iat: now, exp: now + SIGNUP_TTL,
    };
    return sign(env, claims);
}

export function verifySignupTicket(env: Env, token: string): Promise<SignupClaims | null> {
    return verify<SignupClaims>(env, token, AUD_SIGNUP);
}

// ------------------------------------------------------------------ PKCE ----
//
// Derived, not stored. The verifier must survive from /auth/verify (where the
// challenge is computed) to /auth/callback (where the verifier is sent), and
// the usual way to do that is a KV row per attempt. Deriving it from a server
// secret and the request id gets the same guarantee for free: the browser
// never sees the verifier, and nobody without IDENT_KEY can produce it.

export async function pkceVerifier(env: Env, rid: string): Promise<string> {
    return hmacId(env.IDENT_KEY, `pkce:${rid}`, 43);
}

export async function pkceChallenge(verifier: string): Promise<string> {
    return b64urlEncode(await sha256(utf8(verifier)));
}

// ---------------------------------------------------------------- result ----

export type AuthResult =
    | { kind: "session"; token: string; account: Record<string, unknown> }
    | { kind: "signup"; ticket: string; suggested?: string }
    | { kind: "linked"; provider: ProviderId; account: Record<string, unknown> }
    | { kind: "error"; code: string; message: string };

const resultKey = (rid: string) => `res:${rid}`;

/** The one KV write in the whole login path. */
export async function storeResult(env: Env, rid: string, result: AuthResult): Promise<void> {
    await env.OD_ACCOUNTS.put(resultKey(rid), JSON.stringify(result), {
        expirationTtl: RESULT_TTL,
    });
}

/**
 * Collect a result.
 *
 * Deliberately does NOT delete after reading. A delete counts against the free
 * plan's separate 1,000/day delete budget, and buys little: reading requires
 * the device_secret, which only the client that started the flow has, and the
 * row expires in five minutes regardless.
 */
export async function readResult(env: Env, rid: string): Promise<AuthResult | null> {
    return (await env.OD_ACCOUNTS.get(resultKey(rid), "json")) as AuthResult | null;
}
