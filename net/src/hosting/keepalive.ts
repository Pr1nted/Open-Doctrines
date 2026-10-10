// Keep a free-plan game server awake.
//
// Render's free web services stop after fifteen minutes without an INBOUND
// request, and a tournament server hosted through this service's relay gets
// none: players reach it through the relay, which the server dialled out to.
// Asleep, it misses its turn deadlines and drops off the relay until somebody
// happens to load its URL.
//
// So the cron that already runs every minute also requests each URL in
// KEEPALIVE_URLS every ten minutes. The server answers on /healthz from a tiny
// responder (deploy/server/health.py) whatever it is doing, so this costs it
// nothing. Unset, nothing is requested.

import type { Env } from "../env.js";

/** Every ten minutes, on the minute -- well inside the fifteen. */
export const KEEPALIVE_EVERY_MINUTES = 10;
/** A handful at most: this is for the project's own servers, not a service. */
export const KEEPALIVE_MAX_TARGETS = 5;

export function keepaliveDue(when: Date, every = KEEPALIVE_EVERY_MINUTES): boolean {
    return when.getUTCMinutes() % every === 0;
}

/**
 * The URLs to request: comma- or whitespace-separated, https only (anything
 * else is a typo or a way to make this service fetch something it should not),
 * deduplicated, and capped.
 */
export function keepaliveTargets(raw: string | undefined): string[] {
    const out: string[] = [];
    for (const part of (raw ?? "").split(/[\s,]+/)) {
        if (!part) continue;
        let url: URL;
        try { url = new URL(part); } catch { continue; }
        if (url.protocol !== "https:" || url.username || url.password) continue;
        const s = url.toString();
        if (!out.includes(s)) out.push(s);
        if (out.length >= KEEPALIVE_MAX_TARGETS) break;
    }
    return out;
}

/** Request each target once; how many answered 2xx. Never throws. */
export async function pingKeepalive(
    env: Env,
    fetcher: (input: string, init?: RequestInit) => Promise<Response> = fetch,
): Promise<number> {
    let answered = 0;
    await Promise.all(keepaliveTargets(env.KEEPALIVE_URLS).map(async (url) => {
        try {
            const res = await fetcher(url, {
                method: "GET",
                headers: { "User-Agent": "opendoctrines-keepalive" },
                // A server that is waking up takes a while; one that is gone
                // must not hold the cron for long.
                signal: AbortSignal.timeout(20_000),
            });
            if (res.ok) answered += 1;
        } catch {
            // Down, asleep or unreachable: the next run tries again.
        }
    }));
    return answered;
}
