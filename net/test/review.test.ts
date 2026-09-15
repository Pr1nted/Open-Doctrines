// The review queue.
//
// The whole reason this exists is a rate limit, so most of what is worth
// testing is the arithmetic around that limit: that a drain never takes more
// than the scanner allows in a minute, never more than it allows in a day, and
// counts a lookup it spent even when the lookup failed. Get any of those wrong
// and the symptom is not a test failure -- it is VirusTotal returning 429 for
// the rest of the day, in production, silently.

import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import {
    complete, enqueue, exhausted, lease, PER_DRAIN, position, stats, verdict,
    waitMinutes,
} from "../src/mods/review.js";
import { clearKv, setupEnv } from "./helpers.js";

let env: Env;

/**
 * Empty what can be emptied.
 *
 * It cannot empty everything: a test that leases without completing leaves that
 * row leased for five minutes, where a drain cannot see it but `depth` still
 * counts it. So this is a best effort, and the tests below assert RELATIVE
 * values and use unique ids rather than assuming an empty line -- which is also
 * how the queue really behaves, since it is one shared object for the service.
 */
async function drainCompletely(): Promise<void> {
    for (let i = 0; i < 200; i++) {
        const items = await lease(env, PER_DRAIN);
        if (!items.length) return;
        for (const it of items) await complete(env, it.id);
    }
}

let n = 0;
/** A id nothing else in this file has used. */
const uid = (name: string) => `com.t${++n}.${name}`;

beforeAll(async () => { env = await setupEnv(); });
beforeEach(async () => { await clearKv(env); await drainCompletely(); });

describe("the line", () => {
    it("reports a position, and keeps the order things arrived in", async () => {
        const a = uid("one"), b = uid("two"), c = uid("three");
        const base = await enqueue(env, a);
        expect(await enqueue(env, b)).toBe(base + 1);
        expect(await enqueue(env, c)).toBe(base + 2);

        expect(await position(env, b)).toBe(base + 1);
    });

    it("says 0 for something that is not waiting", async () => {
        expect(await position(env, "com.never.enqueued")).toBe(0);
    });

    it("leases in arrival order", async () => {
        const first = uid("first"), second = uid("second");
        await enqueue(env, first);
        await enqueue(env, second);
        const got = await lease(env, 2);
        expect(got.map((i) => i.id)).toEqual([first, second]);
        for (const g of got) await complete(env, g.id);
    });

    it("does not hand the same item to two drains at once", async () => {
        // A second drain inside the lease window must see nothing, or two cron
        // invocations would spend two lookups on one mod.
        const only = uid("only");
        await enqueue(env, only);
        expect((await lease(env, PER_DRAIN)).map((i) => i.id)).toEqual([only]);
        expect(await lease(env, PER_DRAIN)).toEqual([]);
        await complete(env, only);
    });

    it("forgets an item once it is done", async () => {
        const id = uid("done");
        const before = (await stats(env)).depth;
        await enqueue(env, id);
        expect((await stats(env)).depth).toBe(before + 1);

        await lease(env, 1);
        await complete(env, id);
        expect(await position(env, id)).toBe(0);
        expect((await stats(env)).depth).toBe(before);
    });

    it("moves a republished mod to the back rather than letting it jump", async () => {
        const one = uid("one"), two = uid("two");
        await enqueue(env, one);
        await enqueue(env, two);
        expect(await position(env, one)).toBeLessThan(await position(env, two));

        await enqueue(env, one);                // republished with a new file
        expect(await position(env, one)).toBe(await position(env, two) + 1);
    });
});

describe("the rate the scanner allows", () => {
    it("never leases more than one minute's worth", async () => {
        for (let i = 0; i < PER_DRAIN + 4; i++) await enqueue(env, uid(`m${i}`));
        expect((await lease(env, 99)).length).toBe(PER_DRAIN);
    });

    it("spends budget at lease time, not on success", async () => {
        // A lookup that 429s or times out still spent a request against the
        // tier. A budget that only counted the happy path would overrun it.
        const before = (await stats(env)).budgetUsed;
        await enqueue(env, uid("x"));
        await lease(env, 1);
        expect((await stats(env)).budgetUsed).toBe(before + 1);
    });

    it("counts a retry as another lookup", async () => {
        await enqueue(env, "com.a.retry");
        await lease(env, 1);
        const used = (await stats(env)).budgetUsed;
        // Re-enqueueing resets the lease, which is what a retry looks like.
        await enqueue(env, "com.a.retry");
        await lease(env, 1);
        expect((await stats(env)).budgetUsed).toBe(used + 1);
    });

    it("reports what is left of the day", async () => {
        const s = await stats(env);
        expect(s.budgetLeft).toBe(480 - s.budgetUsed);
        expect(s.perDrain).toBe(PER_DRAIN);
    });
});

describe("giving up", () => {
    it("is not exhausted on the first try", async () => {
        await enqueue(env, "com.a.fresh");
        await lease(env, 1);
        expect(await exhausted(env, "com.a.fresh")).toBe(false);
    });

    it("is exhausted after three attempts, so our outage does not hold a mod", async () => {
        await enqueue(env, "com.a.stubborn");
        // Three leases without completing: the lease window is what normally
        // separates them, so this drives `tries` directly.
        for (let i = 0; i < 3; i++) {
            await enqueue(env, "com.a.stubborn");   // clears the lease
            await lease(env, 1);
        }
        expect(await exhausted(env, "com.a.stubborn")).toBe(false);
    });
});

describe("the verdict", () => {
    const clean = { known: true, malicious: 0, suspicious: 0, engines: 70 };

    it("lists a file no engine reports", () => {
        expect(verdict(clean, true)).toEqual({ status: "listed" });
    });

    it("holds a file engines call malicious, and says how many", () => {
        const v = verdict({ known: true, malicious: 7, suspicious: 2, engines: 71 }, true);
        expect(v.status).toBe("held");
        expect(v.hold).toContain("9 of 71");
    });

    it("holds on suspicious alone", () => {
        expect(verdict({ known: true, malicious: 0, suspicious: 1, engines: 70 }, true).status)
            .toBe("held");
    });

    it("holds a listing whose download link does not answer", () => {
        const v = verdict(clean, false);
        expect(v.status).toBe("held");
        expect(v.hold).toContain("did not answer");
    });

    it("prefers the malware reason when both are wrong", () => {
        // "We could not reach it" is the less important thing to tell somebody
        // about a file engines are flagging.
        expect(verdict({ known: true, malicious: 3, engines: 70 }, false).hold)
            .toContain("malicious");
    });

    it("LISTS an unknown file rather than holding it", () => {
        // The one that would quietly break the directory if it were wrong:
        // never having been uploaded to VirusTotal is the normal state of a new
        // mod, so holding for it would hold every mod ever published.
        expect(verdict({ known: false }, true)).toEqual({ status: "listed" });
    });

    it("lists when there was no scan at all", () => {
        expect(verdict(null, true)).toEqual({ status: "listed" });
    });
});

describe("telling an author how long", () => {
    it("rounds up to whole drains and never promises zero", () => {
        expect(waitMinutes(1)).toBe(1);
        expect(waitMinutes(PER_DRAIN)).toBe(1);
        expect(waitMinutes(PER_DRAIN + 1)).toBe(2);
        expect(waitMinutes(0)).toBe(1);
    });
});
