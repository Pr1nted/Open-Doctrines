// Bug reports, suggestions and ratings, on their way from the game to wherever
// the maintainer reads them.
//
// WHY A RELAY AND NOT A WEBHOOK IN THE CLIENT
//
// The obvious build posts straight to a Discord webhook or the GitHub issues
// API from the game. Both need a credential, and a credential shipped in a
// binary is a credential everybody has: the first person to run `strings` on
// the executable owns the bug tracker and can post to it forever, at any rate
// they like, without the game. Here the secret lives in the Worker and the game
// knows only a URL, which is not a secret and cannot be abused into anything
// worse than what this file already allows.
//
// WHAT THIS FILE IS DEFENDING AGAINST
//
// An open endpoint that forwards text into a chat channel is a spam relay
// unless it is built not to be. Four things stand in the way, and none of them
// is enough alone: a per-address rate limit (in http.ts, applied by the route),
// a per-install daily cap, a duplicate window that swallows the same report
// sent twice, and hard size caps enforced before anything is parsed.
//
// PRIVACY
//
// Everything here is written by the player. Nothing is collected: the game
// gathers a diagnostics block only when the player ticks the box, shows it to
// them in full before sending, and this file stores none of it -- the report
// is forwarded and forgotten, and only two short hashes outlive the request
// (the duplicate marker and the daily counter), neither reversible.

import type { Env } from "../env.js";
import { hmacId } from "../util/crypto.js";

/** What the report is about. The game shows these as the category chips. */
export const CATEGORIES = [
    "ui", "security", "ai", "data", "multiplayer", "scripting", "mods", "other",
] as const;
export type Category = (typeof CATEGORIES)[number];

// No "rating" here, and that is a security property rather than a trim.
//
// A rating was one tap and needed no account, so it was the one kind that could
// reach this endpoint unauthenticated -- and it still forwarded to the chat
// channel, which made an open write path into somebody's Discord. The install
// id it was rationed by is a string the client makes up, so rotating it is
// free. Ratings now go to the itch.io page, where a rating is public, useful to
// other players, and not our spam problem. Every kind that remains needs an
// account, so the endpoint has no unauthenticated write path at all.
export const KINDS = ["bug", "suggestion"] as const;
export type Kind = (typeof KINDS)[number];

/** Caps. Enforced on the parsed values as well as on the raw body length. */
export const LIMITS = {
    title: 140,
    body: 4000,
    diagnostics: 24 * 1024,
    version: 40,
    platform: 24,
    install: 64,
    /** Reports one install may send in a day, across every kind. */
    perInstallDaily: 12,
    /** How long the same report sent twice is swallowed rather than forwarded. */
    duplicateWindowSeconds: 6 * 3600,
};

export interface Report {
    kind: Kind;
    category: Category;
    title: string;
    body: string;
    diagnostics?: string;
    version: string;
    platform: string;
    install: string;
    /**
     * The account nickname to publish, or absent.
     *
     * Filled in by the ROUTE from the verified session token, never from the
     * request body -- a name the client could choose is a name anyone could
     * choose, and this one ends up on a public issue next to an accusation that
     * something is broken.
     */
    reporter?: string;
    /** The reporter asked not to be named. Set from the body; see parseReport. */
    anonymous?: boolean;
}

/**
 * Every kind needs an account.
 *
 * Kept as a function rather than inlined at the call site: it is the sentence
 * "this endpoint has no anonymous write path", and it should be somewhere a
 * person can read it and a test can assert it.
 */
export function needsAccount(_kind: Kind): boolean {
    return true;
}

/**
 * How a report is signed, as a person reads it.
 *
 * "Anonymous" is a real answer, not a missing one: the reporter has an account
 * and the service knows who they are, so the rate limits and the ban list still
 * apply -- they have simply asked not to be named on a public tracker, which is
 * a reasonable thing to want when reporting that something is embarrassingly
 * broken.
 */
export function byline(report: Report): string {
    if (report.anonymous) return "Anonymous (signed in)";
    return report.reporter ? report.reporter : "unknown";
}

