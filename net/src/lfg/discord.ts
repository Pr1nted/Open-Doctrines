// The Discord half of the board: the bot's messages, and the commands it answers.
//
// ── A BOT WITHOUT A MACHINE TO RUN ON ──
//
// Discord bots usually hold a gateway socket open, which needs a process that
// never stops, which is a server, which this project does not have and does not
// want to pay for. Everything below uses the two halves that need no socket:
//
//   - The REST API, to post and edit the bot's own messages.
//   - HTTP interactions: Discord POSTs a signed request to this Worker when
//     somebody runs /lfg or presses a button, and the reply is the response.
//
// The cost is that the bot cannot read what people type in the channel. That is
// why the channel is set so members cannot post in it and must use /lfg: a
// structured form is the only way in, so the guidelines are enforced by the
// shape of a listing rather than by reading messages after the fact.
//
// ── NOTHING HERE IS TRUSTED ──
//
// Every interaction is verified against the application's public key before it
// is read, and a request that fails verification gets 401 without being parsed.
// Discord tests exactly this when you set the endpoint URL.

import type { Env } from "../env.js";
import { LIMITS, type Listing } from "./board.js";

const API = "https://discord.com/api/v10";

/** Discord interaction types and response types, by name. */
export const enum Interaction { Ping = 1, Command = 2, Component = 3, ModalSubmit = 5 }
export const enum Reply { Pong = 1, Message = 4, Deferred = 5, Modal = 9 }

/** Only these are ever sent; a listing carries no other user-controlled text. */
function field(name: string, value: string, inline = true) {
    return { name, value: value.slice(0, 1024) || "-", inline };
}

function pace(listing: Listing): string {
    if (listing.mode === "rapid") return `Rapid, ${listing.turnSeconds}s a turn`;
    const hours = listing.turnHours ?? 0;
    return `Longform, ${hours}h a turn`;
}

function embedFor(listing: Listing, state?: string) {
    const seats = listing.slotsTotal
        ? `${listing.slotsTaken ?? 0}/${listing.slotsTotal}`
        : "-";
    return {
        title: listing.kind === "hosting"
            ? `Hosting: ${listing.map}`
            : `Looking for a game: ${listing.map}`,
        description: listing.note || undefined,
        color: state ? 0x6b7280 : listing.kind === "hosting" ? 0x2f9e68 : 0x3b7dd8,
        fields: [
            field("Host", listing.nick),
            field("Mode", pace(listing)),
            field("Players", seats),
            field("Language", listing.language),
            field("Region", listing.region),
            field(state ? "Closed" : "Open until", state
                ? state
                : `<t:${listing.expiresAt}:R>`),
        ],
        footer: { text: state ? "This game is no longer open." : "Open Doctrines · /lfg to post your own" },
    };
}

function componentsFor(env: Env, listing: Listing) {
    const rows: unknown[] = [];
    const buttons: unknown[] = [];
    if (listing.kind === "hosting" && listing.code) {
        buttons.push({
            type: 2, style: 5, label: "Join this game",
            url: `${env.ISSUER}/join/${encodeURIComponent(listing.code)}`,
        });
    }
    buttons.push({ type: 2, style: 2, label: "Report", custom_id: `lfg:report:${listing.id}` });
    rows.push({ type: 1, components: buttons });
    return rows;
}

async function discord(env: Env, path: string, init: RequestInit): Promise<Response | null> {
    if (!env.DISCORD_BOT_TOKEN) return null;
    return fetch(`${API}${path}`, {
        ...init,
        headers: {
            "authorization": `Bot ${env.DISCORD_BOT_TOKEN}`,
            "content-type": "application/json",
            ...(init.headers as Record<string, string> | undefined),
        },
    });
}

/**
 * Put a listing in the channel. Returns the message id, or undefined when no
 * bot is configured -- which is a fork without Discord, not an error: the board
 * works in the game either way.
 */
export async function postListing(env: Env, listing: Listing): Promise<string | undefined> {
    if (!env.DISCORD_LFG_CHANNEL_ID) return undefined;
    const res = await discord(env, `/channels/${env.DISCORD_LFG_CHANNEL_ID}/messages`, {
        method: "POST",
        body: JSON.stringify({ embeds: [embedFor(listing)], components: componentsFor(env, listing) }),
    });
    if (!res || !res.ok) return undefined;
    const message = (await res.json()) as { id?: string };
    return message.id;
}

