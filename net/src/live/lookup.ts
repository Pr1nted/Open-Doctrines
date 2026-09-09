// Asking each platform whether a channel is live, without asking too often.
//
// ── THE CACHE IS NOT AN OPTIMISATION ──
//
// Every player in a lobby may ask about every other player, every time they
// open a screen. Unbounded that is a request per viewer per platform per
// refresh, against APIs with per-app rate limits -- the whole game's key gets
// throttled and NOBODY sees a live marker.
//
// It uses `caches.default` rather than KV on purpose: Workers KV allows a
// thousand writes a day on the free plan and account creation already spends
// three of them (see accounts/store.ts). Caching a liveness check in KV would
// eat the budget the game actually needs. The cache API has no such limit and
// is exactly what it is for.
//
// ── AND A MISS IS "NOT LIVE", NEVER AN ERROR ──
//
// A rate limit, a missing credential, a platform having a bad afternoon: all of
// them mean the marker is absent. A live badge is decoration; a lobby that will
// not open because Twitch is down is a broken game.

import type { Env } from "../env.js";
import { readFor, safeChannel, type Live, type Platform } from "./platforms.js";

/** How long an answer is reused. Long enough to matter, short enough to be true. */
const CACHE_SECONDS = 60;

const OFFLINE: Live = { live: false };

interface Upstream {
    url: string;
    headers: Record<string, string>;
}

/**
 * Where to ask, and with what. Null when this deployment has no credential for
 * the platform -- in which case nobody on it is ever marked live, which is the
 * right behaviour for a fork that has not registered an app.
 */
async function upstreamFor(env: Env, platform: Platform, channel: string):
        Promise<Upstream | null> {
    if (platform === "twitch") {
        const id = env.TWITCH_CLIENT_ID;
        const secret = env.TWITCH_CLIENT_SECRET;
        if (!id || !secret) return null;
        const token = await twitchAppToken(env, id, secret);
        if (!token) return null;
        return {
            url: `https://api.twitch.tv/helix/streams?user_login=${encodeURIComponent(channel)}`,
            headers: { "client-id": id, authorization: `Bearer ${token}` },
        };
    }
    if (platform === "youtube") {
        // The Data API takes a key rather than OAuth for public reads. Note
        // search.list is expensive in quota terms, which is the other reason
        // the cache above is not optional.
        const key = env.YOUTUBE_API_KEY;
        if (!key) return null;
        return {
            url: "https://www.googleapis.com/youtube/v3/search?part=snippet&type=video"
               + `&eventType=live&channelId=${encodeURIComponent(channel)}`
               + `&key=${encodeURIComponent(key)}`,
            headers: {},
        };
    }
    // Kick's public channel read needs an app token like Twitch's.
    const id = env.KICK_CLIENT_ID;
    const secret = env.KICK_CLIENT_SECRET;
    if (!id || !secret) return null;
    const token = await kickAppToken(env, id, secret);
    if (!token) return null;
    return {
        url: `https://api.kick.com/public/v1/channels?slug=${encodeURIComponent(channel)}`,
        headers: { authorization: `Bearer ${token}` },
    };
}

/** A client-credentials token, cached for most of its life. */
async function appToken(env: Env, cacheKey: string, url: string, body: string):
        Promise<string | null> {
    const key = new Request(`https://od.invalid/token/${cacheKey}`);
    const cache = caches.default;
    const hit = await cache.match(key);
    if (hit) {
        const cached = await hit.json<{ token?: string }>()
            .catch(() => ({ token: undefined }));
        if (cached.token) return cached.token;
    }
    const res = await fetch(url, {
        method: "POST",
        headers: { "content-type": "application/x-www-form-urlencoded" },
        body,
    }).catch(() => null);
    if (!res || !res.ok) return null;
    const json = await res.json<{ access_token?: string; expires_in?: number }>()
        .catch(() => ({} as { access_token?: string; expires_in?: number }));
    if (!json.access_token) return null;
    // Re-fetched well before it expires: a token that goes stale mid-request
    // costs every liveness check until the next one.
    const ttl = Math.max(60, Math.min(json.expires_in ?? 3600, 3600) - 300);
    await cache.put(key, new Response(JSON.stringify({ token: json.access_token }), {
        headers: { "cache-control": `max-age=${ttl}`, "content-type": "application/json" },
    }));
    return json.access_token;
}

function twitchAppToken(env: Env, id: string, secret: string): Promise<string | null> {
    return appToken(env, "twitch", "https://id.twitch.tv/oauth2/token",
        `client_id=${encodeURIComponent(id)}&client_secret=${encodeURIComponent(secret)}`
        + "&grant_type=client_credentials");
}

function kickAppToken(env: Env, id: string, secret: string): Promise<string | null> {
    return appToken(env, "kick", "https://id.kick.com/oauth/token",
        `client_id=${encodeURIComponent(id)}&client_secret=${encodeURIComponent(secret)}`
        + "&grant_type=client_credentials");
}

/** Is `channel` live on `platform`? Cached, and never throws. */
export async function isLive(env: Env, platform: Platform, rawChannel: string):
        Promise<Live> {
    const channel = safeChannel(rawChannel);
    if (!channel) return OFFLINE;

    const cacheKey = new Request(`https://od.invalid/live/${platform}/${channel}`);
    const cache = caches.default;
    const hit = await cache.match(cacheKey);
    if (hit) return await hit.json<Live>().catch(() => OFFLINE);

    const up = await upstreamFor(env, platform, channel);
    if (!up) return OFFLINE;

    const res = await fetch(up.url, { headers: up.headers }).catch(() => null);
    // A rate limit or a bad afternoon is "not live", not an error: a live badge
    // is decoration, and a lobby that will not open because Twitch is down is a
    // broken game.
    if (!res || !res.ok) return OFFLINE;
    const body = await res.json().catch(() => null);
    const live = readFor(platform, body);

    await cache.put(cacheKey, new Response(JSON.stringify(live), {
        headers: { "cache-control": `max-age=${CACHE_SECONDS}`,
                   "content-type": "application/json" },
    }));
    return live;
}
