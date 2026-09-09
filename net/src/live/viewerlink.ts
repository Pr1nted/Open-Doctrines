// A link a viewer can click to join, without the invite code being on camera.
//
// ── THE PROBLEM WITH JUST SHOWING THE CODE ──
//
// A streamer who reads their invite code out loud has given it to everybody
// watching, everybody watching the clip, and everybody watching the VOD next
// year. There is no way to take it back, because the code IS the key: it does
// not expire when the stream ends and it cannot be rotated without breaking the
// people already in the lobby.
//
// ── SO THE LINK IS NOT THE CODE ──
//
// A viewer link is a separate short token that RESOLVES to a session, and it
// has the properties the code cannot have:
//
//   IT EXPIRES. Minutes, not for ever, so a VOD is not a way in next year.
//   IT IS COUNTABLE. A limit on uses, so a link pasted somewhere it should not
//     have been stops working rather than filling the lobby.
//   IT IS REVOCABLE. The host can drop it and mint another; the people already
//     in the game are unaffected, because their connection does not depend on
//     the token they arrived through.
//
// The code itself never leaves the host's own screen -- and in stream-safe mode
// it never even reaches that.

import type { Env } from "../env.js";

export interface ViewerLink {
    token: string;
    code: string;      // the session it opens
    expires: number;   // unix seconds
    uses: number;      // how many joins are left
}

/** Minted short-lived on purpose; see the header. */
export const LINK_TTL_SECONDS = 60 * 60;      // an hour: one stream, not one year
export const LINK_MAX_USES = 200;

const KEY_PREFIX = "vlink:";

/**
 * Tokens are random, not derived from the code.
 *
 * A token computed from the session would let anybody holding one work out the
 * others, and would mean revoking a link required changing the session.
 */
/**
 * The characters a token may contain.
 *
 * No i, l, o, 0 or 1: read off a stream and typed on a phone, those are the
 * same glyph to somebody squinting at 480p.
 *
 * THE ALPHABET AND THE VALIDATOR ARE THE SAME LIST, and they must be. The first
 * version had "i" in the alphabet and not in the validator's character class,
 * so about a quarter of all minted tokens failed their own isToken() check and
 * resolved as "unknown" -- which in production is "some viewer links just do
 * not work", with nothing in common between the ones that failed.
 */
const ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789";

export function mintToken(random: Uint8Array): string {
    let out = "";
    for (const byte of random) out += ALPHABET[byte % ALPHABET.length];
    return out.slice(0, 10);
}

export function isToken(value: string): boolean {
    if (typeof value !== "string" || value.length !== 10) return false;
    // Checked against the alphabet itself rather than a hand-written character
    // class, so the two cannot drift apart again.
    for (const ch of value) if (!ALPHABET.includes(ch)) return false;
    return true;
}

export async function createLink(
    env: Env, code: string, now: number, uses = LINK_MAX_USES,
): Promise<ViewerLink> {
    const bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    const link: ViewerLink = {
        token: mintToken(bytes),
        code,
        expires: now + LINK_TTL_SECONDS,
        uses: Math.max(1, Math.min(uses, LINK_MAX_USES)),
    };
    // KV expiry does the forgetting, so an abandoned link costs nothing and
    // there is no sweep to write.
    await env.OD_ACCOUNTS.put(KEY_PREFIX + link.token, JSON.stringify(link),
                              { expirationTtl: LINK_TTL_SECONDS });
    return link;
}

export type Resolution =
    | { ok: true; code: string }
    | { ok: false; reason: "unknown" | "expired" | "spent" };

/**
 * Turn a token back into a session code, spending one use.
 *
 * The three refusals are kept apart because they mean different things to the
 * person holding the link: unknown is a typo or a revoked link, expired is a
 * stream that finished, spent is a link that went further than it was meant to.
 */
export async function useLink(env: Env, token: string, now: number): Promise<Resolution> {
    if (!isToken(token)) return { ok: false, reason: "unknown" };
    const raw = await env.OD_ACCOUNTS.get(KEY_PREFIX + token);
    if (!raw) return { ok: false, reason: "unknown" };

    let link: ViewerLink;
    try {
        link = JSON.parse(raw) as ViewerLink;
    } catch {
        return { ok: false, reason: "unknown" };
    }
    if (!link.code || typeof link.expires !== "number") return { ok: false, reason: "unknown" };
    if (now >= link.expires) return { ok: false, reason: "expired" };
    if (link.uses <= 0) return { ok: false, reason: "spent" };

    // Spent before it is handed back. A crash between the two loses one join
    // from the budget, which is the safe direction to be wrong in -- the other
    // way round is an unlimited link.
    const left = link.uses - 1;
    const ttl = Math.max(60, link.expires - now);
    await env.OD_ACCOUNTS.put(KEY_PREFIX + token,
                              JSON.stringify({ ...link, uses: left }),
                              { expirationTtl: ttl });
    return { ok: true, code: link.code };
}

export async function revokeLink(env: Env, token: string): Promise<void> {
    if (isToken(token)) await env.OD_ACCOUNTS.delete(KEY_PREFIX + token);
}
