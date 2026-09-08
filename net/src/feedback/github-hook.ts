// GitHub telling us an issue has been closed, so the Discord thread it came
// from can be told too.
//
// WHY THIS IS THE DIRECTION THAT EXISTS
//
// GitHub is where the work is tracked, so closing there is the real signal, and
// GitHub can push events to a URL. Discord cannot: a webhook only sends, so
// hearing a forum thread being resolved would need a bot holding a gateway
// connection -- a Durable Object with a permanent WebSocket and a second
// always-on credential, which is a different project and fights the free-tier
// shape of the rest of this service.
//
// THE SIGNATURE IS THE WHOLE SECURITY MODEL
//
// This endpoint is public and it makes the service post into a Discord channel.
// Without verification it is a spam relay with extra steps: anyone who guesses
// the path could announce that any issue was closed by anyone. Every request is
// checked against an HMAC of the RAW body before a single field is read, and
// compared in constant time.

import type { Env } from "../env.js";
import { hmac, timingSafeEqual } from "../util/crypto.js";
import { announceIssueState } from "./report.js";

function hex(bytes: Uint8Array): string {
    return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

/**
 * Whether this request really came from GitHub.
 *
 * Over the RAW body text, never a re-serialised object: `JSON.parse` followed
 * by `JSON.stringify` does not reproduce the bytes GitHub signed (key order,
 * whitespace, unicode escapes), so a signature checked against a round-tripped
 * body fails for honest requests and teaches you to disable the check.
 */
export async function verifyGithubSignature(
    secret: string, rawBody: string, header: string | null,
): Promise<boolean> {
    if (!header) return false;
    const [algorithm, signature] = header.split("=");
    if (algorithm !== "sha256" || !signature) return false;
    const expected = hex(await hmac(secret, rawBody));
    return timingSafeEqual(signature, expected);
}

export interface HookOutcome {
    status: number;
    body: { ok: boolean; action?: string; announced?: boolean; error?: string };
}

/**
 * Handle one delivery.
 *
 * Everything that is not an issue being closed or reopened is answered 200 and
 * ignored. GitHub retries and disables endpoints that error, and it sends the
 * whole `issues` event family plus a ping on setup -- answering "not for me"
 * with a failure would eventually switch the hook off.
 */
export async function handleGithubHook(
    env: Env, rawBody: string, signature: string | null, event: string | null,
): Promise<HookOutcome> {
    const secret = env.FEEDBACK_GITHUB_WEBHOOK_SECRET;
    if (!secret) {
        return { status: 503, body: { ok: false, error: "not_configured" } };
    }
    if (!await verifyGithubSignature(secret, rawBody, signature)) {
        return { status: 401, body: { ok: false, error: "bad_signature" } };
    }

    // The handshake GitHub sends when the hook is saved. Answering it plainly
    // is what turns the tick green in the repository settings.
    if (event === "ping") return { status: 200, body: { ok: true, action: "ping" } };
    if (event !== "issues") return { status: 200, body: { ok: true, action: "ignored" } };

    let payload: {
        action?: string;
        issue?: { number?: number; title?: string };
        sender?: { login?: string };
    };
    try {
        payload = JSON.parse(rawBody);
    } catch {
        return { status: 400, body: { ok: false, error: "bad_json" } };
    }

    const action = payload.action ?? "";
    if (action !== "closed" && action !== "reopened") {
        return { status: 200, body: { ok: true, action: "ignored" } };
    }
    const number = payload.issue?.number;
    if (!number) return { status: 200, body: { ok: true, action: "ignored" } };

    const announced = await announceIssueState(
        env, number, action,
        payload.issue?.title ?? `#${number}`,
        payload.sender?.login ?? "someone",
    );
    // Not an error when there is nothing to tell: most issues on a repository
    // were opened by hand and never had a thread.
    return { status: 200, body: { ok: true, action, announced } };
}
