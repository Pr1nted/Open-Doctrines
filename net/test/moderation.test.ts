// Reporting a player, and the powers that follow from it.
//
// The dangerous half of this feature is not the reporting, it is the deciding:
// a route that can ban any account, reachable from a game menu. So most of this
// file is about who may reach it and what it does when they do.

import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import type { Env } from "../src/env.js";
import type { Account } from "../src/accounts/store.js";
import {
    actOnAccount, announce, checkReporterQuota, decide, describeDays, fileReport,
    findAccount, getReport, isModerator, profileOf,
    LIMITS, listReports, parseReportInput, REASONS,
    type Report, type Rejected,
} from "../src/moderation/reports.js";
import { normalize } from "../src/accounts/nickname.js";
import { clearKv, setupEnv } from "./helpers.js";

let env: Env;

/** A stand-in account, written straight to the store the way the service does. */
async function makeAccount(id: string, nick: string, badges: string[] = []): Promise<Account> {
    const account = {
        id, nick, nickNorm: nick.toLowerCase(), created: 1, nickChangedAt: 0,
        badges: badges as Account["badges"], identities: [],
    } as Account;
    await env.OD_ACCOUNTS.put(`acct:${id}`, JSON.stringify(account));
    return account;
}

function input(over: Record<string, unknown> = {}) {
    return {
        accusedId: "acct-bad", reason: "harassment",
        note: "Kept at it after being asked to stop.",
        message: "you are worthless and should quit",
        context: ["earlier line", "another earlier line"],
        server: "Vlad's game", ...over,
    };
}

/** Set or clear the one optional binding, without an unsound cast. */
function setWebhook(url: string | undefined): void {
    if (url === undefined) delete env.MODERATION_DISCORD_WEBHOOK;
    else env.MODERATION_DISCORD_WEBHOOK = url;
}

beforeAll(async () => { env = await setupEnv(); });
beforeEach(async () => {
    await clearKv(env);
    vi.restoreAllMocks();
    setWebhook(undefined);
});

describe("what a report must contain", () => {
    it("takes an ordinary one", () => {
        const r = parseReportInput(input()) as ReturnType<typeof parseReportInput>;
        expect((r as { reason: string }).reason).toBe("harassment");
    });

    it("accepts every reason the game offers", () => {
        for (const reason of REASONS) {
            const r = parseReportInput(input({ reason }));
            expect((r as { reason: string }).reason).toBe(reason);
        }
    });

    it("refuses a reason it does not know", () => {
        expect((parseReportInput(input({ reason: "vibes" })) as Rejected).code).toBe("bad_reason");
    });

    it("REFUSES A REPORT WITH NO MESSAGE IN IT", () => {
        // The one that matters for the queue's usefulness: an accusation with
        // nothing attached cannot be judged, so accepting it would build a
        // queue of things nobody can act on.
        expect((parseReportInput(input({ message: "" })) as Rejected).code).toBe("no_message");
        expect((parseReportInput(input({ message: "   " })) as Rejected).code).toBe("no_message");
    });

    it("clips long fields and caps how much context can be attached", () => {
        const r = parseReportInput(input({
            message: "m".repeat(9000),
            note: "n".repeat(9000),
            context: Array(50).fill("line"),
        })) as { message: string; note: string; context: string[] };
        expect(r.message.length).toBe(LIMITS.message);
        expect(r.note.length).toBe(LIMITS.note);
        expect(r.context.length).toBe(LIMITS.context);
    });

    it("strips control characters from what it stores", () => {
        const r = parseReportInput(input({ message: "a\u0000b\u0007c" })) as { message: string };
        expect(r.message).toBe("abc");
    });
});