export interface Accepted {
    ok: true;
    /** True when this exact report was already seen and was not forwarded again. */
    duplicate: boolean;
    /** Where it went. Empty when nothing is configured, which is not an error. */
    destinations: string[];
}

export interface Rejected {
    ok: false;
    status: number;
    code: string;
    message: string;
    retryAfter?: number;
}

/**
 * Strip anything that would let a report forge structure in the channel it
 * lands in.
 *
 * Control characters (other than newline and tab) are removed outright: they
 * are never typed by a player and they are how a message pretends to be two
 * messages. Backticks and at-signs are left alone -- a bug report about
 * scripting will contain code, and mangling it to protect a chat client's
 * formatting would make the reports worse at the only job they have. Discord
 * mentions are neutralised at the send site instead, where the mechanism that
 * cares about them lives.
 */
export function clean(value: unknown, max: number): string {
    if (typeof value !== "string") return "";
    // eslint-disable-next-line no-control-regex
    const stripped = value.replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "");
    return stripped.trim().slice(0, max);
}

/** Parse and validate. Returns a Report, or the refusal to send back. */
export function parseReport(raw: unknown): Report | Rejected {
    if (!raw || typeof raw !== "object") {
        return { ok: false, status: 400, code: "bad_request", message: "Empty report." };
    }
    const r = raw as Record<string, unknown>;

    const kind = r.kind as Kind;
    if (!KINDS.includes(kind)) {
        return { ok: false, status: 400, code: "bad_kind", message: "Unknown report kind." };
    }
    const category = (r.category ?? "other") as Category;
    if (!CATEGORIES.includes(category)) {
        return { ok: false, status: 400, code: "bad_category", message: "Unknown category." };
    }

    const title = clean(r.title, LIMITS.title);
    const body = clean(r.body, LIMITS.body);
    const install = clean(r.install, LIMITS.install);

    // Everything has to actually say something.
    if (body.length < 8) {
        return {
            ok: false, status: 400, code: "too_short",
            message: "Please describe the problem in a sentence or two.",
        };
    }
    if (!install) {
        return { ok: false, status: 400, code: "bad_request", message: "Missing install id." };
    }

    return {
        kind,
        category,
        title: title || "(no title)",
        body,
        diagnostics: clean(r.diagnostics, LIMITS.diagnostics) || undefined,
        version: clean(r.version, LIMITS.version) || "unknown",
        platform: clean(r.platform, LIMITS.platform) || "unknown",
        install,
        // `reporter` is NOT read from the body. The route sets it from the
        // verified token, because a byline the client controls is a byline
        // anybody can forge, and this one is published.
        anonymous: r.anonymous === true,
    };
}

/**
 * The two counters this endpoint keeps, both keyed on a hash of the install id
 * so that what is stored cannot be turned back into it.
 *
 * The daily cap is the one that stops a single copy of the game becoming a
 * firehose; the per-address limiter in front of the route stops a script that
 * makes up a new install id every time. Neither is much use without the other.
 */
async function installKeys(env: Env, report: Report): Promise<{ day: string; dup: string }> {
    const id = await hmacId(env.IDENT_KEY, `fb:${report.install}`, 22);
    const day = new Date().toISOString().slice(0, 10);
    const fingerprint = await hmacId(
        env.IDENT_KEY, `fbdup:${report.install}:${report.kind}:${report.title}:${report.body}`, 22,
    );
    return { day: `fb:day:${id}:${day}`, dup: `fb:dup:${fingerprint}` };
}

/** How many reports this install has had accepted today, and whether it may send another. */
export async function checkQuota(env: Env, report: Report): Promise<Rejected | null> {
    const { day } = await installKeys(env, report);
    const seen = Number((await env.OD_ACCOUNTS.get(day)) ?? "0");
    if (seen >= LIMITS.perInstallDaily) {
        return {
            ok: false, status: 429, code: "daily_limit",
            message: `That is ${LIMITS.perInstallDaily} reports today. Thank you — please continue tomorrow.`,
            retryAfter: 3600,
        };
    }
    return null;
}

