// Request/response plumbing shared by every route.

import type { Env } from "./env.js";
import { getAccount, type Account } from "./accounts/store.js";
import { verifySessionToken } from "./auth/token.js";
import { hmacId } from "./util/crypto.js";

// The web build is served from itch.io, so every call from it is
// cross-origin. `*` is safe here because authentication is a Bearer header and
// never a cookie -- there is no ambient credential for another site to ride.
// If a cookie is ever added, this must become an explicit origin allowlist on
// the same day.
const CORS: Record<string, string> = {
    "access-control-allow-origin": "*",
    // PUT is here for long-form turn storage, which is the only route that
    // writes with it. Without it the web build cannot publish a turn at all:
    // the preflight fails before the request is ever made.
    "access-control-allow-methods": "GET, PUT, POST, OPTIONS",
    "access-control-allow-headers": "content-type, authorization",
    "access-control-max-age": "86400",
};

/**
 * Put the CORS headers on a response that came from somewhere else.
 *
 * Needed because a Durable Object's reply is returned to the caller verbatim,
 * and it has never heard of CORS -- so every response produced inside LobbyDO
 * reaches a browser with no `access-control-allow-origin` and is discarded
 * before the game can read it. Not for a 101: an upgrade has no headers to
 * rewrite and the WebSocket handshake is not subject to CORS anyway.
 */
export function withCors(response: Response): Response {
    const headers = new Headers(response.headers);
    for (const [name, value] of Object.entries(CORS)) headers.set(name, value);
    return new Response(response.body, { status: response.status, headers });
}

export function json(body: unknown, status = 200, headers: Record<string, string> = {}): Response {
    return new Response(JSON.stringify(body), {
        status,
        headers: { "content-type": "application/json; charset=utf-8", ...CORS, ...headers },
    });
}

export function text(body: string, status = 200, contentType = "text/plain; charset=utf-8"): Response {
    return new Response(body, { status, headers: { "content-type": contentType, ...CORS } });
}

export function preflight(): Response {
    return new Response(null, { status: 204, headers: CORS });
}

/**
 * An error the client can act on. `code` is stable and meant to be switched
 * on; `message` is for a human and may change.
 */
export function fail(status: number, code: string, message: string, extra: object = {}): Response {
    return json({ error: code, message, ...extra }, status);
}

/**
 * Refuse a caller that is asking too often. Returns the refusal, or null to
 * carry on.
 *
 * KEYED ON A HASH OF THE ADDRESS, NEVER THE ADDRESS. PRIVACY.md says in as many
 * words that we do not collect your IP address, and this must not quietly make
 * that untrue. What the limiter sees is HMAC(IDENT_KEY, "rl:" + ip) -- not
 * reversible without a secret it does not have, never written to KV, never
 * logged, and behind it a counter that lives in one colo's memory for ten
 * seconds. The address itself does not outlive the expression.
 *
 * The domain prefix keeps this from colliding with the other things IDENT_KEY
 * hashes, exactly as `pkce:` does in device.ts.
 */
export async function rateLimit(
    limiter: RateLimit, request: Request, env: Env, retryAfter: number,
): Promise<Response | null> {
    const ip = request.headers.get("cf-connecting-ip");
    // Cloudflare sets this on every request that reaches the edge, OVERWRITING
    // whatever the client sent, so in production it is always present and never
    // forgeable. Absent means nothing is in front of us -- the vitest pool,
    // which calls the worker directly. Fail open there: there is no attacker to
    // limit and no real address to key on.
    //
    // `wrangler dev` is NOT that case. Miniflare synthesises the header as
    // 127.0.0.1, so the limiter is live locally and every client on the machine
    // shares one bucket. Worth knowing before blaming a local 429 on the game.
    if (!ip) return null;

    const { success } = await limiter.limit({ key: await hmacId(env.IDENT_KEY, `rl:${ip}`, 22) });
    if (success) return null;

    // A 429 with no retry-after invites the client to decide for itself, and
    // the client that is hammering us is the one least likely to decide well.
    return json(
        { error: "rate_limited", message: "Too many requests. Wait a moment and try again." },
        429,
        { "retry-after": String(retryAfter) },
    );
}

export function bearer(request: Request): string | null {
    const header = request.headers.get("authorization");
    if (!header) return null;
    const [scheme, ...rest] = header.split(" ");
    if (!scheme || scheme.toLowerCase() !== "bearer") return null;
    const token = rest.join(" ").trim();
    return token || null;
}

/**
 * Resolve the caller's account from a session token.
 *
 * Returns null for a missing, malformed, expired, wrong-audience or
 * unknown-account token, all indistinguishably. A join ticket presented here
 * lands in exactly that bucket -- its audience is `od-relay:<id>`, not
 * `od-api` -- which is the mechanism that stops a captured ticket from ever
 * acting as the player.
 */
export async function authenticate(request: Request, env: Env): Promise<Account | null> {
    const token = bearer(request);
    if (!token) return null;
    const claims = await verifySessionToken(env, token);
    if (!claims) return null;
    return getAccount(env, claims.sub);
}

export async function readJson<T>(request: Request, maxBytes = 8 * 1024): Promise<T | null> {
    const declared = Number(request.headers.get("content-length") ?? "0");
    if (declared > maxBytes) return null;
    try {
        const body = await request.text();
        if (body.length > maxBytes) return null;
        return JSON.parse(body) as T;
    } catch {
        return null;
    }
}
