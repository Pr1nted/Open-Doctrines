// The board's endpoints: the game's, the moderators', and Discord's.
//
// The game and Discord post through the SAME validator and the same object, so
// a listing made with /lfg and one made in the lobby are the same thing, expire
// the same way and are reported the same way. That is what "doubled" has to
// mean, or the two boards drift apart within a day.

import type { Env } from "../env.js";
import { fail, json, readJson, text, authenticate } from "../http.js";
import { accountForIdentity, identSubHash, banInForce, type Account } from "../accounts/store.js";
import { loadBlocklist } from "../accounts/nickname.js";
import { isSessionCode } from "../lobby/session.js";
import { LIMITS, forClients as listingForClients, validate, type Listing, type ListingInput } from "./board.js";
import {
    Interaction, Reply, LFG_COMMAND, ephemeral, optionsOf, reportToModerators, verifyInteraction,
} from "./discord.js";

function board(env: Env): DurableObjectStub {
    // One object for the whole board; the name is a constant, not a user input.
    return env.LFG_BOARD.get(env.LFG_BOARD.idFromName("board"));
}

async function call(env: Env, path: string, body?: unknown): Promise<any> {
    const response = await board(env).fetch(new Request(`https://lfg${path}`, {
        method: body === undefined ? "GET" : "POST",
        body: body === undefined ? undefined : JSON.stringify(body),
        headers: { "content-type": "application/json" },
    }));
    return await response.json();
}

function newId(): string {
    return crypto.randomUUID().replace(/-/g, "").slice(0, 12);
}

/** The board, for the game's browser and for anybody curious. */
export async function lfgList(env: Env): Promise<Response> {
    const result = await call(env, "/list");
    // Short cache: the board changes when somebody posts, and a game refreshing
    // every few seconds must not cost a Durable Object request each time.
    return json({ listings: result.listings ?? [], limits: LIMITS }, 200,
        { "cache-control": "public, max-age=15" });
}

/** Post a listing from the game. */
export async function lfgPost(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "no_account", "Sign in to post a game.");
    if (banInForce(account)) return fail(403, "banned", "This account cannot post listings.");

    const input = await readJson<ListingInput>(request, 4 * 1024);
    if (!input) return fail(400, "bad_request", "Empty listing.");
    return await create(env, input, account.id, account.nick);
}

async function create(env: Env, input: ListingInput, accountId: string, nick: string): Promise<Response> {
    const checked = validate(input, {
        id: newId(), accountId, nick,
        now: Math.floor(Date.now() / 1000),
        blocklist: await loadBlocklist(env),
    });
    if (!checked.ok) return fail(400, "bad_listing", checked.reason);

    const result = await call(env, "/post", { listing: checked.value });
    if (!result.ok) return fail(result.status ?? 400, "refused", result.reason ?? "Listing refused.");
    return json({ listing: result.listing });
}

/** Take your own listing down. A host whose game filled up does this. */
export async function lfgClose(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "no_account", "Sign in first.");
    const body = await readJson<{ id?: string }>(request);
    if (!body?.id) return fail(400, "bad_request", "Which listing?");
    const result = await call(env, "/close", { id: body.id, accountId: account.id, moderator: isModerator(account) });
    if (!result.ok) return fail(result.status ?? 400, "refused", result.reason ?? "Could not close it.");
    return json({ ok: true });
}

/** Report a listing. Anyone signed in may; the queue is a person's job. */
export async function lfgReport(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "no_account", "Sign in to report a listing.");
    const body = await readJson<{ id?: string; reason?: string; note?: string }>(request);
    if (!body?.id) return fail(400, "bad_request", "Which listing?");

    const result = await call(env, "/report", {
        id: body.id, reporterId: account.id, reason: body.reason ?? "other", note: body.note ?? "",
    });
    if (!result.ok) return fail(result.status ?? 400, "refused", result.reason ?? "Could not report it.");
    await reportToModerators(env, result.listing as Listing, account.nick, String(body.reason ?? "other"), String(body.note ?? ""));
    return json({ ok: true, reports: result.reports });
}

function isModerator(account: Account | null): boolean {
    return !!account?.badges?.includes("developer");
}

/** Everything, hidden included. Developer badge only, like the other queues. */
export async function lfgModerationList(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!isModerator(account)) return fail(404, "not_found", "No such endpoint.");
    return json(await call(env, "/all"));
}

export async function lfgModerate(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!isModerator(account)) return fail(404, "not_found", "No such endpoint.");
    const body = await readJson<{ op?: string; id?: string }>(request);
    if (!body?.op || !body.id) return fail(400, "bad_request", "Need op and id.");
    const result = await call(env, "/moderate", { op: body.op, id: body.id });
    if (!result.ok) return fail(result.status ?? 400, "refused", result.reason ?? "Could not do that.");
    return json(result);
}

// ------------------------------------------------------------------- Discord

/**
 * What Discord POSTs when somebody runs /lfg or presses a button.
 *
 * Verified first, parsed second. Discord sends deliberately bad signatures when
 * you save the endpoint URL, and an endpoint that answers those is rejected.
 */
