// The feedback endpoint: what it accepts, what it refuses, and what it must
// never do.
//
// The last of those is the reason this file is longer than the feature. An
// endpoint that forwards player text into a chat channel is one mistake away
// from being a spam relay or a way to ping a whole Discord server, and both
// mistakes are invisible in a manual test that only ever sends a polite bug
// report.

import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import type { Env } from "../src/env.js";
import {
    byline, formatBody, CATEGORIES, KINDS, checkQuota, clean, forward, isDuplicate,
    isPrivateCategory, LIMITS, needsAccount, parseReport, recordAccepted, threadForIssue,
    type Report, type Rejected,
} from "../src/feedback/report.js";
import { clearKv, setupEnv } from "./helpers.js";

let env: Env;

/** The four optional destination settings, set and cleared per test. */
type Destination = "FEEDBACK_DISCORD_WEBHOOK" | "FEEDBACK_DISCORD_SECURITY_WEBHOOK"
                 | "FEEDBACK_DISCORD_SUGGESTION_WEBHOOK"
                 | "FEEDBACK_GITHUB_TOKEN" | "FEEDBACK_GITHUB_REPO";
const DESTINATIONS: Destination[] = [
    "FEEDBACK_DISCORD_WEBHOOK", "FEEDBACK_DISCORD_SECURITY_WEBHOOK",
    "FEEDBACK_DISCORD_SUGGESTION_WEBHOOK", "FEEDBACK_GITHUB_TOKEN", "FEEDBACK_GITHUB_REPO",
];
function configure(which: Partial<Record<Destination, string>>): void {
    for (const [k, v] of Object.entries(which)) env[k as Destination] = v;
}

beforeAll(async () => { env = await setupEnv(); });
beforeEach(async () => {
    await clearKv(env);
    vi.restoreAllMocks();
    for (const key of DESTINATIONS) delete env[key];
});

function bug(over: Partial<Report> = {}): Report {
    return {
        kind: "bug", category: "ui", title: "Panel overlaps",
        body: "The districts panel draws over the minimap at 1280x720.",
        version: "1.1.2a", platform: "macos", install: "install-aaa", ...over,
    } as Report;
}

describe("parseReport", () => {
    it("takes an ordinary bug report", () => {
        const r = parseReport(bug()) as Report;
        expect(r.kind).toBe("bug");
        expect(r.category).toBe("ui");
        expect(r.title).toBe("Panel overlaps");
    });

    it("refuses a kind or category it does not know", () => {
        expect((parseReport(bug({ kind: "exploit" as never })) as Rejected).code).toBe("bad_kind");
        expect((parseReport(bug({ category: "haunted" as never })) as Rejected).code)
            .toBe("bad_category");
    });

    it("accepts every category the game offers", () => {
        for (const category of CATEGORIES) {
            expect((parseReport(bug({ category })) as Report).category).toBe(category);
        }
    });

    it("refuses a report with nothing in it", () => {
        expect((parseReport(bug({ body: "eh" })) as Rejected).code).toBe("too_short");
        expect((parseReport(null) as Rejected).code).toBe("bad_request");
    });




    it("clips every field rather than trusting the client's caps", () => {
        const r = parseReport(bug({
            title: "t".repeat(5000),
            body: "b".repeat(50_000),
            diagnostics: "d".repeat(200_000),
        })) as Report;
        expect(r.title.length).toBe(LIMITS.title);
        expect(r.body.length).toBe(LIMITS.body);
        expect(r.diagnostics!.length).toBe(LIMITS.diagnostics);
    });
});

