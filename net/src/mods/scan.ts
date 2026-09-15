// Asking a third party whether a mod's file is known to be malicious.
//
// WHAT THIS CAN AND CANNOT TELL YOU, BECAUSE THE DIFFERENCE IS THE WHOLE POINT
//
// The registry does not hold mod files -- they live on the author's own host.
// So this does not scan anything. It looks up the SHA-256 THE AUTHOR DECLARED
// against VirusTotal's existing corpus and reports what comes back.
//
// Three outcomes, and they must stay three:
//
//   flagged     engines have seen this hash and some call it malicious.
//   clean       engines have seen this hash and none do.
//   unknown     VirusTotal has never seen this hash.
//
// `unknown` is the normal state of a mod nobody has ever uploaded there. It is
// ALSO the state of a file built this morning to attack somebody. Collapsing it
// into "clean" would turn this feature from a weak signal into a false
// assurance, which is worse than having no feature -- so registry.ts renders it
// as its own word and says so in the payload.
//
// AND THE HASH IS DECLARED, NOT OBSERVED. We never fetch the .odmod, so we
// cannot say the bytes at the download URL still hash to the value we looked
// up. An author who wanted to could publish one file, get it scanned, and serve
// another. What the declared hash IS good for: the game shows a mod's hash, so
// a player who downloads can compare by hand, and multiplayer already refuses a
// client whose .odmod hash does not match the host's.
//
// The honest summary, which belongs in the UI and not only here: a clean result
// means nobody has reported these bytes, not that the mod is safe.

import type { Env } from "../env.js";
import type { Scan } from "./registry.js";

const VT_FILE = "https://www.virustotal.com/api/v3/files/";

/** Bounded so a slow third party cannot hold a publish request open. */
const TIMEOUT_MS = 6000;

/**
 * Look one hash up.
 *
 * Returns null when we could not ask at all -- no key configured, a network
 * failure, a rate limit. Null is NOT recorded on the listing: an absent scan
 * renders as "unscanned", and writing a fake verdict because the lookup failed
 * is the one thing this must never do.
 */
export async function lookup(env: Env, sha256: string): Promise<Scan | null> {
    if (!env.VIRUSTOTAL_API_KEY) return null;

    const at = Math.floor(Date.now() / 1000);
    let res: Response;
    try {
        res = await fetch(VT_FILE + sha256, {
            headers: { "x-apikey": env.VIRUSTOTAL_API_KEY, accept: "application/json" },
            signal: AbortSignal.timeout(TIMEOUT_MS),
        });
    } catch {
        return null;
    }

    // Never seen. A real answer, and the one most new mods get.
    if (res.status === 404) return { at, known: false };

    // 401 is a misconfigured key and 429 is the free tier's four-a-minute.
    // Both mean "we did not learn anything", not "nothing was found".
    if (!res.ok) return null;

    let body: unknown;
    try { body = await res.json(); } catch { return null; }

    const stats = (body as Record<string, Record<string, Record<string, unknown>>>)
        ?.data?.attributes?.last_analysis_stats as Record<string, number> | undefined;
    if (!stats) return { at, known: false };

    const num = (k: string) => Number(stats[k] ?? 0) || 0;
    const malicious = num("malicious");
    const suspicious = num("suspicious");
    // The denominator, so a count is never shown without one.
    const engines = malicious + suspicious + num("harmless") + num("undetected")
                  + num("timeout") + num("failure") + num("type-unsupported");

    return {
        at, known: true, malicious, suspicious, engines,
        permalink: `https://www.virustotal.com/gui/file/${sha256}`,
    };
}

/**
 * Is the download URL actually there?
 *
 * Not security -- a dead link is simply the most common thing wrong with a
 * listing, and catching it while the author is still looking at the form is
 * worth one HEAD request. A host that refuses HEAD is not a failure: plenty do,
 * and the listing is fine.
 */
export async function reachable(url: string): Promise<{ ok: boolean; size?: number }> {
    try {
        const res = await fetch(url, {
            method: "HEAD", redirect: "follow",
            signal: AbortSignal.timeout(TIMEOUT_MS),
        });
        if (!res.ok) return { ok: false };
        const len = Number(res.headers.get("content-length") ?? "0");
        return { ok: true, ...(len > 0 ? { size: len } : {}) };
    } catch {
        return { ok: true };   // could not tell; do not block a publish on it
    }
}