export async function discordInteractions(request: Request, env: Env): Promise<Response> {
    const body = await verifyInteraction(request, env);
    if (body === null) return new Response("bad signature", { status: 401 });
    const interaction = JSON.parse(body) as any;

    if (interaction.type === Interaction.Ping) return json({ type: Reply.Pong });

    if (interaction.type === Interaction.Command && interaction.data?.name === LFG_COMMAND.name) {
        return json(await lfgCommand(env, interaction));
    }

    if (interaction.type === Interaction.Component) {
        const id = String(interaction.data?.custom_id ?? "");
        if (id.startsWith("lfg:report:")) {
            // A reason box, because "reported" with no words is a report a
            // moderator cannot act on.
            return json({
                type: Reply.Modal,
                data: {
                    custom_id: `lfg:reportmodal:${id.slice("lfg:report:".length)}`,
                    title: "Report this listing",
                    components: [{
                        type: 1,
                        components: [{
                            type: 4, custom_id: "why", style: 2, required: true, max_length: 400,
                            label: "What is wrong with it?",
                            placeholder: "Not an Open Doctrines game, rude, missing details...",
                        }],
                    }],
                },
            });
        }
        return json(ephemeral("That button is no longer active."));
    }

    if (interaction.type === Interaction.ModalSubmit) {
        const id = String(interaction.data?.custom_id ?? "");
        if (id.startsWith("lfg:reportmodal:")) {
            const listingId = id.slice("lfg:reportmodal:".length);
            const why = String(
                interaction.data?.components?.[0]?.components?.[0]?.value ?? "").slice(0, 400);
            const who = discordUser(interaction);
            const result = await call(env, "/report", {
                id: listingId, reporterId: `discord:${who.id}`, reason: "discord", note: why,
            });
            if (!result.ok) return json(ephemeral(result.reason ?? "That listing is gone."));
            await reportToModerators(env, result.listing as Listing, `${who.name} (Discord)`, "discord", why);
            return json(ephemeral("Thank you. A moderator will look at it."));
        }
    }

    return json(ephemeral("Unsupported interaction."));
}

function discordUser(interaction: any): { id: string; name: string } {
    const user = interaction.member?.user ?? interaction.user ?? {};
    return { id: String(user.id ?? ""), name: String(user.global_name ?? user.username ?? "someone") };
}

/**
 * /lfg, from inside Discord.
 *
 * It needs an Open Doctrines account, and it finds one by the Discord sign-in
 * the person already linked in the game. That is the whole trick: no second
 * identity, no second nickname to moderate, and a rule-breaker is the same
 * account whichever side they posted from.
 */
async function lfgCommand(env: Env, interaction: any): Promise<Record<string, unknown>> {
    const who = discordUser(interaction);
    if (!who.id) return ephemeral("Discord did not say who you are. Try again.");

    const account = await accountForIdentity(env, "discord", await identSubHash(env, "discord", who.id));
    if (!account) {
        return ephemeral(
            "Link this Discord account first: open Open Doctrines, Main menu → Account → Sign in with Discord. " +
            "Then /lfg posts under your Open Doctrines nickname.");
    }
    if (banInForce(account)) return ephemeral("This account cannot post listings.");

    const options = optionsOf(interaction.data);
    const mode = String(options.mode ?? "");
    const pace = Number(options.pace ?? 0);
    const input: ListingInput = {
        kind: options.tag,
        map: options.map,
        mode,
        turnSeconds: mode === "rapid" ? pace : undefined,
        turnHours: mode === "longform" ? pace : undefined,
        code: options.code,
        slotsTotal: options.seats,
        slotsTaken: options.taken,
        language: options.language,
        region: options.region,
        note: options.note,
    };

    const response = await create(env, input, account.id, account.nick);
    if (!response.ok) {
        const problem = (await response.json()) as { message?: string };
        return ephemeral(problem.message ?? "That listing was refused.");
    }
    return ephemeral(
        options.tag === "hosting"
            ? "Posted. It is in the channel and in the game's board, and it closes itself when it expires."
            : "Posted. Hosts will see it in the game too.");
}

/**
 * The /lfg command's definition, for the script that registers it.
 *
 * Served rather than duplicated in tools/register-lfg-command.mjs: the options
 * Discord SHOWS and the options lfgCommand() READS have to be the same list,
 * and a hand-copied JSON file in a tools directory is exactly the copy that
 * silently stops matching the day somebody adds a field. Nothing here is
 * secret -- Discord shows this shape to everyone who types a slash.
 */
export function lfgCommandDefinition(): Response {
    return json(LFG_COMMAND, 200, { "cache-control": "public, max-age=300" });
}

// ---------------------------------------------------------------------- join

/**
 * The page behind a Join button.
 *
 * A Discord button can only open a URL, and `opendoctrines://join/<code>` is
 * not one Discord will accept. So this page is the hop: it hands the code to
 * the game through the scheme the game already registers for stream links, and
 * says what to do when nothing happens, which is what a person without the game
 * installed needs to read.
 */
export function joinPage(env: Env, code: string): Response {
    if (!isSessionCode(code)) return fail(404, "not_found", "That is not an invite code.");
    const safe = code.replace(/[^A-Za-z0-9_-]/g, "");
    const deep = `opendoctrines://join/${safe}`;
    return text(`<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Join an Open Doctrines game</title>
<style>body{background:#11161a;color:#e8eef2;font:16px/1.6 system-ui,sans-serif;margin:0;display:grid;place-items:center;height:100vh}
main{max-width:34rem;padding:2rem;text-align:center}code{background:#1b2228;padding:.2em .45em;border-radius:.3em}
a.button{display:inline-block;margin:1.2rem .4rem;padding:.7rem 1.2rem;border-radius:.5rem;background:#2f9e68;color:#08130d;font-weight:600;text-decoration:none}
a.plain{color:#8fc7ff}</style>
<main>
<h1>Join this game</h1>
<p>Invite code <code>${safe}</code></p>
<a class="button" href="${deep}">Open Open Doctrines</a>
<p>If nothing happens, open the game yourself, then <b>Multiplayer → Join a game</b> and paste the code above.</p>
<p><a class="plain" href="${env.DOCS_BASE ?? "https://opendoctrines.pages.dev"}">Don't have the game? It is free.</a></p>
<script>location.replace(${JSON.stringify(deep)});</script>
</main></html>`, 200, "text/html; charset=utf-8");
}
