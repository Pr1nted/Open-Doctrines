// The KV schema, and every read and write that touches it.
//
// WHAT AN ACCOUNT ACTUALLY CONTAINS
//
//   a keyed hash per linked provider, a nickname the player chose, any badges,
//   and two timestamps.
//
// That is the whole of it. No email, no avatar, no real name, no IP, no login
// history. It is short because PRIVACY.md has to list it, and a list you would
// rather not publish is a design smell.
//
// WHY PROVIDER IDS ARE HASHED
//
// `ident:` keys hold HMAC(IDENT_KEY, "provider:sub"), not the raw subject.
// Lookup still works -- we recompute the hash from the id the provider just
// handed us -- but a leaked KV export contains no Google, Discord or GitHub
// user ids, so it cannot be joined against anything else. The cost is that
// IDENT_KEY can never be rotated without orphaning every login; see README.
//
// THE WRITE BUDGET IS A DESIGN CONSTRAINT
//
// Workers KV allows 1,000 writes/day on the free plan. Account creation costs
// three (acct, ident, nick), so ~330 signups/day. Logging in costs ZERO -- no
// lastSeen, no counters, no touch. That is both a budget decision and a
// privacy one, and it is why nothing here writes on a read path.

import type { Env } from "../env.js";
import type { ProviderId } from "../auth/providers.js";
import { hmacId, randomId } from "../util/crypto.js";
import { canonicalize, normalize } from "./nickname.js";

export type Badge = "developer" | "playtester";
export const BADGES: Badge[] = ["developer", "playtester"];

export function isBadge(v: string): v is Badge {
    return (BADGES as string[]).includes(v);
}

/**
 * A provider subject that has already been through `identSubHash`.
 *
 * Branded so the compiler refuses a raw provider id anywhere a hash is
 * expected. That is not ceremony: the two are both strings, both come from the
 * same variable name three lines apart, and storing the raw one by mistake
 * would silently undo the reason for hashing at all.
 */
export type SubjectHash = string & { readonly __subjectHash: unique symbol };

export interface LinkedIdentity {
    provider: ProviderId;
    subHash: SubjectHash;
    linkedAt: number;
}

export interface Account {
    id: string;
    nick: string;          // canonical form, as displayed
    nickNorm: string;      // matching form; the uniqueness key
    created: number;       // unix seconds
    nickChangedAt: number;
    badges: Badge[];
    identities: LinkedIdentity[];

    /**
     * Set when this account may not join any game.
     *
     * Deliberately a field on the account rather than a separate `ban:` key:
     * /ticket already loads the account to mint from it, so checking here costs
     * no extra read on the join path. A ban list that made every join more
     * expensive would be a ban list somebody eventually switched off.
     *
     * Servers are never told a ban exists. The account service simply declines
     * to mint a ticket, so pairwise pseudonymity is untouched -- no server
     * learns who was banned, or that anyone was.
     */
    banned?: {
        reason: string;
        at: number;
        /**
         * Unix seconds when the ban lifts. Absent means permanent.
         *
         * Stored rather than derived because a ban's length is a decision, and
         * a player is owed the actual date rather than "indefinitely". It is
         * checked at ticket-minting, so an expired ban simply stops applying
         * without anything having to sweep the store.
         */
        until?: number;
    };
}

/**
 * A nickname may be changed once per this window.
 *
 * Short enough that a regretted name is not a life sentence, long enough that
 * changing name is not a way to shed a reputation every afternoon. It is NOT
 * the same as the tombstone below, which is about a name nobody owns any more.
 */
export const NICK_COOLDOWN_SECONDS = 7 * 24 * 60 * 60;

/**
 * How long a deleted account's nickname stays unclaimable.
 *
 * Releasing it immediately would let anyone take the name of a player who just
 * left and speak as them. The tombstone holds a normalized string and a date
 * and nothing else -- with the account gone there is nothing to tie it back to
 * a person, so retaining it is not retaining personal data.
 */
export const TOMBSTONE_SECONDS = 30 * 24 * 60 * 60;