describe("filing one", () => {
    it("records who accused whom", async () => {
        const reporter = await makeAccount("acct-good", "Kaiserin");
        await makeAccount("acct-bad", "Troublemaker");

        const filed = await fileReport(env, reporter,
            parseReportInput(input()) as never) as Report;
        expect(filed.status).toBe("open");
        expect(filed.reporterNick).toBe("Kaiserin");
        expect(filed.accusedNick).toBe("Troublemaker");
        expect(filed.message).toContain("worthless");
        expect(await getReport(env, filed.id)).toMatchObject({ id: filed.id });
    });

    it("refuses a report against an account that does not exist", async () => {
        // Otherwise the queue can be filled with accusations addressed to
        // nothing, which costs a maintainer time and costs the filer nothing.
        const reporter = await makeAccount("acct-good", "Kaiserin");
        const filed = await fileReport(env, reporter,
            parseReportInput(input({ accusedId: "acct-nobody" })) as never);
        expect((filed as Rejected).code).toBe("no_account");
    });

    it("refuses a report against yourself", async () => {
        const reporter = await makeAccount("acct-good", "Kaiserin");
        const filed = await fileReport(env, reporter,
            parseReportInput(input({ accusedId: "acct-good" })) as never);
        expect((filed as Rejected).code).toBe("self");
    });

    it("stops one person filing endlessly", async () => {
        const reporter = await makeAccount("acct-good", "Kaiserin");
        await makeAccount("acct-bad", "Troublemaker");
        for (let i = 0; i < LIMITS.perReporterDaily; ++i) {
            expect(await checkReporterQuota(env, reporter.id)).toBeNull();
            await fileReport(env, reporter, parseReportInput(input()) as never);
        }
        expect((await checkReporterQuota(env, reporter.id))?.code).toBe("too_many");
        // And one person's flood does not silence everybody else.
        expect(await checkReporterQuota(env, "acct-someone-else")).toBeNull();
    });
});

describe("naming somebody you only know as a pseudonym", () => {
    it("resolves a psid through the note the ticket mint left", async () => {
        // The only identity a multiplayer client ever sees is the psid. Without
        // this resolution the report button cannot work at all in the games it
        // was asked for.
        const reporter = await makeAccount("acct-good", "Kaiserin");
        await makeAccount("acct-bad", "Troublemaker");
        await env.OD_ACCOUNTS.put("mod:psid:psid-of-the-bad-one", "acct-bad");

        const filed = await fileReport(env, reporter,
            parseReportInput(input({ accusedId: "", accusedPsid: "psid-of-the-bad-one" })) as never,
        ) as Report;
        expect(filed.accusedId).toBe("acct-bad");
        expect(filed.accusedNick).toBe("Troublemaker");
    });

    it("says so plainly when the pseudonym has expired", async () => {
        // Thirty days on, the note is gone and nobody -- including us -- can
        // say who that was. Better to say that than to guess.
        const reporter = await makeAccount("acct-good", "Kaiserin");
        const filed = await fileReport(env, reporter,
            parseReportInput(input({ accusedId: "", accusedPsid: "psid-long-gone" })) as never);
        expect((filed as Rejected).code).toBe("no_account");
        expect((filed as Rejected).message).toContain("30 days");
    });

    it("still refuses a report against yourself, named either way", async () => {
        const reporter = await makeAccount("acct-good", "Kaiserin");
        await env.OD_ACCOUNTS.put("mod:psid:my-own-psid", "acct-good");
        const filed = await fileReport(env, reporter,
            parseReportInput(input({ accusedId: "", accusedPsid: "my-own-psid" })) as never);
        expect((filed as Rejected).code).toBe("self");
    });

    it("needs one identity or the other", () => {
        expect((parseReportInput(input({ accusedId: "", accusedPsid: "" })) as Rejected).code)
            .toBe("bad_request");
    });
});

describe("saying a duration the way a person would", () => {
    it("reads back as what was typed", () => {
        expect(describeDays(1 / 1440)).toBe("1 minute");
        expect(describeDays(30 / 1440)).toBe("30 minutes");
        expect(describeDays(90 / 1440)).toBe("1.5 hours");   // not "90 minutes"
        expect(describeDays(1 / 24)).toBe("1 hour");
        expect(describeDays(36 / 24)).toBe("1.5 days");
        expect(describeDays(1)).toBe("1 day");
        expect(describeDays(14)).toBe("14 days");
    });
});