/** True when this exact report has already been forwarded recently. */
export async function isDuplicate(env: Env, report: Report): Promise<boolean> {
    const { dup } = await installKeys(env, report);
    return (await env.OD_ACCOUNTS.get(dup)) !== null;
}

/**
 * Record that a report was accepted: one write for the counter, one for the
 * marker that recognises it if it is sent again.
 *
 * `remember` is false when nothing took the report. The two writes are for
 * different jobs and only one of them should happen in that case: the daily
 * counter is abuse protection and counts the ATTEMPT, but writing the duplicate
 * marker for a report that went nowhere would mean the player's retry is
 * recognised as a repeat and answered with a cheerful "already got that" for
 * the next six hours. The one case where that matters most is the one where it
 * would hurt most -- a security report with no destination configured.
 */
export async function recordAccepted(
    env: Env, report: Report, remember = true,
): Promise<void> {
    const { day, dup } = await installKeys(env, report);
    const seen = Number((await env.OD_ACCOUNTS.get(day)) ?? "0");
    await env.OD_ACCOUNTS.put(day, String(seen + 1), { expirationTtl: 2 * 86400 });
    if (remember) {
        await env.OD_ACCOUNTS.put(dup, "1", { expirationTtl: LIMITS.duplicateWindowSeconds });
    }
}

/**
 * A security report never becomes a public GitHub issue.
 *
 * Filing "here is how to get past the mod sandbox" in a public tracker
 * publishes the exploit to everyone before it is fixed. Those go to the private
 * channel only, and the game says so on the form so that a reporter knows what
 * they are choosing when they pick the category.
 */
export function isPrivateCategory(report: Report): boolean {
    return report.category === "security";
}

function heading(report: Report): string {
    const kindLabel = report.kind === "bug" ? "Bug" : "Suggestion";
    return `[${kindLabel}/${report.category}] ${report.title}`;
}

/** The body as a person should read it, shared by both destinations. */
export function formatBody(report: Report): string {
    const lines = [report.body];
    lines.push(`\n---\nReported by ${byline(report)}`);
    lines.push(`version ${report.version} · ${report.platform}`);
    if (report.diagnostics) {
        lines.push("\nDiagnostics (attached by the player):\n```\n" + report.diagnostics + "\n```");
    }
    return lines.join("\n");
}

/**
 * Send to Discord.
 *
 * `allowed_mentions: {parse: []}` is the load-bearing line: without it a report
 * containing `@everyone` pings the whole server, which makes the endpoint a
 * megaphone rather than a mailbox. Discord's own field caps are 2000 characters
 * for content, so the body is split -- the diagnostics go as a file attachment
 * where they belong, and the message stays readable.
 */
async function sendDiscord(
    webhook: string, report: Report,
): Promise<{ thread: string } | null> {
    const summary = `**${heading(report)}**\n${report.body}`.slice(0, 1800);
    const meta = `${byline(report)} · version ${report.version} · ${report.platform}`;

    const build = (asThread: boolean): FormData => {
        const payload: Record<string, unknown> = {
            content: `${summary}\n_${meta}_`,
            allowed_mentions: { parse: [] },
        };
        // A forum or media channel REQUIRES a thread to post into: the webhook
        // fails outright without one. 100 characters is the cap on a thread
        // name, and a report title may be 140.
        if (asThread) payload.thread_name = heading(report).slice(0, 100);

        const form = new FormData();
        form.append("payload_json", JSON.stringify(payload));
        if (report.diagnostics) {
            form.append(
                "files[0]",
                new Blob([report.diagnostics], { type: "text/plain" }),
                "diagnostics.txt",
            );
        }
        return form;
    };

    // Try as a forum post first, fall back to a plain message.
    //
    // The channel type cannot be asked for: GET /webhooks/{id}/{token} returns
    // the channel id but not its type, and reading the type needs a bot token,
    // which is exactly the credential this design exists to avoid shipping.
    // So the send discovers it. A forum channel refuses a message with no
    // thread_name, and a text channel refuses (or ignores) one with it -- one
    // wasted request the first time, against a feature that handles at most a
    // dozen reports per install per day.
    //
    // Ordered forum-first deliberately: a forum is the better home for this
    // (one post per report, sortable, with its own comment thread) and is what
    // a project that has thought about where bug reports go tends to have.
    // `wait=true` so the reply carries the created message, which is what makes
    // the thread id knowable. For a forum post the message lives IN the new
    // thread, so its channel_id IS that thread -- and that id is the only
    // handle by which a GitHub issue closing can later find its way back here.
    const url = webhook + (webhook.includes("?") ? "&" : "?") + "wait=true";
    let response = await fetch(url, { method: "POST", body: build(true) });
    if (response.status === 400) {
        response = await fetch(url, { method: "POST", body: build(false) });
    }
    if (!response.ok) return null;
    try {
        const message = await response.json() as { channel_id?: string };
        return { thread: message.channel_id ?? "" };
    } catch {
        // Delivered; we simply cannot link it. Not a failure of the send.
        return { thread: "" };
    }
}

