// Reporting a MOD, and what a maintainer can do about it.
//
// SEPARATE FROM moderation/reports.ts ON PURPOSE, AND THE SEPARATION IS THE
// FEATURE
//
// That file reports a PERSON for something they said, and the power it grants
// is banning an account everywhere. This one reports a THING somebody
// published, and the power it grants is taking the listing down and stopping
// that account publishing again.
//
// Those must not be the same lever. Somebody who published a mod with a dodgy
// download link should stop being able to publish; they should not stop being
// able to play. Collapsing the two would mean every mod complaint arriving at a
// control whose only setting is "remove this person from the game", and a
// moderator holding that lever uses it or does nothing.
//
// So: its own reasons, its own queue, its own outcomes, and a restriction that
// lives in `account.restricted.mods` where nothing on the join path reads it.

import type { Env } from "../env.js";
import type { Account } from "../accounts/store.js";
import { getAccount, setModsRestricted } from "../accounts/store.js";
import { randomId } from "../util/crypto.js";
import { getListing, setStatus, type Listing } from "./registry.js";

/** What the reporter says is wrong with the mod. Stable wire values. */
export const MOD_REASONS = [
    // Deliberately first: it is the reason the scanner exists and the one that
    // needs acting on fastest.
    "malware",
    "stolen",        // somebody else's work, republished
    "illegal",
    "sexual",
    "broken",        // the download is gone, or is not the mod it claims to be
    "spam",
    "other",
] as const;
export type ModReason = (typeof MOD_REASONS)[number];

export const LIMITS = {
    note: 1000,
    perReporterDaily: 20,
    /** Kept no longer than a player report is. */
    ttlSeconds: 90 * 86400,
};

export interface ModReport {
    id: string;
    at: number;
    reporterId: string;
    reporterNick: string;
    /** The listing this is about, as it stood when reported. */
    modId: string;
    modName: string;
    ownerId: string;
    ownerNick: string;
    reason: ModReason;
    note: string;
    status: "open" | "actioned" | "dismissed";
    outcome?: string;
    decidedBy?: string;
    decidedAt?: number;
    /** When a publishing restriction lifts. Read back off the account record. */
    until?: number;
}

export interface Rejected {
    ok: false;
    status: number;
    code: string;
    message: string;
}

const reportKey = (id: string) => `pkg:report:${id}`;
const dayKey = (accountId: string) =>
    `pkg:rq:${accountId}:${new Date().toISOString().slice(0, 10)}`;

export function isModReason(v: string): v is ModReason {
    return (MOD_REASONS as readonly string[]).includes(v);
}

export async function checkReporterQuota(
    env: Env, reporter: Account,
): Promise<Rejected | null> {
    const seen = Number((await env.OD_ACCOUNTS.get(dayKey(reporter.id))) ?? "0");
    if (seen >= LIMITS.perReporterDaily) {
        return {
            ok: false, status: 429, code: "quota",
            message: "That is a lot of reports today. Please continue tomorrow.",
        };
    }
    return null;
}

export interface ModReportInput {
    modId: string;
    reason: ModReason;
    note: string;
}

export function parseModReportInput(body: unknown): ModReportInput | Rejected {
    const b = (body ?? {}) as Record<string, unknown>;
    const reason = String(b.reason ?? "");
    if (!isModReason(reason)) {
        return { ok: false, status: 400, code: "bad_reason", message: "Unknown reason." };
    }
    const note = typeof b.note === "string"
        ? b.note.trim().slice(0, LIMITS.note) : "";
    if (note.length < 4) {
        return {
            ok: false, status: 400, code: "bad_note",
            message: "Say briefly what is wrong with it.",
        };
    }
    return { modId: String(b.modId ?? ""), reason, note };
}

/**
 * File one.
 *
 * The listing is looked up rather than trusted, the same way a player report
 * resolves the accused: a report about a mod id that does not exist is one
 * nobody can action, and accepting it would let anyone fill the queue with
 * complaints addressed to nothing.
 */
