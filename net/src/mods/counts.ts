// How many people went to get a mod.
//
// WHY A DURABLE OBJECT AND NOT KV
//
// The free plan allows 1,000 KV WRITES A DAY, shared with everything else the
// service does -- creating an account costs three of them, which puts the
// practical ceiling at a few hundred new accounts a day (README.md, "The limits
// that actually bind"). A download counter in KV would spend that entire budget
// on a few hundred clicks and then start failing account creation. It is not a
// close call: counters and KV are the wrong shape for each other.
//
// Durable Object requests bill against a separate 100,000/day budget, and a DO
// with SQLite can do a read-modify-write atomically, which KV cannot do at all.
// So the counter lives here, one object per mod, keyed by the mod id.
//
// ONE OBJECT PER MOD, not one for the registry. A single object would serialise
// every download of every mod through one thread; per-mod, two mods never wait
// on each other, and a mod nobody downloads costs nothing because its object is
// never instantiated.
//
// WHAT "UNIQUE" MEANS HERE, EXACTLY
//
// The first hit from an address on a given day. Not the first ever.
//
// That is a deliberate retreat from the obvious definition, and the reason is
// PRIVACY.md: this service promises it keeps no address. Counting unique
// downloaders for all time means keeping a per-person marker for all time, and
// a keyed hash of an IP held indefinitely is still a record of who visited.
// A DAILY marker, salted with the date and dropped after 48 hours, answers
// "how many different people today" without keeping anything that outlives the
// question.
//
// The cost is honest and has to be said wherever the number is shown: somebody
// who downloads on Monday and again on Friday counts twice. `uniqueBasis` below
// travels with the number so a client cannot render it as "unique people".

import { DurableObject } from "cloudflare:workers";
import type { Env } from "../env.js";

/** Days of per-address markers kept. Two, so "yesterday" is still complete. */
const MARKER_DAYS = 2;

export interface Counts {
    downloads: number;
    unique: number;
    uniqueBasis: string;
}

export const UNIQUE_BASIS = "first download per address per day, summed over days";

export class ModCountsDO extends DurableObject<Env> {
    private sql: SqlStorage;

    constructor(ctx: DurableObjectState, env: Env) {
        super(ctx, env);
        this.sql = ctx.storage.sql;
        this.sql.exec(`
            CREATE TABLE IF NOT EXISTS n    (k TEXT PRIMARY KEY, v INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS seen (h TEXT NOT NULL, day INTEGER NOT NULL,
                                             PRIMARY KEY (h, day));
        `);
    }

    private get(k: string): number {
        const rows = [...this.sql.exec<{ v: number }>("SELECT v FROM n WHERE k = ?", k)];
        return rows[0]?.v ?? 0;
    }

    private bump(k: string, by = 1): number {
        const v = this.get(k) + by;
        this.sql.exec("INSERT OR REPLACE INTO n (k, v) VALUES (?, ?)", k, v);
        return v;
    }

    override async fetch(request: Request): Promise<Response> {
        const url = new URL(request.url);

        if (url.pathname === "/hit") {
            const marker = url.searchParams.get("m") ?? "";
            const day = Number(url.searchParams.get("d") ?? "0");
            const downloads = this.bump("downloads");

            let unique = this.get("unique");
            if (marker) {
                // INSERT OR IGNORE makes "have we seen this marker" and "record
                // it" one statement. Checking first and then inserting is two,
                // and two statements are two chances for a concurrent hit to
                // slip between them and be counted twice.
                // THE KEY IS (marker, day), NOT THE MARKER ALONE.
                //
                // Callers already salt the marker with the date, so in practice
                // the same person on two days arrives as two different markers.
                // Keying on the marker alone would nonetheless be a rule held
                // in a DIFFERENT FILE from the table that depends on it: pass
                // an unsalted marker and "unique per day" silently degrades
                // into "unique until the prune below removes the row", which is
                // a two-day window rather than a day and is nobody's intent.
                //
                // Both, then. The composite key makes this table correct on its
                // own, and the salt keeps a stored hash uncorrelatable across
                // days by anyone who reads it.
                const c = this.sql.exec(
                    "INSERT OR IGNORE INTO seen (h, day) VALUES (?, ?)", marker, day,
                );
                if (c.rowsWritten > 0) unique = this.bump("unique");
                this.sql.exec("DELETE FROM seen WHERE day < ?", day - MARKER_DAYS);
            }
            return Response.json({ downloads, unique, uniqueBasis: UNIQUE_BASIS });
        }

        if (url.pathname === "/counts") {
            return Response.json({
                downloads: this.get("downloads"),
                unique: this.get("unique"),
                uniqueBasis: UNIQUE_BASIS,
            });
        }

        // Only when a listing is withdrawn. Left otherwise: a counter that
        // resets when a mod is updated would make the number meaningless.
        if (url.pathname === "/forget") {
            this.sql.exec("DELETE FROM n");
            this.sql.exec("DELETE FROM seen");
            return Response.json({ ok: true });
        }

        return new Response("not found", { status: 404 });
    }
}

/** Days since the epoch. The bucket a marker is salted with and stored under. */
export const dayNumber = (now = Date.now()) => Math.floor(now / 86_400_000);

function stub(env: Env, modId: string): DurableObjectStub {
    return env.MOD_COUNTS.get(env.MOD_COUNTS.idFromName(modId));
}

export async function readCounts(env: Env, modId: string): Promise<Counts> {
    const res = await stub(env, modId).fetch("https://counts/counts");
    return await res.json<Counts>();
}

export async function recordHit(
    env: Env, modId: string, marker: string, day = dayNumber(),
): Promise<Counts> {
    const res = await stub(env, modId).fetch(
        `https://counts/hit?m=${encodeURIComponent(marker)}&d=${day}`,
    );
    return await res.json<Counts>();
}

export async function forgetCounts(env: Env, modId: string): Promise<void> {
    await stub(env, modId).fetch("https://counts/forget");
}
