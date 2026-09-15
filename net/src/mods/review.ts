// The review queue: a new listing is checked before it is shown, at the speed
// the scanner's limits actually allow.
//
// WHY A QUEUE AND NOT A CHECK ON PUBLISH
//
// The first version looked the hash up inside the publish request. That works
// for one person and falls apart for two, because the free VirusTotal tier
// allows FOUR LOOKUPS A MINUTE and 500 a day. Two authors publishing in the
// same minute meant the second got a 429, and scan.ts correctly records a
// failed lookup as no lookup -- so the listing went up reading "unscanned" and
// nothing ever came back to it. The check silently stopped happening exactly
// when the directory got busy enough to need it.
//
// So the rate limit shapes the process instead of breaking it. Publishing puts
// a listing in this queue as `pending`; a cron trigger drains it a few at a
// time; only then does it become `listed` or `held`. Nobody is refused and
// nothing is skipped -- a busy day makes the queue longer, not the checks
// weaker.
//
// WHY A DURABLE OBJECT
//
// Two reasons, and both are the free plan. A queue in KV would spend a write
// per enqueue and per drain against the 1,000/day that account creation lives
// on. And the daily scanner budget has to be counted exactly once across every
// concurrent cron invocation, which is a read-modify-write -- the one thing KV
// cannot do atomically and a Durable Object can.
//
// ONE OBJECT, not one per mod: a queue's whole job is to be a single line.

import { DurableObject } from "cloudflare:workers";
import type { Env } from "../env.js";

/**
 * Lookups per drain.
 *
 * VirusTotal's free tier is 4 requests a minute, and the cron runs once a
 * minute, so this is that number and not a tuning knob. Raising it without
 * raising the tier just moves the 429 from the publish path into the drain.
 */
export const PER_DRAIN = 4;

/**
 * Lookups a day.
 *
 * The tier allows 500. Twenty are held back so a manual re-check, or a second
 * drain racing the clock at midnight, cannot be the request that runs the
 * account into its limit.
 */
export const PER_DAY = 480;

/** How long a leased item stays leased before another drain may retry it. */
const LEASE_SECONDS = 300;

/**
 * Attempts before a listing is let through unreviewed.
 *
 * It is let through rather than held: three failed lookups is evidence that
 * VirusTotal is unreachable, not that the mod is bad, and holding somebody's
 * work hostage to our own outage would be the wrong way round. It goes live
 * reading "unscanned", which is exactly what it is.
 */
const MAX_TRIES = 3;

export interface QueueItem {
    id: string;
    tries: number;
}

export interface QueueStats {
    depth: number;
    leased: number;
    budgetUsed: number;
    budgetLeft: number;
    perDrain: number;
}

const today = (now = Date.now()) => Math.floor(now / 86_400_000);

export class ReviewDO extends DurableObject<Env> {
    private sql: SqlStorage;

    constructor(ctx: DurableObjectState, env: Env) {
        super(ctx, env);
        this.sql = ctx.storage.sql;
        this.sql.exec(`
            CREATE TABLE IF NOT EXISTS q (
                id     TEXT PRIMARY KEY,
                -- ORDER IS A SEQUENCE, NOT A CLOCK. Ordering by a
                -- second-resolution timestamp ties whenever two people publish
                -- in the same second, and then "first come first served" is
                -- whatever order SQLite happens to return -- including a
                -- republish failing to go to the back of the line.
                seq    INTEGER NOT NULL,
                at     INTEGER NOT NULL,
                tries  INTEGER NOT NULL DEFAULT 0,
                lease  INTEGER NOT NULL DEFAULT 0
            );
            CREATE INDEX IF NOT EXISTS q_seq ON q (seq);
            CREATE TABLE IF NOT EXISTS budget (day INTEGER PRIMARY KEY, used INTEGER NOT NULL);
        `);
    }

    private spent(day: number): number {
        const r = [...this.sql.exec<{ used: number }>(
            "SELECT used FROM budget WHERE day = ?", day)];
        return r[0]?.used ?? 0;
    }

    private spend(day: number, n: number): void {
        this.sql.exec(
            "INSERT INTO budget (day, used) VALUES (?, ?) " +
            "ON CONFLICT(day) DO UPDATE SET used = used + ?", day, n, n);
        // Yesterday's row is of no interest once the day has turned.
        this.sql.exec("DELETE FROM budget WHERE day < ?", day - 1);
    }

