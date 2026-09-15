// The mod registry: listings, never files.
//
// WHAT THIS IS, AND WHY IT IS SHAPED LIKE THIS
//
// A mod author publishes a RECORD here -- a name, a page to read about it, and
// an https URL where the `.odmod` can be fetched. The bytes stay on the
// author's own host. Nothing in this service stores, proxies or serves a mod.
//
// That is not only about Cloudflare's free plan, though it is that too. It is
// the same decision the game already made and says out loud in
// src/mods/ModUpdates.h: "LOOKS, NEVER TOUCHES." The game will not download or
// install a mod, because doing so means fetching and running code chosen by a
// third party, which is the one thing the capability sandbox exists to prevent.
// A registry that hosted the files would be the natural place to undo that, so
// it does not hold them.
//
// WHAT A LISTING IS FOR, THEN: being found. It advertises, it points at the
// author's page, and it counts how often people went there.
//
// THE KEY PREFIX IS `pkg:`, NOT `mod:`
//
// `mod:` is already MODERATION -- `mod:report:` and `mod:psid:` in
// moderation/reports.ts. Two features called "mod" that mean different things
// share one namespace only once, and then a listing id that happened to look
// like a report id would read somebody else's record.

import type { Env } from "../env.js";
import type { Account } from "../accounts/store.js";

/** Bumped when the guidelines change in a way that needs agreeing to again. */
export const GUIDELINES_VERSION = "2026-09-15";

export const LIMITS = {
    name: 80,
    summary: 160,
    description: 4000,
    url: 512,
    tag: 24,
    tags: 8,
    /** Listings one account may hold. Not a quality bar -- a flood ceiling. */
    perOwner: 50,
    /** How many a browse page returns. */
    page: 20,
};

/** Mods held before the computed `modmaker` tag applies. */
export const MODMAKER_THRESHOLD = 5;

// --------------------------------------------------------------- the id ----
//
// ONE list, and the matcher is BUILT from it rather than written out beside it.
// The rule is ModPackage.cpp's, and it has to stay ModPackage.cpp's: a listing
// id that the game would refuse to load is a listing nobody can install.
//
// Lowercase is load-bearing rather than tidy. The id is both the `Storage`
// namespace and the trust-pinning key in the game, so on a case-insensitive
// filesystem `com.a.Mod` and `com.a.mod` are two identities sharing one store.
export const ID_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789._-";
const ID_CLASS = `[${ID_CHARS.replace(/[.\-\]\\]/g, "\\$&")}]`;
const ID_RE = new RegExp(`^${ID_CLASS}{3,128}$`);

export function isModId(v: string): boolean {
    if (!ID_RE.test(v)) return false;
    if (!v.includes(".")) return false;              // reverse-DNS, so: a dot
    if (v.startsWith(".") || v.endsWith(".")) return false;
    return true;
}

/** semver, as MANIFEST.json means it. */
const VERSION_RE = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/;
export const isVersion = (v: string) => VERSION_RE.test(v) && v.length <= 64;

const SHA256_RE = /^[0-9a-f]{64}$/;
export const isSha256 = (v: string) => SHA256_RE.test(v);

/**
 * The same URL rule the GAME applies before it will fetch an updateUrl, in
 * src/mods/ModUpdates.cpp: https only, bounded, and no character that could end
 * a shell word or start a command.
 *
 * The game invokes curl without a shell, and so does nothing here -- this is a
 * Worker. It is kept identical anyway, because a URL this service accepts and
 * the game later refuses is a listing that looks published and cannot work.
 */
export function isPublishableUrl(u: string): boolean {
    if (!u.startsWith("https://")) return false;
    if (u.length > LIMITS.url) return false;
    for (const ch of u) {
        const c = ch.charCodeAt(0);
        if (c < 0x21 || c > 0x7e) return false;
        if ("\"'`\\<>|;&$(){}[]^".includes(ch)) return false;
    }
    return true;
}

// ------------------------------------------------------------- the record ----

export type Side = "client" | "server" | "both";
export const SIDES: Side[] = ["client", "server", "both"];

/**
 * Where a listing is in its life.
 *
 * `pending` is the state everything starts in and the reason the queue exists:
 * a listing is checked BEFORE it is shown, not after. `held` is what a failed
 * check produces -- the author is told why and can fix it and republish, which
 * is different from `removed`, which a moderator did and the author cannot undo.
 */
