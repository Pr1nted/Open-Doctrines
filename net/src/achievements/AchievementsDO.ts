// One account's achievements: what it has been granted, and its clock.
//
// A Durable Object per account, for the same reason the mod counters are one
// (mods/counts.ts): every grant is a write, the free plan's 1,000 KV writes a
// day are the budget account creation lives on, and an account that earns ten
// achievements in an evening must not be able to stop strangers signing up.
// Per account rather than one global object so that two players never queue
// behind each other, and an account that never plays costs nothing.
//
// The object decides nothing about the catalog -- claim.ts does. What it owns
// is state that has to be read-modify-written atomically: the grant set, the
// first time this account ever started a play session, and the budget.

import { DurableObject } from "cloudflare:workers";
import type { Env } from "../env.js";

/** Grants a burst may spend before the budget has to refill. */
export const BUDGET_CAPACITY = 12;
/** Seconds of real time per refilled grant. 15 an hour. */
export const BUDGET_REFILL_SECONDS = 240;

export interface Decision {
    ach: string;
    /** Signed token to store, when granted now or previously. */
    token?: string;
    /** Seconds until this claim could succeed, when only time stands in the way. */
    retryAfter?: number;
    /** Why it will never succeed as sent. */
    refused?: string;
}

export class AchievementsDO extends DurableObject<Env> {
    private sql: SqlStorage;

    constructor(ctx: DurableObjectState, env: Env) {
        super(ctx, env);
        this.sql = ctx.storage.sql;
        this.sql.exec(`
            CREATE TABLE IF NOT EXISTS grants (ach TEXT PRIMARY KEY, token TEXT NOT NULL, iat INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS meta   (k TEXT PRIMARY KEY, v INTEGER NOT NULL);
        `);
    }

    private meta(k: string): number | null {
        const rows = [...this.sql.exec<{ v: number }>("SELECT v FROM meta WHERE k = ?", k)];
        return rows.length ? rows[0]!.v : null;
    }

    private setMeta(k: string, v: number): void {
        this.sql.exec("INSERT OR REPLACE INTO meta (k, v) VALUES (?, ?)", k, v);
    }

    private grants(): Map<string, { token: string; iat: number }> {
        const out = new Map<string, { token: string; iat: number }>();
        for (const r of this.sql.exec<{ ach: string; token: string; iat: number }>(
            "SELECT ach, token, iat FROM grants ORDER BY iat")) out.set(r.ach, { token: r.token, iat: r.iat });
        return out;
    }

    /** Budget after refill, without spending. */
    private budget(now: number): number {
        const level = this.meta("budget") ?? BUDGET_CAPACITY;
        const at = this.meta("budget_at") ?? now;
        const refilled = Math.floor((now - at) / BUDGET_REFILL_SECONDS);
        return Math.min(BUDGET_CAPACITY, level + Math.max(0, refilled));
    }

    private spend(now: number): void {
        const level = this.budget(now) - 1;
        // budget_at advances only by whole refill periods, so a partial period
        // already accrued is not thrown away by a spend.
        const at = this.meta("budget_at") ?? now;
        const periods = Math.max(0, Math.floor((now - at) / BUDGET_REFILL_SECONDS));
        this.setMeta("budget", level);
        this.setMeta("budget_at", level + 1 >= BUDGET_CAPACITY ? now : at + periods * BUDGET_REFILL_SECONDS);
    }

    override async fetch(request: Request): Promise<Response> {
        const url = new URL(request.url);
        const now = Math.floor(Date.now() / 1000);

        if (url.pathname === "/session") {
            // The first play session this account ever opened. Kept as the
            // EARLIEST, so a second device cannot reset the clock forward or
            // back.
            const first = this.meta("first_play");
            if (first === null) this.setMeta("first_play", now);
            return Response.json({ firstPlay: first ?? now });
        }

        if (url.pathname === "/list") {
            const list = [...this.grants().values()].map((g) => g.token);
            return Response.json({ grants: list, firstPlay: this.meta("first_play") });
        }

        if (url.pathname === "/wipe") {
            this.sql.exec("DELETE FROM grants");
            this.sql.exec("DELETE FROM meta");
            return Response.json({ ok: true });
        }

        if (url.pathname === "/decide") {
            // Phase one of a claim: what the clock and budget allow, given the
            // catalog requirements claim.ts passes in. Nothing is written.
            const body = await request.json() as {
                claims: { ach: string; requires: string[]; minFirstPlay: number }[];
            };
            const have = this.grants();
            const first = this.meta("first_play");
            let budget = this.budget(now);
            const decisions: Decision[] = [];
            const willHave = new Set(have.keys());
            for (const c of body.claims) {
                const existing = have.get(c.ach);
                if (existing) { decisions.push({ ach: c.ach, token: existing.token }); continue; }
                if (first === null) { decisions.push({ ach: c.ach, refused: "no_session" }); continue; }
                const missing = c.requires.filter((r) => !willHave.has(r));
                if (missing.length) { decisions.push({ ach: c.ach, refused: `requires:${missing.join(",")}` }); continue; }
                const wait = first + c.minFirstPlay - now;
                if (wait > 0) { decisions.push({ ach: c.ach, retryAfter: wait }); continue; }
                if (budget <= 0) {
                    const at = this.meta("budget_at") ?? now;
                    decisions.push({ ach: c.ach, retryAfter: Math.max(1, at + BUDGET_REFILL_SECONDS - now) });
                    continue;
                }
                budget -= 1;
                willHave.add(c.ach);
                decisions.push({ ach: c.ach });
            }
            return Response.json({ decisions });
        }

        if (url.pathname === "/commit") {
            // Phase two: store what claim.ts signed. Re-checks the budget, so
            // two concurrent claims cannot both spend the last grant.
            const body = await request.json() as { grants: { ach: string; token: string }[] };
            const have = this.grants();
            const stored: { ach: string; token: string }[] = [];
            for (const g of body.grants) {
                const existing = have.get(g.ach);
                if (existing) { stored.push({ ach: g.ach, token: existing.token }); continue; }
                if (this.budget(now) <= 0) continue;
                this.spend(now);
                this.sql.exec("INSERT INTO grants (ach, token, iat) VALUES (?, ?, ?)", g.ach, g.token, now);
                stored.push(g);
            }
            return Response.json({ stored });
        }

        return new Response("not found", { status: 404 });
    }
}