describe("who may decide", () => {
    it("is the developer badge and nothing else", async () => {
        expect(isModerator(await makeAccount("a", "Nobody"))).toBe(false);
        expect(isModerator(await makeAccount("b", "Tester", ["playtester"]))).toBe(false);
        expect(isModerator(await makeAccount("c", "Dev", ["developer"]))).toBe(true);
        expect(isModerator(null)).toBe(false);
    });
});

describe("deciding", () => {
    async function openReport(): Promise<{ report: Report; dev: Account }> {
        const reporter = await makeAccount("acct-good", "Kaiserin");
        await makeAccount("acct-bad", "Troublemaker");
        const dev = await makeAccount("acct-dev", "Vlad", ["developer"]);
        const report = await fileReport(env, reporter,
            parseReportInput(input()) as never) as Report;
        return { report, dev };
    }

    it("a ban is permanent and recorded", async () => {
        const { report, dev } = await openReport();
        const done = await decide(env, report, { action: "ban", reason: "Abuse." }, dev) as Report;
        expect(done.status).toBe("actioned");
        expect(done.outcome).toBe("banned");
        expect(done.decidedBy).toBe("Vlad");

        const accused = JSON.parse((await env.OD_ACCOUNTS.get("acct:acct-bad"))!);
        expect(accused.banned).toBeTruthy();
        expect(accused.banned.until).toBeUndefined();   // permanent
    });

    it("a timeout ends by itself", async () => {
        const { report, dev } = await openReport();
        const done = await decide(env, report,
            { action: "timeout", days: 7, reason: "Cool off." }, dev) as Report;
        expect(done.outcome).toBe("timeout 7 days");

        const accused = JSON.parse((await env.OD_ACCOUNTS.get("acct:acct-bad"))!);
        expect(accused.banned.until).toBeGreaterThan(Math.floor(Date.now() / 1000));
    });

    it("takes whatever length the maintainer typed, fractions included", async () => {
        // The point of the change: 36 hours has to stay 36 hours. Rounding it
        // to whole days turned short cooling-off periods into long ones, and a
        // moderator who cannot express "an hour" reaches for a ban instead.
        const cases: [number, string][] = [
            [1 / 1440, "1 minute"],
            [90 / 1440, "1.5 hours"],   // over an hour, so said in hours
            [1 / 24,   "1 hour"],
            [1.5,      "1.5 days"],
            [36 / 24,  "1.5 days"],
            [1,        "1 day"],
            [7,        "7 days"],
            [365,      "365 days"],
        ];
        for (const [days, said] of cases) {
            await clearKv(env);
            const { report, dev } = await openReport();
            const done = await decide(env, report,
                { action: "timeout", days, reason: "" }, dev) as Report;
            expect(done.outcome).toBe(`timeout ${said}`);
        }
    });

    it("a short timeout really is short on the account record", async () => {
        const { report, dev } = await openReport();
        const before = Math.floor(Date.now() / 1000);
        const done = await decide(env, report,
            { action: "timeout", days: 1 / 24, reason: "" }, dev) as Report;
        // An hour, not a day. The record is what a banned player is held to.
        expect(done.until! - before).toBeGreaterThanOrEqual(3500);
        expect(done.until! - before).toBeLessThanOrEqual(3700);
    });

    it("refuses to make a timeout so short it looks like nothing happened", async () => {
        const { report, dev } = await openReport();
        const before = Math.floor(Date.now() / 1000);
        const done = await decide(env, report,
            { action: "timeout", days: 0.0000001, reason: "" }, dev) as Report;
        expect(done.until! - before).toBeGreaterThanOrEqual(55);   // a minute, floored
    });

    it("records when the timeout lifts, from the account and not from arithmetic", async () => {
        // The number a banned player is told, so it has to be the number the
        // ban was written with. Deriving it a second time from decidedAt is a
        // second copy of a rule, and copies disagree.
        const { report, dev } = await openReport();
        const done = await decide(env, report,
            { action: "timeout", days: 7, reason: "" }, dev) as Report;

        const accused = JSON.parse((await env.OD_ACCOUNTS.get("acct:acct-bad"))!);
        expect(done.until).toBe(accused.banned.until);
        expect(done.until).toBeGreaterThan(Math.floor(Date.now() / 1000));
    });

    it("has no lift time for a permanent ban or a dismissal", async () => {
        {
            const { report, dev } = await openReport();
            const done = await decide(env, report, { action: "ban", reason: "" }, dev) as Report;
            expect(done.until).toBeUndefined();
        }
        await clearKv(env);
        {
            const { report, dev } = await openReport();
            const done = await decide(env, report,
                { action: "dismiss", reason: "" }, dev) as Report;
            expect(done.until).toBeUndefined();
        }
    });

    it("clamps an absurd timeout rather than trusting the number", async () => {
        const { report, dev } = await openReport();
        const done = await decide(env, report,
            { action: "timeout", days: 99999, reason: "" }, dev) as Report;
        expect(done.outcome).toBe("timeout 3650 days");
    });

    it("DISMISSING IS A REAL OUTCOME AND PUNISHES NOBODY", async () => {
        // A queue whose only clearing action is a punishment is a queue that
        // punishes people, so this path has to exist and has to be recorded.
        const { report, dev } = await openReport();
        const done = await decide(env, report, { action: "dismiss", reason: "" }, dev) as Report;
        expect(done.status).toBe("dismissed");
        const accused = JSON.parse((await env.OD_ACCOUNTS.get("acct:acct-bad"))!);
        expect(accused.banned).toBeFalsy();
    });

    it("cannot be decided twice", async () => {
        const { report, dev } = await openReport();
        await decide(env, report, { action: "dismiss", reason: "" }, dev);
        const again = await decide(env, await getReport(env, report.id) as Report,
                                   { action: "ban", reason: "" }, dev);
        expect((again as Rejected).code).toBe("already_decided");
    });
});