/** Grey the message out and take the buttons off. The game is gone. */
export async function closeMessage(env: Env, messageId: string, listing: Listing, why: string): Promise<void> {
    if (!env.DISCORD_LFG_CHANNEL_ID) return;
    await discord(env, `/channels/${env.DISCORD_LFG_CHANNEL_ID}/messages/${messageId}`, {
        method: "PATCH",
        body: JSON.stringify({ embeds: [embedFor(listing, why)], components: [] }),
    });
}

/** The moderation channel already used for reports, reused for listings. */
export async function reportToModerators(env: Env, listing: Listing, reporter: string, reason: string, note: string): Promise<void> {
    if (!env.MODERATION_DISCORD_WEBHOOK) return;
    await fetch(env.MODERATION_DISCORD_WEBHOOK, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
            content: `Listing reported: \`${listing.id}\` by ${reporter} (${reason})`,
            embeds: [embedFor(listing), { title: "What the reporter said", description: note.slice(0, 1000) || "-" }],
        }),
    });
}

// ---------------------------------------------------------------- interactions

function hexToBytes(hex: string): Uint8Array {
    const out = new Uint8Array(hex.length / 2);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
    return out;
}

/**
 * Verify Discord's signature over timestamp + body.
 *
 * Returns the raw body only when it verifies, so a caller cannot accidentally
 * read an unverified one: there is nothing to read until this says yes.
 */
export async function verifyInteraction(request: Request, env: Env): Promise<string | null> {
    const signature = request.headers.get("x-signature-ed25519");
    const timestamp = request.headers.get("x-signature-timestamp");
    if (!signature || !timestamp || !env.DISCORD_PUBLIC_KEY) return null;
    const body = await request.text();
    try {
        const key = await crypto.subtle.importKey(
            "raw", hexToBytes(env.DISCORD_PUBLIC_KEY), { name: "Ed25519" }, false, ["verify"]);
        const ok = await crypto.subtle.verify(
            "Ed25519", key, hexToBytes(signature), new TextEncoder().encode(timestamp + body));
        return ok ? body : null;
    } catch {
        return null;
    }
}

/** A plain, private reply: only the person who pressed the button sees it. */
export function ephemeral(content: string) {
    return { type: Reply.Message, data: { content, flags: 64 } };
}

/** The /lfg command, as Discord needs it registered. `tools/discord-register.mjs` sends this. */
export const LFG_COMMAND = {
    name: "lfg",
    description: "Post that you are hosting an Open Doctrines game, or looking for one",
    options: [
        {
            type: 3, name: "tag", description: "Which one this is", required: true,
            choices: [
                { name: "hosting a game", value: "hosting" },
                { name: "looking for a game", value: "looking" },
            ],
        },
        { type: 3, name: "map", description: "Which map", required: true, max_length: LIMITS.map },
        {
            type: 3, name: "mode", description: "Turn mode", required: true,
            choices: [{ name: "rapid", value: "rapid" }, { name: "longform", value: "longform" }],
        },
        {
            type: 4, name: "pace", description: "Seconds a turn (rapid) or hours a turn (longform)",
            required: true, min_value: 1, max_value: LIMITS.turnSecondsMax,
        },
        { type: 3, name: "code", description: "Your lobby's invite code (hosting only)", required: false, max_length: 32 },
        { type: 4, name: "seats", description: "How many players the game seats (hosting only)", required: false, min_value: LIMITS.slotsMin, max_value: LIMITS.slotsMax },
        { type: 4, name: "taken", description: "How many are in already (hosting only)", required: false, min_value: 0, max_value: LIMITS.slotsMax },
        { type: 3, name: "language", description: "Language at the table", required: false, max_length: LIMITS.language },
        { type: 3, name: "region", description: "Where you are, for latency", required: false, max_length: LIMITS.region },
        { type: 3, name: "note", description: "Anything else. No links.", required: false, max_length: LIMITS.note },
    ],
} as const;

/** Options arrive as a list; this is the lookup the handler wants. */
export function optionsOf(data: any): Record<string, string | number> {
    const out: Record<string, string | number> = {};
    for (const option of (data?.options ?? []) as { name: string; value: string | number }[]) {
        out[option.name] = option.value;
    }
    return out;
}
