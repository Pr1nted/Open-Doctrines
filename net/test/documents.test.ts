// The two documents a player is asked to agree to, served by the worker.
//
// Both are imported from the repository files rather than pasted here, which
// is the same trick index.ts uses: the published text and the tree cannot
// drift apart. What these tests add is that the ROUTES exist -- the terms were
// written, committed, linked to from the Account screen, and answered 404 on
// the deployed service for a week, because nothing anywhere asked for them.

import { env, SELF } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import PRIVACY_POLICY from "../PRIVACY.md";
import TERMS_OF_USE from "../TERMS.md";

// Any absolute URL routes; only the path is read. The links the service hands
// out are built from its own ISSUER, which under test is whatever .dev.vars
// says rather than the deployed host.
const ORIGIN = "https://opendoctrines-net.test.invalid";
const ISSUER = (env as unknown as { ISSUER: string }).ISSUER;

describe("the documents the game links to", () => {
    it("serves the terms of use, verbatim", async () => {
        const res = await SELF.fetch(`${ORIGIN}/terms`);
        expect(res.status).toBe(200);
        expect(res.headers.get("content-type")).toContain("text/markdown");
        expect(await res.text()).toBe(TERMS_OF_USE);
    });

    it("serves the privacy policy, verbatim", async () => {
        const res = await SELF.fetch(`${ORIGIN}/privacy`);
        expect(res.status).toBe(200);
        expect(res.headers.get("content-type")).toContain("text/markdown");
        expect(await res.text()).toBe(PRIVACY_POLICY);
    });

    // The game builds these URLs itself (AccountClient::termsUrl), so the
    // fields are for anything else reading the service. They are also the only
    // machine-readable statement that both documents exist.
    it("advertises both from the service root", async () => {
        const res = await SELF.fetch(`${ORIGIN}/`);
        expect(res.status).toBe(200);
        const body = await res.json() as { privacy: string; terms: string };
        expect(body.privacy).toBe(`${ISSUER}/privacy`);
        expect(body.terms).toBe(`${ISSUER}/terms`);
    });

    // Reachable is not the same as present. An empty file would pass every
    // check above while leaving a player agreeing to nothing at all.
    it("both say something, and say who to ask", async () => {
        for (const doc of [TERMS_OF_USE, PRIVACY_POLICY]) {
            expect(doc.length).toBeGreaterThan(2000);
            expect(doc).toContain("opendoctrines@gmail.com");
        }
    });
});

// ── THE SAME URL, ANSWERED TWO WAYS ──
//
// Markdown is right for a program and wrong for a person: a browser shows the
// source, ## and ** included, which is what anyone clicking "Privacy policy"
// in the Account screen actually got. A browser announces itself in Accept, so
// it is sent to the rendered copy instead.
//
// The direction that matters is the DEFAULT. A program that gets an
// unrequested redirect to a styled page breaks; a person handed markdown can
// still read it. So anything that does not explicitly ask for HTML must keep
// getting the bytes.
describe("a browser is sent to the rendered copy", () => {
    const DOCS = (env as unknown as { DOCS_BASE?: string }).DOCS_BASE;
    const browser = { headers: { accept: "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8" } };

    it("is configured with somewhere to send it", () => {
        // If this is unset the redirect cannot happen at all, and the rest of
        // this block would pass by doing nothing.
        expect(DOCS).toBeTruthy();
    });

    for (const path of ["/privacy", "/terms"]) {
        it(`redirects a browser asking for ${path}`, async () => {
            const res = await SELF.fetch(`${ORIGIN}${path}`, { ...browser, redirect: "manual" });
            expect(res.status).toBe(302);
            expect(res.headers.get("location")).toBe(`${DOCS}${path}`);
        });

        it(`still serves ${path} as markdown to anything else`, async () => {
            const expected = path === "/privacy" ? PRIVACY_POLICY : TERMS_OF_USE;
            // No Accept at all -- curl, and most library HTTP clients.
            const bare = await SELF.fetch(`${ORIGIN}${path}`);
            expect(bare.status).toBe(200);
            expect(await bare.text()).toBe(expected);

            // `*/*` is "anything", which is not "I am a browser". curl sends
            // exactly this, and it must not be redirected.
            const any = await SELF.fetch(`${ORIGIN}${path}`, { headers: { accept: "*/*" } });
            expect(any.status).toBe(200);
            expect(await any.text()).toBe(expected);

            // Something that wants the source on purpose.
            const md = await SELF.fetch(`${ORIGIN}${path}`, { headers: { accept: "text/markdown" } });
            expect(md.status).toBe(200);
            expect(await md.text()).toBe(expected);
        });
    }

    // Matching "html" loosely would redirect this, because the string contains
    // it. Only a real text/html entry counts.
    it("is not fooled by a media type that merely contains 'html'", async () => {
        const res = await SELF.fetch(`${ORIGIN}/privacy`, {
            headers: { accept: "application/xhtml+xml" }, redirect: "manual",
        });
        expect(res.status).toBe(200);
        expect(await res.text()).toBe(PRIVACY_POLICY);
    });
});