/** Post into an existing thread. Used to say a report has been dealt with. */
async function sayInThread(webhook: string, thread: string, text: string): Promise<boolean> {
    const url = `${webhook}${webhook.includes("?") ? "&" : "?"}thread_id=${encodeURIComponent(thread)}`;
    const response = await fetch(url, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ content: text.slice(0, 1900), allowed_mentions: { parse: [] } }),
    });
    return response.ok;
}

/** File a GitHub issue. Never called for a private category -- see above. */
async function sendGithub(
    token: string, repo: string, report: Report, notes: string[] = [],
): Promise<{ number: number } | null> {
    const response = await fetch(`https://api.github.com/repos/${repo}/issues`, {
        method: "POST",
        headers: {
            "authorization": `Bearer ${token}`,
            "accept": "application/vnd.github+json",
            "content-type": "application/json",
            // GitHub refuses an API request with no user agent.
            "user-agent": "opendoctrines-feedback",
        },
        body: JSON.stringify({
            title: heading(report).slice(0, 250),
            body: formatBody(report),
        }),
    });
    if (!response.ok) return null;

    // Labels are BEST EFFORT, and a minimal token will not manage them.
    //
    // Measured against a fine-grained token with Issues: read and write: the
    // issue is created, and both label calls answer 403 "Resource not
    // accessible by personal access token". Creating an issue and labelling one
    // are not the same permission -- see the note on the create endpoint, "only
    // users with push access can set labels for new issues".
    //
    // The answer is NOT to ask for a wider token. Write access to the code, so
    // that a bug report arrives with a coloured tag on it, is a bad trade. The
    // category is already in the title -- "[Bug/scripting] ..." -- which is
    // what the tracker is searched on anyway. A broader token gets labels; a
    // minimal one gets an unlabelled issue and a note saying why.
    //
    // Labels go in a SECOND call, and that is not redundancy.
    //
    // Passing `labels` to issue creation looks like it works and does nothing:
    // "Only users with push access can set labels for new issues. Labels are
    // silently dropped otherwise" -- and a fine-grained token with Issues:write
    // does not have push access, which is Contents:write. The issue appears,
    // unlabelled, with no error anywhere. POST .../labels needs only
    // Issues:write and does what was asked.
    //
    // Best effort: an unlabelled issue is a filed issue, and the category is in
    // the title regardless.
    let number = 0;
    try {
        const issue = await response.json() as { number?: number };
        if (!issue.number) {
            notes.push("labels skipped: the created issue had no number");
            return { number: 0 };
        }
        number = issue.number;
        {
            const labelled = await fetch(
                `https://api.github.com/repos/${repo}/issues/${issue.number}/labels`,
                {
                    method: "POST",
                    headers: {
                        "authorization": `Bearer ${token}`,
                        "accept": "application/vnd.github+json",
                        "content-type": "application/json",
                        "user-agent": "opendoctrines-feedback",
                    },
                    body: JSON.stringify({ labels: [report.kind, report.category] }),
                },
            );
            if (!labelled.ok) {
                let detail = "";
                try { detail = (await labelled.text()).slice(0, 200); } catch { /* none */ }
                notes.push(`labels HTTP ${labelled.status} ${detail}`);
            }
        }
    } catch (e) {
        // The issue is filed; the label is decoration. But say so, or the next
        // person spends an evening wondering where the labels went -- which is
        // exactly what this feature already cost once.
        notes.push(`labels failed: ${e instanceof Error ? e.message : String(e)}`);
    }
    return { number };
}

