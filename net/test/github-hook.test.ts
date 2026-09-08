// The issues webhook: what it accepts, what it refuses, and the one thing it
// must never become.
//
// This endpoint is public and it makes the service post into a Discord channel.
// The signature check is the entire security model, so most of this file is
// about that rather than about the happy path.

import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import type { Env } from "../src/env.js";
import { handleGithubHook, verifyGithubSignature } from "../src/feedback/github-hook.js";
import { linkIssueToThread, threadForIssue } from "../src/feedback/report.js";
import { hmac } from "../src/util/crypto.js";
import { clearKv, setupEnv } from "./helpers.js";

let env: Env;
const SECRET = "hook-secret-for-tests";

function hex(bytes: Uint8Array): string {
    return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}
async function sign(body: string, secret = SECRET): Promise<string> {
    return `sha256=${hex(await hmac(secret, body))}`;
}
function closedEvent(number = 12): string {
    return JSON.stringify({
        action: "closed",
        issue: { number, title: "Panel overlaps the minimap" },
        sender: { login: "Pr1nted" },
    });
}

beforeAll(async () => { env = await setupEnv(); });
beforeEach(async () => {
    await clearKv(env);
    vi.restoreAllMocks();
    (env as Env).FEEDBACK_GITHUB_WEBHOOK_SECRET = SECRET;
    (env as Env).FEEDBACK_DISCORD_WEBHOOK = "https://discord.invalid/hook";
    (env as Env).FEEDBACK_GITHUB_REPO = "owner/repo";
});

describe("the signature, which is the whole security model", () => {
    it("accepts a delivery GitHub really signed", async () => {
        const body = closedEvent();
        expect(await verifyGithubSignature(SECRET, body, await sign(body))).toBe(true);
    });

    it("refuses a forged one, a missing one, and the wrong algorithm", async () => {
        const body = closedEvent();
        expect(await verifyGithubSignature(SECRET, body, "sha256=deadbeef")).toBe(false);
        expect(await verifyGithubSignature(SECRET, body, null)).toBe(false);
        expect(await verifyGithubSignature(SECRET, body, "sha1=" + "0".repeat(40))).toBe(false);
        expect(await verifyGithubSignature(SECRET, body, "")).toBe(false);
    });

    it("refuses a body that was altered after signing", async () => {
        // The point of signing the body rather than just holding a secret URL.
        const signature = await sign(closedEvent(12));
        expect(await verifyGithubSignature(SECRET, closedEvent(999), signature)).toBe(false);
    });

    it("refuses a signature made with a different secret", async () => {
        const body = closedEvent();
        expect(await verifyGithubSignature(SECRET, body, await sign(body, "wrong"))).toBe(false);
    });

    it("REFUSES EVERYTHING when no secret is set, rather than trusting the caller", async () => {
        delete (env as Env).FEEDBACK_GITHUB_WEBHOOK_SECRET;
        const body = closedEvent();
        const out = await handleGithubHook(env, body, await sign(body), "issues");
        expect(out.status).toBe(503);
        expect(out.body.ok).toBe(false);
    });

    it("does not act on an unsigned delivery", async () => {
        const fetchSpy = vi.fn();
        vi.stubGlobal("fetch", fetchSpy);
        await linkIssueToThread(env, 12, "thread-12");

        const out = await handleGithubHook(env, closedEvent(), null, "issues");
        expect(out.status).toBe(401);
        expect(fetchSpy).not.toHaveBeenCalled();   // nothing was posted anywhere
    });
});

describe("what it does with a real delivery", () => {
    it("tells the thread its issue was closed, and by whom", async () => {
        await linkIssueToThread(env, 12, "thread-12");
        let url = ""; let body: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (u: string, init: RequestInit) => {
            url = String(u); body = JSON.parse(init.body as string);
            return new Response(null, { status: 204 });
        });

        const payload = closedEvent(12);
        const out = await handleGithubHook(env, payload, await sign(payload), "issues");
        expect(out.status).toBe(200);
        expect(out.body.announced).toBe(true);
        expect(url).toContain("thread_id=thread-12");
        expect(String(body.content)).toContain("Closed");
        expect(String(body.content)).toContain("Pr1nted");
        expect(String(body.content)).toContain("https://github.com/owner/repo/issues/12");
        // A closing note must not be able to ping the server either.
        expect(body.allowed_mentions).toEqual({ parse: [] });
    });

    it("handles reopening too, so a thread is not left saying the wrong thing", async () => {
        await linkIssueToThread(env, 12, "thread-12");
        let body: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (_u: string, init: RequestInit) => {
            body = JSON.parse(init.body as string);
            return new Response(null, { status: 204 });
        });

        const payload = JSON.stringify({
            action: "reopened", issue: { number: 12, title: "t" }, sender: { login: "Pr1nted" },
        });
        await handleGithubHook(env, payload, await sign(payload), "issues");
        expect(String(body.content)).toContain("Reopened");
    });

    it("says yes to the ping GitHub sends when the hook is saved", async () => {
        const out = await handleGithubHook(env, "{}", await sign("{}"), "ping");
        expect(out.status).toBe(200);
        expect(out.body.action).toBe("ping");
    });

    it("ignores events it does not care about, with a 200", async () => {
        // GitHub disables an endpoint that keeps erroring, and it sends the
        // whole issues family plus pushes and stars. "Not for me" is a success.
        for (const [event, payload] of [
            ["push", "{}"],
            ["issues", JSON.stringify({ action: "labeled", issue: { number: 12 } })],
            ["issues", JSON.stringify({ action: "closed" })],   // no issue number
        ] as const) {
            const out = await handleGithubHook(env, payload, await sign(payload), event);
            expect(out.status).toBe(200);
            expect(out.body.ok).toBe(true);
        }
    });

    it("is quiet about an issue that never came from a report", async () => {
        // Most issues on a repository were opened by hand. Nothing to tell.
        const fetchSpy = vi.fn();
        vi.stubGlobal("fetch", fetchSpy);
        const payload = closedEvent(9999);
        const out = await handleGithubHook(env, payload, await sign(payload), "issues");
        expect(out.status).toBe(200);
        expect(out.body.announced).toBe(false);
        expect(fetchSpy).not.toHaveBeenCalled();
    });

    it("survives malformed JSON that was correctly signed", async () => {
        const out = await handleGithubHook(env, "{not json", await sign("{not json"), "issues");
        expect(out.status).toBe(400);
    });
});

describe("the link between an issue and a thread", () => {
    it("round-trips", async () => {
        await linkIssueToThread(env, 41, "thread-41");
        expect(await threadForIssue(env, 41)).toBe("thread-41");
    });

    it("is absent for an issue nobody linked", async () => {
        expect(await threadForIssue(env, 404)).toBeNull();
    });
});
