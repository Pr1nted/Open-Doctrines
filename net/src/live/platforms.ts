// Is this channel live right now?
//
// ── THREE PLATFORMS, THREE DIFFERENT ANSWERS ──
//
// Twitch answers with a list that is EMPTY when the channel is offline, so
// "live" is "the list has something in it". YouTube answers a search with items
// whose snippet says liveBroadcastContent. Kick answers with a channel object
// carrying a nullable livestream. None of them has a boolean called "live", and
// two of them say "not live" by omitting something rather than by saying so.
//
// ── WHY THE READING IS SEPARATED FROM THE FETCHING ──
//
// The fetching needs app credentials and a network; the reading is where the
// mistakes are. An "is it live" check that is wrong in the FALSE direction is
// invisible (nobody is ever marked live) and wrong in the TRUE direction is
// worse (everybody is). Both are one missing field away, so the readers are
// pure and every shape is pinned in test/live.test.ts.

/** What a platform said, once it has been understood. */
export interface Live {
    live: boolean;
    /** What they are streaming, when the platform says. Never required. */
    title?: string;
    /** Viewers, when the platform says. */
    viewers?: number;
}

const OFFLINE: Live = { live: false };

/**
 * Twitch: GET https://api.twitch.tv/helix/streams?user_login=<name>
 *
 * The `data` array is EMPTY for an offline channel -- there is no "live: false"
 * to read, so anything that is not a non-empty array means offline.
 */
export function readTwitch(body: unknown): Live {
    if (!body || typeof body !== "object") return OFFLINE;
    const data = (body as Record<string, unknown>).data;
    if (!Array.isArray(data) || data.length === 0) return OFFLINE;
    const row = data[0] as Record<string, unknown>;
    // Helix only returns rows for live streams, but it also has a `type` field
    // that reads "live" -- checked, because a future row type would otherwise
    // be read as a stream.
    if (typeof row.type === "string" && row.type !== "live") return OFFLINE;
    return {
        live: true,
        title: typeof row.title === "string" ? row.title : undefined,
        viewers: typeof row.viewer_count === "number" ? row.viewer_count : undefined,
    };
}

/**
 * YouTube: GET .../search?part=snippet&channelId=<id>&eventType=live&type=video
 *
 * An offline channel answers with an empty `items`. A channel that has an
 * UPCOMING stream answers with items whose liveBroadcastContent is "upcoming",
 * which is not live and must not be read as such -- the commonest way this
 * check goes wrong in the true direction.
 */
export function readYouTube(body: unknown): Live {
    if (!body || typeof body !== "object") return OFFLINE;
    const items = (body as Record<string, unknown>).items;
    if (!Array.isArray(items) || items.length === 0) return OFFLINE;
    const snippet = (items[0] as Record<string, unknown>)?.snippet;
    if (!snippet || typeof snippet !== "object") return OFFLINE;
    const s = snippet as Record<string, unknown>;
    if (s.liveBroadcastContent !== "live") return OFFLINE;
    return { live: true, title: typeof s.title === "string" ? s.title : undefined };
}

/**
 * Kick: GET https://api.kick.com/public/v1/channels?slug=<name>
 *
 * The channel object always comes back; `stream` is null when offline, and
 * carries `is_live` when it is not. Both are checked: an object that exists but
 * says is_live false is a channel that has just ended.
 */
export function readKick(body: unknown): Live {
    if (!body || typeof body !== "object") return OFFLINE;
    const data = (body as Record<string, unknown>).data;
    const row = Array.isArray(data) && data.length > 0
        ? (data[0] as Record<string, unknown>)
        : (body as Record<string, unknown>);
    const stream = row.stream;
    if (!stream || typeof stream !== "object") return OFFLINE;
    const st = stream as Record<string, unknown>;
    if (st.is_live !== true) return OFFLINE;
    return {
        live: true,
        title: typeof row.stream_title === "string" ? row.stream_title : undefined,
        viewers: typeof st.viewer_count === "number" ? st.viewer_count : undefined,
    };
}

export type Platform = "twitch" | "youtube" | "kick";

export function readFor(platform: Platform, body: unknown): Live {
    switch (platform) {
        case "twitch":  return readTwitch(body);
        case "youtube": return readYouTube(body);
        case "kick":    return readKick(body);
    }
    return OFFLINE;
}

/**
 * A channel name as it may be used in a request.
 *
 * The name reaches us from a linked account or from a game's lobby, and goes
 * into a URL. Anything that is not a plain handle is refused rather than
 * escaped: there is no legitimate channel with a slash in it, and a refusal
 * here is one fewer place to have got the escaping right.
 */
export function safeChannel(raw: string): string | null {
    if (typeof raw !== "string") return null;
    const v = raw.trim().replace(/^#/, "");
    if (!v || v.length > 64) return null;
    // YouTube channel ids carry dashes and underscores; Twitch and Kick logins
    // are letters, digits and underscore.
    return /^[A-Za-z0-9_-]+$/.test(v) ? v : null;
}
