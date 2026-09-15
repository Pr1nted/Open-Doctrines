// The board itself: every open listing, and the reports filed against them.
//
// ── WHY A DURABLE OBJECT, AND ONE OF THEM ──
//
// A listing is a WRITE, and the free plan's 1,000 KV writes a day are the same
// budget account creation spends (see mods/counts.ts for the same argument).
// A board that people actually use would spend that budget and then start
// failing signups. Durable Object requests bill against their own 100,000/day.
//
// One object rather than one per listing: the board is read as a whole ("what
// games are open right now"), a per-account limit has to be counted across
// listings, and expiry is a single alarm instead of hundreds.
//
// ── THE ALARM IS WHAT KEEPS THE CHANNEL HONEST ──
//
// A listing that has expired is worse than no listing: somebody joins a game
// that ended an hour ago and concludes the board is dead. The alarm removes
// them here AND closes the Discord message, so the two never disagree.

import { DurableObject } from "cloudflare:workers";
import type { Env } from "../env.js";
import { closeMessage, postListing } from "./discord.js";
import { LIMITS, forClients, visible, type Listing } from "./board.js";

/** How often the sweeper runs. Listings expire on their own clock, not this. */
const ALARM_SECONDS = 60;

interface Row extends Record<string, SqlStorageValue> {
    id: string;
    account_id: string;
    created: number;
    expires: number;
    closed: number;
    hidden: number;
    message_id: string | null;
    payload: string;
}

export class LfgBoardDO extends DurableObject<Env> {
    private sql: SqlStorage;

    constructor(ctx: DurableObjectState, env: Env) {
        super(ctx, env);
        this.sql = ctx.storage.sql;
        this.sql.exec(`
            CREATE TABLE IF NOT EXISTS listings (
                id         TEXT PRIMARY KEY,
                account_id TEXT NOT NULL,
                created    INTEGER NOT NULL,
                expires    INTEGER NOT NULL,
                closed     INTEGER NOT NULL DEFAULT 0,
                hidden     INTEGER NOT NULL DEFAULT 0,
                message_id TEXT,
                payload    TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS reports (
                listing_id  TEXT NOT NULL,
                reporter_id TEXT NOT NULL,
                reason      TEXT NOT NULL,
                note        TEXT,
                at          INTEGER NOT NULL,
                PRIMARY KEY (listing_id, reporter_id)
            );
            CREATE TABLE IF NOT EXISTS posts (
                account_id TEXT NOT NULL,
                at         INTEGER NOT NULL
            );
        `);
    }

    override async fetch(request: Request): Promise<Response> {
        const url = new URL(request.url);
        const body = request.method === "POST" ? ((await request.json()) as Record<string, any>) : {};
        const now = Math.floor(Date.now() / 1000);

        switch (url.pathname) {
            case "/list":     return this.json({ listings: this.open(now).map(forClients) });
            case "/all":      return this.json({ listings: this.everything().map(forClients) });
            case "/post":     return this.json(await this.post(body.listing as Listing, now));
            case "/close":    return this.json(await this.close(String(body.id), String(body.accountId ?? ""), !!body.moderator, now));
            case "/report":   return this.json(this.report(body, now));
            case "/moderate": return this.json(await this.moderate(String(body.op), String(body.id), now));
            default:          return new Response("no such board route", { status: 404 });
        }
    }

    // ---------------------------------------------------------------- reading

    private rows(): Row[] {
        return this.sql.exec<Row>("SELECT * FROM listings ORDER BY created DESC").toArray();
    }

    private listingOf(row: Row): Listing {
        const listing = JSON.parse(row.payload) as Listing;
        listing.closed = !!row.closed;
        listing.hidden = !!row.hidden;
        listing.discordMessageId = row.message_id ?? undefined;
        listing.reports = this.sql
            .exec<{ n: number }>("SELECT COUNT(*) AS n FROM reports WHERE listing_id = ?", row.id)
            .one().n;
        return listing;
    }

    private open(now: number): Listing[] {
        return visible(this.rows().map((r) => this.listingOf(r)), now);
    }

    /** Everything, hidden and closed included. The moderator's view. */
    private everything(): Listing[] {
        return this.rows().map((r) => this.listingOf(r));
    }

    // ---------------------------------------------------------------- writing

