// Keeping a free-plan game server awake: see src/hosting/keepalive.ts.

import { describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import {
    KEEPALIVE_MAX_TARGETS, keepaliveDue, keepaliveTargets, pingKeepalive,
} from "../src/hosting/keepalive.js";

describe("keepalive schedule", () => {
    it("fires every ten minutes, well inside Render's fifteen", () => {
        const at = (m: number) => new Date(Date.UTC(2026, 9, 10, 12, m));
        const fired = [...Array(60).keys()].filter((m) => keepaliveDue(at(m)));
        expect(fired).toEqual([0, 10, 20, 30, 40, 50]);
        for (let i = 1; i < fired.length; i++) expect(fired[i]! - fired[i - 1]!).toBeLessThan(15);
    });
});

describe("keepalive targets", () => {
    it("reads a comma- or space-separated list", () => {
        expect(keepaliveTargets("https://a.onrender.com/healthz, https://b.onrender.com/healthz"))
            .toEqual(["https://a.onrender.com/healthz", "https://b.onrender.com/healthz"]);
        expect(keepaliveTargets("https://a.onrender.com/healthz https://a.onrender.com/healthz"))
            .toEqual(["https://a.onrender.com/healthz"]);
    });
    it("is nothing when unset or empty", () => {
        expect(keepaliveTargets(undefined)).toEqual([]);
        expect(keepaliveTargets("")).toEqual([]);
    });
    it("refuses anything that is not plain https", () => {
        expect(keepaliveTargets("http://a.onrender.com/healthz")).toEqual([]);
        expect(keepaliveTargets("ftp://x")).toEqual([]);
        expect(keepaliveTargets("not a url")).toEqual([]);
        expect(keepaliveTargets("https://user:pw@a.onrender.com/")).toEqual([]);
    });
    it("is capped", () => {
        const many = [...Array(20).keys()].map((i) => `https://s${i}.onrender.com/healthz`).join(",");
        expect(keepaliveTargets(many)).toHaveLength(KEEPALIVE_MAX_TARGETS);
    });
});

describe("keepalive ping", () => {
    const env = (urls: string) => ({ KEEPALIVE_URLS: urls }) as unknown as Env;

    it("requests every target and counts the ones that answered", async () => {
        const asked: string[] = [];
        const fetcher = async (url: string) => {
            asked.push(url);
            return new Response("ok", { status: url.includes("down") ? 503 : 200 });
        };
        const n = await pingKeepalive(env("https://up.onrender.com/healthz,https://down.onrender.com/healthz"), fetcher);
        expect(asked.sort()).toEqual(["https://down.onrender.com/healthz", "https://up.onrender.com/healthz"]);
        expect(n).toBe(1);
    });
    it("never throws, whatever the server does", async () => {
        const fetcher = async () => { throw new Error("connection refused"); };
        await expect(pingKeepalive(env("https://gone.onrender.com/healthz"), fetcher)).resolves.toBe(0);
    });
    it("requests nothing when none are configured", async () => {
        let calls = 0;
        await pingKeepalive(env(""), async () => { calls++; return new Response("ok"); });
        expect(calls).toBe(0);
    });
});
