// The mod registry.
//
// The parts worth testing are not "does a record round-trip". They are the
// places where a plausible-looking implementation would be quietly wrong:
//
//   - the input cleaner, which twice became a range that ate spaces and
//     punctuation out of every name on the site while still looking like a
//     control-character filter;
//   - `unknown` from a scanner rendering as `clean`, which turns a weak signal
//     into a false assurance;
//   - a mod restriction leaking into `banned`, which would make every mod
//     complaint a way to remove somebody from the game.

import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import type { Account } from "../src/accounts/store.js";
import { modsRestricted, setModsRestricted, banInForce } from "../src/accounts/store.js";
import {
    browse, countOwned, GUIDELINES_VERSION, getListing, isModId, isPublishableUrl,
    isSha256, isVersion, MODMAKER_THRESHOLD, parseDraft, publicListing, publish,
    scanForClients, setScan, tagsFrom, withdraw, type Draft, type Listing,
} from "../src/mods/registry.js";
import { dayNumber, readCounts, recordHit } from "../src/mods/counts.js";
import {
    decideModReport, fileModReport, listModReports, parseModReportInput,
} from "../src/mods/reports.js";
import { clearKv, setupEnv } from "./helpers.js";

let env: Env;

async function makeAccount(id: string, nick: string, badges: string[] = []): Promise<Account> {
    const account = {
        id, nick, nickNorm: nick.toLowerCase(), created: 1, nickChangedAt: 0,
        badges: badges as Account["badges"], identities: [],
        modGuidelines: { version: GUIDELINES_VERSION, at: 1 },
    } as Account;
    await env.OD_ACCOUNTS.put(`acct:${id}`, JSON.stringify(account));
    return account;
}

function draft(over: Record<string, unknown> = {}) {
    return {
        id: "com.example.better-ai",
        name: "Better AI",
        version: "1.2.0",
        summary: "Retunes the AI's war scoring so it stops trading armies.",
        description: "Longer text.",
        page: "https://example.com/better-ai",
        downloadUrl: "https://example.com/better-ai.odmod",
        sha256: "a".repeat(64),
        gearbox: "1.2",
        side: "client",
        modules: ["Core", "GameState.Read"],
        tags: ["ai"],
        ...over,
    };
}

function asDraft(over: Record<string, unknown> = {}): Draft {
    const d = parseDraft(draft(over));
    if ("ok" in (d as object)) throw new Error(`draft rejected: ${JSON.stringify(d)}`);
    return d as Draft;
}

beforeAll(async () => { env = await setupEnv(); });
beforeEach(async () => { await clearKv(env); });

// ------------------------------------------------------------ the id rule ----

describe("mod ids follow the game's rule, not a looser one", () => {
    // A listing the game would refuse to load is a listing nobody can install,
    // so this mirrors ModPackage.cpp deliberately.
    it("accepts a reverse-DNS id", () => {
        expect(isModId("com.example.better-ai")).toBe(true);
        expect(isModId("a.b")).toBe(true);
    });

    it("requires a dot, and refuses a leading or trailing one", () => {
        expect(isModId("betterai")).toBe(false);
        expect(isModId(".com.example")).toBe(false);
        expect(isModId("com.example.")).toBe(false);
    });

    it("refuses uppercase", () => {
        // Not tidiness: the id is the Storage namespace and the trust-pinning
        // key, so on a case-insensitive filesystem com.a.Mod and com.a.mod
        // would be two identities sharing one store.
        expect(isModId("com.example.Mod")).toBe(false);
    });

    it("refuses a path separator or a space", () => {
        expect(isModId("com.example/mod")).toBe(false);
        expect(isModId("com.example mod")).toBe(false);
    });
});

describe("urls are held to the game's own rule", () => {
    it("requires https", () => {
        expect(isPublishableUrl("http://example.com/x")).toBe(false);
        expect(isPublishableUrl("https://example.com/x")).toBe(true);
    });

    it("refuses shell metacharacters and non-printables", () => {
        expect(isPublishableUrl("https://example.com/`id`")).toBe(false);
        expect(isPublishableUrl("https://example.com/a;b")).toBe(false);
        expect(isPublishableUrl("https://example.com/a b")).toBe(false);
    });

    it("refuses one past the length bound", () => {
        expect(isPublishableUrl("https://example.com/" + "a".repeat(600))).toBe(false);
    });
});

