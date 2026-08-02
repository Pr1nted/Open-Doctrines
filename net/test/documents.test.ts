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
