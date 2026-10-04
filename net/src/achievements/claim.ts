// Turning a claim into a grant, or not.
//
// THE HONEST LIMIT FIRST
//
// The game is open source and runs on the player's own machine. Nothing a
// server does can PROVE that a single-player game happened the way a claim says
// it did -- anyone can build a client that sends any claim it likes. What this
// file can do is make a forged collection cost what an earned one costs, and
// it does that with time, which is the one thing a forger cannot fake from
// their side of the wire:
//
//   1. THE ACCOUNT CLOCK. Every achievement has `minHours`: how long after the
//      account first opened a play session the achievement can plausibly be
//      reached. "Win a world war" is not plausible ten minutes after first
//      launch. The first session is recorded by THIS service, at the moment it
//      happened, so it cannot be backdated by the client.
//   2. THE SESSION CLOCK. Some achievements are about one long sitting
//      (`minSessionMinutes`). A play ticket is signed here when the session
//      starts and carries the server's own time, so the client cannot claim a
//      sitting longer than the one it actually had.
//   3. THE BUDGET. A token bucket per account: at most BUDGET_CAPACITY grants
//      in a burst, refilling one every few minutes of real time. The whole
//      catalog takes hours of wall clock to collect however it is collected.
//   4. PREREQUISITES. Tiers in order: nobody holds "Superpower" without having
//      first held "Regional Power".
//
// So editing a file, or hand-building a .odstate, gets nothing: the game only
// believes signed grants. And a modified client gets, at best, the collection
// it would have earned by leaving the game running -- which is where the cost
// of cheating should sit for a game nobody is paid to win.
//
// A mismatched rule-table seal is marked, not refused. See grant.ts.

import type { Env } from "../env.js";
import { authenticate, fail, json, readJson } from "../http.js";
import { banInForce } from "../accounts/store.js";
import { sign, verify } from "../auth/token.js";
import { CATALOG, type CatalogEntry } from "./catalog.gen.js";
import {
    AUD_PLAY, PLAY_TTL, grantPublicKeyRaw, grantsConfigured, signGrant, type GrantClaims, type PlayClaims,
} from "./grant.js";
import type { Decision } from "./AchievementsDO.js";
import { sha256 } from "../util/crypto.js";
import { utf8 } from "../util/encoding.js";

/** Claims per request. A client with more pending sends several requests. */
export const MAX_CLAIMS = 24;

const BY_ID: Map<string, CatalogEntry> = new Map(CATALOG.map((c) => [c.id, c]));
const ORDER: Map<string, number> = new Map(CATALOG.map((c, i) => [c.id, i]));

function store(env: Env, accountId: string): DurableObjectStub {
    return env.ACHIEVEMENTS.get(env.ACHIEVEMENTS.idFromName(accountId));
}

async function call<T>(env: Env, accountId: string, path: string, body?: unknown): Promise<T> {
    const res = await store(env, accountId).fetch(new Request(`https://ach${path}`, {
        method: body === undefined ? "GET" : "POST",
        body: body === undefined ? undefined : JSON.stringify(body),
        headers: { "content-type": "application/json" },
    }));
    return await res.json() as T;
}

function isHexWord(s: unknown): s is string {
    return typeof s === "string" && /^[0-9a-f]{1,16}$/.test(s);
}

function sealIsKnown(env: Env, seal: string): boolean {
    const list = (env.ACHIEVEMENT_KNOWN_SEALS ?? "").split(",").map((s) => s.trim()).filter(Boolean);
    // No list configured means nothing is known to be wrong. Marking every
    // grant "modified" on a deployment that never listed a release would be a
    // false accusation against every player.
    return list.length === 0 || list.includes(seal);
}

/** The catalog without the server's clock fields, for clients that want names. */
export function achievementsCatalog(): Response {
    return json({ achievements: CATALOG.map((c) => ({ id: c.id, steam: c.steam, hidden: c.hidden })) },
        200, { "cache-control": "public, max-age=3600" });
}

/** The public key grants verify against, raw and as a JWK. */
export function achievementsKey(env: Env): Response {
    if (!grantsConfigured(env)) return fail(503, "not_configured", "This service issues no achievements.");
    return json({ key: grantPublicKeyRaw(env), jwk: JSON.parse(env.ACHIEVEMENT_PUBLIC_JWK!), issuer: env.ISSUER },
        200, { "cache-control": "public, max-age=3600" });
}

