// What the service is allowed to know about how much the game is played.
//
// The point of these tests is not that the arithmetic works. It is that the
// endpoint CANNOT become a record of a person: the game promises in
// PRIVACY.md that there is no usage reporting anywhere in it, and this sits on
// the other side of that line only for as long as it stays aggregate. A future
// change that adds a pseudonym, a join code or a nickname to a row would pass
// every arithmetic check and quietly break the promise, so that is what is
// asserted here.

import { env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { createAccount, identSubHash } from "../src/accounts/store.js";
import { setBadge } from "../src/accounts/badges.js";
import { issueSessionToken } from "../src/auth/token.js";

const ORIGIN = "https://opendoctrines-net.test.invalid";
const KV = (env as unknown as { OD_ACCOUNTS: KVNamespace }).OD_ACCOUNTS;

const today = () => new Date().toISOString().slice(0, 10);

async function row(s: number, j: number, p: number, day = today()) {
    await KV.put(`stat:${day}:${crypto.randomUUID()}`, JSON.stringify({ s, j, p }));
}

beforeEach(async () => {
    // BOTH prefixes. Clearing only one let a bucket posted by an earlier test
    // survive into the next and inflate its count -- a test failing for a
    // reason that had nothing to do with what it was testing.
    // "acct:" joins the sweep now that the endpoint can report how many there
    // are: an account left behind by an earlier test would be counted by the
    // next one and the number would be right for the wrong reason.
    for (const prefix of ["stat:", "usage:", "acct:", "nick:", "ident:"]) {
        const listed = await KV.list({ prefix });
        for (const k of listed.keys) await KV.delete(k.name);
    }
});

describe("what a day of play looks like", () => {
    it("says nothing happened when nothing did", async () => {
        const res = await SELF.fetch(`${ORIGIN}/stats?days=1`);
        expect(res.status).toBe(200);
        const body = await res.json() as { days: Array<{ sessions: number }> };
        expect(body.days[0]!.sessions).toBe(0);
    });

    it("counts sessions, arrivals and the biggest lobby", async () => {
        await row(600, 3, 3);
        await row(120, 1, 1);
        await row(900, 7, 5);
        const res = await SELF.fetch(`${ORIGIN}/stats?days=1`);
        const d = (await res.json() as { days: Array<Record<string, number>> }).days[0]!;
        expect(d.sessions).toBe(3);
        expect(d.joins).toBe(11);
        expect(d.biggestLobby).toBe(5);
        expect(d.longestSeconds).toBe(900);
    });

    it("separates a lobby that filled from one that was abandoned", async () => {
        // The two are the same "session" and completely different problems:
        // one is a game, the other is somebody who opened a lobby and gave up
        // waiting. An aggregate that merged them would hide the second.
        await row(30, 0, 0);
        await row(30, 0, 0);
        await row(600, 4, 4);
        const res = await SELF.fetch(`${ORIGIN}/stats?days=1`);
        const d = (await res.json() as { days: Array<Record<string, number>> }).days[0]!;
        expect(d.sessions).toBe(3);
        expect(d.sessionsWithAPlayer).toBe(1);
    });

    it("uses the median, so one abandoned long-form game cannot swamp it", async () => {
        // A long-form session can legitimately sit for a week. Under a mean,
        // one of those makes every other number meaningless.
        await row(300, 2, 2);
        await row(360, 2, 2);
        await row(420, 2, 2);
        await row(604800, 1, 1);              // a week
        const res = await SELF.fetch(`${ORIGIN}/stats?days=1`);
        const d = (await res.json() as { days: Array<Record<string, number>> }).days[0]!;
        expect(d.medianSeconds).toBeLessThan(1000);
        expect(d.longestSeconds).toBe(604800);
    });

    it("bounds the window it will look at", async () => {
        const res = await SELF.fetch(`${ORIGIN}/stats?days=99999`);
        const body = await res.json() as { days: unknown[] };
        expect(body.days.length).toBeLessThanOrEqual(90);
    });
});

describe("THE LINE THIS MUST NOT CROSS", () => {
    it("returns no individual rows, only counts", async () => {
        await row(600, 3, 3);
        const text = await (await SELF.fetch(`${ORIGIN}/stats?days=1`)).text();
        // The raw row keys are "s"/"j"/"p"; the reply speaks in aggregates.
        expect(text).not.toContain('"s"');
        expect(text).toContain("sessions");
    });

    it("carries nothing that could name a person or a game", async () => {
        await row(600, 3, 3);
        const text = (await (await SELF.fetch(`${ORIGIN}/stats?days=1`)).text()).toLowerCase();
        for (const forbidden of ["psid", "nickname", "account", "code", "ip", "token", "issuer"]) {
            expect(text).not.toContain(forbidden);
        }
    });

    it("a stored row itself holds only three numbers", async () => {
        // If this ever grows a field, it should be a deliberate act with this
        // test in front of whoever does it.
        await row(600, 3, 3);
        const listed = await KV.list({ prefix: "stat:" });
        const raw = await KV.get(listed.keys[0]!.name);
        expect(Object.keys(JSON.parse(raw!)).sort()).toEqual(["j", "p", "s"]);
    });
});

// ── THE OPT-IN PLAY REPORT ──
//
// This one exists because a player turned it on. Everything asserted below is
// about keeping it worth the trust that took: a fixed set of values, nothing
// that could carry an identifier, and no way for a caller to widen it.

describe("what a player who opted in may report", () => {
    it("accepts the buckets the service records", async () => {
        for (const bucket of ["<1m", "1-5m", "5-15m", "15-60m", "60m+"]) {
            const res = await SELF.fetch(`${ORIGIN}/usage`, {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ bucket, surface: "web" }),
            });
            expect(res.status, bucket).toBe(200);
        }
    });

    it("REFUSES ANYTHING THAT IS NOT ONE OF THEM", async () => {
        // The point is not tidiness. A free-text duration is a field somebody
        // can put a nickname, an address or a session code into, and it would
        // be stored and served back out by /stats.
        const bad = [
            "7 minutes", "", "<1m ", "vlad@example.com", "ABCD-2345",
            "<script>", "0", "9999m",
        ];
        for (const bucket of bad) {
            const res = await SELF.fetch(`${ORIGIN}/usage`, {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ bucket, surface: "web" }),
            });
            expect(res.status, JSON.stringify(bucket)).toBe(400);
        }
    });

    it("refuses a surface it does not know", async () => {
        const res = await SELF.fetch(`${ORIGIN}/usage`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ bucket: "5-15m", surface: "vlads-laptop" }),
        });
        expect(res.status).toBe(400);
    });

    it("shows up in /stats as counts per bucket and nothing more", async () => {
        for (const b of ["5-15m", "5-15m", "60m+"]) {
            await SELF.fetch(`${ORIGIN}/usage`, {
                method: "POST",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({ bucket: b, surface: "web" }),
            });
        }
        const res = await SELF.fetch(`${ORIGIN}/stats?days=1`);
        const d = (await res.json() as { days: Array<{ plays: Record<string, number> }> }).days[0]!;
        expect(d.plays["5-15m"]).toBe(2);
        expect(d.plays["60m+"]).toBe(1);
    });

    it("a stored report holds only a bucket and a surface", async () => {
        await SELF.fetch(`${ORIGIN}/usage`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ bucket: "1-5m", surface: "desktop" }),
        });
        const listed = await KV.list({ prefix: "usage:" });
        const raw = await KV.get(listed.keys[0]!.name);
        expect(Object.keys(JSON.parse(raw!)).sort()).toEqual(["b", "f"]);
    });
});