const acctKey = (id: string) => `acct:${id}`;
const nickKey = (norm: string) => `nick:${norm}`;
const tombKey = (norm: string) => `tomb:${norm}`;

/**
 * A provider identity that may not create a new account.
 *
 * Written only when a BANNED account is deleted. Without it, "delete my account
 * and sign up again" would clear a ban in two clicks, which would make the ban
 * list decorative.
 *
 * This is a keyed hash of a provider user id and nothing else -- no name, no
 * nickname, no account id. It is retained under legitimate interests (abuse
 * prevention, GDPR Art. 6(1)(f) and the Art. 17(3) carve-out), it expires, and
 * PRIVACY.md says so. Deletion still happens: what survives is a hash that
 * cannot be turned back into a person by anyone, including us.
 */
const banIdentKey = (provider: ProviderId, subHash: SubjectHash) =>
    `banident:${provider}:${subHash}`;

/** How long a banned account's provider identities stay blocked from signup. */
export const BAN_IDENTITY_SECONDS = 365 * 24 * 60 * 60;

/** The only way to produce a SubjectHash, and therefore the only way in. */
export async function identSubHash(
    env: Env, provider: ProviderId, sub: string,
): Promise<SubjectHash> {
    return await hmacId(env.IDENT_KEY, `${provider}:${sub}`, 43) as SubjectHash;
}

/** For a hash that has already been carried in a signed token. */
export function asSubjectHash(value: string): SubjectHash {
    return value as SubjectHash;
}

const identKey = (provider: ProviderId, subHash: SubjectHash) => `ident:${provider}:${subHash}`;

export async function getAccount(env: Env, id: string): Promise<Account | null> {
    const raw = await env.OD_ACCOUNTS.get(acctKey(id), "json");
    return (raw as Account | null) ?? null;
}

export async function putAccount(env: Env, account: Account): Promise<void> {
    await env.OD_ACCOUNTS.put(acctKey(account.id), JSON.stringify(account));
}

export function accountIdForIdentity(
    env: Env, provider: ProviderId, subHash: SubjectHash,
): Promise<string | null> {
    return env.OD_ACCOUNTS.get(identKey(provider, subHash));
}

export async function accountForIdentity(
    env: Env, provider: ProviderId, subHash: SubjectHash,
): Promise<Account | null> {
    const id = await accountIdForIdentity(env, provider, subHash);
    return id ? getAccount(env, id) : null;
}

export type NickClaim =
    | { ok: true }
    | { ok: false; reason: "taken" | "tombstoned" };

/**
 * Claim a normalized nickname for an account.
 *
 * KV IS EVENTUALLY CONSISTENT, so this cannot be a true compare-and-swap and
 * is not presented as one. Two clients racing for the same free name can both
 * observe it free and both write. The window is small and the outcome is two
 * accounts sharing a normalized nickname, which is a support ticket rather
 * than a security failure -- so we narrow it (check, write, read back, and
 * roll our own write back if we lost) instead of building a Durable Object
 * registry that every signup would have to pay for.
 */
async function claimNick(env: Env, norm: string, accountId: string): Promise<NickClaim> {
    const existing = await env.OD_ACCOUNTS.get(nickKey(norm));
    if (existing && existing !== accountId) return { ok: false, reason: "taken" };
    if (!existing && await env.OD_ACCOUNTS.get(tombKey(norm))) {
        return { ok: false, reason: "tombstoned" };
    }

    await env.OD_ACCOUNTS.put(nickKey(norm), accountId);

    // Read back to narrow the race. If we lost it, release the key rather than
    // leaving a claim pointing at an account that does not own the name.
    const readback = await env.OD_ACCOUNTS.get(nickKey(norm));
    if (readback && readback !== accountId) return { ok: false, reason: "taken" };
    return { ok: true };
}

export interface CreateResult {
    account: Account;
    /** Set when the account could not be created. */
    nickError?: "taken" | "tombstoned" | "banned";
}