describe("versions and hashes", () => {
    it("takes semver and refuses two-part versions", () => {
        expect(isVersion("1.2.0")).toBe(true);
        expect(isVersion("1.2.0-beta.1")).toBe(true);
        expect(isVersion("1.2")).toBe(false);
    });

    it("wants 64 hex characters", () => {
        expect(isSha256("a".repeat(64))).toBe(true);
        expect(isSha256("A".repeat(64))).toBe(false);
        expect(isSha256("a".repeat(63))).toBe(false);
    });
});

// ---------------------------------------------------------- the cleaner ----

describe("input cleaning keeps what people write", () => {
    // THE REGRESSION THIS FILE EXISTS FOR. Written as literal characters rather
    // than escapes, the control class collapses into the range space-to-hyphen
    // and silently strips spaces, commas, brackets and full stops out of every
    // name and summary. It still looks like a filter and still passes anything
    // that only checks "did it return a string".
    it("leaves ordinary punctuation and spacing alone", () => {
        const name = "Grand Campaign - 1914, v2.0 (beta)";
        const d = parseDraft(draft({ name }));
        expect((d as Draft).name).toBe(name);
    });

    it("still removes real control characters", () => {
        const d = parseDraft(draft({
            name: `Bell${String.fromCharCode(7)}Mod`,
            summary: `A summary${String.fromCharCode(31)} with a control char in it.`,
        }));
        expect((d as Draft).name).toBe("BellMod");
        expect((d as Draft).summary).not.toContain(String.fromCharCode(31));
    });
});

describe("parseDraft refuses what the game would", () => {
    it("names the id rule rather than saying 'invalid'", () => {
        const r = parseDraft(draft({ id: "betterai" })) as { code: string; message: string };
        expect(r.code).toBe("bad_id");
        expect(r.message).toContain("MANIFEST.json");
    });

    it("refuses an unknown capability module instead of dropping it", () => {
        // The loader's rule: silently losing a capability a mod believes it has
        // is worse than refusing it.
        const r = parseDraft(draft({ modules: ["Core", "Telepathy"] })) as { code: string };
        expect(r.code).toBe("bad_module");
    });

    it("refuses a non-https download url", () => {
        const r = parseDraft(draft({ downloadUrl: "http://example.com/x.odmod" })) as
            { code: string };
        expect(r.code).toBe("bad_download");
    });
});

// -------------------------------------------------------------- publish ----

describe("publishing", () => {
    it("creates, then updates in place", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const first = await publish(env, jane, asDraft());
        expect((first as { created: boolean }).created).toBe(true);

        const second = await publish(env, jane, asDraft({ version: "1.3.0" }));
        expect((second as { created: boolean }).created).toBe(false);

        const stored = await getListing(env, "com.example.better-ai");
        expect(stored?.version).toBe("1.3.0");
        // created is the ORIGINAL date -- browse order must not jump every
        // time somebody fixes a typo.
        expect(stored?.created).toBe((first as { listing: Listing }).listing.created);
    });

    it("refuses an id already held by somebody else", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const bob = await makeAccount("acct-bob", "Bob");
        await publish(env, jane, asDraft());

        const r = await publish(env, bob, asDraft()) as { ok: false; status: number; code: string };
        expect(r.ok).toBe(false);
        expect(r.status).toBe(409);
        expect(r.code).toBe("id_taken");
    });

    it("will not let a removed listing be republished", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const p = await publish(env, jane, asDraft()) as { listing: Listing };
        await env.OD_ACCOUNTS.put(`pkg:mod:${p.listing.id}`,
                                  JSON.stringify({ ...p.listing, status: "removed" }));

        const r = await publish(env, jane, asDraft()) as { ok: false; code: string };
        expect(r.code).toBe("removed");
    });

    it("drops the scan when the declared hash changes", async () => {
        // Carrying a verdict across a new upload would be the registry
        // vouching for bytes nothing ever looked at.
        const jane = await makeAccount("acct-jane", "Jane");
        const p = await publish(env, jane, asDraft()) as { listing: Listing };
        await setScan(env, p.listing, { at: 1, known: true, malicious: 0, engines: 70 });
        expect((await getListing(env, p.listing.id))?.scan).toBeTruthy();

        await publish(env, jane, asDraft({ sha256: "b".repeat(64) }));
        expect((await getListing(env, p.listing.id))?.scan).toBeUndefined();
    });

    it("withdrawing removes the record and both index keys", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const p = await publish(env, jane, asDraft()) as { listing: Listing };
        expect(await countOwned(env, jane.id)).toBe(1);

        await withdraw(env, p.listing);
        expect(await getListing(env, p.listing.id)).toBeNull();
        expect(await countOwned(env, jane.id)).toBe(0);
        expect((await browse(env)).mods).toHaveLength(0);
    });
});