/** POST /achievements/session -- start the session clock. */
export async function achievementsSession(request: Request, env: Env): Promise<Response> {
    if (!grantsConfigured(env)) return fail(503, "not_configured", "This service issues no achievements.");
    const account = await authenticate(request, env);
    if (!account) return fail(401, "no_account", "Sign in to earn achievements.");
    const body = await readJson<{ seal?: string; build?: string }>(request, 1024);
    const seal = isHexWord(body?.seal) ? body!.seal! : "0";
    const build = typeof body?.build === "string" ? body.build.slice(0, 32) : "";

    const { firstPlay } = await call<{ firstPlay: number }>(env, account.id, "/session", {});
    const now = Math.floor(Date.now() / 1000);
    const claims: PlayClaims = {
        iss: env.ISSUER, aud: AUD_PLAY, sub: account.id, iat: now, exp: now + PLAY_TTL, seal, build,
    };
    return json({ play: await sign(env, claims), firstPlay, now });
}

/** GET /achievements/mine -- every grant this account holds. */
export async function achievementsMine(request: Request, env: Env): Promise<Response> {
    if (!grantsConfigured(env)) return fail(503, "not_configured", "This service issues no achievements.");
    const account = await authenticate(request, env);
    if (!account) return fail(401, "no_account", "Sign in first.");
    const r = await call<{ grants: string[]; firstPlay: number | null }>(env, account.id, "/list");
    return json(r);
}

/** POST /achievements/claim */
export async function achievementsClaim(request: Request, env: Env): Promise<Response> {
    if (!grantsConfigured(env)) return fail(503, "not_configured", "This service issues no achievements.");
    const account = await authenticate(request, env);
    if (!account) return fail(401, "no_account", "Sign in to earn achievements.");
    if (banInForce(account)) return fail(403, "banned", "This account cannot earn achievements.");

    const body = await readJson<{ play?: string; claims?: { ach?: string; ev?: string }[] }>(request, 16 * 1024);
    if (!body || typeof body.play !== "string" || !Array.isArray(body.claims))
        return fail(400, "bad_request", "Expected {play, claims}.");
    if (body.claims.length > MAX_CLAIMS) return fail(400, "too_many", `At most ${MAX_CLAIMS} claims per request.`);

    const play = await verify<PlayClaims>(env, body.play, AUD_PLAY);
    if (!play || play.sub !== account.id) return fail(401, "bad_play", "That play session is not valid. Start a new one.");

    const now = Math.floor(Date.now() / 1000);
    const refused: Decision[] = [];
    const wanted: { entry: CatalogEntry; ev: string }[] = [];
    const seen = new Set<string>();
    for (const c of body.claims) {
        const entry = typeof c?.ach === "string" ? BY_ID.get(c.ach) : undefined;
        if (!entry) { refused.push({ ach: String(c?.ach ?? "").slice(0, 48), refused: "unknown" }); continue; }
        if (seen.has(entry.id)) continue;
        seen.add(entry.id);
        const sessionWait = play.iat + entry.minSessionMinutes * 60 - now;
        if (sessionWait > 0) { refused.push({ ach: entry.id, retryAfter: sessionWait }); continue; }
        const ev = typeof c.ev === "string" ? c.ev.slice(0, 4096) : "";
        wanted.push({ entry, ev });
    }
    // Catalog order, so a tier claimed in the same batch as its predecessor
    // finds the predecessor already decided.
    wanted.sort((a, b) => ORDER.get(a.entry.id)! - ORDER.get(b.entry.id)!);

    const { decisions } = await call<{ decisions: Decision[] }>(env, account.id, "/decide", {
        claims: wanted.map((w) => ({ ach: w.entry.id, requires: w.entry.requires, minFirstPlay: w.entry.minHours * 3600 })),
    });

    const modified = !sealIsKnown(env, play.seal);
    const toSign: { ach: string; token: string }[] = [];
    const out: Decision[] = [...refused];
    for (let i = 0; i < decisions.length; ++i) {
        const d = decisions[i]!;
        if (d.token || d.refused || d.retryAfter !== undefined) { out.push(d); continue; }
        const evHash = Array.from(await sha256(utf8(wanted[i]!.ev)))
            .map((b) => b.toString(16).padStart(2, "0")).join("");
        const claims: GrantClaims = {
            iss: env.ISSUER, aud: "od-achievement", sub: account.id, ach: d.ach, iat: now, ev: evHash,
            ...(modified ? { mod: 1 as const } : {}),
        };
        toSign.push({ ach: d.ach, token: await signGrant(env, claims) });
    }
    if (toSign.length) {
        const { stored } = await call<{ stored: { ach: string; token: string }[] }>(env, account.id, "/commit", { grants: toSign });
        const storedSet = new Map(stored.map((s) => [s.ach, s.token]));
        for (const t of toSign) {
            const tok = storedSet.get(t.ach);
            out.push(tok ? { ach: t.ach, token: tok } : { ach: t.ach, retryAfter: 240 });
        }
    }
    return json({ decisions: out });
}

/** Called by account deletion: an account that no longer exists holds nothing. */
export async function wipeAchievements(env: Env, accountId: string): Promise<void> {
    if (!env.ACHIEVEMENTS) return;
    await call(env, accountId, "/wipe", {});
}
