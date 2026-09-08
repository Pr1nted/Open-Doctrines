// Reporting a player, and what a maintainer can do about it.
//
// TWO DESTINATIONS, ON PURPOSE
//
// A message can be reported to the SERVER OWNER, who can kick or ban from their
// own game, and separately to the ACCOUNT SERVICE, which can ban the account
// everywhere. Those are different powers and they belong to different people:
// most bad behaviour is a matter for whoever runs the room, and only a small
// part of it is a matter for whoever runs the accounts. The first never touches
// this file -- it happens between the player and the host, inside the game.
// This is only the second.
//
// WHAT IS STORED, AND WHY IT HAS TO BE
//
// A report cannot be judged without the message it is about, so the message is
// stored, along with whatever surrounding lines the reporter chose to attach.
// That is player-written content held on the service, which the rest of this
// project goes out of its way to avoid -- so it expires on its own, it is
// readable only by an account carrying the `developer` badge, and PRIVACY.md
// says plainly that it exists.
//
// WHY A BADGE AND NOT THE ADMIN SECRET
//
// The existing /admin routes are gated by a shared secret in a header, which is
// right for a command line and wrong for a game menu: it would mean shipping a
// client that holds a credential capable of banning anybody. The badge is
// already on the account, already proven by the session token, and already
// follows the person to a new machine.

import type { Env } from "../env.js";
import type { Account } from "../accounts/store.js";
import { getAccount, setBanned } from "../accounts/store.js";
import { normalize } from "../accounts/nickname.js";
import { randomId } from "../util/crypto.js";

/** What the reporter says is wrong. Stable wire values. */
export const REASONS = [
    "harassment", "hate", "threats", "sexual", "spam", "cheating", "other",
] as const;
export type Reason = (typeof REASONS)[number];

export const LIMITS = {
    message: 2000,
    note: 1000,
    // Surrounding lines a reporter may attach. Generous, because the argument
    // before the message is usually what decides whether it was one bad moment
    // or a pattern -- and a maintainer who cannot see that is guessing.
    context: 40,
    contextLine: 500,
    perReporterDaily: 20,
    /** How long a report is kept before it expires on its own. */
    ttlSeconds: 90 * 86400,
};

export interface Report {
    id: string;
    at: number;
    /** Who reported. Kept so a pattern of false reports is visible. */
    reporterId: string;
    reporterNick: string;
    accusedId: string;
    accusedNick: string;
    reason: Reason;
    /** The reporter's own words. */
    note: string;
    /** The message complained about. */
    message: string;
    /** Lines around it, if the reporter chose to attach them. */
    context: string[];
    /** Which game it happened in, as the client reports it. Untrusted. */
    server: string;
    status: "open" | "actioned" | "dismissed";
    outcome?: string;
    decidedBy?: string;
    decidedAt?: number;
    /**
     * When the timeout actually lifts, as unix seconds, straight off the
     * account record. Absent for a permanent ban and for a dismissal.
     *
     * READ BACK rather than recomputed. The first version derived it as
     * `decidedAt + days * 86400`, which is a second copy of a rule that lives
     * in setBanned -- and a second copy is a thing that can disagree. It is the
     * number a banned player will be told, so it should be the number the ban
     * was actually written with.
     */
    until?: number;
}

export interface Rejected {
    ok: false;
    status: number;
    code: string;
    message: string;
}

/**
 * Strip control characters, which are how one message pretends to be two.
 *
 * Newline and tab survive: a reported message may be several lines and that
 * shape is part of what is being reported.
 */
function clean(value: unknown, max: number): string {
    if (typeof value !== "string") return "";
    // eslint-disable-next-line no-control-regex
    const stripped = value.replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "");
    return stripped.trim().slice(0, max);
}

export interface ReportInput {
    /** Either an account id, or a psid to be resolved. One of the two. */
    accusedId: string;
    /** How a multiplayer client names somebody: the only identity it has. */
    accusedPsid: string;
    reason: Reason;
    note: string;
    message: string;
    context: string[];
    server: string;
}