    private async post(listing: Listing, now: number): Promise<Record<string, unknown>> {
        // One open listing per account. Posting again replaces the one you
        // have: a person whose game filled up and started a new one should not
        // have to remember to close the old listing, and two listings from one
        // account is the shape spam takes here.
        const day = now - 24 * 60 * 60;
        this.sql.exec("DELETE FROM posts WHERE at < ?", day);
        const today = this.sql
            .exec<{ n: number }>("SELECT COUNT(*) AS n FROM posts WHERE account_id = ?", listing.accountId)
            .one().n;
        if (today >= LIMITS.perDay) {
            return { ok: false, status: 429, reason: `That is ${LIMITS.perDay} listings today. Try again tomorrow.` };
        }

        for (const row of this.sql.exec<Row>(
            "SELECT * FROM listings WHERE account_id = ? AND closed = 0", listing.accountId).toArray()) {
            await this.closeRow(row, "replaced by a newer listing", now);
        }

        const messageId = await postListing(this.env, listing);
        this.sql.exec(
            "INSERT INTO listings (id, account_id, created, expires, closed, hidden, message_id, payload)" +
            " VALUES (?, ?, ?, ?, 0, 0, ?, ?)",
            listing.id, listing.accountId, listing.createdAt, listing.expiresAt,
            messageId ?? null, JSON.stringify(listing));
        this.sql.exec("INSERT INTO posts (account_id, at) VALUES (?, ?)", listing.accountId, now);
        await this.ctx.storage.setAlarm(Date.now() + ALARM_SECONDS * 1000);
        return { ok: true, listing: forClients({ ...listing, discordMessageId: messageId ?? undefined }) };
    }

    private async close(id: string, accountId: string, moderator: boolean, now: number): Promise<Record<string, unknown>> {
        const row = this.sql.exec<Row>("SELECT * FROM listings WHERE id = ?", id).toArray()[0];
        if (!row) return { ok: false, status: 404, reason: "No such listing." };
        if (!moderator && row.account_id !== accountId) {
            return { ok: false, status: 403, reason: "That listing is not yours." };
        }
        await this.closeRow(row, moderator ? "closed by a moderator" : "closed by the host", now);
        return { ok: true };
    }

    private report(body: Record<string, any>, now: number): Record<string, unknown> {
        const id = String(body.id ?? "");
        const row = this.sql.exec<Row>("SELECT * FROM listings WHERE id = ?", id).toArray()[0];
        if (!row) return { ok: false, status: 404, reason: "No such listing." };
        // One report per account per listing. A second one is not more
        // evidence, and counting it would let one person look like a crowd.
        this.sql.exec(
            "INSERT OR REPLACE INTO reports (listing_id, reporter_id, reason, note, at) VALUES (?, ?, ?, ?, ?)",
            id, String(body.reporterId ?? ""), String(body.reason ?? "other"),
            String(body.note ?? "").slice(0, 400), now);
        const count = this.sql
            .exec<{ n: number }>("SELECT COUNT(*) AS n FROM reports WHERE listing_id = ?", id).one().n;
        return { ok: true, listing: forClients(this.listingOf(row)), reports: count };
    }

    private async moderate(op: string, id: string, now: number): Promise<Record<string, unknown>> {
        const row = this.sql.exec<Row>("SELECT * FROM listings WHERE id = ?", id).toArray()[0];
        if (!row) return { ok: false, status: 404, reason: "No such listing." };
        if (op === "hide") {
            this.sql.exec("UPDATE listings SET hidden = 1 WHERE id = ?", id);
            await this.closeRow(row, "removed by a moderator", now);
        } else if (op === "show") {
            this.sql.exec("UPDATE listings SET hidden = 0 WHERE id = ?", id);
        } else if (op === "purge") {
            this.sql.exec("DELETE FROM listings WHERE id = ?", id);
            this.sql.exec("DELETE FROM reports WHERE listing_id = ?", id);
        } else {
            return { ok: false, status: 400, reason: "op must be hide, show or purge." };
        }
        return { ok: true, listings: this.everything().map(forClients) };
    }

    /** Mark closed, and say so in the channel. Both, or the two disagree. */
    private async closeRow(row: Row, why: string, now: number): Promise<void> {
        this.sql.exec("UPDATE listings SET closed = 1, expires = ? WHERE id = ?", now, row.id);
        if (row.message_id) await closeMessage(this.env, row.message_id, this.listingOf(row), why);
    }

    // ----------------------------------------------------------------- alarm

    override async alarm(): Promise<void> {
        const now = Math.floor(Date.now() / 1000);
        for (const row of this.sql.exec<Row>(
            "SELECT * FROM listings WHERE closed = 0 AND expires <= ?", now).toArray()) {
            await this.closeRow(row, "expired", now);
        }
        // A closed listing is kept for a day so a moderator can still read what
        // was posted, and so a report has something to point at.
        this.sql.exec("DELETE FROM listings WHERE closed = 1 AND expires < ?", now - 24 * 60 * 60);
        this.sql.exec("DELETE FROM reports WHERE listing_id NOT IN (SELECT id FROM listings)");
        const remaining = this.sql.exec<{ n: number }>("SELECT COUNT(*) AS n FROM listings").one().n;
        if (remaining > 0) await this.ctx.storage.setAlarm(Date.now() + ALARM_SECONDS * 1000);
    }

    private json(body: unknown): Response {
        return new Response(JSON.stringify(body), { headers: { "content-type": "application/json" } });
    }
}