describe("who a report is signed by", () => {
    it("requires an account for the kinds that get published", () => {
        // Every kind. The endpoint has no anonymous write path, which is the
        // property that stops it being an open pipe into a Discord channel.
        for (const kind of KINDS) expect(needsAccount(kind)).toBe(true);
        expect(KINDS).not.toContain("rating");
    });

    it("NEVER takes the byline from the request body", () => {
        // The whole point. A name the client can set is a name anyone can set,
        // and it ends up on a public issue next to "this is broken".
        const r = parseReport({ ...bug(), reporter: "SomeoneElse" }) as Report;
        expect(r.reporter).toBeUndefined();
    });

    it("names the reporter once the route has verified them", () => {
        const r = { ...bug(), reporter: "Kaiserin" } as Report;
        expect(byline(r)).toBe("Kaiserin");
        expect(formatBody(r)).toContain("Reported by Kaiserin");
    });

    it("says Anonymous, and says it is still a signed-in person", () => {
        // Not the same as unknown: the service knows exactly who this is, so
        // the ban list and the quota still apply. They asked not to be named.
        const r = { ...bug(), reporter: "Kaiserin", anonymous: true } as Report;
        expect(byline(r)).toBe("Anonymous (signed in)");
        expect(formatBody(r)).not.toContain("Kaiserin");
    });

    it("takes the anonymous flag from the body, because that IS the player's choice", () => {
        expect((parseReport({ ...bug(), anonymous: true }) as Report).anonymous).toBe(true);
        expect((parseReport(bug()) as Report).anonymous).toBe(false);
        // Only a real boolean counts.
        expect((parseReport({ ...bug(), anonymous: "yes" }) as Report).anonymous).toBe(false);
    });

    it("puts the byline in front of whoever reads the channel", async () => {
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/hook" });
        let payload: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (_url: string, init: RequestInit) => {
            payload = JSON.parse((init.body as FormData).get("payload_json") as string);
            return new Response(null, { status: 204 });
        });

        await forward(env, { ...bug(), reporter: "Kaiserin" } as Report);
        expect(String(payload.content)).toContain("Kaiserin");
    });
});

describe("clean", () => {
    it("strips control characters, which is how one message pretends to be two", () => {
        expect(clean("a\u0000b\u0007c", 100)).toBe("abc");
    });

    it("keeps newlines and tabs, which are how a report is laid out", () => {
        expect(clean("one\ntwo\tthree", 100)).toBe("one\ntwo\tthree");
    });

    it("keeps backticks and code, because scripting bugs are made of them", () => {
        const script = "set var.x to 1\n`foreach district in country.USA`";
        expect(clean(script, 500)).toBe(script);
    });

    it("is not fooled by a non-string", () => {
        expect(clean(42, 10)).toBe("");
        expect(clean(undefined, 10)).toBe("");
        expect(clean({ toString: () => "x" }, 10)).toBe("");
    });
});

describe("quota and duplicates", () => {
    it("lets a normal reporter through and stops a firehose", async () => {
        const r = bug();
        for (let i = 0; i < LIMITS.perInstallDaily; ++i) {
            expect(await checkQuota(env, r)).toBeNull();
            await recordAccepted(env, { ...r, title: `report ${i}`, body: `body number ${i}` });
        }
        const refused = await checkQuota(env, r);
        expect(refused?.code).toBe("daily_limit");
        expect(refused?.status).toBe(429);
    });

    it("counts installs separately, so one spammer cannot mute everyone else", async () => {
        const spammer = bug({ install: "install-spam" });
        for (let i = 0; i < LIMITS.perInstallDaily; ++i) {
            await recordAccepted(env, { ...spammer, title: `t${i}`, body: `body ${i}` });
        }
        expect((await checkQuota(env, spammer))?.code).toBe("daily_limit");
        expect(await checkQuota(env, bug({ install: "install-someone-else" }))).toBeNull();
    });

    it("recognises the same report sent twice", async () => {
        const r = bug();
        expect(await isDuplicate(env, r)).toBe(false);
        await recordAccepted(env, r);
        expect(await isDuplicate(env, r)).toBe(true);
    });

    it("does not treat a different report from the same install as a duplicate", async () => {
        await recordAccepted(env, bug());
        expect(await isDuplicate(env, bug({ body: "A completely different problem." })))
            .toBe(false);
    });

    it("does not remember a report that went nowhere, so the retry is not swallowed", async () => {
        const r = bug({ category: "security" });
        await recordAccepted(env, r, false);
        // Counted against the day -- the attempt happened and abuse protection
        // is about attempts. But NOT recognised as a duplicate, or the player's
        // second try would be answered "already got that" for six hours.
        expect(await isDuplicate(env, r)).toBe(false);
        expect(await checkQuota(env, r)).toBeNull();
    });

    it("stores no key that contains the install id", async () => {
        await recordAccepted(env, bug({ install: "recognisable-install-id" }));
        const keys = (await env.OD_ACCOUNTS.list()).keys.map((k) => k.name);
        expect(keys.length).toBeGreaterThan(0);
        for (const k of keys) expect(k).not.toContain("recognisable-install-id");
    });
});