describe("announcing it", () => {
    async function actioned(outcome: "ban" | "timeout"): Promise<Report> {
        const reporter = await makeAccount("acct-good", "Kaiserin");
        await makeAccount("acct-bad", "Troublemaker");
        const dev = await makeAccount("acct-dev", "Vlad", ["developer"]);
        const report = await fileReport(env, reporter,
            parseReportInput(input()) as never) as Report;
        return await decide(env, report,
            { action: outcome, days: 3, reason: "Abuse." }, dev) as Report;
    }

    it("says who and until when", async () => {
        setWebhook("https://discord.invalid/mod");
        let body: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (_u: string, init: RequestInit) => {
            body = JSON.parse(init.body as string);
            return new Response(null, { status: 204 });
        });

        expect(await announce(env, await actioned("timeout"))).toBe(true);
        expect(String(body.content)).toContain("Timed out");
        expect(String(body.content)).toContain("Troublemaker");
        // The absolute time AND the relative one, both rendered in the
        // reader's own timezone by Discord.
        expect(String(body.content)).toMatch(/<t:\d+:f>/);
        expect(String(body.content)).toMatch(/<t:\d+:R>/);
        expect(body.allowed_mentions).toEqual({ parse: [] });
    });

    it("NEVER republishes the message it was reporting", async () => {
        // A moderation channel is still a room full of people. Announcing that
        // abuse was dealt with by reprinting the abuse gives it a second
        // audience, and names the reporter to everyone who reads it.
        setWebhook("https://discord.invalid/mod");
        let body: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (_u: string, init: RequestInit) => {
            body = JSON.parse(init.body as string);
            return new Response(null, { status: 204 });
        });

        await announce(env, await actioned("ban"));
        expect(String(body.content)).not.toContain("worthless");
        expect(String(body.content)).not.toContain("Kaiserin");
    });

    it("announces the SAME instant the ban record holds", async () => {
        // The whole point of reading it back: what the channel says and what
        // the account says must be one number, not two that agree today.
        setWebhook("https://discord.invalid/mod");
        let body: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (_u: string, init: RequestInit) => {
            body = JSON.parse(init.body as string);
            return new Response(null, { status: 204 });
        });

        await announce(env, await actioned("timeout"));
        const accused = JSON.parse((await env.OD_ACCOUNTS.get("acct:acct-bad"))!);
        expect(String(body.content)).toContain(`<t:${accused.banned.until}:f>`);
    });

    it("gives a permanent ban no lift time at all", async () => {
        setWebhook("https://discord.invalid/mod");
        let body: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (_u: string, init: RequestInit) => {
            body = JSON.parse(init.body as string);
            return new Response(null, { status: 204 });
        });

        await announce(env, await actioned("ban"));
        expect(String(body.content)).toContain("Banned");
        expect(String(body.content)).not.toMatch(/<t:\d+/);   // no "until" at all
    });

    it("says nothing at all about a dismissal", async () => {
        // "We looked and did nothing" is not an announcement, it is a person's
        // name in a channel for no reason.
        setWebhook("https://discord.invalid/mod");
        const fetchSpy = vi.fn();
        vi.stubGlobal("fetch", fetchSpy);

        const reporter = await makeAccount("acct-good", "Kaiserin");
        await makeAccount("acct-bad", "Troublemaker");
        const dev = await makeAccount("acct-dev", "Vlad", ["developer"]);
        const report = await fileReport(env, reporter, parseReportInput(input()) as never) as Report;
        const done = await decide(env, report, { action: "dismiss", reason: "" }, dev) as Report;

        expect(await announce(env, done)).toBe(false);
        expect(fetchSpy).not.toHaveBeenCalled();
    });

    it("a channel being down does not undo the ban", async () => {
        setWebhook("https://discord.invalid/mod");
        vi.stubGlobal("fetch", async () => { throw new Error("network is down"); });
        const done = await actioned("ban");
        expect(await announce(env, done)).toBe(false);
        const accused = JSON.parse((await env.OD_ACCOUNTS.get("acct:acct-bad"))!);
        expect(accused.banned).toBeTruthy();    // still banned
    });

    it("is silent when no channel is configured, which is a fork's normal state", async () => {
        vi.stubGlobal("fetch", async () => { throw new Error("must not be called"); });
        expect(await announce(env, await actioned("ban"))).toBe(false);
    });
});