    override async fetch(request: Request): Promise<Response> {
        const url = new URL(request.url);
        const now = Math.floor(Date.now() / 1000);
        const day = today();

        // Put one in, or move it back to the end if it is already waiting --
        // republishing with a new hash should be reviewed, not jump the queue.
        if (url.pathname === "/enqueue") {
            const id = url.searchParams.get("id") ?? "";
            if (!id) return Response.json({ ok: false });
            const next = [...this.sql.exec<{ n: number }>(
                "SELECT COALESCE(MAX(seq), 0) + 1 AS n FROM q")][0]?.n ?? 1;
            this.sql.exec(
                "INSERT INTO q (id, seq, at, tries, lease) VALUES (?, ?, ?, 0, 0) " +
                "ON CONFLICT(id) DO UPDATE SET seq = ?, at = ?, tries = 0, lease = 0",
                id, next, now, next, now);
            return Response.json({ ok: true, position: this.positionOf(id) });
        }

        if (url.pathname === "/lease") {
            const want = Math.min(Number(url.searchParams.get("n") ?? PER_DRAIN), PER_DRAIN);
            const left = Math.max(0, PER_DAY - this.spent(day));
            const take = Math.min(want, left);
            if (take <= 0) return Response.json({ items: [], budgetLeft: 0 });

            const rows = [...this.sql.exec<{ id: string; tries: number }>(
                "SELECT id, tries FROM q WHERE lease < ? ORDER BY seq ASC LIMIT ?",
                now - LEASE_SECONDS, take)];
            for (const r of rows) {
                this.sql.exec("UPDATE q SET lease = ?, tries = tries + 1 WHERE id = ?", now, r.id);
            }
            // Counted at LEASE time, not on success. A lookup that 429s or
            // times out still spent a request against the tier, and a budget
            // that only counts the happy path is a budget that overruns.
            if (rows.length) this.spend(day, rows.length);

            return Response.json({
                items: rows.map((r) => ({ id: r.id, tries: r.tries + 1 })),
                budgetLeft: left - rows.length,
            });
        }

        if (url.pathname === "/done") {
            this.sql.exec("DELETE FROM q WHERE id = ?", url.searchParams.get("id") ?? "");
            return Response.json({ ok: true });
        }

        /** Has this one been tried too many times to keep trying? */
        if (url.pathname === "/exhausted") {
            const id = url.searchParams.get("id") ?? "";
            const r = [...this.sql.exec<{ tries: number }>(
                "SELECT tries FROM q WHERE id = ?", id)];
            return Response.json({ exhausted: (r[0]?.tries ?? 0) >= MAX_TRIES });
        }

        if (url.pathname === "/position") {
            return Response.json({
                position: this.positionOf(url.searchParams.get("id") ?? ""),
            });
        }

        if (url.pathname === "/stats") {
            const depth = [...this.sql.exec<{ n: number }>("SELECT COUNT(*) AS n FROM q")][0]?.n ?? 0;
            const leased = [...this.sql.exec<{ n: number }>(
                "SELECT COUNT(*) AS n FROM q WHERE lease >= ?", now - LEASE_SECONDS)][0]?.n ?? 0;
            const used = this.spent(day);
            return Response.json({
                depth, leased, budgetUsed: used,
                budgetLeft: Math.max(0, PER_DAY - used), perDrain: PER_DRAIN,
            } satisfies QueueStats);
        }

        return new Response("not found", { status: 404 });
    }

    /** 1-based place in the line, or 0 if it is not waiting. */
    private positionOf(id: string): number {
        const mine = [...this.sql.exec<{ seq: number }>("SELECT seq FROM q WHERE id = ?", id)];
        if (!mine.length) return 0;
        const ahead = [...this.sql.exec<{ n: number }>(
            "SELECT COUNT(*) AS n FROM q WHERE seq < ?", mine[0]!.seq)][0]?.n ?? 0;
        return ahead + 1;
    }
}

/**
 * What the review decides, given what the two checks found.
 *
 * A pure function on purpose: buried inside the cron handler this would only
 * ever run in production, and it is the part with actual judgement in it.
 *
 * Order matters. A malicious file is held even if the link also happens to be
 * broken, because "we could not reach it" is the less important thing to tell
 * somebody about a file engines are flagging.
 */
export function verdict(
    scan: { known: boolean; malicious?: number; suspicious?: number; engines?: number } | null,
    downloadReachable: boolean,
): { status: "listed" | "held"; hold?: string } {
    const bad = scan?.known ? (scan.malicious ?? 0) + (scan.suspicious ?? 0) : 0;
    if (bad > 0) {
        return {
            status: "held",
            hold: `${bad} of ${scan?.engines ?? 0} engines report this file as malicious.`,
        };
    }
    if (!downloadReachable) {
        return {
            status: "held",
            hold: "The download link did not answer, so nobody could install this.",
        };
    }
    // An unknown or absent scan does NOT hold a listing. "Nobody has ever
    // uploaded this file to VirusTotal" is the normal state of a new mod, and
    // holding every new mod for it would hold every mod.
    return { status: "listed" };
}

// ------------------------------------------------------------- the client ----

function stub(env: Env): DurableObjectStub {
    return env.MOD_REVIEW.get(env.MOD_REVIEW.idFromName("queue"));
}

export async function enqueue(env: Env, modId: string): Promise<number> {
    const r = await stub(env).fetch(
        `https://review/enqueue?id=${encodeURIComponent(modId)}`);
    const body = await r.json<{ position?: number }>();
    return body.position ?? 0;
}

export async function lease(env: Env, n = PER_DRAIN): Promise<QueueItem[]> {
    const r = await stub(env).fetch(`https://review/lease?n=${n}`);
    return (await r.json<{ items: QueueItem[] }>()).items;
}

export async function complete(env: Env, modId: string): Promise<void> {
    await stub(env).fetch(`https://review/done?id=${encodeURIComponent(modId)}`);
}

export async function exhausted(env: Env, modId: string): Promise<boolean> {
    const r = await stub(env).fetch(
        `https://review/exhausted?id=${encodeURIComponent(modId)}`);
    return (await r.json<{ exhausted: boolean }>()).exhausted;
}

export async function position(env: Env, modId: string): Promise<number> {
    const r = await stub(env).fetch(
        `https://review/position?id=${encodeURIComponent(modId)}`);
    return (await r.json<{ position: number }>()).position;
}

export async function stats(env: Env): Promise<QueueStats> {
    return await (await stub(env).fetch("https://review/stats")).json<QueueStats>();
}

/**
 * Roughly how long a wait is, in minutes, for telling an author.
 *
 * Deliberately a range in the caller's wording rather than a promise: the
 * drain is one cron tick a minute and a tick can be skipped or delayed.
 */
export function waitMinutes(pos: number): number {
    return Math.max(1, Math.ceil(pos / PER_DRAIN));
}
