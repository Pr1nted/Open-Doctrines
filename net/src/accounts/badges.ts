// Developer and playtester badges.
//
// Granted by hand, by whoever holds ADMIN_SECRET. There is no self-serve path
// and no automation, because the whole value of a badge is that it means
// somebody decided -- and because a badge is what a rogue auth provider would
// forge first if it could (see the "unofficial issuer" rendering in the client).

import type { Env } from "../env.js";
import { timingSafeEqual } from "../util/crypto.js";
import { getAccount, isBadge, putAccount, type Account, type Badge } from "./store.js";

export function isAdmin(request: Request, env: Env): boolean {
    const header = request.headers.get("x-od-admin");
    if (!header || !env.ADMIN_SECRET) return false;
    return timingSafeEqual(header, env.ADMIN_SECRET);
}

export type BadgeResult =
    | { ok: true; account: Account }
    | { ok: false; reason: "no_account" | "bad_badge" };

export async function setBadge(
    env: Env, accountId: string, badge: string, on: boolean,
): Promise<BadgeResult> {
    if (!isBadge(badge)) return { ok: false, reason: "bad_badge" };
    const account = await getAccount(env, accountId);
    if (!account) return { ok: false, reason: "no_account" };

    const has = account.badges.includes(badge);
    if (has === on) return { ok: true, account };   // no write for a no-op

    const badges: Badge[] = on
        ? [...account.badges, badge]
        : account.badges.filter((b) => b !== badge);

    const updated = { ...account, badges };
    await putAccount(env, updated);
    return { ok: true, account: updated };
}