describe("browse", () => {
    it("returns newest first", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        await publish(env, jane, asDraft({ id: "com.example.old" }), 1000);
        await publish(env, jane, asDraft({ id: "com.example.new" }), 2000);

        const page = await browse(env);
        expect(page.mods.map((m) => m.id)).toEqual(["com.example.new", "com.example.old"]);
    });

    it("hides an unlisted mod", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const p = await publish(env, jane, asDraft()) as { listing: Listing };
        await env.OD_ACCOUNTS.put(`pkg:mod:${p.listing.id}`,
                                  JSON.stringify({ ...p.listing, status: "unlisted" }));
        expect((await browse(env)).mods).toHaveLength(0);
    });

    it("does not leak the owner's account id", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const p = await publish(env, jane, asDraft()) as { listing: Listing };
        const shown = publicListing(p.listing);
        expect(shown.by).toBe("Jane");
        expect(JSON.stringify(shown)).not.toContain("acct-jane");
    });
});

// ----------------------------------------------------------------- scan ----

describe("a scanner result is never rounded up", () => {
    it("calls an unseen hash unknown, not clean", () => {
        const r = scanForClients({ at: 1, known: false });
        expect(r.state).toBe("unknown");
        expect((r as { note: string }).note).toContain("not the same as clean");
    });

    it("distinguishes never-asked from asked-and-unseen", () => {
        expect(scanForClients(undefined).state).toBe("unscanned");
    });

    it("flags on suspicious alone, not only on malicious", () => {
        const r = scanForClients({ at: 1, known: true, malicious: 0, suspicious: 2, engines: 70 });
        expect(r.state).toBe("flagged");
    });

    it("reports the denominator with the count", () => {
        const r = scanForClients({ at: 1, known: true, malicious: 0, suspicious: 0, engines: 70 });
        expect(r.state).toBe("clean");
        expect((r as { engines: number }).engines).toBe(70);
    });
});

// --------------------------------------------------------------- counts ----

describe("download counts", () => {
    it("counts every hit, and one unique per marker per day", async () => {
        const day = dayNumber();
        await recordHit(env, "com.example.counted", "marker-a", day);
        await recordHit(env, "com.example.counted", "marker-a", day);
        const c = await recordHit(env, "com.example.counted", "marker-b", day);

        expect(c.downloads).toBe(3);
        expect(c.unique).toBe(2);
    });

    it("counts the same person again on a later day, and says so", async () => {
        // The documented cost of not keeping an address for ever. It has to be
        // asserted, because a reader who assumes otherwise will misreport it.
        const day = dayNumber();
        await recordHit(env, "com.example.daily", "marker-a", day);
        const c = await recordHit(env, "com.example.daily", "marker-a", day + 1);

        expect(c.downloads).toBe(2);
        expect(c.unique).toBe(2);
        expect(c.uniqueBasis).toContain("per day");
    });

    it("counts a hit with no address as a download but not a unique", async () => {
        const c = await recordHit(env, "com.example.noip", "", dayNumber());
        expect(c.downloads).toBe(1);
        expect(c.unique).toBe(0);
    });

    it("keeps separate objects per mod", async () => {
        await recordHit(env, "com.example.one", "m", dayNumber());
        expect((await readCounts(env, "com.example.two")).downloads).toBe(0);
    });
});

// ---------------------------------------------------------------- tags ----

describe("the modmaker tag", () => {
    it("appears at the threshold and not before", () => {
        expect(tagsFrom([], MODMAKER_THRESHOLD - 1)).not.toContain("modmaker");
        expect(tagsFrom([], MODMAKER_THRESHOLD)).toContain("modmaker");
    });

    it("sits beside granted badges without becoming one", () => {
        // badges.ts: granted by hand, no automation. An earned tag must not be
        // able to end up in account.badges, or that sentence stops being true.
        const tags = tagsFrom(["developer"], MODMAKER_THRESHOLD);
        expect(tags).toEqual(["developer", "modmaker"]);
    });

    it("goes away again when listings are withdrawn", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        for (let i = 0; i < MODMAKER_THRESHOLD; i++) {
            await publish(env, jane, asDraft({ id: `com.example.m${i}` }));
        }
        expect(tagsFrom([], await countOwned(env, jane.id))).toContain("modmaker");

        const one = await getListing(env, "com.example.m0");
        await withdraw(env, one!);
        expect(tagsFrom([], await countOwned(env, jane.id))).not.toContain("modmaker");
    });
});

// ------------------------------------------------------- mod moderation ----

