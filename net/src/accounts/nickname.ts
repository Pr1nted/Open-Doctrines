// Nickname rules: what a name may look like, what it may not say, and what
// counts as "the same name" for uniqueness.
//
// THREE FORMS, and keeping them apart is most of the design:
//
//   input       whatever the player typed
//   canonical   what we store and display. NFKC-folded and de-homoglyphed, so
//               fullwidth and Cyrillic lookalikes collapse onto plain ASCII.
//   normalized  never shown to anyone. Casefolded, separators stripped, leet
//               undone, repeated runs collapsed. Used for blocklist matching
//               and for uniqueness -- which is why "Adm1n" cannot shadow
//               "admin", and why "A_d_m_i_n" cannot either.
//
// WHY ASCII ONLY
//
// The canonical form is restricted to ASCII letters, digits and a few
// separators. That is a real cost -- a player whose name is written in Greek,
// Cyrillic, Arabic or Han cannot use it here -- and it is deliberate, because
// this game shows developer and playtester badges next to names. Allowing
// mixed scripts means "Аdmin" with a Cyrillic А renders identically to
// "Admin", and no amount of filtering fixes that for a reader. Rather than
// reject those inputs with a confusing error, canonicalize() folds the common
// lookalikes onto their ASCII twin, so the name is accepted as the ASCII one
// it appears to be -- and then has to pass the same checks as anyone typing it
// directly.

import type { Env } from "../env.js";

export const NICK_MIN = 3;
export const NICK_MAX = 24;

// Words nobody may take, because taking one is a claim about who you are.
// Substring-matched against the normalized form, so "the_admin_" is refused
// too. This list is committed because it contains nothing anyone needs
// shielding from; the profanity list is not, and lives in KV (see below).
const RESERVED = [
    "admin", "administrator", "moderator", "mod", "staff", "official",
    "system", "server", "console", "operator", "owner", "root", "sudo",
    "opendoctrines", "doctrines", "gearbox",
    "support", "help", "helpdesk", "billing", "payment", "security",
    "developer", "playtester", "verified",
    "null", "undefined", "anonymous", "deleted", "unknown", "everyone", "here",
];

// Homoglyph folding. Only the characters that actually render as an ASCII
// letter in the fonts this game ships -- this is not a general Unicode
// confusables table, and it is not trying to be.
const CONFUSABLES: Record<string, string> = {
    // Cyrillic
    "а": "a", "в": "b", "с": "c", "е": "e", "н": "h", "к": "k", "м": "m",
    "о": "o", "р": "p", "т": "t", "х": "x", "у": "y", "ѕ": "s", "і": "i",
    "ј": "j", "ԁ": "d", "ɡ": "g", "ν": "v",
    // Greek
    "α": "a", "β": "b", "ε": "e", "ι": "i", "κ": "k", "ο": "o", "ρ": "p",
    "τ": "t", "υ": "u", "χ": "x", "γ": "y", "ω": "w", "σ": "o", "μ": "u",
    // Latin lookalikes outside plain ASCII
    "ɑ": "a", "ᴀ": "a", "ʙ": "b", "ᴄ": "c", "ᴅ": "d", "ᴇ": "e", "ɢ": "g",
    "ʜ": "h", "ɪ": "i", "ᴊ": "j", "ᴋ": "k", "ʟ": "l", "ᴍ": "m", "ɴ": "n",
    "ᴏ": "o", "ᴘ": "p", "ʀ": "r", "ᴛ": "t", "ᴜ": "u", "ᴠ": "v", "ᴡ": "w",
    "ʏ": "y", "ᴢ": "z", "ſ": "s", "ł": "l", "ø": "o", "đ": "d",
};

// Undone before blocklist matching only -- never in the stored name, because
// a player is entitled to call themselves "L33T" if the underlying word is
// fine.
const LEET: Record<string, string> = {
    "0": "o", "1": "i", "3": "e", "4": "a", "5": "s", "6": "g", "7": "t",
    "8": "b", "9": "g", "@": "a", "$": "s", "!": "i", "+": "t", "|": "l",
};

const SEPARATORS = new Set([" ", "_", "-", "."]);

/**
 * Fold input onto the ASCII form we store and display. Diacritics are stripped
 * (NFD then drop combining marks) so "Ádmin" is not a way around "admin".
 */
export function canonicalize(input: string): string {
    const folded = input.normalize("NFKC").normalize("NFD")
        .replace(/\p{M}+/gu, "");
    let out = "";
    for (const ch of folded) {
        const lower = ch.toLowerCase();
        const mapped = CONFUSABLES[lower];
        if (mapped === undefined) { out += ch; continue; }
        // Keep the case the player typed: an uppercase lookalike folds to an
        // uppercase ASCII letter, so "Аdmin" reads back as "Admin".
        out += ch === lower ? mapped : mapped.toUpperCase();
    }
    return out.trim();
}

/**
 * The matching form. Not reversible and not for display: it deliberately
 * destroys information so that evasions collapse onto the thing they are
 * evading.
 */
export function normalize(canonical: string): string {
    let out = "";
    for (const ch of canonical.toLowerCase()) {
        if (SEPARATORS.has(ch)) continue;      // a_d_m_i_n -> admin
        out += LEET[ch] ?? ch;                 // adm1n     -> admin
    }
    // Collapse runs: aaadmiiin -> admin.
    //
    // This deliberately makes "Bookkeeper" and "Bokeper" the same name, which
    // costs a legitimate player the second spelling. That is the intended
    // trade: a name one doubled letter away from someone else's is exactly how
    // impersonation works, and the alternative is letting "aaadmin" through.
    let collapsed = "";
    for (const ch of out) {
        if (collapsed[collapsed.length - 1] !== ch) collapsed += ch;
    }
    return collapsed;
}