export function parseReportInput(raw: unknown): ReportInput | Rejected {
    if (!raw || typeof raw !== "object") {
        return { ok: false, status: 400, code: "bad_request", message: "Empty report." };
    }
    const r = raw as Record<string, unknown>;
    const reason = r.reason as Reason;
    if (!REASONS.includes(reason)) {
        return { ok: false, status: 400, code: "bad_reason", message: "Unknown reason." };
    }
    const accusedId = clean(r.accusedId, 64);
    const accusedPsid = clean(r.accusedPsid, 64);
    if (!accusedId && !accusedPsid) {
        return { ok: false, status: 400, code: "bad_request", message: "Missing account." };
    }
    const message = clean(r.message, LIMITS.message);
    if (!message) {
        // A report with no message is unjudgeable, and accepting one would mean
        // a queue full of accusations nobody can act on.
        return {
            ok: false, status: 400, code: "no_message",
            message: "Please include the message you are reporting.",
        };
    }
    const context: string[] = [];
    if (Array.isArray(r.context)) {
        for (const line of r.context.slice(0, LIMITS.context)) {
            const c = clean(line, LIMITS.contextLine);
            if (c) context.push(c);
        }
    }
    return {
        accusedId,
        accusedPsid,
        reason,
        note: clean(r.note, LIMITS.note),
        message,
        context,
        server: clean(r.server, 120),
    };
}

const dayKey = (id: string) => `mod:day:${id}:${new Date().toISOString().slice(0, 10)}`;

/** Whether this reporter may file another today. */
export async function checkReporterQuota(env: Env, reporterId: string): Promise<Rejected | null> {
    const seen = Number((await env.OD_ACCOUNTS.get(dayKey(reporterId))) ?? "0");
    if (seen >= LIMITS.perReporterDaily) {
        return {
            ok: false, status: 429, code: "too_many",
            message: "That is a lot of reports today. Please continue tomorrow.",
        };
    }
    return null;
}

/**
 * File one.
 *
 * The accused is looked up rather than trusted: a report against an account id
 * that does not exist is a report nobody can action, and letting one be filed
 * would let anybody fill the queue with noise addressed to nothing.
 */
export async function fileReport(
    env: Env, reporter: Account, input: ReportInput,
): Promise<Report | Rejected> {
    if (input.accusedId === reporter.id) {
        return { ok: false, status: 400, code: "self", message: "You cannot report yourself." };
    }
    // A multiplayer client knows only a psid. Resolved through the note the
    // ticket mint left; see auth/ticket.ts for why that note exists and what it
    // costs. An expired note means the game was long enough ago that we can no
    // longer say who this was, which is the correct answer rather than a guess.
    let accusedId = input.accusedId;
    if (!accusedId && input.accusedPsid) {
        accusedId = (await env.OD_ACCOUNTS.get(`mod:psid:${input.accusedPsid}`)) ?? "";
        if (!accusedId) {
            return {
                ok: false, status: 404, code: "no_account",
                message: "That player can no longer be identified. Reports must be made "
                       + "within 30 days.",
            };
        }
    }
    if (accusedId === reporter.id) {
        return { ok: false, status: 400, code: "self", message: "You cannot report yourself." };
    }
    const accused = await getAccount(env, accusedId);
    if (!accused) {
        return { ok: false, status: 404, code: "no_account", message: "No such account." };
    }

    const report: Report = {
        id: randomId(22),
        at: Math.floor(Date.now() / 1000),
        reporterId: reporter.id,
        reporterNick: reporter.nick,
        accusedId: accused.id,
        accusedNick: accused.nick,
        reason: input.reason,
        note: input.note,
        message: input.message,
        context: input.context,
        server: input.server,
        status: "open",
    };
    await env.OD_ACCOUNTS.put(`mod:report:${report.id}`, JSON.stringify(report),
                              { expirationTtl: LIMITS.ttlSeconds });
    const seen = Number((await env.OD_ACCOUNTS.get(dayKey(reporter.id))) ?? "0");
    await env.OD_ACCOUNTS.put(dayKey(reporter.id), String(seen + 1), { expirationTtl: 2 * 86400 });
    return report;
}

export async function getReport(env: Env, id: string): Promise<Report | null> {
    const raw = await env.OD_ACCOUNTS.get(`mod:report:${id}`);
    if (!raw) return null;
    try { return JSON.parse(raw) as Report; } catch { return null; }
}

/** Everything on file, newest first. */
export async function listReports(env: Env, limit = 50): Promise<Report[]> {
    const listed = await env.OD_ACCOUNTS.list({ prefix: "mod:report:", limit: 1000 });
    const out: Report[] = [];
    for (const key of listed.keys) {
        const raw = await env.OD_ACCOUNTS.get(key.name);
        if (!raw) continue;
        try { out.push(JSON.parse(raw) as Report); } catch { /* skip a corrupt one */ }
    }
    out.sort((a, b) => b.at - a.at);
    return out.slice(0, limit);
}

