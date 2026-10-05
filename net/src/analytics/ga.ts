// Google Analytics, for the people who said yes -- and through here, never
// straight from the player's machine.
//
// WHY A RELAY
//
// GA4's Measurement Protocol needs an API secret, and a secret in a shipped
// binary is a secret everyone has: anyone could then write to our property.
// Kept here, it stays a secret. And the request Google receives comes from a
// Cloudflare address, not the player's -- so the one thing GA would otherwise
// always learn, the IP, is never sent to it at all.
//
// WHAT CAN BE SENT
//
// A closed list of event names, each with a closed list of parameters whose
// values are short tokens (a version, a platform, a tab name). No free text
// crosses this route, so a client cannot be made to report a world name, a
// nickname or a file path even by mistake. Anything outside the list is
// dropped, not forwarded.
//
// CONSENT IS THE CLIENT'S TO ENFORCE AND OURS TO DESCRIBE
//
// Unifico asks once, defaults to no, and only after a yes creates the random
// client id GA needs; a no deletes it. The game's own report (the "/usage"
// session length, already opt-in) is counted here too, under a fresh random id
// each time, so those reports stay exactly as unlinkable as PRIVACY.md says.
// See PRIVACY.md, "Launcher statistics".

import type { Env } from "../env.js";
import { fail, json, readJson } from "../http.js";

const TOKEN = /^[A-Za-z0-9._:-]{1,40}$/;

/** Event -> the parameters it may carry. */
export const GA_EVENTS: Record<string, readonly string[]> = {
    launcher_open: ["launcher_version", "platform", "language"],
    game_launch: ["game_version", "platform", "source"],
    game_exit: ["game_version", "minutes_bucket", "exit_kind"],
    version_install: ["game_version", "platform"],
    version_uninstall: ["game_version"],
    tab_view: ["tab"],
    map_translate: ["direction"],
    other_game_launch: ["game"],
    templeos_launch: ["mode"],
    mod_install: ["source"],
    language_change: ["language"],
    launcher_update: ["from_version", "to_version"],
    session_end: ["minutes_bucket", "surface"],
};

export interface GaEvent { name: string; params?: Record<string, string | number> }

export function cleanEvents(events: unknown): GaEvent[] {
    if (!Array.isArray(events)) return [];
    const out: GaEvent[] = [];
    for (const e of events.slice(0, 20)) {
        if (!e || typeof e !== "object") continue;
        const name = (e as GaEvent).name;
        const allowed = typeof name === "string" ? GA_EVENTS[name] : undefined;
        if (!allowed) continue;
        const params: Record<string, string | number> = {};
        const raw = (e as GaEvent).params ?? {};
        for (const key of allowed) {
            const v = (raw as Record<string, unknown>)[key];
            if (typeof v === "number" && Number.isFinite(v)) params[key] = Math.round(v);
            else if (typeof v === "string" && TOKEN.test(v)) params[key] = v;
        }
        out.push({ name, params });
    }
    return out;
}

export function gaConfigured(env: Env): boolean {
    return !!env.GA_MEASUREMENT_ID && !!env.GA_API_SECRET;
}

/**
 * A session id GA4 accepts: a positive integer. Realtime and the session
 * reports ignore Measurement Protocol events that have none -- they are stored
 * but never shown, which looked exactly like "nothing arrives". It identifies
 * one launcher run, not a person: the launcher makes a new one each start.
 */
export function cleanSessionId(v: unknown): number {
    const n = typeof v === "number" ? v : typeof v === "string" ? Number(v) : NaN;
    if (Number.isInteger(n) && n > 0 && n < 1e15) return n;
    return Math.floor(Date.now() / 1000);
}

export async function forward(env: Env, clientId: string, events: GaEvent[], sessionId?: number): Promise<boolean> {
    if (!gaConfigured(env) || events.length === 0) return false;
    const url = `https://www.google-analytics.com/mp/collect?measurement_id=${encodeURIComponent(env.GA_MEASUREMENT_ID!)}` +
        `&api_secret=${encodeURIComponent(env.GA_API_SECRET!)}`;
    const body = {
        client_id: clientId,
        // No user_id, no user_properties, no IP override: the documented ways
        // to tie events to a person are exactly the ones not used.
        non_personalized_ads: true,
        events: events.map((e) => ({
            name: e.name,
            params: { ...e.params, session_id: sessionId ?? cleanSessionId(undefined), engagement_time_msec: 100 },
        })),
    };
    try {
        const r = await fetch(url, { method: "POST", body: JSON.stringify(body) });
        return r.ok;
    } catch {
        return false;
    }
}

/** POST /analytics/event -- from Unifico, only after the player said yes. */
export async function analyticsEvent(request: Request, env: Env, ctx?: ExecutionContext): Promise<Response> {
    const body = await readJson<{ client_id?: string; session_id?: unknown; events?: unknown }>(request, 4 * 1024);
    const clientId = body?.client_id ?? "";
    // GA's own client-id shape, or a UUID: a random token, never an account.
    if (!/^[0-9a-f-]{16,40}$|^\d{6,12}\.\d{6,12}$/.test(clientId)) return fail(400, "bad_request", "Bad client id.");
    const events = cleanEvents(body?.events);
    if (events.length === 0) return json({ ok: true, sent: 0 });
    const p = forward(env, clientId, events, cleanSessionId(body?.session_id));
    if (ctx) ctx.waitUntil(p); else await p;
    return json({ ok: true, sent: events.length });
}