export type NickRejection =
    | "too_short" | "too_long" | "charset" | "separator" | "no_letter"
    | "reserved" | "blocked";

export interface NickCheck {
    ok: boolean;
    canonical: string;
    normalized: string;
    reason?: NickRejection;
    message?: string;
}

const MESSAGES: Record<NickRejection, string> = {
    too_short: `Nicknames need at least ${NICK_MIN} characters.`,
    too_long: `Nicknames can be at most ${NICK_MAX} characters.`,
    charset: "Use letters, numbers, and single spaces, dots, hyphens or underscores.",
    separator: "Spaces, dots, hyphens and underscores must sit between other characters, one at a time.",
    no_letter: "Nicknames need at least one letter.",
    reserved: "That nickname is reserved.",
    blocked: "That nickname is not allowed.",
};

function reject(canonical: string, normalized: string, reason: NickRejection): NickCheck {
    return { ok: false, canonical, normalized, reason, message: MESSAGES[reason] };
}

/**
 * Shape rules only -- no blocklist, no KV, no I/O. Split out so the game
 * client can run exactly this check locally and tell the player before they
 * make a request.
 */
export function checkShape(input: string): NickCheck {
    const canonical = canonicalize(input);
    const normalized = normalize(canonical);

    // Length is measured in code points, not UTF-16 units, so an astral
    // character counts once. It will fail the charset rule anyway, but a
    // length error would be the more confusing of the two to report.
    const points = [...canonical];
    if (points.length < NICK_MIN) return reject(canonical, normalized, "too_short");
    if (points.length > NICK_MAX) return reject(canonical, normalized, "too_long");

    for (const ch of points) {
        const isAlnum = /[A-Za-z0-9]/.test(ch);
        if (!isAlnum && !SEPARATORS.has(ch)) return reject(canonical, normalized, "charset");
    }
    const first = points[0]!;
    const last = points[points.length - 1]!;
    if (SEPARATORS.has(first) || SEPARATORS.has(last)) {
        return reject(canonical, normalized, "separator");
    }
    for (let i = 1; i < points.length; i++) {
        if (SEPARATORS.has(points[i]!) && SEPARATORS.has(points[i - 1]!)) {
            return reject(canonical, normalized, "separator");
        }
    }
    if (!/[A-Za-z]/.test(canonical)) return reject(canonical, normalized, "no_letter");

    return { ok: true, canonical, normalized };
}

// ------------------------------------------------------------- blocklist ----
//
// The profanity list is NOT in this repository. It is uploaded to KV at deploy
// time (`cfg:blocklist`), for two reasons: the repo does not need to carry a
// list of slurs, and the list can be updated without a redeploy when someone
// finds a gap.
//
// Format: one term per line, '#' comments. A line starting with '!' is an
// EXCEPTION -- an exact normalized name that is allowed even though it
// contains a blocked substring. Every substring filter needs this: the
// classic case is a real place name that contains a rude word, and without an
// escape hatch the only fix is weakening the filter for everyone.

interface Blocklist { terms: string[]; exceptions: Set<string> }

let cached: { at: number; value: Blocklist } | null = null;
const BLOCKLIST_TTL_MS = 60_000;

export function parseBlocklist(text: string): Blocklist {
    const terms: string[] = [];
    const exceptions = new Set<string>();
    for (const rawLine of text.split("\n")) {
        const line = rawLine.trim();
        if (!line || line.startsWith("#")) continue;
        if (line.startsWith("!")) {
            exceptions.add(normalize(canonicalize(line.slice(1))));
        } else {
            // Terms are stored normalized so the comparison is like-for-like:
            // a list written with spaces or leetspeak still matches.
            const t = normalize(canonicalize(line));
            if (t) terms.push(t);
        }
    }
    return { terms, exceptions };
}

export async function loadBlocklist(env: Env): Promise<Blocklist> {
    const now = Date.now();
    if (cached && now - cached.at < BLOCKLIST_TTL_MS) return cached.value;
    const text = await env.OD_ACCOUNTS.get("cfg:blocklist");
    // A missing list is not a reason to refuse every nickname, nor to let
    // everything through silently: shape rules and RESERVED still apply, and
    // README.md makes uploading the list a deployment step.
    const value = parseBlocklist(text ?? "");
    cached = { at: now, value };
    return value;
}

/** Test seam: drop the isolate-level blocklist cache. */
export function resetBlocklistCache(): void {
    cached = null;
}

export function checkAgainstBlocklist(check: NickCheck, list: Blocklist): NickCheck {
    if (!check.ok) return check;
    if (list.exceptions.has(check.normalized)) return check;

    for (const word of RESERVED) {
        if (check.normalized.includes(word)) {
            return reject(check.canonical, check.normalized, "reserved");
        }
    }
    for (const term of list.terms) {
        if (check.normalized.includes(term)) {
            return reject(check.canonical, check.normalized, "blocked");
        }
    }
    return check;
}

/** Full check: shape, then reserved words, then the deploy-time blocklist. */
export async function checkNickname(env: Env, input: string): Promise<NickCheck> {
    const shape = checkShape(input);
    if (!shape.ok) return shape;
    return checkAgainstBlocklist(shape, await loadBlocklist(env));
}