/**
 * File a security report as a DRAFT REPOSITORY SECURITY ADVISORY.
 *
 * This is where a security report belongs, and it is better than the private
 * chat channel it used to go to. A draft advisory is visible only to people
 * with admin on the repository, it is the same object a coordinated disclosure
 * is eventually published from, and it cannot be forwarded out of a chat client
 * by someone who happened to be in the room.
 *
 * `vulnerabilities` is REQUIRED by the API, so one entry is always sent. The
 * ecosystem is "other" because a game is not an npm package, and the version
 * the player was running goes in `vulnerable_version_range` -- it is the only
 * version we can honestly claim anything about.
 *
 * SEVERITY IS DELIBERATELY NOT SET. The API takes `severity` or
 * `cvss_vector_string`, never both, and both are a judgement about a report
 * nobody has triaged yet. Guessing "medium" on every report would make the
 * field worthless in exactly the cases it matters; the maintainer sets it on
 * the draft, which is what a draft is for.
 *
 * Needs a token with `repository_advisories:write` (or classic `repo`). A token
 * that can only file issues will 403 here -- which is why forward() treats a
 * failure on this path as a failure to deliver rather than swallowing it.
 */
async function sendGithubAdvisory(
    token: string, repo: string, report: Report,
): Promise<boolean> {
    const response = await fetch(`https://api.github.com/repos/${repo}/security-advisories`, {
        method: "POST",
        headers: {
            "authorization": `Bearer ${token}`,
            "accept": "application/vnd.github+json",
            "content-type": "application/json",
            "user-agent": "opendoctrines-feedback",
        },
        body: JSON.stringify({
            summary: heading(report).slice(0, 1024),
            description: formatBody(report).slice(0, 65535),
            vulnerabilities: [{
                package: { ecosystem: "other", name: "OpenDoctrines" },
                vulnerable_version_range: report.version,
                patched_versions: null,
                vulnerable_functions: null,
            }],
        }),
    });
    if (!response.ok) {
        // The status, and GitHub's own sentence, kept for the caller.
        //
        // "Could not be delivered" is unactionable for whoever has to fix it:
        // 403 means the token lacks `repository_advisories:write`, 422 means
        // this payload is wrong, 404 means the repo name is. Swallowing the
        // difference cost an evening of guessing which.
        let detail = "";
        try { detail = (await response.text()).slice(0, 300); } catch { /* no body */ }
        throw new Error(`advisory HTTP ${response.status} ${detail}`);
    }
    return true;
}

/**
 * Forward an accepted report. Returns the destinations that took it.
 *
 * A destination that fails is not an error the player should see: their report
 * is written and their part is done, and telling them "Discord returned 500"
 * asks them to solve a problem they cannot reach. The route reports success if
 * ANY destination took it, and the caller logs nothing either way.
 */