export async function identityIsBanned(
    env: Env, provider: ProviderId, subHash: SubjectHash,
): Promise<boolean> {
    return (await env.OD_ACCOUNTS.get(banIdentKey(provider, subHash))) !== null;
}

export async function createAccount(
    env: Env, provider: ProviderId, subHash: SubjectHash, nickInput: string,
    now = Math.floor(Date.now() / 1000),
): Promise<CreateResult | { account: null; nickError: "taken" | "tombstoned" | "banned" }> {
    if (await identityIsBanned(env, provider, subHash)) {
        return { account: null, nickError: "banned" };
    }
    const nick = canonicalize(nickInput);
    const norm = normalize(nick);
    const id = randomId(22);

    const claim = await claimNick(env, norm, id);
    if (!claim.ok) return { account: null, nickError: claim.reason };

    const account: Account = {
        id,
        nick,
        nickNorm: norm,
        created: now,
        // Backdated by the cooldown so a player who mistypes during signup can
        // fix it immediately. The cooldown exists to stop name-churn used for
        // evasion, not to punish a typo on the first screen.
        nickChangedAt: now - NICK_COOLDOWN_SECONDS,
        badges: [],
        identities: [{ provider, subHash, linkedAt: now }],
    };

    await putAccount(env, account);
    await env.OD_ACCOUNTS.put(identKey(provider, subHash), id);
    return { account };
}

export type NickChangeResult =
    | { ok: true; account: Account }
    | { ok: false; reason: "taken" | "tombstoned" | "cooldown"; retryAfter?: number };

export async function changeNickname(
    env: Env, account: Account, nickInput: string,
    now = Math.floor(Date.now() / 1000),
): Promise<NickChangeResult> {
    const nick = canonicalize(nickInput);
    const norm = normalize(nick);

    // Byte-identical: nothing to do, and doing it anyway would spend a KV
    // write on a no-op. Reported as success because from the player's side it
    // is one -- their nickname is what they asked for.
    if (nick === account.nick) return { ok: true, account };

    // EVERY change costs the cooldown, including a restyle.
    //
    // This used to exempt "same name, different capitalisation or separators"
    // as a harmless convenience. It is not harmless: Vlad -> V-l-a-d -> V_l_a_d
    // are all the same normalized name, so the cooldown never engaged, and each
    // one is a KV write. The free plan allows 1,000 a day, so a player idly
    // restyling their name could exhaust the whole service's write budget for
    // everyone. A convenience that can take the service down is not one.
    const elapsed = now - account.nickChangedAt;
    if (elapsed < NICK_COOLDOWN_SECONDS) {
        return { ok: false, reason: "cooldown", retryAfter: NICK_COOLDOWN_SECONDS - elapsed };
    }

    if (norm === account.nickNorm) {
        // Only the styling moved, so the uniqueness key is unchanged and there
        // is nothing to claim or release -- one write instead of three.
        const updated = { ...account, nick, nickChangedAt: now };
        await putAccount(env, updated);
        return { ok: true, account: updated };
    }

    const claim = await claimNick(env, norm, account.id);
    if (!claim.ok) return { ok: false, reason: claim.reason };

    const previous = account.nickNorm;
    const updated: Account = { ...account, nick, nickNorm: norm, nickChangedAt: now };
    await putAccount(env, updated);

    // Release the old name straight away rather than tombstoning it. The
    // account still exists and can be found under its new name, so there is
    // nobody to impersonate -- unlike deletion, where the account is gone.
    if (previous) await env.OD_ACCOUNTS.delete(nickKey(previous));

    return { ok: true, account: updated };
}

export type LinkResult =
    | { ok: true; account: Account }
    | { ok: false; reason: "already_linked_here" | "linked_elsewhere" };

