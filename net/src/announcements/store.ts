// The announcement board: what the game's main menu is told, and who may say it.
//
// ── THE CLIENT DOES NOT TRUST THIS, AND THAT IS BY DESIGN ──
//
// Everything written here is re-checked by the game before a pixel of it is
// drawn: src/net/Announcements.h caps every length, refuses any button action
// it does not already implement, and drops an entry whole rather than render
// half of it. So this file is not the security boundary -- it is the editing
// model. Validating here as well is worth it anyway, because an announcement
// that is silently dropped by every client is indistinguishable from one that
// was never posted, and whoever wrote it deserves the error at the moment they
// wrote it.
//
// ── DELETED IS NOT GONE ──
//
// The board is edited live, in front of everybody, usually in a hurry before a
// tournament. So "delete" hides an entry and keeps it; putting it back is one
// call with the same id. Nothing here destroys anything a person wrote except
// an explicit purge.

import type { Env } from "../env.js";

/** One announcement, exactly as the game will read it. */
export interface Announcement {
    id: string;
    title: string;
    body: string;              // the dialogue markup the tutorial uses
    buttonLabel?: string;
    buttonAction?: "join" | "community" | "account";
    buttonParam?: string;
    postedAt?: number;         // unix seconds
    until?: number;            // unix seconds; the entry expires itself
    /** Hidden from the board, kept so it can be put back. */
    hidden?: boolean;
}

const KEY = "announcements";

// The same numbers as src/net/Announcements.h. They are repeated rather than
// shared because the two live in different languages on different machines; if
// they ever drift, the CLIENT's are the ones that decide what is shown, and
// these only decide what can be saved.
const LIMITS = {
    id: 64, title: 80, body: 1200, label: 32, param: 64, items: 8,
} as const;

const ACTIONS = ["join", "community", "account"] as const;

export type Invalid = { ok: false; reason: string };
export type Valid = { ok: true; value: Announcement };

/** Check one announcement. The messages are for whoever is writing it. */
export function validate(a: Announcement): Valid | Invalid {
    if (!a || typeof a !== "object") return { ok: false, reason: "not an object" };
    if (typeof a.id !== "string" || !a.id || a.id.length > LIMITS.id)
        return { ok: false, reason: `id must be 1-${LIMITS.id} characters` };
    // The id is a key, not prose: it ends up in a KV record and in the game's
    // "already seen" bookkeeping.
    if (!/^[A-Za-z0-9._-]+$/.test(a.id))
        return { ok: false, reason: "id may use letters, digits, dot, dash and underscore" };

    const title = a.title ?? "";
    const body = a.body ?? "";
    if (typeof title !== "string" || title.length > LIMITS.title)
        return { ok: false, reason: `title must be at most ${LIMITS.title} characters` };
    if (typeof body !== "string" || body.length > LIMITS.body)
        return { ok: false, reason: `body must be at most ${LIMITS.body} characters` };
    if (!title && !body) return { ok: false, reason: "an announcement needs a title or a body" };

    // A button is all-or-nothing, the same rule the client applies: a label
    // with no action is a button that does nothing, and an action with no label
    // is a button nobody can read.
    const hasAny = a.buttonLabel || a.buttonAction || a.buttonParam;
    if (hasAny) {
        if (!a.buttonLabel || a.buttonLabel.length > LIMITS.label)
            return { ok: false, reason: `buttonLabel must be 1-${LIMITS.label} characters` };
        if (!a.buttonAction || !(ACTIONS as readonly string[]).includes(a.buttonAction))
            return { ok: false, reason: `buttonAction must be one of ${ACTIONS.join(", ")}` };
        if (a.buttonAction === "join") {
            if (!a.buttonParam || !/^[A-Za-z0-9_-]{1,32}$/.test(a.buttonParam))
                return { ok: false, reason: "a join button needs buttonParam to be an invite code" };
        } else if (a.buttonParam) {
            return { ok: false, reason: `${a.buttonAction} takes no buttonParam` };
        }
    }

    for (const [name, v] of [["postedAt", a.postedAt], ["until", a.until]] as const) {
        if (v === undefined) continue;
        if (typeof v !== "number" || !Number.isFinite(v) || v < 1600000000 || v > 4102444800)
            return { ok: false, reason: `${name} must be unix seconds between 2020 and 2100` };
    }
    return { ok: true, value: a };
}

export async function readAll(env: Env): Promise<Announcement[]> {
    const raw = await env.OD_ACCOUNTS.get(KEY);
    if (!raw) return [];
    try {
        const parsed = JSON.parse(raw);
        return Array.isArray(parsed) ? parsed : [];
    } catch {
        return [];   // a corrupt record is an empty board, never a 500
    }
}

async function writeAll(env: Env, all: Announcement[]): Promise<void> {
    await env.OD_ACCOUNTS.put(KEY, JSON.stringify(all));
}

/** What the game sees: visible, unexpired, newest first, capped. */
export function forClients(all: Announcement[], now: number): Announcement[] {
    return all
        .filter((a) => !a.hidden)
        .filter((a) => !a.until || a.until > now)
        .sort((x, y) => (y.postedAt ?? 0) - (x.postedAt ?? 0))
        .slice(0, LIMITS.items);
}

export type Edit =
    | { op: "put"; item: Announcement }
    | { op: "hide"; id: string }
    | { op: "show"; id: string }
    | { op: "purge"; id: string };

export type EditResult = { ok: true; all: Announcement[] } | Invalid;

/** Apply one edit. `put` creates or replaces by id. */
export async function applyEdit(env: Env, edit: Edit, now: number): Promise<EditResult> {
    const all = await readAll(env);

    if (edit.op === "put") {
        const checked = validate(edit.item);
        if (!checked.ok) return checked;
        const item: Announcement = { ...checked.value };
        // Stamped here when the author did not choose one, so every entry has a
        // date without anybody having to remember to set it.
        if (item.postedAt === undefined) item.postedAt = now;
        const at = all.findIndex((a) => a.id === item.id);
        const existing = at >= 0 ? all[at] : undefined;
        // An edit keeps whether it was hidden: correcting a typo in a withdrawn
        // announcement must not silently put it back on the board.
        if (existing) {
            item.hidden = existing.hidden;
            all[at] = item;
        } else {
            all.push(item);
        }
        await writeAll(env, all);
        return { ok: true, all };
    }

    const at = all.findIndex((a) => a.id === edit.id);
    const found = at >= 0 ? all[at] : undefined;
    if (!found) return { ok: false, reason: "no announcement with that id" };

    if (edit.op === "hide") all[at] = { ...found, hidden: true };
    else if (edit.op === "show") all[at] = { ...found, hidden: false };
    else all.splice(at, 1);

    await writeAll(env, all);
    return { ok: true, all };
}