export type Status = "pending" | "listed" | "held" | "unlisted" | "removed";

export interface Scan {
    /** When we asked. */
    at: number;
    /**
     * Whether VirusTotal had ever seen this hash.
     *
     * FALSE IS NOT "CLEAN" AND MUST NEVER BE RENDERED AS CLEAN. An unknown hash
     * is the normal state for a mod nobody has uploaded there, and it is also
     * the state of a file written five minutes ago to attack somebody. The two
     * are indistinguishable from here.
     */
    known: boolean;
    malicious?: number;
    suspicious?: number;
    /** How many engines reported at all, so a count has its denominator. */
    engines?: number;
    permalink?: string;
}

export interface Listing {
    id: string;                 // the MANIFEST.json id. Immutable, the identity.
    ownerId: string;
    ownerNick: string;

    name: string;
    version: string;
    summary: string;
    description: string;

    /** Where a person reads ABOUT the mod. This is what browse links to. */
    page: string;
    /** Where the .odmod itself is fetched from. Behind /mods/<id>/get. */
    downloadUrl: string;
    /** Mirrors MANIFEST "updateUrl", if the author publishes one. */
    updateUrl?: string;

    /**
     * The author's declared SHA-256 of the .odmod.
     *
     * Declared, and the word matters everywhere this is shown. We do not hold
     * the file, so we cannot confirm the bytes at `downloadUrl` still hash to
     * this. It is what the scan below was asked about and what a cautious
     * player can check by hand after downloading.
     */
    sha256: string;
    sizeBytes?: number;

    gearbox: string;            // "1.2"
    side: Side;
    /**
     * The capability modules MANIFEST.json asks for.
     *
     * Listed because it is the most useful thing a browsing player can be
     * told: a mod requesting GameProcess changes how turns resolve, and one
     * requesting only UI cannot. It comes from the author like everything else
     * here -- the game is what enforces it at load.
     */
    modules: string[];
    tags: string[];
    thumbnail?: string;

    created: number;
    updated: number;
    status: Status;
    /** Absent until a scan has been attempted. */
    scan?: Scan;
    /** When the review queue last looked at it. */
    reviewedAt?: number;
    /** Why it is `held`, in words the author is shown. */
    hold?: { reason: string; at: number };
    /** Which guidelines the owner had agreed to when this was last written. */
    guidelines: string;
}

/**
 * The tags an account holds right now.
 *
 * `modmaker` is COMPUTED and never granted, which is the whole reason it is not
 * a Badge. accounts/badges.ts is explicit that a badge means somebody decided --
 * "no self-serve path and no automation" -- so an automatically earned tag must
 * live somewhere else or that sentence stops being true.
 *
 * Derived rather than stored, so it costs no KV write, cannot drift from the
 * listings it describes, and goes away again if they are withdrawn.
 */
export function tagsFrom(badges: readonly string[], ownedCount: number): string[] {
    return [...badges, ...(ownedCount >= MODMAKER_THRESHOLD ? ["modmaker"] : [])];
}

// ----------------------------------------------------------------- keys ----

const modKey = (id: string) => `pkg:mod:${id}`;
const ownerKey = (accountId: string, id: string) => `pkg:own:${accountId}:${id}`;

/**
 * The browse index, newest first.
 *
 * KV lists lexicographically by key and offers no other order, so "newest
 * first" has to be built into the key. The timestamp is SUBTRACTED from a fixed
 * ceiling so that a larger `created` sorts earlier, and zero-padded so the
 * comparison is not string-vs-number.
 *
 * It costs one extra write per publish. Publishing is rare -- browsing is not,
 * and the alternative is reading every listing to sort them.
 */
const NEWEST_CEILING = 9_999_999_999;
const browseKey = (created: number, id: string) =>
    `pkg:new:${String(NEWEST_CEILING - created).padStart(10, "0")}:${id}`;

// ------------------------------------------------------------ validation ----

// Written as escapes, never as the characters themselves. Typed literally
// this class collapsed three times into the range space-to-hyphen, which
// strips spaces and punctuation out of every name and summary it touches.
// eslint-disable-next-line no-control-regex
const CONTROL = /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g;

/**
 * Strip control characters and clip. Same treatment tournamentSet gives its
 * input, and for the same reason: this is served to everybody who opens browse,
 * so it is the one place a stray field would reach every client.
 */
