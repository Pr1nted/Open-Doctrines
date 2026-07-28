// Access, portability and erasure.
//
// GDPR Articles 15, 17 and 20, and the CCPA/CPRA rights to know and to delete.
// They are one small file because the data is one small object -- which is the
// argument for collecting little in the first place.
//
// EXPORT returns every field we hold, including the ones a normal API response
// hides. `subHash` is in there: it is a keyed hash of a provider user id, it is
// data about the person, and Article 15 is "what do you hold", not "what do
// you usually show". The raw provider id is not in there because we never had
// it -- we hashed it on the way in and threw the original away.
//
// DELETE is two steps. The first returns a signed confirmation token and a
// plain statement of what is about to go; the second spends it. Nothing is
// stored between the two: the token IS the state.
//
// WHAT DELETION CANNOT REACH, and we say so rather than bury it: a game server
// is an independent controller of whatever it recorded -- a psid, a display
// name, chat. We do not have it and cannot erase it. See PRIVACY.md.

import type { Env } from "../env.js";
import { sign, verify } from "../auth/token.js";
import {
    deleteAccount, TOMBSTONE_SECONDS, type Account,
} from "./store.js";

export const AUD_DELETE = "od-delete";
export const DELETE_CONFIRM_TTL = 300;

interface DeleteClaims {
    iss: string;
    aud: typeof AUD_DELETE;
    sub: string;
    iat: number;
    exp: number;
}

/**
 * Everything held about this account, in the storage layout rather than a
 * prettified one -- a portability export that hides the shape is not much of
 * an export.
 */
export function exportAccount(env: Env, account: Account): Record<string, unknown> {
    return {
        exportedAt: new Date().toISOString(),
        issuer: env.ISSUER,
        note:
            "This is everything opendoctrines-net stores about your account. " +
            "We never collected your email address, your real name, your avatar, " +
            "your IP address or a login history. Game servers you have joined are " +
            "separate operators and may hold their own records; we cannot see or " +
            "delete those.",
        account: {
            id: account.id,
            nickname: account.nick,
            nicknameMatchForm: account.nickNorm,
            createdAt: new Date(account.created * 1000).toISOString(),
            nicknameLastChangedAt: new Date(account.nickChangedAt * 1000).toISOString(),
            badges: account.badges,
            linkedIdentities: account.identities.map((i) => ({
                provider: i.provider,
                // Keyed hash of your user id at that provider. We store this
                // instead of the id itself so that a copy of our database
                // cannot be matched against any other service.
                subjectHash: i.subHash,
                linkedAt: new Date(i.linkedAt * 1000).toISOString(),
            })),
        },
    };
}

export function issueDeleteConfirmation(
    env: Env, account: Account, now = Math.floor(Date.now() / 1000),
): Promise<string> {
    const claims: DeleteClaims = {
        iss: env.ISSUER, aud: AUD_DELETE, sub: account.id,
        iat: now, exp: now + DELETE_CONFIRM_TTL,
    };
    return sign(env, claims);
}

export function describeDeletion(account: Account): Record<string, unknown> {
    return {
        willDelete: [
            `your account (${account.nick})`,
            `${account.identities.length} linked sign-in ${account.identities.length === 1 ? "method" : "methods"}`,
            account.badges.length ? `your badges (${account.badges.join(", ")})` : null,
        ].filter(Boolean),
        willKeep: [
            "Your nickname stays unclaimable for 30 days so nobody can take it " +
            "and speak as you. After that it is released. The record kept is the " +
            "nickname and a date, with nothing linking it to you.",
        ],
        cannotReach: [
            "Game servers you joined are run by other people and are separate " +
            "data controllers. Anything they recorded is theirs to delete, not ours.",
        ],
        tombstoneDays: TOMBSTONE_SECONDS / 86400,
        irreversible: true,
    };
}

export type DeleteResult = { ok: true } | { ok: false; reason: "bad_confirmation" };

export async function confirmDeletion(
    env: Env, account: Account, confirmation: string,
): Promise<DeleteResult> {
    const claims = await verify<DeleteClaims>(env, confirmation, AUD_DELETE);
    // Bound to this account as well as merely valid: a confirmation minted for
    // one account must not delete another.
    if (!claims || claims.sub !== account.id) return { ok: false, reason: "bad_confirmation" };
    await deleteAccount(env, account);
    return { ok: true };
}