/**
 * A duration as a person would say it: "90 minutes", "36 hours", "3 days".
 *
 * For the record and the announcement, so both read the way the moderator
 * typed it rather than as a decimal fraction of a day.
 */
export function describeDays(days: number): string {
    const minutes = Math.round(days * 1440);
    if (minutes < 60) return `${minutes} minute${minutes === 1 ? "" : "s"}`;
    if (minutes < 1440) {
        const hours = Math.round(minutes / 60 * 10) / 10;
        return `${hours} hour${hours === 1 ? "" : "s"}`;
    }
    const d = Math.round(minutes / 1440 * 10) / 10;
    return `${d} day${d === 1 ? "" : "s"}`;
}

export type Action = "ban" | "timeout" | "dismiss";

export interface Decision {
    action: Action;
    /**
     * How long a timeout lasts, in days, and FRACTIONS ARE MEANINGFUL.
     *
     * 0.5 is twelve hours and 1/24 is one hour. The first version rounded this
     * to whole days, which quietly turned "thirty-six hours" into two days and
     * "ninety minutes" into nothing at all -- and a moderator who cannot
     * express a short cooling-off period reaches for a longer ban instead.
     */
    days?: number;
    /** What the maintainer wants recorded and, for a ban, shown to the account. */
    reason: string;
}

/**
 * Act on a report.
 *
 * Dismissing is a real outcome and is recorded as one: a queue where the only
 * way to clear something is to punish somebody is a queue that punishes people.
 */
export async function decide(
    env: Env, report: Report, decision: Decision, by: Account,
): Promise<Report | Rejected> {
    if (report.status !== "open") {
        return { ok: false, status: 409, code: "already_decided", message: "Already dealt with." };
    }

    let outcome = "dismissed";
    let until: number | undefined;
    if (decision.action !== "dismiss") {
        const accused = await getAccount(env, report.accusedId);
        if (!accused) {
            return { ok: false, status: 404, code: "no_account", message: "That account is gone." };
        }
        // A minute at the short end -- below that a timeout is over before the
        // person notices it, which is worse than doing nothing because it looks
        // like the report was ignored. Ten years at the long end, where the
        // honest answer is a ban rather than a very long timeout.
        const days = decision.action === "timeout"
            ? Math.max(1 / 1440, Math.min(3650, decision.days ?? 7))
            : undefined;
        const banned = await setBanned(env, accused, true,
                                       decision.reason || "Conduct in chat.", days);
        // The authority's own number, not ours.
        until = banned.banned?.until;
        outcome = days ? `timeout ${describeDays(days)}` : "banned";
    }

    const updated: Report = {
        ...report,
        status: decision.action === "dismiss" ? "dismissed" : "actioned",
        outcome,
        until,
        decidedBy: by.nick,
        decidedAt: Math.floor(Date.now() / 1000),
    };
    await env.OD_ACCOUNTS.put(`mod:report:${report.id}`, JSON.stringify(updated),
                              { expirationTtl: LIMITS.ttlSeconds });
    return updated;
}

/**
 * Say in Discord that somebody was banned or timed out.
 *
 * Deliberately carries WHO and UNTIL WHEN and nothing else -- not the message,
 * not the reporter. A moderation channel is still a room full of people, and
 * republishing the abuse in order to announce that it was dealt with hands it a
 * second audience. Never fires for a dismissal either: "we looked and did
 * nothing" is not an announcement, it is a person's name in a channel for no
 * reason.
 */
export async function announce(env: Env, report: Report): Promise<boolean> {
    const webhook = env.MODERATION_DISCORD_WEBHOOK;
    if (!webhook || report.status !== "actioned") return false;

    // Discord renders <t:unix:f> in each reader's own timezone, and <t:unix:R>
    // as "in 6 days" -- both are right for a server whose members are not in
    // one place. The number is the one the ban was written with, read back off
    // the account rather than recomputed here.
    const until = report.until
        ? ` until <t:${report.until}:f> (<t:${report.until}:R>)`
        : "";
    const verb = report.outcome === "banned" ? "Banned" : "Timed out";
    const text = `**${verb}** ${report.accusedNick}${until}\n` +
                 `reason: ${report.reason} - by ${report.decidedBy ?? "?"}`;
    try {
        const response = await fetch(webhook, {
            method: "POST",
            headers: { "content-type": "application/json" },
            // Same reason as every other webhook here: without this a nickname
            // containing @everyone would ping the server.
            body: JSON.stringify({ content: text.slice(0, 1900), allowed_mentions: { parse: [] } }),
        });
        return response.ok;
    } catch {
        return false;   // a channel being down must not undo a ban
    }
}