function clean(v: unknown, max: number): string {
    return typeof v === "string"
        ? v.replace(CONTROL, "").trim().slice(0, max)
        : "";
}

export interface Rejected {
    ok: false;
    status: number;
    code: string;
    message: string;
}

export interface Draft {
    id: string;
    name: string;
    version: string;
    summary: string;
    description: string;
    page: string;
    downloadUrl: string;
    updateUrl?: string;
    sha256: string;
    sizeBytes?: number;
    gearbox: string;
    side: Side;
    modules: string[];
    tags: string[];
    thumbnail?: string;
}

const KNOWN_MODULES = [
    "Core", "UI", "GameState.Read", "GameState.Write", "GameProcess",
    "Assets", "Neural", "Diplomacy", "Map", "Storage",
];

export function parseDraft(body: unknown): Draft | Rejected {
    const b = (body ?? {}) as Record<string, unknown>;
    const no = (code: string, message: string): Rejected =>
        ({ ok: false, status: 400, code, message });

    const id = clean(b.id, 128).toLowerCase();
    if (!isModId(id)) {
        return no("bad_id",
            "A mod id is 3-128 characters of a-z, 0-9, dot, dash or underscore, "
            + "contains a dot, and does not start or end with one. It must be the "
            + "same id as MANIFEST.json, because that is what the game installs.");
    }

    const version = clean(b.version, 64);
    if (!isVersion(version)) return no("bad_version", "Version must be semver, like 1.2.0.");

    const sha256 = clean(b.sha256, 64).toLowerCase();
    if (!isSha256(sha256)) {
        return no("bad_sha256", "sha256 must be the 64-character hex digest of the .odmod file.");
    }

    const page = clean(b.page, LIMITS.url);
    const downloadUrl = clean(b.downloadUrl, LIMITS.url);
    if (!isPublishableUrl(page)) return no("bad_page", "The mod's page must be an https:// URL.");
    if (!isPublishableUrl(downloadUrl)) {
        return no("bad_download", "The download URL must be an https:// URL.");
    }

    const updateUrl = clean(b.updateUrl, LIMITS.url);
    if (updateUrl && !isPublishableUrl(updateUrl)) {
        return no("bad_update", "updateUrl must be an https:// URL.");
    }
    const thumbnail = clean(b.thumbnail, LIMITS.url);
    if (thumbnail && !isPublishableUrl(thumbnail)) {
        return no("bad_thumbnail", "The thumbnail must be an https:// URL.");
    }

    const name = clean(b.name, LIMITS.name);
    if (name.length < 2) return no("bad_name", "Give the mod a name.");
    const summary = clean(b.summary, LIMITS.summary);
    if (summary.length < 8) return no("bad_summary", "Write a one-line summary.");

    const side = SIDES.includes(b.side as Side) ? (b.side as Side) : "both";

    const gearbox = clean(b.gearbox, 8);
    if (!/^\d+\.\d+$/.test(gearbox)) {
        return no("bad_gearbox", "gearbox is MAJOR.MINOR, like 1.2.");
    }

    // An UNKNOWN module is refused rather than dropped, which is the rule the
    // loader already follows: silently losing a capability a mod believes it
    // holds is worse than saying no.
    const modules = Array.isArray(b.modules)
        ? (b.modules as unknown[]).map((m) => clean(m, 32)).filter(Boolean).slice(0, 12)
        : [];
    for (const m of modules) {
        if (!KNOWN_MODULES.includes(m)) return no("bad_module", `Unknown capability module: ${m}`);
    }

    const tags = Array.isArray(b.tags)
        ? (b.tags as unknown[])
            .map((t) => clean(t, LIMITS.tag).toLowerCase())
            .filter(Boolean).slice(0, LIMITS.tags)
        : [];

    const sizeRaw = Number(b.sizeBytes ?? 0);
    const sizeBytes = Number.isFinite(sizeRaw) && sizeRaw > 0
        ? Math.min(Math.floor(sizeRaw), 2_000_000_000) : undefined;

    return {
        id, name, version, summary,
        description: clean(b.description, LIMITS.description),
        page, downloadUrl,
        ...(updateUrl ? { updateUrl } : {}),
        sha256,
        ...(sizeBytes ? { sizeBytes } : {}),
        gearbox, side, modules, tags,
        ...(thumbnail ? { thumbnail } : {}),
    };
}

// ------------------------------------------------------------- the store ----