describe("deleting it", () => {
    it("is refused to anyone without the developer badge, and denies existing", async () => {
        const res = await SELF.fetch(`${ORIGIN}/usage/forget`, { method: "POST" });
        // 404, not 403: the route does not confirm itself to a stranger.
        expect(res.status).toBe(404);
    });
});

// ── who is allowed to know how big the service is ──────────────────────────
//
// countAccounts() has its own tests; these are about EXPOSURE. The number was
// already reachable at /moderation/overview, which is moderator-gated and
// answers 404 rather than 403 so a prober cannot even confirm the route. This
// endpoint is public, so putting the same figure here unconditionally would
// have undone that decision by a side door -- and it would have done it
// silently, because the arithmetic would still have been correct.

describe("how many accounts exist", () => {
    async function moderator(): Promise<string> {
        const e = env as unknown as Parameters<typeof createAccount>[0];
        const made = await createAccount(e, "discord", await identSubHash(e, "discord", "mod-1"), "Warden");
        await setBadge(e, made.account!.id, "developer", true);
        return issueSessionToken(e, made.account!.id);
    }

    it("DOES NOT tell an anonymous caller, even when there are accounts to count", async () => {
        const e = env as unknown as Parameters<typeof createAccount>[0];
        await createAccount(e, "discord", await identSubHash(e, "discord", "p1"), "Player");

        const res = await SELF.fetch(`${ORIGIN}/stats?days=1`);
        expect(res.status).toBe(200);
        const body = await res.json() as Record<string, unknown>;
        // Absent, not zero. A dashboard has to be able to tell "you may not ask"
        // apart from "nobody has signed up", and 0 says the second.
        expect(body).not.toHaveProperty("accounts");
        expect(body).toHaveProperty("days");
    });

    it("does not tell a caller whose token is not a moderator's", async () => {
        const e = env as unknown as Parameters<typeof createAccount>[0];
        const made = await createAccount(e, "discord", await identSubHash(e, "discord", "p2"), "Ordinary");
        const token = await issueSessionToken(e, made.account!.id);

        const res = await SELF.fetch(`${ORIGIN}/stats?days=1`, {
            headers: { authorization: `Bearer ${token}` },
        });
        expect(await res.json()).not.toHaveProperty("accounts");
    });

    it("tells a moderator, and says whether the figure is the whole truth", async () => {
        const token = await moderator();
        const e = env as unknown as Parameters<typeof createAccount>[0];
        await createAccount(e, "discord", await identSubHash(e, "discord", "p3"), "Another");

        const res = await SELF.fetch(`${ORIGIN}/stats?days=1`, {
            headers: { authorization: `Bearer ${token}` },
        });
        const body = await res.json() as { accounts: number; accountsExact: boolean };
        // The moderator and the player: the count is of accounts, not of
        // sign-ups by somebody else.
        expect(body.accounts).toBe(2);
        expect(body.accountsExact).toBe(true);
    });
});
