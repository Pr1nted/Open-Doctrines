// Is the service up?
//
// This route existed as a URL before it existed as a route: tests/net_live_
// check.cpp has probed /health for as long as it has existed, and the service
// answered 404 the whole time. That check passes on ANY status -- what it tests
// is the TLS connect, not the route -- so nothing ever said so. The same shape
// as the terms and privacy routes in documents.test.ts: written down, linked
// to, and 404 on the deployed service because nothing asked for them.
//
// So what is asserted here is the contract a monitor depends on, which is the
// part that must not drift: a 200, a machine-readable body, and no caching.

import { SELF } from "cloudflare:test";
import { describe, expect, it } from "vitest";

const ORIGIN = "https://opendoctrines-net.test.invalid";

describe("the health endpoint", () => {
    it("answers 200 with a body a monitor can read", async () => {
        const res = await SELF.fetch(`${ORIGIN}/health`);
        expect(res.status).toBe(200);
        expect(res.headers.get("content-type")).toContain("application/json");
        expect(await res.json()).toEqual({ ok: true, service: "opendoctrines-net" });
    });

    it("is never cached, so a cached 200 cannot outlive an outage", async () => {
        const res = await SELF.fetch(`${ORIGIN}/health`);
        expect(res.headers.get("cache-control")).toBe("no-store");
    });

    it("needs no session, no account and no credential", async () => {
        // Nothing is set up in this file -- no KV, no session, no Durable
        // Object. A health check that only answers once the service has state
        // is one that reports an outage during a cold start.
        const res = await SELF.fetch(`${ORIGIN}/health`);
        expect(res.status).toBe(200);
    });

    it("answers a HEAD, which is what most uptime monitors send", async () => {
        // Missed first time round, and it is the whole audience for this
        // route: the deployed service answered GET 200 and HEAD 404.
        const res = await SELF.fetch(`${ORIGIN}/health`, { method: "HEAD" });
        expect(res.status).toBe(200);
    });

    it("does not answer a POST to the same path", async () => {
        const res = await SELF.fetch(`${ORIGIN}/health`, { method: "POST" });
        expect(res.status).not.toBe(200);
    });

    it("is reachable from a browser", async () => {
        // The account screen and any status page are cross-origin to this
        // worker, so a health route without CORS is a health route they cannot
        // read.
        const res = await SELF.fetch(`${ORIGIN}/health`);
        expect(res.headers.get("access-control-allow-origin")).toBe("*");
    });
});
