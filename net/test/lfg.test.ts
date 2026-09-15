// The guidelines, as tests.
//
// Each rule pinned in the Discord channel has a test here, because the rules
// are enforced by validate() and nowhere else: if one of these passes when it
// should not, the channel's rules have quietly stopped applying.

import { describe, expect, it } from "vitest";
import { LIMITS, validate, visible, type Listing } from "../src/lfg/board.js";

const blocklist = { terms: ["badword"], exceptions: new Set<string>() };
const context = { id: "abc123", accountId: "acct_1", nick: "Vlad", now: 1_800_000_000, blocklist };

const hosting = {
    kind: "hosting",
    code: "ABCD-EFGH",
    map: "1914",
    mode: "rapid",
    turnSeconds: 120,
    slotsTotal: 6,
    slotsTaken: 2,
    language: "English",
    region: "EU",
};

function check(input: Record<string, unknown>) {
    return validate(input, context);
}

describe("a listing carries the parameters of the game", () => {
    it("accepts a complete hosting listing", () => {
        const result = check(hosting);
        expect(result.ok).toBe(true);
        if (result.ok) {
            expect(result.value.code).toBe("ABCD-EFGH");
            expect(result.value.slotsTotal).toBe(6);
            expect(result.value.expiresAt).toBe(context.now + LIMITS.defaultTtl);
        }
    });

    it("refuses one with no map", () => {
        const result = check({ ...hosting, map: "" });
        expect(result.ok).toBe(false);
    });

    it("refuses one with no turn length", () => {
        expect(check({ ...hosting, turnSeconds: undefined }).ok).toBe(false);
        expect(check({ ...hosting, mode: "longform", turnSeconds: 120 }).ok).toBe(false);
    });

    it("refuses a turn length outside the allowed range", () => {
        expect(check({ ...hosting, turnSeconds: 5 }).ok).toBe(false);
        expect(check({ ...hosting, mode: "longform", turnHours: 1000 }).ok).toBe(false);
    });

    it("refuses seats that do not add up", () => {
        expect(check({ ...hosting, slotsTaken: 9, slotsTotal: 6 }).ok).toBe(false);
        expect(check({ ...hosting, slotsTotal: 1 }).ok).toBe(false);
    });
});

describe("the tag has to be right", () => {
    it("refuses a listing with no tag", () => {
        expect(check({ ...hosting, kind: "" }).ok).toBe(false);
        expect(check({ ...hosting, kind: "both" }).ok).toBe(false);
    });

    it("refuses a hosting listing with no invite code", () => {
        expect(check({ ...hosting, code: "" }).ok).toBe(false);
    });

    it("refuses a looking listing that carries a code", () => {
        const result = check({ kind: "looking", map: "1914", mode: "rapid", turnSeconds: 90, code: "ABCD-EFGH" });
        expect(result.ok).toBe(false);
    });

    it("accepts a looking listing without seats or a code", () => {
        const result = check({ kind: "looking", map: "any", mode: "longform", turnHours: 24 });
        expect(result.ok).toBe(true);
    });
});

describe("Open Doctrines games only", () => {
    it("refuses a link in the note", () => {
        for (const note of [
            "join my server at https://example.com",
            "discord.gg/abcdef",
            "we play on hoi4.com every friday",
            "www.somewhere.net",
        ]) {
            expect(check({ ...hosting, note }).ok).toBe(false);
        }
    });

    it("keeps an ordinary note", () => {
        const result = check({ ...hosting, note: "New players welcome, we explain the rules." });
        expect(result.ok).toBe(true);
        if (result.ok) expect(result.value.note).toContain("New players welcome");
    });
});

describe("be respectful", () => {
    it("refuses a blocked word in the note", () => {
        expect(check({ ...hosting, note: "no badword allowed" }).ok).toBe(false);
    });

    it("refuses a blocked word written with lookalikes", () => {
        expect(check({ ...hosting, note: "b a d w o r d" }).ok).toBe(false);
    });

    it("refuses markup and mentions", () => {
        expect(check({ ...hosting, note: "<@everyone> come here" }).ok).toBe(false);
    });
});

describe("how long a listing lives", () => {
    it("takes a shorter life when asked", () => {
        const result = check({ ...hosting, minutes: 30 });
        expect(result.ok).toBe(true);
        if (result.ok) expect(result.value.expiresAt).toBe(context.now + 1800);
    });

    it("refuses a life longer than the maximum", () => {
        expect(check({ ...hosting, minutes: 24 * 60 }).ok).toBe(false);
        expect(check({ ...hosting, minutes: 5 }).ok).toBe(false);
    });
});

describe("what the board shows", () => {
    const base: Listing = {
        id: "1", kind: "hosting", accountId: "a", nick: "n", map: "m", mode: "rapid",
        turnSeconds: 60, language: "any", region: "any", createdAt: 100, expiresAt: 200,
    };

    it("drops hidden, closed and expired listings", () => {
        const all: Listing[] = [
            { ...base, id: "open", expiresAt: 500 },
            { ...base, id: "hidden", expiresAt: 500, hidden: true },
            { ...base, id: "closed", expiresAt: 500, closed: true },
            { ...base, id: "expired", expiresAt: 150 },
        ];
        expect(visible(all, 300).map((l) => l.id)).toEqual(["open"]);
    });

    it("shows the newest first and caps the page", () => {
        const many: Listing[] = Array.from({ length: LIMITS.page + 5 }, (_, i) => ({
            ...base, id: String(i), createdAt: i, expiresAt: 10_000,
        }));
        const shown = visible(many, 1000);
        expect(shown).toHaveLength(LIMITS.page);
        expect(shown[0]!.id).toBe(String(LIMITS.page + 4));
    });
});
