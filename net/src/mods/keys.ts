// Publish keys: how a release pipeline updates a listing without a browser.
//
// THE PROBLEM
//
// Publishing needs a session token, and a session token comes from a device
// flow that opens a consent screen. That is right for a person and useless in
// CI: an author who cuts a release every fortnight should not have to visit a
// web form to retype a hash their build already computed.
//
// A THIRD KIND OF CREDENTIAL, AND DELIBERATELY THE NARROWEST
//
// auth/token.ts is explicit that the session token "is the only thing that can
// call /account/*", and that the audience split is what makes that true. A key
// that could be swapped in for a session token would quietly undo it -- so this
// is not a session token with a longer life. It is a different thing that
// `authenticate()` never returns and that only the publish path resolves.
//
// What a key can do:      publish or update a listing, and read its own.
// What a key cannot do:   withdraw a listing, agree to the guidelines, touch
//                         the account, mint a join ticket, or reach anything
//                         under /moderation -- INCLUDING when the account
//                         behind it carries the developer badge.
//
// Withdrawing is left out on purpose. It is the one irreversible thing an
// author can do to their own work (the download counts go with it), automating
// it buys nobody anything, and a leaked key that can only ever ADD is a much
// smaller problem than one that can delete.
//
// AGREEING IS STILL A PERSON'S ACT. A key cannot accept the guidelines. If they
// change, the pipeline starts failing with a message saying so, and a human
// goes and reads them. That is the correct failure: consent that a script can
// give on your behalf is not consent.
//
// WHAT IS STORED: a keyed hash, never the key. A copy of this database does not
// let anybody publish as anyone.

import type { Env } from "../env.js";
import type { Account } from "../accounts/store.js";
import { getAccount } from "../accounts/store.js";
import { hmacId, randomId } from "../util/crypto.js";
import { bearer } from "../http.js";

/**
 * The prefix, and it is load-bearing rather than decorative.
 *
 * Session tokens start `od1`. A publish key that could be mistaken for one --
 * in a log, in a support question, in `verifySessionToken` -- is the failure
 * this prevents. It also makes a leaked key recognisable to secret scanners.
 */
export const KEY_PREFIX = "odmp_";

/** Keys one account may hold at once. A flood ceiling, not a quality bar. */
export const MAX_KEYS = 10;

export const LABEL_MAX = 60;

export interface PublishKey {
    /** The keyed hash. The key itself is never stored, here or anywhere. */
    hash: string;
    accountId: string;
    /** What the author called it: "GitHub Actions", "release box". */
    label: string;
    created: number;
}

const keyRecord = (hash: string) => `pkg:key:${hash}`;
const keyOwned = (accountId: string, hash: string) => `pkg:keyown:${accountId}:${hash}`;

/** The stored form of a presented key. Never reversible. */
function hashOf(env: Env, token: string): Promise<string> {
    return hmacId(env.IDENT_KEY, `pkgkey:${token}`, 43);
}

export interface Minted {
    /** Shown ONCE. We cannot show it again, because we do not keep it. */
    token: string;
    key: PublishKey;
}

export async function mint(
    env: Env, account: Account, label: string, now = Math.floor(Date.now() / 1000),
): Promise<Minted | { ok: false; status: number; code: string; message: string }> {
    const held = await env.OD_ACCOUNTS.list({
        prefix: `pkg:keyown:${account.id}:`, limit: MAX_KEYS + 1,
    });
    if (held.keys.length >= MAX_KEYS) {
        return {
            ok: false, status: 429, code: "too_many_keys",
            message: `An account may hold ${MAX_KEYS} publish keys. Revoke one first.`,
        };
    }

    const token = KEY_PREFIX + randomId(43);
    const hash = await hashOf(env, token);
    const key: PublishKey = {
        hash, accountId: account.id,
        label: label.slice(0, LABEL_MAX) || "unnamed",
        created: now,
    };
    await env.OD_ACCOUNTS.put(keyRecord(hash), JSON.stringify(key));
    await env.OD_ACCOUNTS.put(keyOwned(account.id, hash), key.label);
    return { token, key };
}

export async function list(env: Env, accountId: string): Promise<PublishKey[]> {
    const owned = await env.OD_ACCOUNTS.list({
        prefix: `pkg:keyown:${accountId}:`, limit: MAX_KEYS + 1,
    });
    const out: PublishKey[] = [];
    for (const k of owned.keys) {
        const hash = k.name.slice(`pkg:keyown:${accountId}:`.length);
        const raw = await env.OD_ACCOUNTS.get(keyRecord(hash));
        if (!raw) continue;
        try { out.push(JSON.parse(raw) as PublishKey); } catch { /* skip a corrupt one */ }
    }
    out.sort((a, b) => b.created - a.created);
    return out;
}

/**
 * Revoke by hash, which is what the author's own list shows them.
 *
 * The ownership check is not decoration: the hash is the storage key, so
 * without it anybody who learned one could revoke somebody else's pipeline.
 */
export async function revoke(env: Env, accountId: string, hash: string): Promise<boolean> {
    const raw = await env.OD_ACCOUNTS.get(keyRecord(hash));
    if (!raw) return false;
    let key: PublishKey;
    try { key = JSON.parse(raw) as PublishKey; } catch { return false; }
    if (key.accountId !== accountId) return false;

    await env.OD_ACCOUNTS.delete(keyRecord(hash));
    await env.OD_ACCOUNTS.delete(keyOwned(accountId, hash));
    return true;
}

/** Does this look like a publish key at all? Cheap, and no storage touched. */
export function looksLikeKey(token: string | null): boolean {
    return !!token && token.startsWith(KEY_PREFIX);
}

/**
 * Resolve the account behind a presented publish key.
 *
 * Returns null for anything that is not a live key, indistinguishably -- a
 * wrong key and a revoked one are the same answer.
 *
 * NOTE WHAT THIS IS NOT: it is not `authenticate()`, and nothing that gates an
 * account action may call it. The only callers are the publish path and the
 * author's own listing read.
 */
export async function publisher(request: Request, env: Env): Promise<Account | null> {
    const token = bearer(request);
    if (!looksLikeKey(token)) return null;
    const raw = await env.OD_ACCOUNTS.get(keyRecord(await hashOf(env, token!)));
    if (!raw) return null;
    let key: PublishKey;
    try { key = JSON.parse(raw) as PublishKey; } catch { return null; }
    return getAccount(env, key.accountId);
}

/** What a key looks like to its owner. Never includes anything reversible. */
export function publicKey(k: PublishKey) {
    return { id: k.hash, label: k.label, created: k.created };
}