export async function fileModReport(
    env: Env, reporter: Account, input: ModReportInput,
): Promise<ModReport | Rejected> {
    const listing = await getListing(env, input.modId);
    if (!listing) {
        return { ok: false, status: 404, code: "no_mod", message: "No such mod." };
    }
    if (listing.ownerId === reporter.id) {
        return {
            ok: false, status: 400, code: "self",
            message: "That is your own listing. Withdraw it instead.",
        };
    }

    const report: ModReport = {
        id: randomId(22),
        at: Math.floor(Date.now() / 1000),
        reporterId: reporter.id,
        reporterNick: reporter.nick,
        modId: listing.id,
        modName: listing.name,
        ownerId: listing.ownerId,
        ownerNick: listing.ownerNick,
        reason: input.reason,
        note: input.note,
        status: "open",
    };
    await env.OD_ACCOUNTS.put(reportKey(report.id), JSON.stringify(report),
                              { expirationTtl: LIMITS.ttlSeconds });
    const seen = Number((await env.OD_ACCOUNTS.get(dayKey(reporter.id))) ?? "0");
    await env.OD_ACCOUNTS.put(dayKey(reporter.id), String(seen + 1),
                              { expirationTtl: 2 * 86400 });
    return report;
}

export async function getModReport(env: Env, id: string): Promise<ModReport | null> {
    const raw = await env.OD_ACCOUNTS.get(reportKey(id));
    if (!raw) return null;
    try { return JSON.parse(raw) as ModReport; } catch { return null; }
}

export async function listModReports(env: Env, limit = 50): Promise<ModReport[]> {
    const listed = await env.OD_ACCOUNTS.list({ prefix: "pkg:report:", limit: 1000 });
    const out: ModReport[] = [];
    for (const key of listed.keys) {
        const raw = await env.OD_ACCOUNTS.get(key.name);
        if (!raw) continue;
        try { out.push(JSON.parse(raw) as ModReport); } catch { /* skip a corrupt one */ }
    }
    out.sort((a, b) => b.at - a.at);
    return out.slice(0, limit);
}

/**
 * What a maintainer can do.
 *
 * `unlist` is about the MOD. `restrict` is about the ACCOUNT'S ability to
 * publish, and it unlists too, because a restriction that left the reported
 * listing up would be a half-measure nobody wanted.
 *
 * There is no "ban" here, and that is not an omission. Banning an account is
 * moderation/reports.ts's power, it needs the evidence that file collects, and
 * a mod complaint is not that evidence.
 */
export type ModAction = "unlist" | "restrict" | "dismiss";

export interface ModDecision {
    action: ModAction;
    /** Days a publishing restriction lasts. Omit for indefinite. */
    days?: number;
    reason: string;
}

export async function decideModReport(
    env: Env, report: ModReport, decision: ModDecision, by: Account,
): Promise<ModReport | Rejected> {
    if (report.status !== "open") {
        return {
            ok: false, status: 409, code: "already_decided",
            message: "Already dealt with.",
        };
    }

    let outcome = "dismissed";
    let until: number | undefined;

    if (decision.action !== "dismiss") {
        const listing = await getListing(env, report.modId);
        // A listing withdrawn by its owner between report and decision is not an
        // error: the complaint is moot, and saying so is a real outcome.
        if (listing) await setStatus(env, listing as Listing, "removed");
        outcome = "unlisted";

        if (decision.action === "restrict") {
            const owner = await getAccount(env, report.ownerId);
            if (owner) {
                const updated = await setModsRestricted(
                    env, owner, true, decision.reason, decision.days,
                );
                until = updated.restricted?.mods?.until;
                outcome = decision.days
                    ? `unlisted, publishing restricted for ${decision.days} days`
                    : "unlisted, publishing restricted";
            }
        }
    }

    const decided: ModReport = {
        ...report,
        status: decision.action === "dismiss" ? "dismissed" : "actioned",
        outcome,
        decidedBy: by.nick,
        decidedAt: Math.floor(Date.now() / 1000),
        ...(until ? { until } : {}),
    };
    await env.OD_ACCOUNTS.put(reportKey(decided.id), JSON.stringify(decided),
                              { expirationTtl: LIMITS.ttlSeconds });
    return decided;
}