describe("looking somebody up", () => {
    async function seed() {
        const troublemaker = await makeAccount("acct-bad", "TestTroublemaker");
        await env.OD_ACCOUNTS.put(`nick:${normalize("TestTroublemaker")}`, "acct-bad");
        const reporter = await makeAccount("acct-good", "Kaiserin");
        await env.OD_ACCOUNTS.put(`nick:${normalize("Kaiserin")}`, "acct-good");
        return { troublemaker, reporter };
    }

    it("finds them by account id and by nickname", async () => {
        await seed();
        expect((await findAccount(env, "acct-bad"))?.nick).toBe("TestTroublemaker");
        expect((await findAccount(env, "TestTroublemaker"))?.id).toBe("acct-bad");
        // However it was typed: nicknames are matched on their normalised form.
        expect((await findAccount(env, "  testtroublemaker "))?.id).toBe("acct-bad");
        expect(await findAccount(env, "nobody-at-all")).toBeNull();
        expect(await findAccount(env, "")).toBeNull();
    });

    it("shows their history on both sides", async () => {
        const { troublemaker, reporter } = await seed();
        await fileReport(env, reporter, parseReportInput(input()) as never);
        await fileReport(env, troublemaker,
            parseReportInput(input({ accusedId: "acct-good" })) as never);

        const p = await profileOf(env, troublemaker);
        expect(p.nickname).toBe("TestTroublemaker");
        expect(p.against).toHaveLength(1);   // reported by somebody
        expect(p.filed).toHaveLength(1);     // and reported somebody
        expect(p.banned).toBe(false);
    });

    it("NEVER exposes the linked provider identities", async () => {
        // A moderator can act on behaviour. Which provider somebody signed in
        // with tells them nothing actionable, and the hashed subject is the one
        // thing in the store that is nobody's business but the account's.
        const { troublemaker } = await seed();
        const p = await profileOf(env, troublemaker) as unknown as Record<string, unknown>;
        expect(p.identities).toBeUndefined();
        expect(JSON.stringify(p)).not.toContain("subHash");
        expect(typeof (p as { linkedCount: number }).linkedCount).toBe("number");
    });

    it("shows a ban and when it lifts", async () => {
        const { troublemaker } = await seed();
        await actOnAccount(env, troublemaker, { action: "timeout", days: 2, reason: "Cool off." });
        const p = await profileOf(env, await findAccount(env, "acct-bad") as never);
        expect(p.banned).toBe(true);
        expect(p.bannedUntil).toBeGreaterThan(Math.floor(Date.now() / 1000));
        expect(p.banReason).toBe("Cool off.");
    });
});