export async function getListing(env: Env, id: string): Promise<Listing | null> {
    if (!isModId(id)) return null;
    const raw = await env.OD_ACCOUNTS.get(modKey(id));
    if (!raw) return null;
    try { return JSON.parse(raw) as Listing; } catch { return null; }
}

/** How many listings an account holds, capped -- callers only ever compare. */
export async function countOwned(env: Env, accountId: string, cap = 64): Promise<number> {
    const listed = await env.OD_ACCOUNTS.list({
        prefix: `pkg:own:${accountId}:`, limit: cap,
    });
    return listed.keys.length;
}

export async function listOwned(env: Env, accountId: string): Promise<string[]> {
    const listed = await env.OD_ACCOUNTS.list({
        prefix: `pkg:own:${accountId}:`, limit: LIMITS.perOwner + 1,
    });
    return listed.keys.map((k) => k.name.slice(`pkg:own:${accountId}:`.length));
}

export type PublishResult = { ok: true; listing: Listing; created: boolean } | Rejected;

/**
 * Create or update a listing.
 *
 * The id is the identity and it does not move: an update must come from the
 * account that first published it. Otherwise the first person to notice a
 * popular mod's id could republish it pointing somewhere else, which is the
 * whole attack this registry could otherwise introduce to a game that had none.
 */
export async function publish(
    env: Env, owner: Account, draft: Draft, now = Math.floor(Date.now() / 1000),
): Promise<PublishResult> {
    const existing = await getListing(env, draft.id);

    if (existing && existing.ownerId !== owner.id) {
        // Deliberately not "that id belongs to somebody else" -- a probe should
        // not be able to map which ids are taken and by whom.
        return {
            ok: false, status: 409, code: "id_taken",
            message: "That mod id is already published by another account.",
        };
    }
    if (existing && existing.status === "removed") {
        return {
            ok: false, status: 403, code: "removed",
            message: "This listing was removed by a moderator and cannot be republished.",
        };
    }
    if (!existing) {
        const held = await countOwned(env, owner.id, LIMITS.perOwner + 1);
        if (held >= LIMITS.perOwner) {
            return {
                ok: false, status: 429, code: "too_many",
                message: `An account may hold ${LIMITS.perOwner} listings.`,
            };
        }
    }

    const created = existing?.created ?? now;

    // WHAT NEEDS REVIEWING: anything new, and anything whose FILE changed.
    //
    // Fixing a typo in a summary does not, and that matters more than it looks:
    // the scanner's budget is 500 lookups a day for the whole service, so
    // re-reviewing on every edit would let one author editing their description
    // spend the day's checks and push everybody else's first review to tomorrow.
    const fileChanged = !existing || existing.sha256 !== draft.sha256;

    const listing: Listing = {
        ...draft,
        ownerId: owner.id,
        ownerNick: owner.nick,
        created,
        updated: now,
        status: fileChanged
            ? "pending"
            : (existing.status === "held" ? "held" : existing.status),
        ...(existing?.scan ? { scan: existing.scan } : {}),
        ...(existing?.hold ? { hold: existing.hold } : {}),
        guidelines: GUIDELINES_VERSION,
    };

    // A CHANGED HASH INVALIDATES THE SCAN AND ANY HOLD. Carrying the old verdict
    // across a new upload would be the registry vouching for bytes nothing ever
    // looked at -- and leaving a hold on a file that has since been replaced
    // would punish the author for having fixed it.
    if (fileChanged) {
        delete listing.scan;
        delete listing.hold;
        delete listing.reviewedAt;
    }

    await env.OD_ACCOUNTS.put(modKey(listing.id), JSON.stringify(listing));
    if (!existing) {
        await env.OD_ACCOUNTS.put(ownerKey(owner.id, listing.id), "");
        await env.OD_ACCOUNTS.put(browseKey(created, listing.id), "");
    }
    return { ok: true, listing, created: !existing };
}

export async function setStatus(
    env: Env, listing: Listing, status: Status,
): Promise<Listing> {
    const updated = { ...listing, status };
    await env.OD_ACCOUNTS.put(modKey(listing.id), JSON.stringify(updated));
    return updated;
}

export async function setScan(env: Env, listing: Listing, scan: Scan): Promise<Listing> {
    const updated = { ...listing, scan };
    await env.OD_ACCOUNTS.put(modKey(listing.id), JSON.stringify(updated));
    return updated;
}

