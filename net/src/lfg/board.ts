// The looking-for-a-game board: what a listing is, and what it must pass.
//
// ── THE GUIDELINES ARE THE VALIDATOR ──
//
// The rules pinned in the Discord channel are enforced here, in one place,
// because there are two ways in and only one of them is the game:
//
//   Be respectful                 -> the note runs through the nickname blocklist
//   Post the parameters           -> map, mode, pace and slots are required fields,
//                                    not something a person remembers to type
//   Promote Open Doctrines ONLY   -> a listing carries an invite code, never a link;
//                                    any URL in the text is refused
//   Use the correct tag           -> `kind` is hosting or looking, and a hosting
//                                    listing without a code cannot exist
//
// A rule that is a FIELD cannot be forgotten, misspelled or argued about, which
// is why the form asks for the parameters instead of asking people to include
// them. The only free text is the note, and it is the only thing a moderator
// ever has to read.
//
// ── WHY THERE IS NO "SERVER ADDRESS" FIELD ──
//
// An invite code names a session on an account service this project runs. An
// address field would be a link by another name, and the rule about promoting
// only Open Doctrines games would then have to be judged by a person on every
// listing. With codes, a listing either points at a game on this service or it
// does not exist.

import { isSessionCode } from "../lobby/session.js";
import { canonicalize, normalize, type Blocklist } from "../accounts/nickname.js";

export const KINDS = ["hosting", "looking"] as const;
export type Kind = (typeof KINDS)[number];

export const MODES = ["rapid", "longform"] as const;
export type Mode = (typeof MODES)[number];

export const LIMITS = {
    map: 48,
    note: 240,
    language: 24,
    region: 24,
    /** Listings a client is shown at once. */
    page: 40,
    /** Per account: one open listing, and this many posts a day. */
    perDay: 6,
    /** How long a listing lives unless it says otherwise. */
    defaultTtl: 2 * 60 * 60,
    maxTtl: 6 * 60 * 60,
    /** Rapid games: seconds a turn is given. Longform: hours. */
    turnSecondsMin: 30,
    turnSecondsMax: 3600,
    turnHoursMin: 1,
    turnHoursMax: 168,
    slotsMin: 2,
    slotsMax: 32,
} as const;

export interface Listing {
    id: string;
    kind: Kind;
    /** Who posted it. Never sent to clients; the nickname is. */
    accountId: string;
    nick: string;
    /** Invite code, for a hosting listing. A looking listing has none. */
    code?: string;
    map: string;
    mode: Mode;
    /** Rapid games only. */
    turnSeconds?: number;
    /** Longform games only. */
    turnHours?: number;
    /** Hosting listings only: how full the game is. */
    slotsTaken?: number;
    slotsTotal?: number;
    language: string;
    region: string;
    note?: string;
    createdAt: number;
    expiresAt: number;
    closed?: boolean;
    hidden?: boolean;
    /** The message the bot posted, so it can be closed there too. */
    discordMessageId?: string;
    reports?: number;
}

/** What a listing looks like before it has an id, an owner or a clock. */
export interface ListingInput {
    kind?: unknown;
    code?: unknown;
    map?: unknown;
    mode?: unknown;
    turnSeconds?: unknown;
    turnHours?: unknown;
    slotsTaken?: unknown;
    slotsTotal?: unknown;
    language?: unknown;
    region?: unknown;
    note?: unknown;
    minutes?: unknown;
}

export type Rejected = { ok: false; reason: string };
export type Accepted = { ok: true; value: Listing };

const URL_LIKE = /(https?:\/\/|www\.|discord\.gg\/|\b[a-z0-9-]+\.(com|net|org|io|gg|dev|xyz|ru|me)\b)/i;