export async function linkIdentity(
    env: Env, account: Account, provider: ProviderId, subHash: SubjectHash,
    now = Math.floor(Date.now() / 1000),
): Promise<LinkResult> {
    const owner = await env.OD_ACCOUNTS.get(identKey(provider, subHash));
    if (owner === account.id) return { ok: false, reason: "already_linked_here" };
    // Refused rather than moved. Silently re-pointing an identity would let
    // anyone who briefly controls a provider account migrate it away from its
    // real owner; unlinking from the other side first is a deliberate act by
    // whoever actually holds that account.
    if (owner) return { ok: false, reason: "linked_elsewhere" };

    const updated: Account = {
        ...account,
        identities: [...account.identities, { provider, subHash, linkedAt: now }],
    };
    await putAccount(env, updated);
    await env.OD_ACCOUNTS.put(identKey(provider, subHash), account.id);
    return { ok: true, account: updated };
}

export type UnlinkResult =
    | { ok: true; account: Account }
    | { ok: false; reason: "not_linked" | "last_identity" };

export async function unlinkIdentity(
    env: Env, account: Account, provider: ProviderId,
): Promise<UnlinkResult> {
    const entry = account.identities.find((i) => i.provider === provider);
    if (!entry) return { ok: false, reason: "not_linked" };
    // Removing the last one would leave an account nobody can ever sign in to,
    // holding a nickname nobody can ever release. If that is what the player
    // wants, it is called deletion and it has its own endpoint.
    if (account.identities.length === 1) return { ok: false, reason: "last_identity" };

    const updated: Account = {
        ...account,
        identities: account.identities.filter((i) => i.provider !== provider),
    };
    await putAccount(env, updated);
    await env.OD_ACCOUNTS.delete(identKey(provider, entry.subHash));
    return { ok: true, account: updated };
}

/**
 * Erase an account. Order matters: the identity keys go FIRST, so that if this
 * is interrupted the account can no longer be signed in to. Failing half-way
 * must leave a player locked out of a stub, never signed in to a corpse.
 */
export async function deleteAccount(env: Env, account: Account): Promise<void> {
    for (const identity of account.identities) {
        // A banned account deleting itself must not be a way to start over.
        // Only the hash is kept, and only for a year.
        if (account.banned) {
            await env.OD_ACCOUNTS.put(
                banIdentKey(identity.provider, identity.subHash),
                String(Math.floor(Date.now() / 1000)),
                { expirationTtl: BAN_IDENTITY_SECONDS },
            );
        }
        await env.OD_ACCOUNTS.delete(identKey(identity.provider, identity.subHash));
    }
    await env.OD_ACCOUNTS.delete(nickKey(account.nickNorm));
    await env.OD_ACCOUNTS.put(tombKey(account.nickNorm), String(Math.floor(Date.now() / 1000)), {
        expirationTtl: TOMBSTONE_SECONDS,
    });
    await env.OD_ACCOUNTS.delete(acctKey(account.id));
}

export async function setBanned(
    env: Env, account: Account, on: boolean, reason: string,
    days?: number, now = Math.floor(Date.now() / 1000),
): Promise<Account> {
    const updated: Account = on
        ? {
            ...account,
            banned: {
                reason, at: now,
                ...(days && days > 0 ? { until: now + Math.floor(days * 86400) } : {}),
            },
        }
        : { ...account, banned: undefined };
    await putAccount(env, updated);
    return updated;
}

/**
 * Whether a ban is in force right now.
 *
 * An expired ban is left on the record rather than deleted -- erasing it would
 * cost a KV write on a read path, and the history is worth keeping so a repeat
 * offender is visible. It simply stops being enforced.
 */
export function banInForce(
    account: Account, now = Math.floor(Date.now() / 1000),
): boolean {
    if (!account.banned) return false;
    return account.banned.until === undefined || account.banned.until > now;
}

/** The public view of an account. Never includes `identities[].subHash`. */
export function publicAccount(account: Account): Record<string, unknown> {
    return {
        id: account.id,
        nickname: account.nick,
        badges: account.badges,
        created: account.created,
        // Which providers are linked, but not which user at those providers.
        linked: account.identities.map((i) => i.provider),
        // Shown so a banned player is told why and for how long, rather than
        // silently failing to join every server they try.
        ...(banInForce(account) ? { banned: account.banned } : {}),
    };
}
