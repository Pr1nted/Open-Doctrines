// Server identity and session descriptors.
//
// WHY A SERVER NEEDS AN IDENTITY AT ALL
//
// The per-player pseudonym is HMAC(key, accountId + ":" + serverId), so
// `serverId` decides which pseudonym a player wears. If a host could simply
// assert its own serverId, a second operator could claim the first one's and
// immediately see the same pseudonyms -- which is exactly the cross-server
// correlation the pairwise scheme exists to prevent.
//
// So serverId is minted by us and handed back inside a token WE signed. A host
// keeps that credential in its config and presents it to open a session. It
// proves ownership without us storing anything: the signature is the record.
//
// A DESCRIPTOR is the public half. It names the session and the server behind
// it, signed, so a joining client can hand it to /ticket and we can compute the
// right pseudonym without a lookup -- and without a host being able to lie
// about which server it is.

import type { Env } from "../env.js";
import { sign, verify } from "../auth/token.js";
import { humanCode, randomId } from "../util/crypto.js";

export const AUD_SERVER_CRED = "od-server-cred";
export const AUD_SESSION = "od-session";

/** Sessions are short-lived; a descriptor outliving the game is pointless risk. */
export const SESSION_TTL = 24 * 60 * 60;

/**
 * A server credential is configuration, not a session: it lives in a config
 * file, and a host that had to re-register every day would simply script
 * around it. So it is long-lived rather than exempt from expiry -- every token
 * in this system has an `exp`, and one special case that skips the check is
 * how a verifier ends up with a branch an attacker can aim at.
 */
export const SERVER_CRED_TTL = 10 * 365 * 24 * 60 * 60;

export interface ServerCredentialClaims {
    iss: string;
    aud: typeof AUD_SERVER_CRED;
    srv: string;          // the stable server id
    owner: string;        // account that registered it
    iat: number;
    exp: number;
}

export interface SessionDescriptorClaims {
    iss: string;
    aud: typeof AUD_SESSION;
    sid: string;
    srv: string;
    iat: number;
    exp: number;
}

export function issueServerCredential(
    env: Env, ownerAccountId: string, now = Math.floor(Date.now() / 1000),
): Promise<string> {
    const claims: ServerCredentialClaims = {
        iss: env.ISSUER, aud: AUD_SERVER_CRED,
        srv: randomId(22), owner: ownerAccountId,
        iat: now, exp: now + SERVER_CRED_TTL,
    };
    return sign(env, claims);
}

export function verifyServerCredential(
    env: Env, token: string,
): Promise<ServerCredentialClaims | null> {
    return verify<ServerCredentialClaims>(env, token, AUD_SERVER_CRED);
}

export function newSessionCode(): string {
    return humanCode(8);
}

export function issueSessionDescriptor(
    env: Env, sessionId: string, serverId: string, now = Math.floor(Date.now() / 1000),
): Promise<string> {
    const claims: SessionDescriptorClaims = {
        iss: env.ISSUER, aud: AUD_SESSION, sid: sessionId, srv: serverId,
        iat: now, exp: now + SESSION_TTL,
    };
    return sign(env, claims);
}

export function verifySessionDescriptor(
    env: Env, descriptor: string,
): Promise<SessionDescriptorClaims | null> {
    return verify<SessionDescriptorClaims>(env, descriptor, AUD_SESSION);
}