/** Letters, digits, spaces and a few separators. No markup, no mentions. */
const PLAIN = /^[\p{L}\p{N} .,!?'()\/+:;-]*$/u;

function short(value: unknown, max: number): string {
    return typeof value === "string" ? value.trim().slice(0, max + 1) : "";
}

function whole(value: unknown): number | undefined {
    return typeof value === "number" && Number.isInteger(value) ? value : undefined;
}

/**
 * Check a listing and fill in everything the poster does not decide.
 *
 * `blocklist` is the same list nicknames are checked against; passing it in
 * keeps this function pure, which is what lets the game run the identical
 * check locally and refuse a listing before it costs a request.
 */
export function validate(
    input: ListingInput,
    context: { id: string; accountId: string; nick: string; now: number; blocklist: Blocklist },
): Accepted | Rejected {
    const kind = short(input.kind, 16) as Kind;
    if (!(KINDS as readonly string[]).includes(kind)) {
        return { ok: false, reason: "Choose a tag: hosting a game, or looking for one." };
    }

    const mode = short(input.mode, 16) as Mode;
    if (!(MODES as readonly string[]).includes(mode)) {
        return { ok: false, reason: "Choose the turn mode: rapid or longform." };
    }

    const map = short(input.map, LIMITS.map);
    if (!map) return { ok: false, reason: "Say which map the game is on." };
    if (map.length > LIMITS.map) return { ok: false, reason: `The map name is at most ${LIMITS.map} characters.` };
    if (!PLAIN.test(map)) return { ok: false, reason: "The map name may use letters, digits and simple punctuation." };

    const language = short(input.language, LIMITS.language) || "any";
    const region = short(input.region, LIMITS.region) || "any";
    for (const [name, value] of [["language", language], ["region", region]] as const) {
        if (value.length > LIMITS[name]) return { ok: false, reason: `The ${name} is at most ${LIMITS[name]} characters.` };
        if (!PLAIN.test(value)) return { ok: false, reason: `The ${name} may use letters, digits and simple punctuation.` };
    }

    const listing: Listing = {
        id: context.id,
        kind,
        accountId: context.accountId,
        nick: context.nick,
        map,
        mode,
        language,
        region,
        createdAt: context.now,
        expiresAt: context.now + LIMITS.defaultTtl,
    };

    // ── The pace, which is half of "post the parameters" ──
    if (mode === "rapid") {
        const seconds = whole(input.turnSeconds);
        if (seconds === undefined || seconds < LIMITS.turnSecondsMin || seconds > LIMITS.turnSecondsMax) {
            return {
                ok: false,
                reason: `A rapid game needs its turn length in seconds, ${LIMITS.turnSecondsMin}-${LIMITS.turnSecondsMax}.`,
            };
        }
        listing.turnSeconds = seconds;
    } else {
        const hours = whole(input.turnHours);
        if (hours === undefined || hours < LIMITS.turnHoursMin || hours > LIMITS.turnHoursMax) {
            return {
                ok: false,
                reason: `A longform game needs its turn length in hours, ${LIMITS.turnHoursMin}-${LIMITS.turnHoursMax}.`,
            };
        }
        listing.turnHours = hours;
    }

    // ── The tag decides what else is required ──
    //
    // A hosting listing is an invitation and must be joinable; a looking
    // listing is a person, and giving it a code would be a second way to
    // advertise a game without saying that is what it is.
    if (kind === "hosting") {
        const code = short(input.code, 32);
        if (!isSessionCode(code)) {
            return { ok: false, reason: "A hosting listing needs the invite code from your lobby." };
        }
        const total = whole(input.slotsTotal);
        const taken = whole(input.slotsTaken) ?? 1;
        if (total === undefined || total < LIMITS.slotsMin || total > LIMITS.slotsMax) {
            return { ok: false, reason: `Say how many players the game seats, ${LIMITS.slotsMin}-${LIMITS.slotsMax}.` };
        }
        if (taken < 0 || taken > total) return { ok: false, reason: "Players in the game cannot exceed the seats." };
        listing.code = code;
        listing.slotsTotal = total;
        listing.slotsTaken = taken;
    } else if (input.code) {
        return { ok: false, reason: "A looking-for-a-game listing carries no invite code. Tag it as hosting instead." };
    }

    const minutes = whole(input.minutes);
    if (minutes !== undefined) {
        if (minutes < 15 || minutes * 60 > LIMITS.maxTtl) {
            return { ok: false, reason: `A listing lasts between 15 minutes and ${LIMITS.maxTtl / 3600} hours.` };
        }
        listing.expiresAt = context.now + minutes * 60;
    }

    const note = short(input.note, LIMITS.note);
    if (note) {
        if (note.length > LIMITS.note) return { ok: false, reason: `The note is at most ${LIMITS.note} characters.` };
        // "Promote Open Doctrines servers ONLY", enforced rather than judged.
        if (URL_LIKE.test(note)) {
            return {
                ok: false,
                reason: "Listings carry an invite code, not links. Anything that looks like a link is refused.",
            };
        }
        if (!PLAIN.test(note)) {
            return { ok: false, reason: "The note may use letters, digits and simple punctuation." };
        }
        // "Be respectful", using the list nicknames already answer to.
        const folded = normalize(canonicalize(note));
        for (const term of context.blocklist.terms) {
            if (term && folded.includes(term)) {
                return { ok: false, reason: "That note is not allowed. Say the same thing without that word." };
            }
        }
        listing.note = note;
    }

    return { ok: true, value: listing };
}

/** What clients and the bot are shown: no account ids, ever. */
export function forClients(listing: Listing): Record<string, unknown> {
    const { accountId: _accountId, hidden: _hidden, ...rest } = listing;
    return rest;
}

/** Newest first, capped, and nothing hidden, closed or expired. */
export function visible(all: Listing[], now: number): Listing[] {
    return all
        .filter((l) => !l.hidden && !l.closed && l.expiresAt > now)
        .sort((a, b) => b.createdAt - a.createdAt)
        .slice(0, LIMITS.page);
}