/**
 * Find somebody by nickname or by account id.
 *
 * Both, because a maintainer has one or the other depending on where they heard
 * about the person: a nickname off a screenshot, an id out of a report. Tried
 * as an id first -- ids are opaque and cannot collide with a nickname, whereas
 * a nickname that happens to look like an id would otherwise be unfindable.
 */
export async function findAccount(env: Env, query: string): Promise<Account | null> {
    const q = query.trim();
    if (!q) return null;

    const byId = await getAccount(env, q);
    if (byId) return byId;

    const id = await env.OD_ACCOUNTS.get(`nick:${normalize(q)}`);
    return id ? getAccount(env, id) : null;
}

/** What a maintainer needs to see about somebody, and nothing more. */
export interface Profile {
    id: string;
    nickname: string;
    created: number;
    badges: string[];
    /** How many providers are linked. NOT which, and never the identities. */
    linkedCount: number;
    banned: boolean;
    banReason?: string;
    /** When it lifts. Absent on a permanent ban. */
    bannedUntil?: number;
    bannedAt?: number;
    /** Reports naming this person, newest first. */
    against: Report[];
    /** Reports this person filed. A pattern here is as informative as one above. */
    filed: Report[];
}

/**
 * Everything on file about one account.
 *
 * Deliberately NOT the linked provider identities. Knowing somebody signed in
 * with Google tells a moderator nothing they can act on, and the hashed subject
 * is the one thing in the store that is nobody's business but the account's.
 */
export async function profileOf(env: Env, account: Account): Promise<Profile> {
    const all = await listReports(env, 1000);
    return {
        id: account.id,
        nickname: account.nick,
        created: account.created,
        badges: [...account.badges],
        linkedCount: account.identities.length,
        banned: !!account.banned,
        banReason: account.banned?.reason,
        bannedUntil: account.banned?.until,
        bannedAt: account.banned?.at,
        against: all.filter((r) => r.accusedId === account.id),
        filed: all.filter((r) => r.reporterId === account.id),
    };
}

/**
 * Ban, time out or pardon somebody directly, with no report behind it.
 *
 * A maintainer sees things themselves, and a moderation tool that can only act
 * on a filed report cannot act on what it witnessed. Pardoning is here for the
 * same reason in reverse: a timeout that turns out to have been wrong should be
 * liftable in one action rather than waited out.
 */
export async function actOnAccount(
    env: Env, account: Account, decision: Decision,
): Promise<{ banned: boolean; until?: number; outcome: string }> {
    if (decision.action === "dismiss") {
        // "dismiss" means PARDON on this path: there is no report to close, so
        // the only thing it can mean is lifting what is in force.
        await setBanned(env, account, false, "");
        return { banned: false, outcome: "pardoned" };
    }
    const days = decision.action === "timeout"
        ? Math.max(1 / 1440, Math.min(3650, decision.days ?? 7))
        : undefined;
    const updated = await setBanned(env, account, true,
                                    decision.reason || "Conduct towards other players.", days);
    return {
        banned: true,
        until: updated.banned?.until,
        outcome: days ? `timeout ${describeDays(days)}` : "banned",
    };
}

/**
 * Say that somebody's punishment has been lifted.
 *
 * Announced, unlike a dismissal, and the difference is real: a dismissal
 * decides that nothing should happen, whereas a pardon undoes something that
 * WAS announced. A channel that only ever carries punishments and never their
 * reversal gives a false picture of how the moderation is going.
 */
export async function announcePardon(
    env: Env, nickname: string, by: string,
): Promise<boolean> {
    const webhook = env.MODERATION_DISCORD_WEBHOOK;
    if (!webhook) return false;
    try {
        const response = await fetch(webhook, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({
                content: `**Pardoned** ${nickname}\nby ${by}`.slice(0, 1900),
                allowed_mentions: { parse: [] },
            }),
        });
        return response.ok;
    } catch {
        return false;
    }
}

/** Only an account carrying the developer badge may review reports. */
export function isModerator(account: Account | null): boolean {
    return !!account && account.badges.includes("developer");
}
