// Request/response plumbing shared by every route.

import type { Env } from "./env.js";
import { getAccount, type Account } from "./accounts/store.js";
import { verifySessionToken } from "./auth/token.js";

// The web build is served from itch.io, so every call from it is
// cross-origin. `*` is safe here because authentication is a Bearer header and
// never a cookie -- there is no ambient credential for another site to ride.
// If a cookie is ever added, this must become an explicit origin allowlist on
// the same day.
const CORS: Record<string, string> = {
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET, POST, OPTIONS",
    "access-control-allow-headers": "content-type, authorization",
    "access-control-max-age": "86400",
};

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