describe("acting on somebody directly, with no report behind it", () => {
    async function seed() {
        const a = await makeAccount("acct-bad", "TestTroublemaker");
        await env.OD_ACCOUNTS.put(`nick:${normalize("TestTroublemaker")}`, "acct-bad");
        return a;
    }

    it("bans", async () => {
        const target = await seed();
        const out = await actOnAccount(env, target, { action: "ban", reason: "Saw it myself." });
        expect(out.banned).toBe(true);
        expect(out.outcome).toBe("banned");
        expect(out.until).toBeUndefined();
    });

    it("times out for a typed length, fractions and all", async () => {
        const target = await seed();
        const before = Math.floor(Date.now() / 1000);
        const out = await actOnAccount(env, target,
            { action: "timeout", days: 1.5 / 24, reason: "" });   // 90 minutes
        expect(out.outcome).toBe("timeout 1.5 hours");
        expect(out.until! - before).toBeGreaterThanOrEqual(5350);
        expect(out.until! - before).toBeLessThanOrEqual(5450);
    });

    it("PARDONS, which is the whole reason this path exists both ways", async () => {
        // A timeout that turns out to have been wrong should be liftable in one
        // action rather than waited out.
        const target = await seed();
        await actOnAccount(env, target, { action: "ban", reason: "" });
        expect(JSON.parse((await env.OD_ACCOUNTS.get("acct:acct-bad"))!).banned).toBeTruthy();

        const out = await actOnAccount(env, await findAccount(env, "acct-bad") as never,
                                       { action: "dismiss", reason: "" });
        expect(out.banned).toBe(false);
        expect(out.outcome).toBe("pardoned");
        expect(JSON.parse((await env.OD_ACCOUNTS.get("acct:acct-bad"))!).banned).toBeFalsy();
    });
});

describe("the queue", () => {
    it("comes back newest first", async () => {
        const reporter = await makeAccount("acct-good", "Kaiserin");
        await makeAccount("acct-bad", "Troublemaker");
        const a = await fileReport(env, reporter,
            parseReportInput(input({ message: "first" })) as never) as Report;
        const b = await fileReport(env, reporter,
            parseReportInput(input({ message: "second" })) as never) as Report;
        // Same second is possible in a test; force an order.
        await env.OD_ACCOUNTS.put(`mod:report:${b.id}`,
            JSON.stringify({ ...b, at: a.at + 10 }));

        const list = await listReports(env);
        expect(list.length).toBe(2);
        expect(list[0]?.id).toBe(b.id);
    });
});