describe("forwarding", () => {
    it("neutralises mentions, so a report cannot ping the server", async () => {
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/hook" });
        let sent: FormData | null = null;
        vi.stubGlobal("fetch", async (_url: string, init: RequestInit) => {
            sent = init.body as FormData;
            return new Response(null, { status: 204 });
        });

        await forward(env, bug({ body: "@everyone the game crashed" }));
        const payload = JSON.parse(sent!.get("payload_json") as string);
        expect(payload.allowed_mentions).toEqual({ parse: [] });
        // The text itself is preserved -- it is what the player wrote.
        expect(payload.content).toContain("@everyone the game crashed");
    });

    it("posts to a forum channel as a thread, which it cannot do without a name", async () => {
        // A forum or media webhook REFUSES a message with no thread_name, so
        // without this the bug reports simply never appear and nothing says so.
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/forum" });
        const bodies: FormData[] = [];
        vi.stubGlobal("fetch", async (_url: string, init: RequestInit) => {
            bodies.push(init.body as FormData);
            return new Response(null, { status: 204 });
        });

        const sent = await forward(env, bug({ title: "Panel overlaps" }));
        expect(sent).toEqual(["discord"]);
        expect(bodies).toHaveLength(1);            // no retry needed
        const only = bodies[0];
        if (!only) throw new Error("no request was made");
        const payload = JSON.parse(only.get("payload_json") as string);
        expect(payload.thread_name).toBe("[Bug/ui] Panel overlaps");
    });

    it("clips a thread name to the 100 characters Discord allows", async () => {
        // A title may be 140 characters; a thread name may not.
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/forum" });
        let payload: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (_url: string, init: RequestInit) => {
            payload = JSON.parse((init.body as FormData).get("payload_json") as string);
            return new Response(null, { status: 204 });
        });

        await forward(env, bug({ title: "T".repeat(140) }));
        expect((payload.thread_name as string).length).toBe(100);
    });

    it("falls back to a plain message when the channel is not a forum", async () => {
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/text" });
        const payloads: Record<string, unknown>[] = [];
        vi.stubGlobal("fetch", async (_url: string, init: RequestInit) => {
            const p = JSON.parse((init.body as FormData).get("payload_json") as string);
            payloads.push(p);
            // What a text channel says to a thread_name it cannot honour.
            return p.thread_name
                ? new Response("{}", { status: 400 })
                : new Response(null, { status: 204 });
        });

        const sent = await forward(env, bug());
        expect(sent).toEqual(["discord"]);
        expect(payloads).toHaveLength(2);
        const [first, second] = payloads;
        if (!first || !second) throw new Error("expected two attempts");
        expect(first.thread_name).toBeDefined();
        expect(second.thread_name).toBeUndefined();
        // The report itself is identical either way.
        expect(second.content).toBe(first.content);
    });

    it("does not retry forever on a webhook that is simply gone", async () => {
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/deleted" });
        let calls = 0;
        vi.stubGlobal("fetch", async () => { calls++; return new Response("{}", { status: 404 }); });

        expect(await forward(env, bug())).toEqual([]);
        expect(calls).toBe(1);   // only a 400 is worth a second attempt
    });

    it("sends a suggestion to the suggestions channel, not the bug channel", async () => {
        configure({
            FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/bugs",
            FEEDBACK_DISCORD_SUGGESTION_WEBHOOK: "https://discord.invalid/ideas",
        });
        const calls: string[] = [];
        vi.stubGlobal("fetch", async (url: string) => {
            calls.push(String(url));
            return new Response(null, { status: 204 });
        });

        await forward(env, bug({ kind: "suggestion", title: "Let districts be renamed" }));
        expect(calls).toEqual(["https://discord.invalid/ideas?wait=true"]);
    });

    it("still sends a suggestion to the one channel when that is all there is", async () => {
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/bugs" });
        const calls: string[] = [];
        vi.stubGlobal("fetch", async (url: string) => {
            calls.push(String(url));
            return new Response(null, { status: 204 });
        });

        await forward(env, bug({ kind: "suggestion" }));
        expect(calls).toEqual(["https://discord.invalid/bugs?wait=true"]);
    });

    it("keeps bugs out of the suggestions channel", async () => {
        configure({
            FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/bugs",
            FEEDBACK_DISCORD_SUGGESTION_WEBHOOK: "https://discord.invalid/ideas",
        });
        const calls: string[] = [];
        vi.stubGlobal("fetch", async (url: string) => {
            calls.push(String(url));
            return new Response(null, { status: 204 });
        });

        await forward(env, bug());
        expect(calls).toEqual(["https://discord.invalid/bugs?wait=true"]);
    });

    it("sends diagnostics as a file rather than in the message", async () => {
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/hook" });
        let sent: FormData | null = null;
        vi.stubGlobal("fetch", async (_url: string, init: RequestInit) => {
            sent = init.body as FormData;
            return new Response(null, { status: 204 });
        });

        await forward(env, bug({ diagnostics: "line one\nline two" }));
        expect(sent!.get("files[0]")).toBeInstanceOf(Blob);
    });

    it("NEVER files a security report as a public issue", async () => {
        configure({
            FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/hook",
            FEEDBACK_GITHUB_TOKEN: "ghp_test",
            FEEDBACK_GITHUB_REPO: "owner/repo",
        });
        const calls: string[] = [];
        vi.stubGlobal("fetch", async (url: string) => {
            calls.push(String(url));
            return new Response("{}", { status: 201 });
        });

        const sent = await forward(env, bug({
            category: "security", body: "Mods can read files outside the sandbox.",
        }));
        expect(calls.some((u) => u.endsWith("/issues"))).toBe(false);
        expect(sent).toEqual(["advisory"]);
        expect(isPrivateCategory(bug({ category: "security" }))).toBe(true);
    });

    it("files a security report as a private draft advisory", async () => {
        configure({ FEEDBACK_GITHUB_TOKEN: "ghp_test", FEEDBACK_GITHUB_REPO: "owner/repo" });
        let url = "";
        let body: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (u: string, init: RequestInit) => {
            url = String(u);
            body = JSON.parse(init.body as string);
            return new Response("{}", { status: 201 });
        });

        await forward(env, bug({ category: "security", version: "1.1.2a" }));
        expect(url).toBe("https://api.github.com/repos/owner/repo/security-advisories");
        expect(body.summary).toContain("Panel overlaps");
        expect(body.description).toContain("districts panel");
        // Required by the API, and it refuses the request without it.
        expect(body.vulnerabilities).toEqual([{
            package: { ecosystem: "other", name: "OpenDoctrines" },
            vulnerable_version_range: "1.1.2a",
            patched_versions: null,
            vulnerable_functions: null,
        }]);
        // Not guessed. A severity on an untriaged report is a claim, not data --
        // and the API refuses severity and cvss_vector_string together, so a
        // default here would also be a trap for anyone adding the other one.
        expect(body.severity).toBeUndefined();
        expect(body.cvss_vector_string).toBeUndefined();
    });

    it("does NOT put a security report in the ordinary chat channel", async () => {
        // The regression this routing exists to prevent. Falling back from the
        // security destination to the general one would post an unfixed exploit
        // where everyone who can read the bugs channel can read it -- which is
        // the exact thing keeping it out of the public tracker was for.
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/open" });
        const calls: string[] = [];
        vi.stubGlobal("fetch", async (url: string) => {
            calls.push(String(url));
            return new Response(null, { status: 204 });
        });

        const sent = await forward(env, bug({ category: "security" }));
        expect(calls).toEqual([]);
        expect(sent).toEqual([]);
    });

    it("uses a designated security channel when the advisory cannot be filed", async () => {
        configure({
            FEEDBACK_DISCORD_SECURITY_WEBHOOK: "https://discord.invalid/locked",
            FEEDBACK_GITHUB_TOKEN: "ghp_test",
            FEEDBACK_GITHUB_REPO: "owner/repo",
        });
        const calls: string[] = [];
        vi.stubGlobal("fetch", async (url: string) => {
            calls.push(String(url));
            // A token that can file issues but not advisories answers exactly
            // this, and it is the likeliest way to get this wrong in practice.
            if (String(url).includes("security-advisories")) {
                return new Response("{}", { status: 403 });
            }
            return new Response(null, { status: 204 });
        });

        const sent = await forward(env, bug({ category: "security" }));
        expect(sent).toEqual(["discord-security"]);
        expect(calls).toEqual([
            "https://api.github.com/repos/owner/repo/security-advisories",
            "https://discord.invalid/locked?wait=true",
        ]);
    });

    it("reports a security report as undelivered rather than routing it anywhere else", async () => {
        configure({
            FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/open",
            FEEDBACK_GITHUB_TOKEN: "ghp_test",
            FEEDBACK_GITHUB_REPO: "owner/repo",
        });
        vi.stubGlobal("fetch", async () => new Response("{}", { status: 403 }));

        // Nowhere to put it and nowhere it may be put: the route turns this
        // into a 502 so the reporter knows to reach the maintainer directly.
        await expect(forward(env, bug({ category: "security" }))).resolves.toEqual([]);
    });

    it("does not file a suggestion as a public issue", async () => {
        // The tracker is a list of things that are wrong. An unfiltered stream
        // of ideas in it makes it worse at being that, and the maintainer can
        // open an issue for the ones worth building.
        configure({
            FEEDBACK_GITHUB_TOKEN: "ghp_test", FEEDBACK_GITHUB_REPO: "owner/repo",
            FEEDBACK_DISCORD_SUGGESTION_WEBHOOK: "https://discord.invalid/ideas",
        });
        const calls: string[] = [];
        vi.stubGlobal("fetch", async (url: string) => {
            calls.push(String(url));
            return new Response(JSON.stringify({ number: 41 }), { status: 201 });
        });

        const sent = await forward(env, bug({ kind: "suggestion" }));
        expect(calls.some((u) => u.includes("api.github.com"))).toBe(false);
        expect(sent).toEqual(["discord"]);
    });

    it("pairs the issue with the thread it came from", async () => {
        // The whole point of asking Discord for the created message: without
        // the thread id, closing the issue later has nowhere to say so.
        configure({
            FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/hook",
            FEEDBACK_GITHUB_TOKEN: "ghp_test", FEEDBACK_GITHUB_REPO: "owner/repo",
        });
        vi.stubGlobal("fetch", async (url: string) => {
            if (String(url).includes("discord")) {
                // A forum post's message lives IN the new thread, so its
                // channel_id is that thread.
                return new Response(JSON.stringify({ id: "m1", channel_id: "thread-77" }),
                                    { status: 200 });
            }
            return new Response(JSON.stringify({ number: 77 }), { status: 201 });
        });

        await forward(env, bug());
        expect(await threadForIssue(env, 77)).toBe("thread-77");
    });

    it("files the report anyway when the thread id cannot be learned", async () => {
        // An older channel type, a Discord that answers 204, a body that will
        // not parse: the report still has to arrive. Only the link is lost.
        configure({
            FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/hook",
            FEEDBACK_GITHUB_TOKEN: "ghp_test", FEEDBACK_GITHUB_REPO: "owner/repo",
        });
        vi.stubGlobal("fetch", async (url: string) =>
            String(url).includes("discord")
                ? new Response(null, { status: 204 })
                : new Response(JSON.stringify({ number: 78 }), { status: 201 }));

        const sent = await forward(env, bug());
        expect(sent).toContain("discord");
        expect(sent).toContain("github");
        expect(await threadForIssue(env, 78)).toBeNull();
    });

    it("labels an issue in a second call, because creating one drops them", async () => {
        // The first version passed `labels` to issue creation. GitHub accepted
        // the request, filed the issue, and silently discarded the labels --
        // "only users with push access can set labels for new issues", which a
        // fine-grained Issues:write token is not.
        configure({ FEEDBACK_GITHUB_TOKEN: "ghp_test", FEEDBACK_GITHUB_REPO: "owner/repo" });
        const calls: { url: string; body: Record<string, unknown> }[] = [];
        vi.stubGlobal("fetch", async (url: string, init: RequestInit) => {
            calls.push({ url: String(url), body: JSON.parse(init.body as string) });
            return new Response(JSON.stringify({ number: 41 }), { status: 201 });
        });

        expect(await forward(env, bug())).toContain("github");
        expect(calls).toHaveLength(2);
        const [create, label] = calls;
        if (!create || !label) throw new Error("expected two calls");
        expect(create.url).toBe("https://api.github.com/repos/owner/repo/issues");
        expect(create.body.labels).toBeUndefined();     // not sent where it is ignored
        expect(label.url).toBe("https://api.github.com/repos/owner/repo/issues/41/labels");
        expect(label.body.labels).toEqual(["bug", "ui"]);
    });

    it("still counts the issue as filed when labelling fails", async () => {
        configure({ FEEDBACK_GITHUB_TOKEN: "ghp_test", FEEDBACK_GITHUB_REPO: "owner/repo" });
        vi.stubGlobal("fetch", async (url: string) =>
            String(url).endsWith("/labels")
                ? new Response("{}", { status: 403 })
                : new Response(JSON.stringify({ number: 41 }), { status: 201 }));

        // An unlabelled issue is a filed issue, and the category is in the title.
        expect(await forward(env, bug())).toContain("github");
    });

    it("says WHY a security advisory could not be filed", async () => {
        configure({ FEEDBACK_GITHUB_TOKEN: "ghp_test", FEEDBACK_GITHUB_REPO: "owner/repo" });
        vi.stubGlobal("fetch", async () =>
            new Response('{"message":"Resource not accessible by personal access token"}',
                         { status: 403 }));

        const notes: string[] = [];
        expect(await forward(env, bug({ category: "security" }), notes)).toEqual([]);
        expect(notes).toHaveLength(1);
        // 403 (token permission), 422 (payload) and 404 (repo name) are three
        // different jobs for whoever has to fix it.
        expect(notes[0]).toContain("403");
        expect(notes[0]).toContain("not accessible");
    });

    it("files an ordinary bug as an issue when a repo is configured", async () => {
        configure({ FEEDBACK_GITHUB_TOKEN: "ghp_test", FEEDBACK_GITHUB_REPO: "owner/repo" });
        let body: Record<string, unknown> = {};
        vi.stubGlobal("fetch", async (url: string, init: RequestInit) => {
            if (String(url).endsWith("/issues")) body = JSON.parse(init.body as string);
            return new Response(JSON.stringify({ number: 41 }), { status: 201 });
        });

        const sent = await forward(env, bug());
        expect(sent).toContain("github");
        expect(String(body.title)).toContain("Panel overlaps");
        // Labels are applied by a second call; see the test above.
    });

    it("survives a destination that is down", async () => {
        configure({ FEEDBACK_DISCORD_WEBHOOK: "https://discord.invalid/hook" });
        vi.stubGlobal("fetch", async () => { throw new Error("network is down"); });
        await expect(forward(env, bug())).resolves.toEqual([]);
    });

    it("accepts a report with nowhere to send it, which is a fork's normal state", async () => {
        vi.stubGlobal("fetch", async () => { throw new Error("must not be called"); });
        await expect(forward(env, bug())).resolves.toEqual([]);
    });
});