describe("reporting a mod is scoped, and cannot reach the account ban", () => {
    async function reported() {
        const jane = await makeAccount("acct-jane", "Jane");
        const bob = await makeAccount("acct-bob", "Bob");
        await publish(env, jane, asDraft());
        const input = parseModReportInput({
            modId: "com.example.better-ai", reason: "malware",
            note: "Flagged by three engines.",
        });
        const report = await fileModReport(env, bob, input as never);
        return { jane, bob, report: report as { id: string; modId: string } };
    }

    it("files against a listing that exists", async () => {
        const { report } = await reported();
        expect(report.modId).toBe("com.example.better-ai");
        expect(await listModReports(env)).toHaveLength(1);
    });

    it("refuses a report against a mod that does not exist", async () => {
        const bob = await makeAccount("acct-bob", "Bob");
        const r = await fileModReport(env, bob, {
            modId: "com.example.nope", reason: "spam", note: "nothing here",
        }) as { ok: false; status: number };
        expect(r.status).toBe(404);
    });

    it("tells an owner to withdraw their own listing rather than report it", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        await publish(env, jane, asDraft());
        const r = await fileModReport(env, jane, {
            modId: "com.example.better-ai", reason: "spam", note: "my own",
        }) as { ok: false; code: string };
        expect(r.code).toBe("self");
    });

    it("RESTRICT stops publishing and leaves the account unbanned", async () => {
        // The whole reason mod reports are a separate queue. If this assertion
        // ever fails, every mod complaint has become a way to remove somebody
        // from the game.
        const { jane, report } = await reported();
        const mod = await makeAccount("acct-mod", "Mod", ["developer"]);

        await decideModReport(env, await getReportRecord(report.id), {
            action: "restrict", reason: "Published malware.",
        }, mod);

        const after = JSON.parse(
            (await env.OD_ACCOUNTS.get(`acct:${jane.id}`))!,
        ) as Account;
        expect(modsRestricted(after)).toBe(true);
        expect(banInForce(after)).toBe(false);
        expect(after.banned).toBeUndefined();
    });

    it("UNLIST takes the listing down without touching the account", async () => {
        const { jane, report } = await reported();
        const mod = await makeAccount("acct-mod", "Mod", ["developer"]);

        await decideModReport(env, await getReportRecord(report.id), {
            action: "unlist", reason: "Dead link.",
        }, mod);

        expect((await getListing(env, "com.example.better-ai"))?.status).toBe("removed");
        const after = JSON.parse((await env.OD_ACCOUNTS.get(`acct:${jane.id}`))!) as Account;
        expect(modsRestricted(after)).toBe(false);
    });

    it("dismissing is a real, recorded outcome", async () => {
        const { report } = await reported();
        const mod = await makeAccount("acct-mod", "Mod", ["developer"]);
        const decided = await decideModReport(env, await getReportRecord(report.id), {
            action: "dismiss", reason: "Works fine.",
        }, mod) as { status: string; outcome: string };

        expect(decided.status).toBe("dismissed");
        expect(decided.outcome).toBe("dismissed");
        expect((await getListing(env, "com.example.better-ai"))?.status).toBe("listed");
    });

    it("refuses to decide the same report twice", async () => {
        const { report } = await reported();
        const mod = await makeAccount("acct-mod", "Mod", ["developer"]);
        await decideModReport(env, await getReportRecord(report.id),
                              { action: "dismiss", reason: "x" }, mod);
        const again = await decideModReport(env, await getReportRecord(report.id),
                                            { action: "unlist", reason: "y" }, mod) as
            { ok: false; code: string };
        expect(again.code).toBe("already_decided");
    });

    async function getReportRecord(id: string) {
        const raw = await env.OD_ACCOUNTS.get(`pkg:report:${id}`);
        return JSON.parse(raw!);
    }
});

describe("a restriction expires on its own", () => {
    it("stops applying once its date passes, without a sweep", async () => {
        const jane = await makeAccount("acct-jane", "Jane");
        const now = 1_000_000;
        const restricted = await setModsRestricted(env, jane, true, "spam", 1, now);

        expect(modsRestricted(restricted, now + 3600)).toBe(true);
        expect(modsRestricted(restricted, now + 2 * 86400)).toBe(false);
        // The record survives, so a repeat is visible to a moderator.
        expect(restricted.restricted?.mods?.reason).toBe("spam");
    });

    it("lifting one leaves the account otherwise untouched", async () => {
        const jane = await makeAccount("acct-jane", "Jane", ["playtester"]);
        const on = await setModsRestricted(env, jane, true, "spam");
        const off = await setModsRestricted(env, on, false, "");
        expect(modsRestricted(off)).toBe(false);
        expect(off.badges).toEqual(["playtester"]);
    });
});
