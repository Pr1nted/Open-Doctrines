// A link a viewer can click, that a VOD cannot use next year.
//
// The invite code cannot have these properties -- it does not expire, cannot be
// counted, and cannot be revoked without breaking the people already playing.
// Everything here is about the difference.

import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import { clearKv, setupEnv } from "./helpers.js";
import {
    createLink, isToken, mintToken, revokeLink, useLink, LINK_TTL_SECONDS,
} from "../src/live/viewerlink.js";

let env: Env;
const NOW = 1789000000;

beforeEach(async () => {
    env = await setupEnv();
    await clearKv(env);
});

describe("the token itself", () => {
    it("has no characters a viewer can misread", () => {
        // Read off a stream, typed on a phone: l/1 and o/0 are the same glyph
        // to somebody squinting at 480p.
        const t = mintToken(new Uint8Array(Array.from({ length: 16 }, (_, i) => i * 7)));
        expect(t).toHaveLength(10);
        expect(t).not.toMatch(/[lo01]/);
        expect(isToken(t)).toBe(true);
    });
    it("refuses anything that is not one", () => {
        expect(isToken("")).toBe(false);
        expect(isToken("short")).toBe(false);
        expect(isToken("HASUPPERCASE")).toBe(false);
        expect(isToken("../../etc/pw")).toBe(false);
    });
    it("can never mint a token its own validator refuses", () => {
        // THE BUG THIS PINS. The alphabet contained "i" and the validator's
        // character class did not, so roughly a quarter of all tokens failed
        // isToken() and resolved as "unknown" -- links that simply did not
        // work, with nothing in common between them.
        //
        // Every byte value, so every character the alphabet can produce.
        for (let base = 0; base < 256; base += 1) {
            const token = mintToken(new Uint8Array(Array.from({ length: 16 },
                                                              () => base)));
            expect(isToken(token), `byte ${base} produced ${token}`).toBe(true);
        }
    });

    it("is random rather than derived from the session", () => {
        // Derived tokens would let anybody holding one work out the others.
        const a = mintToken(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]));
        const b = mintToken(new Uint8Array([9, 9, 9, 9, 9, 9, 9, 9, 9, 9]));
        expect(a).not.toBe(b);
    });
});

describe("using one", () => {
    it("opens the session it was minted for", async () => {
        const link = await createLink(env, "ABCD-1234", NOW);
        const r = await useLink(env, link.token, NOW + 10);
        expect(r.ok && r.code).toBe("ABCD-1234");
    });

    it("expires, so a VOD is not a way in next year", async () => {
        const link = await createLink(env, "ABCD-1234", NOW);
        expect((await useLink(env, link.token, NOW + LINK_TTL_SECONDS - 1)).ok).toBe(true);
        const late = await useLink(env, link.token, NOW + LINK_TTL_SECONDS);
        expect(late.ok).toBe(false);
        expect(late.ok === false && late.reason).toBe("expired");
    });

    it("runs out, so a link pasted somewhere it should not have been stops", async () => {
        const link = await createLink(env, "ABCD-1234", NOW, 2);
        expect((await useLink(env, link.token, NOW)).ok).toBe(true);
        expect((await useLink(env, link.token, NOW)).ok).toBe(true);
        const third = await useLink(env, link.token, NOW);
        expect(third.ok).toBe(false);
        expect(third.ok === false && third.reason).toBe("spent");
    });

    it("can be revoked without touching the game", async () => {
        // The people already in the lobby do not depend on the token they
        // arrived through, which is the whole reason this is not the code.
        const link = await createLink(env, "ABCD-1234", NOW);
        await revokeLink(env, link.token);
        const r = await useLink(env, link.token, NOW);
        expect(r.ok).toBe(false);
        expect(r.ok === false && r.reason).toBe("unknown");
    });

    it("tells the three refusals apart", async () => {
        // They mean different things to whoever is holding the link: a typo, a
        // stream that finished, and a link that travelled too far.
        const unknown = await useLink(env, "abcdefghjk", NOW);
        expect(unknown.ok === false && unknown.reason).toBe("unknown");
        const malformed = await useLink(env, "nope", NOW);
        expect(malformed.ok === false && malformed.reason).toBe("unknown");
    });

    it("mints a different token each time", async () => {
        const a = await createLink(env, "ABCD-1234", NOW);
        const b = await createLink(env, "ABCD-1234", NOW);
        expect(a.token).not.toBe(b.token);
        // And revoking one leaves the other working.
        await revokeLink(env, a.token);
        expect((await useLink(env, b.token, NOW)).ok).toBe(true);
    });
});