export async function forward(
    env: Env, report: Report, notes: string[] = [],
): Promise<string[]> {
    const sent: string[] = [];
    const token = env.FEEDBACK_GITHUB_TOKEN;
    const repo = env.FEEDBACK_GITHUB_REPO;

    // ── A security report takes a different road entirely ──
    //
    // Not the public tracker, and NOT the ordinary chat channel either. The
    // old code fell back from the security webhook to the general one, which
    // quietly did the thing this whole path exists to prevent: put an unfixed
    // exploit in front of everyone who can read the bugs channel. A report
    // filed under Security goes to a private draft advisory, or to a webhook
    // explicitly designated for security, or nowhere -- and "nowhere" is
    // reported to the player rather than swallowed, so they know to reach the
    // maintainer another way instead of assuming it landed.
    if (isPrivateCategory(report)) {
        if (token && repo) {
            try {
                if (await sendGithubAdvisory(token, repo, report)) sent.push("advisory");
            } catch (e) {
                // Falls through to the security webhook, if there is one --
                // but the reason travels with it either way.
                notes.push(e instanceof Error ? e.message : String(e));
            }
        }
        if (sent.length === 0 && env.FEEDBACK_DISCORD_SECURITY_WEBHOOK) {
            try {
                if (await sendDiscord(env.FEEDBACK_DISCORD_SECURITY_WEBHOOK, report)) {
                    sent.push("discord-security");
                }
            } catch { /* nothing took it; the route tells the player so */ }
        }
        return sent;
    }

    // ── Everything else: the channel, and the public tracker ──
    //
    // A suggestion goes to its own channel when one is configured. They are not
    // the same kind of thing and they are not read in the same frame of mind:
    // a bug is a job, an idea is a conversation, and a hundred ideas mixed into
    // the bug list makes the bug list worse at being a bug list. Falls back to
    // the general webhook, so a single-channel setup still works unchanged.
    const channel = (report.kind === "suggestion" && env.FEEDBACK_DISCORD_SUGGESTION_WEBHOOK)
        ? env.FEEDBACK_DISCORD_SUGGESTION_WEBHOOK
        : env.FEEDBACK_DISCORD_WEBHOOK;
    let thread = "";
    if (channel) {
        try {
            const posted = await sendDiscord(channel, report);
            if (posted) { sent.push("discord"); thread = posted.thread; }
        } catch { /* a destination that is down is not the player's problem */ }
    }
    // Only a BUG becomes a public issue.
    //
    // A suggestion is somebody's idea, and an issue tracker filling with
    // unfiltered ideas stops being a list of things that are wrong -- which is
    // the only thing it is good for. Ideas land in the channel, where they can
    // be argued about, and the maintainer opens an issue for the ones worth
    // building. That is a curation step, and it should be a person's.
    if (token && repo && report.kind === "bug") {
        try {
            const issue = await sendGithub(token, repo, report, notes);
            if (issue) {
                sent.push("github");
                // Pair them, so closing the issue can find the conversation.
                // Both halves have to exist for the link to mean anything, and
                // failing to record it must not fail the report -- an unlinked
                // report is still a report.
                if (thread && issue.number) {
                    try { await linkIssueToThread(env, issue.number, thread); }
                    catch { notes.push("issue and thread could not be linked"); }
                }
            }
        } catch { /* likewise */ }
    }
    return sent;
}

/**
 * Remember which Discord thread a GitHub issue came from.
 *
 * Six months, because that is a generous upper bound on how long a bug sits
 * open before somebody closes it, and an expired link degrades to exactly the
 * behaviour this feature replaced: the issue closes and the thread is not told.
 * Keyed by issue number, which is public and specific to this repository, so
 * there is nothing here worth hashing.
 */
export async function linkIssueToThread(
    env: Env, issue: number, thread: string,
): Promise<void> {
    await env.OD_ACCOUNTS.put(`fb:issue:${issue}`, thread, { expirationTtl: 180 * 86400 });
}

export async function threadForIssue(env: Env, issue: number): Promise<string | null> {
    return env.OD_ACCOUNTS.get(`fb:issue:${issue}`);
}

/**
 * Tell the Discord thread its issue has been closed or reopened.
 *
 * Posting rather than archiving is deliberate: a webhook can only send, and
 * archiving a thread needs a bot token with Manage Threads -- a second
 * always-on credential for a tidier result. The pain this actually solves is
 * people still arguing in a thread about something that was fixed a week ago,
 * and a message in the thread solves that.
 */
export async function announceIssueState(
    env: Env, issue: number, state: "closed" | "reopened", title: string, by: string,
): Promise<boolean> {
    const webhook = env.FEEDBACK_DISCORD_WEBHOOK;
    if (!webhook) return false;
    const thread = await threadForIssue(env, issue);
    if (!thread) return false;

    const repo = env.FEEDBACK_GITHUB_REPO ?? "";
    const link = repo ? `https://github.com/${repo}/issues/${issue}` : `#${issue}`;
    const text = state === "closed"
        ? `**Closed** by ${by} — ${title}\n${link}`
        : `**Reopened** by ${by} — ${title}\n${link}`;
    return sayInThread(webhook, thread, text);
}
