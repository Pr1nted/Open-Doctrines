// The announcement board's editing model.
//
// The board is edited live, in front of everybody, usually shortly before a
// tournament. So the cases that matter are the ones where somebody is in a
// hurry: taking something down and putting it back, correcting a typo in a
// withdrawn notice, and every way a malformed entry is refused at the moment it
// is written rather than silently dropped by every client later.

import { beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import { clearKv, setupEnv } from "./helpers.js";
import {
    applyEdit, forClients, readAll, validate, type Announcement,
} from "../src/announcements/store.js";

let env: Env;

const NOW = 1789000000;

function item(over: Partial<Announcement> = {}): Announcement {
    return { id: "t1", title: "Tournament", body: "Saturday.", ...over };
}

beforeEach(async () => {
    env = await setupEnv();
    await clearKv(env);
});

describe("what may be posted", () => {
    it("accepts a plain announcement", () => {
        expect(validate(item()).ok).toBe(true);
    });

    it("needs words", () => {
        const r = validate(item({ title: "", body: "" }));
        expect(r.ok).toBe(false);
    });

    it("needs an id that can be a key", () => {
        expect(validate(item({ id: "" })).ok).toBe(false);
        expect(validate(item({ id: "has spaces" })).ok).toBe(false);
        expect(validate(item({ id: "a/../b" })).ok).toBe(false);
        expect(validate(item({ id: "ok-1.2_3" })).ok).toBe(true);
    });

    it("bounds every field", () => {
        expect(validate(item({ title: "x".repeat(81) })).ok).toBe(false);
        expect(validate(item({ body: "x".repeat(1201) })).ok).toBe(false);
        expect(validate(item({ id: "x".repeat(65) })).ok).toBe(false);
    });

    describe("buttons", () => {
        it("takes the three actions the game implements", () => {
            expect(validate(item({ buttonLabel: "Join", buttonAction: "join",
                                   buttonParam: "ABCD-1234" })).ok).toBe(true);
            expect(validate(item({ buttonLabel: "Chat", buttonAction: "community" })).ok).toBe(true);
            expect(validate(item({ buttonLabel: "Sign in", buttonAction: "account" })).ok).toBe(true);
        });

        it("refuses an action the game does not have", () => {
            // The client refuses this too. Refusing it HERE is what tells the
            // author, instead of leaving them with a button nobody can see.
            const r = validate(item({ buttonLabel: "Run", buttonAction: "exec" as never }));
            expect(r.ok).toBe(false);
        });

        it("refuses a label with no action, and an action with no label", () => {
            expect(validate(item({ buttonLabel: "Go" })).ok).toBe(false);
            expect(validate(item({ buttonAction: "community" })).ok).toBe(false);
        });

        it("requires a join button to carry an invite code", () => {
            expect(validate(item({ buttonLabel: "Join", buttonAction: "join" })).ok).toBe(false);
            expect(validate(item({ buttonLabel: "Join", buttonAction: "join",
                                   buttonParam: "; rm -rf /" })).ok).toBe(false);
        });

        it("refuses a parameter on an action that takes none", () => {
            expect(validate(item({ buttonLabel: "Chat", buttonAction: "community",
                                   buttonParam: "anything" })).ok).toBe(false);
        });
    });

    it("bounds timestamps to plausible ones", () => {
        expect(validate(item({ postedAt: 5 })).ok).toBe(false);
        expect(validate(item({ until: 99999999999 })).ok).toBe(false);
        expect(validate(item({ postedAt: NOW, until: NOW + 3600 })).ok).toBe(true);
    });
});

describe("editing the board", () => {
    it("stamps a post with the time when the author did not", async () => {
        const r = await applyEdit(env, { op: "put", item: item() }, NOW);
        expect(r.ok).toBe(true);
        const all = await readAll(env);
        expect(all[0]?.postedAt).toBe(NOW);
    });

    it("takes something down and puts it back", async () => {
        await applyEdit(env, { op: "put", item: item() }, NOW);
        expect(forClients(await readAll(env), NOW)).toHaveLength(1);

        await applyEdit(env, { op: "hide", id: "t1" }, NOW);
        expect(forClients(await readAll(env), NOW)).toHaveLength(0);
        // Hidden, not destroyed: it is still there to be restored.
        expect(await readAll(env)).toHaveLength(1);

        await applyEdit(env, { op: "show", id: "t1" }, NOW);
        expect(forClients(await readAll(env), NOW)).toHaveLength(1);
    });

    it("keeps an entry withdrawn when it is edited", async () => {
        // Correcting a typo in something you have taken down must not put it
        // back in front of everybody.
        await applyEdit(env, { op: "put", item: item() }, NOW);
        await applyEdit(env, { op: "hide", id: "t1" }, NOW);
        await applyEdit(env, { op: "put", item: item({ title: "Tournament (fixed)" }) }, NOW);

        const all = await readAll(env);
        expect(all[0]?.title).toBe("Tournament (fixed)");
        expect(all[0]?.hidden).toBe(true);
        expect(forClients(all, NOW)).toHaveLength(0);
    });

    it("purges only when asked", async () => {
        await applyEdit(env, { op: "put", item: item() }, NOW);
        await applyEdit(env, { op: "purge", id: "t1" }, NOW);
        expect(await readAll(env)).toHaveLength(0);
    });

    it("refuses an edit to something that is not there", async () => {
        const r = await applyEdit(env, { op: "hide", id: "nope" }, NOW);
        expect(r.ok).toBe(false);
    });

    it("replaces by id rather than piling up duplicates", async () => {
        await applyEdit(env, { op: "put", item: item() }, NOW);
        await applyEdit(env, { op: "put", item: item({ title: "Again" }) }, NOW);
        expect(await readAll(env)).toHaveLength(1);
    });
});

describe("what the game is served", () => {
    it("drops expired entries without anybody taking them down", async () => {
        await applyEdit(env, { op: "put", item: item({ id: "over", until: NOW - 1 }) }, NOW);
        await applyEdit(env, { op: "put", item: item({ id: "on", until: NOW + 3600 }) }, NOW);
        const shown = forClients(await readAll(env), NOW);
        expect(shown.map((a) => a.id)).toEqual(["on"]);
    });

    it("puts the newest first", async () => {
        await applyEdit(env, { op: "put", item: item({ id: "old", postedAt: NOW - 500 }) }, NOW);
        await applyEdit(env, { op: "put", item: item({ id: "new", postedAt: NOW }) }, NOW);
        expect(forClients(await readAll(env), NOW).map((a) => a.id)).toEqual(["new", "old"]);
    });

    it("caps how many the menu is asked to draw", async () => {
        for (let i = 0; i < 20; i++)
            await applyEdit(env, { op: "put", item: item({ id: `a${i}` }) }, NOW);
        expect(forClients(await readAll(env), NOW).length).toBeLessThanOrEqual(8);
    });

    it("reads a corrupt record as an empty board, never an error", async () => {
        await env.OD_ACCOUNTS.put("announcements", "{not json");
        expect(await readAll(env)).toEqual([]);
    });
});