/**
 * Write back what the review decided. One KV write, and only from the drain.
 *
 * `scan` is written even when the verdict is "let it through unscanned", so the
 * record says when we last looked rather than staying silent about it.
 */
export async function reviewed(
    env: Env, listing: Listing, status: Status, scan?: Scan, hold?: string,
    now = Math.floor(Date.now() / 1000),
): Promise<Listing> {
    const updated: Listing = {
        ...listing,
        status,
        reviewedAt: now,
        ...(scan ? { scan } : {}),
        ...(hold ? { hold: { reason: hold, at: now } } : {}),
    };
    if (!hold) delete updated.hold;
    await env.OD_ACCOUNTS.put(modKey(listing.id), JSON.stringify(updated));
    return updated;
}

/** Withdraw one's own listing. The browse and owner index keys go with it. */
export async function withdraw(env: Env, listing: Listing): Promise<void> {
    await env.OD_ACCOUNTS.delete(modKey(listing.id));
    await env.OD_ACCOUNTS.delete(ownerKey(listing.ownerId, listing.id));
    await env.OD_ACCOUNTS.delete(browseKey(listing.created, listing.id));
}

// ---------------------------------------------------------------- browse ----

export interface Page {
    mods: Record<string, unknown>[];
    cursor?: string;
}

/**
 * One page of listings, newest first.
 *
 * Reads the index keys, then each record. That is `limit + 1` KV reads a page
 * against a 100,000/day budget, and reads are the thing this plan has plenty
 * of -- it is writes that are scarce.
 */
export async function browse(
    env: Env, cursor?: string, limit = LIMITS.page,
): Promise<Page> {
    const listed = await env.OD_ACCOUNTS.list({
        prefix: "pkg:new:",
        limit: Math.min(Math.max(limit, 1), LIMITS.page),
        ...(cursor ? { cursor } : {}),
    });

    const mods: Record<string, unknown>[] = [];
    for (const key of listed.keys) {
        const id = key.name.slice(key.name.lastIndexOf(":") + 1);
        const listing = await getListing(env, id);
        // An index key with no record is a listing withdrawn between the list
        // and the read, not an error.
        if (listing && listing.status === "listed") mods.push(publicListing(listing));
    }
    return {
        mods,
        ...(listed.list_complete === false && listed.cursor ? { cursor: listed.cursor } : {}),
    };
}

/**
 * What a listing looks like to anybody.
 *
 * `ownerId` is deliberately absent: the nickname is the public identity here,
 * and the account id is the subject of a session token.
 */
export function publicListing(l: Listing, counts?: { downloads: number; unique: number }) {
    return {
        id: l.id,
        name: l.name,
        version: l.version,
        summary: l.summary,
        description: l.description,
        by: l.ownerNick,
        page: l.page,
        /** Through here, so it can be counted. Never the author's URL directly. */
        get: `/mods/${l.id}/get`,
        ...(l.updateUrl ? { updateUrl: l.updateUrl } : {}),
        declaredSha256: l.sha256,
        ...(l.sizeBytes ? { sizeBytes: l.sizeBytes } : {}),
        gearbox: l.gearbox,
        side: l.side,
        modules: l.modules,
        tags: l.tags,
        ...(l.thumbnail ? { thumbnail: l.thumbnail } : {}),
        created: l.created,
        updated: l.updated,
        ...(counts ? { downloads: counts.downloads, uniqueDownloads: counts.unique } : {}),
        scan: scanForClients(l.scan),
    };
}

/**
 * The scan, rendered so it cannot be mistaken for an endorsement.
 *
 * Three states and three words, because the difference between them is the
 * whole value: `unknown` means nobody has ever scanned this file, and a client
 * that paints that green is lying to its user.
 */
export function scanForClients(scan?: Scan) {
    if (!scan) return { state: "unscanned" as const };
    if (!scan.known) {
        return {
            state: "unknown" as const, at: scan.at,
            note: "VirusTotal has never seen this file. That is not the same as clean.",
        };
    }
    const bad = (scan.malicious ?? 0) + (scan.suspicious ?? 0);
    return {
        state: bad > 0 ? ("flagged" as const) : ("clean" as const),
        at: scan.at,
        malicious: scan.malicious ?? 0,
        suspicious: scan.suspicious ?? 0,
        engines: scan.engines ?? 0,
        ...(scan.permalink ? { permalink: scan.permalink } : {}),
    };
}
