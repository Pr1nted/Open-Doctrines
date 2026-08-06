// The relay. One instance per game session.
//
// WHAT IT IS FOR
//
// Everyone dials OUT to it over wss://, including the host. That is the whole
// answer to "no UPnP, no port forwarding, no NAT punchthrough, works from a
// browser": there are no inbound connections to anybody's machine.
//
// WHAT IT DELIBERATELY DOES NOT DO
//
// It never parses a game payload. It does not know what a turn is, what an
// order is, or what the rules are -- those live in the C++ host, in one place,
// where they are authoritative. This object moves bytes and vouches for who
// sent them. Keeping it that dumb is also what keeps it inside the free tier:
// its CPU cost per message is a table lookup and a forward.
//
// THE ONE THING IT DOES VOUCH FOR
//
// It verifies each joiner's ticket and then forwards a STATEMENT about that
// peer to the host -- `{psid, name, badges}` -- never the ticket. So a hostile
// host finishes a session holding an opaque per-server pseudonym and a display
// name, and nothing it could replay anywhere. The ticket dies here.
//
// Because this object is not on any player's machine, "who sent this" is not a
// client-side decision, which is the part that actually stops impersonation.
//
// HIBERNATION
//
// Sockets are accepted via `ctx.acceptWebSocket`, so an idle lobby is evicted
// from memory and costs no duration. Anything that must survive that lives in
// SQLite or in the socket's own attachment -- never in an instance field.

import { DurableObject } from "cloudflare:workers";
import type { Env } from "../env.js";
import { verify, verifySessionToken } from "../auth/token.js";
import { audRelay, type TicketClaims } from "../auth/token.js";
import { psidFor } from "../auth/ticket.js";
import type { SessionDescriptorClaims } from "./session.js";
import { randomId } from "../util/crypto.js";

// --------------------------------------------------------------- envelope ----
//
// A one-byte tag plus a peer id, prepended to traffic on the HOST's socket
// only. Player sockets carry the bare game payload, because the relay already
// knows which socket a message arrived on and the player has nobody to
// disambiguate. The host has many peers, so its side needs the tag.

export const enum ToHost {
    Data = 1,        // payload came from peer
    PeerJoined = 2,  // payload is JSON PeerIdentity
    PeerLeft = 3,    // no payload
}

export const enum FromHost {
    ToPeer = 1,      // send payload to one peer
    Broadcast = 2,   // send payload to every authenticated player
    Kick = 3,        // close that peer, payload is an optional UTF-8 reason
    Ban = 4,         // kick, and refuse that psid for the rest of the session
}

// Asymmetric on purpose. A client sends orders and chat, which are small; a
// host sends turn deltas and, on join, a whole map snapshot. Anything larger
// than a client could legitimately need is abuse, and the cheapest place to
// say so is here.
const MAX_FROM_CLIENT = 256 * 1024;
const MAX_FROM_HOST = 8 * 1024 * 1024;

// Token bucket, per peer. Sized for a turn's worth of order traffic plus chat,
// with headroom for a burst on join.
const RATE_BURST = 60;
const RATE_PER_SECOND = 30;

/** A joiner has this long to send a valid HELLO before it is disconnected. */
const AUTH_GRACE_MS = 5_000;

/**
 * How long a RAPID lobby survives the host dropping.
 *
 * Short on purpose: a rapid game without its authoritative server is not a
 * game, and holding everyone in a dead lobby helps nobody.
 */
const HOST_GRACE_MS = 90_000;

/**
 * How long a LONG-FORM session persists with nobody connected.
 *
 * Ninety days, because the entire point of long-form is that the host is
 * offline between turns -- for days, over a weekend, across a holiday. Applying
 * the rapid grace period here would delete a tournament ninety seconds after
 * the host closed their laptop, which is not a failure mode anyone recovers
 * from.
 */
const LONGFORM_IDLE_MS = 90 * 24 * 60 * 60 * 1000;

const NONCE_TTL_MS = 5 * 60_000;

/** Comfortably more than a full lobby retrying at once, and a hard ceiling. */
const MAX_LIVE_NONCES = 256;

// ------------------------------------------------------- long-form storage ----
//
// Turn bundles and sealed orders, kept with the session they belong to. This is
// TurnStoreKind::DurableObject on the game side (src/net/TurnStore.h), and it
// is the default there because it needs nothing enabled that a working account
// does not already have -- R2 would do the same job but Cloudflare wants a card
// on file before it will switch R2 on.
//
// The asymmetry is the whole design, and it is enforced here:
//
//   turn bundles   PUBLIC and IMMUTABLE. Written by the host, once. Readable by
//                  anyone, which is what lets people spectate a tournament
//                  without joining it.
//   orders         Written by ONE player, for themselves. Sealed before they
//                  leave that player's machine, so this object stores
//                  ciphertext and could not read them if it wanted to.

/**
 * Cap on one stored blob.
 *
 * Turn deltas are KB-scale, so this is orders of magnitude of headroom. The
 * real ceiling is the game's own: HttpRequest::maxResponseBytes defaults to 1
 * MB, so a blob larger than that could be written and then never read back.
 * Staying well under it means that failure cannot happen.
 */
const MAX_BLOB_BYTES = 512 * 1024;

/**
 * Backstop on how many blobs one session may hold.
 *
 * Not the real bound -- writes are authenticated and scoped to this session, so
 * the only people who can fill it are the host and its players. It is here so
 * that a looping host cannot quietly eat the free plan's storage.
 */
const MAX_BLOBS = 2000;

export type PeerRole = "host" | "player" | "spectator";

/** Exactly what the host is told about a peer. Nothing here is a credential. */
export interface PeerIdentity {
    peerId: number;
    psid: string;
    name: string;
    badges: string[];
    /** Issuer of the ticket, so the host can mark non-official badges. */
    issuer: string;
    role: PeerRole;
}

interface Attachment {
    peerId: number;
    role: PeerRole;
    authed: boolean;
    psid?: string;
    name?: string;
    badges?: string[];
    issuer?: string;
    joinedAt: number;
    // Token bucket state, carried on the socket so it survives hibernation.
    tokens: number;
    tokensAt: number;
}

export interface SessionSettings {
    name: string;
    /**
     * True when turns are not on a timer and the host is expected to be away
     * between them. Changes how long the session survives without a host.
     */
    longForm?: boolean;
    /** Shown in the public directory, if the host opted in. */
    listed: boolean;
    maxPlayers: number;
    /** Free-text notice the client shows when auth is not the official issuer. */
    authNotice?: string;
    /** Whether the host displays badges at all. */
    showBadges: boolean;
    /** Mods every client must have, as `id@version#sha256`. */
    requiredMods: string[];
}

function blobFail(status: number, code: string): Response {
    // No prose. Every message a player sees for these comes from the game, in
    // TurnStore.cpp -- a second wording here would be the one nobody reads.
    return new Response(JSON.stringify({ error: code }), {
        status,
        headers: { "content-type": "application/json; charset=utf-8" },
    });
}

function turnNumber(url: URL): number | null {
    const raw = url.searchParams.get("n");
    if (!raw) return null;
    const n = Number(raw);
    return Number.isInteger(n) && n >= 0 ? n : null;
}

/**
 * Validate and normalise one blob.
 *
 * The wire format is the game's: `{"od":1,"turn":N,"data":"<base64url>"}`, from
 * `wrapBlob` in TurnStore.cpp. Three things are checked and they are not
 * ceremony:
 *
 *   - the size, before the body is read, so an oversized write costs nothing;
 *   - that the turn in the BODY matches the turn in the URL, because a client
 *     that disagrees with itself has a bug and storing it would hide it;
 *   - that `data` is base64url, so this cannot be used to park arbitrary JSON.
 *
 * Returns the re-serialised blob rather than what arrived. Echoing the caller's
 * own bytes back out is what would make this a general-purpose host for
 * whatever anyone felt like putting in it.
 */
async function readBlobBody(request: Request, turn: number): Promise<string | null> {
    const declared = Number(request.headers.get("content-length") ?? "0");
    if (declared > MAX_BLOB_BYTES) return null;

    const text = await request.text();
    if (text.length > MAX_BLOB_BYTES) return null;

    let parsed: { od?: unknown; turn?: unknown; data?: unknown };
    try { parsed = JSON.parse(text) as typeof parsed; } catch { return null; }

    if (parsed.od !== 1) return null;
    if (parsed.turn !== turn) return null;
    if (typeof parsed.data !== "string") return null;
    if (!/^[A-Za-z0-9_-]*$/.test(parsed.data)) return null;

    return JSON.stringify({ od: 1, turn, data: parsed.data });
}

export class LobbyDO extends DurableObject<Env> {
    private sql: SqlStorage;

    constructor(ctx: DurableObjectState, env: Env) {
        super(ctx, env);
        this.sql = ctx.storage.sql;
        this.ensureSchema();
    }

    /**
     * Create the tables if they are not there.
     *
     * Called from the constructor AND after every `deleteAll`, which is not
     * belt and braces: `deleteAll` drops the tables, and the instance stays
     * alive and serving afterwards. Without this, the next request to reach
     * that same instance dies on `no such table: meta` -- a 500 where a 404
     * belonged, for a session that had simply ended.
     */
    private ensureSchema(): void {
        this.sql.exec(`
            CREATE TABLE IF NOT EXISTS meta  (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS nonce (n TEXT PRIMARY KEY, at INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS jti   (j TEXT PRIMARY KEY, exp INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS ban   (psid TEXT PRIMARY KEY);
            CREATE TABLE IF NOT EXISTS blob  (k TEXT PRIMARY KEY, body TEXT NOT NULL,
                                              at INTEGER NOT NULL);
        `);
    }

    // ------------------------------------------------------------- meta ----

    private get(key: string): string | null {
        const rows = [...this.sql.exec<{ v: string }>("SELECT v FROM meta WHERE k = ?", key)];
        return rows[0]?.v ?? null;
    }

    private set(key: string, value: string): void {
        this.sql.exec("INSERT OR REPLACE INTO meta (k, v) VALUES (?, ?)", key, value);
    }

    private settings(): SessionSettings | null {
        const raw = this.get("settings");
        return raw ? (JSON.parse(raw) as SessionSettings) : null;
    }

    // ------------------------------------------------------------ routes ----

    override async fetch(request: Request): Promise<Response> {
        const url = new URL(request.url);
        switch (url.pathname) {
            case "/init": return this.handleInit(request);
            case "/info": return this.handleInfo();
            case "/ws": return this.handleUpgrade(request);
            case "/turn": return this.handleTurn(request, url);
            case "/orders": return this.handleOrders(request, url);
            default: return new Response("not found", { status: 404 });
        }
    }

    /**
     * Answer for a session that does not exist, and leave nothing behind.
     *
     * Reaching this object at all CREATED it -- the constructor's CREATE TABLEs
     * are durable -- so a probe for a code nobody ever issued would otherwise
     * leave a real Durable Object with real storage, no session, and no alarm
     * that would ever reclaim it. `isSessionCode` in the router makes a
     * MALFORMED guess free; a well-formed one still lands here, and this is
     * where the rest of that is closed.
     *
     * Unconditionally safe: a session's storage is written by /init, which the
     * Worker calls before the join code is known to anybody at all.
     */
    private async noSession(): Promise<Response> {
        await this.ctx.storage.deleteAll();
        this.ensureSchema();
        return new Response(JSON.stringify({ error: "no_session" }), {
            status: 404,
            headers: { "content-type": "application/json; charset=utf-8" },
        });
    }

    /** Called by the Worker when the host creates the session. Once only. */
    private async handleInit(request: Request): Promise<Response> {
        if (this.get("descriptor")) {
            return new Response(JSON.stringify({ error: "session_exists" }), { status: 409 });
        }
        const body = await request.json<{
            descriptor: string; settings: SessionSettings; hostPsid: string;
        }>();
        this.set("descriptor", body.descriptor);
        this.set("settings", JSON.stringify(body.settings));
        // Recorded at creation, from the account that opened the session.
        // Without this the host slot is first-come-first-served among everyone
        // who knows the join code -- which is everyone who was invited -- and
        // a player could take it before the real host connected.
        this.set("hostPsid", body.hostPsid);
        this.set("createdAt", String(Date.now()));
        this.set("nextPeerId", "1");
        return new Response(JSON.stringify({ ok: true }), {
            headers: { "content-type": "application/json" },
        });
    }

    /**
     * What a prospective joiner needs before it can ask for a ticket: the
     * signed descriptor (which names the server the pseudonym is computed
     * against) and a fresh nonce.
     *
     * The nonce is why a ticket cannot be minted ahead of time and stockpiled:
     * it has to echo a challenge this lobby produced moments ago.
     */
    private async handleInfo(): Promise<Response> {
        const descriptor = this.get("descriptor");
        const settings = this.settings();
        if (!descriptor || !settings) return this.noSession();
        this.sql.exec("DELETE FROM nonce WHERE at < ?", Date.now() - NONCE_TTL_MS);
        // /info needs no credential, so anyone with the join code can call it
        // in a loop. Age alone would not bound the table; this does.
        this.sql.exec(
            "DELETE FROM nonce WHERE n NOT IN (SELECT n FROM nonce ORDER BY at DESC LIMIT ?)",
            MAX_LIVE_NONCES,
        );
        const nonce = randomId(22);
        this.sql.exec("INSERT INTO nonce (n, at) VALUES (?, ?)", nonce, Date.now());

        const peers = this.ctx.getWebSockets().filter((ws) => this.attachment(ws)?.authed);
        return new Response(JSON.stringify({
            descriptor,
            nonce,
            name: settings.name,
            players: peers.length,
            maxPlayers: settings.maxPlayers,
            requiredMods: settings.requiredMods,
            authNotice: settings.authNotice ?? null,
            hostOnline: peers.some((ws) => this.attachment(ws)?.role === "host"),
        }), { headers: { "content-type": "application/json" } });
    }

    private async handleUpgrade(request: Request): Promise<Response> {
        if (request.headers.get("upgrade") !== "websocket") {
            return new Response("expected websocket", { status: 426 });
        }
        if (!this.get("descriptor")) return this.noSession();

        const pair = new WebSocketPair();
        const [client, server] = [pair[0], pair[1]];

        const peerId = Number(this.get("nextPeerId") ?? "1");
        this.set("nextPeerId", String(peerId + 1));

        // Role is claimed here but MEANS nothing until the HELLO is verified;
        // `authed` gates every path that matters.
        const requested = new URL(request.url).searchParams.get("role");
        const role: PeerRole = requested === "host" ? "host"
            : requested === "spectator" ? "spectator" : "player";

        this.ctx.acceptWebSocket(server);
        const now = Date.now();
        server.serializeAttachment({
            peerId, role, authed: false, joinedAt: now,
            tokens: RATE_BURST, tokensAt: now,
        } satisfies Attachment);

        // Anyone who connects and then says nothing is holding a slot for
        // free. Sweep them.
        void this.ctx.storage.setAlarm(now + AUTH_GRACE_MS);

        return new Response(null, { status: 101, webSocket: client });
    }

    // ----------------------------------------------------------- sockets ----

    private attachment(ws: WebSocket): Attachment | null {
        return (ws.deserializeAttachment() as Attachment | null) ?? null;
    }

    override async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
        const peer = this.attachment(ws);
        if (!peer) return void ws.close(1011, "no state");

        if (!this.spendToken(ws, peer)) return void ws.close(1008, "rate limit");

        if (!peer.authed) {
            // The first frame must be the HELLO, and it is the only frame that
            // is ever text. Everything after it is opaque binary that this
            // object forwards without looking inside.
            if (typeof message !== "string") return void ws.close(1008, "expected hello");
            return this.handleHello(ws, peer, message);
        }

        if (typeof message === "string") return void ws.close(1008, "binary only");
        return peer.role === "host"
            ? this.relayFromHost(ws, message)
            : this.relayFromClient(peer, message);
    }

    /**
     * Verify a join ticket and turn it into a peer identity.
     *
     * Every check that can fail here reports the same thing to the client. A
     * relay that says WHICH check failed is an oracle for forging the next
     * attempt.
     */
    private async handleHello(ws: WebSocket, peer: Attachment, raw: string): Promise<void> {
        const settings = this.settings();
        if (!settings) return void ws.close(1011, "no session");

        let hello: { ticket?: string };
        try { hello = JSON.parse(raw) as { ticket?: string }; }
        catch { return void ws.close(1008, "bad hello"); }
        if (typeof hello.ticket !== "string") return void ws.close(1008, "bad hello");

        const sessionId = this.sessionId();
        const claims = sessionId
            ? await verify<TicketClaims>(this.env, hello.ticket, audRelay(sessionId))
            : null;
        if (!claims) return void ws.close(4401, "rejected");

        // The nonce proves the ticket was minted against a challenge THIS
        // lobby issued recently, so one cannot be prepared in advance.
        const nonceRows = [...this.sql.exec("SELECT n FROM nonce WHERE n = ?", claims.nonce)];
        if (nonceRows.length === 0) return void ws.close(4401, "rejected");
        this.sql.exec("DELETE FROM nonce WHERE n = ?", claims.nonce);

        // Single use. Burning the jti is what stops a captured ticket being
        // replayed even at the session it was minted for.
        this.sql.exec("DELETE FROM jti WHERE exp < ?", Math.floor(Date.now() / 1000));
        const replay = [...this.sql.exec("SELECT j FROM jti WHERE j = ?", claims.jti)];
        if (replay.length > 0) return void ws.close(4401, "rejected");
        this.sql.exec("INSERT INTO jti (j, exp) VALUES (?, ?)", claims.jti, claims.exp);

        const banned = [...this.sql.exec("SELECT psid FROM ban WHERE psid = ?", claims.psid)];
        if (banned.length > 0) return void ws.close(4403, "banned");

        if (peer.role === "host") {
            // The host slot belongs to the account that created the session,
            // and to nobody else. Everyone invited knows the join code, so
            // "first to ask" would be no protection at all.
            if (claims.psid !== this.get("hostPsid")) return void ws.close(4403, "not the host");

            const existing = this.ctx.getWebSockets()
                .find((o) => o !== ws && this.attachment(o)?.role === "host");
            // A reconnecting host replaces the stale socket rather than being
            // refused: the common cause of a second host connection is the
            // first one having died without the close being noticed.
            if (existing) existing.close(1000, "replaced by reconnect");
        } else {
            const players = this.ctx.getWebSockets()
                .filter((o) => this.attachment(o)?.authed && this.attachment(o)?.role === "player");
            if (peer.role === "player" && players.length >= settings.maxPlayers) {
                return void ws.close(4409, "session full");
            }
        }

        const identity: PeerIdentity = {
            peerId: peer.peerId,
            psid: claims.psid,
            name: claims.name,
            badges: settings.showBadges ? claims.badges : [],
            issuer: claims.iss,
            role: peer.role,
        };

        ws.serializeAttachment({
            ...peer, authed: true,
            psid: identity.psid, name: identity.name,
            badges: identity.badges, issuer: identity.issuer,
        } satisfies Attachment);

        ws.send(JSON.stringify({ ok: true, peerId: peer.peerId, role: peer.role }));

        if (peer.role !== "host") {
            this.toHost(ToHost.PeerJoined, peer.peerId, new TextEncoder().encode(
                JSON.stringify(identity),
            ));
        }
        // Cancel the host-gone teardown: somebody is here.
        if (peer.role === "host") await this.ctx.storage.deleteAlarm();
    }

    /**
     * The claims inside our own stored descriptor.
     *
     * Read without verifying the signature, deliberately: we minted this token
     * and put it here at /init. It never came from a caller, so there is no
     * attacker in the path to check for.
     */
    private descriptorClaims(): SessionDescriptorClaims | null {
        const descriptor = this.get("descriptor");
        if (!descriptor) return null;
        try {
            const parts = descriptor.split(".");
            return JSON.parse(atob(
                parts[1]!.replace(/-/g, "+").replace(/_/g, "/"),
            )) as SessionDescriptorClaims;
        } catch {
            return null;
        }
    }

    private sessionId(): string | null {
        return this.descriptorClaims()?.sid ?? null;
    }

    /**
     * Who is calling, as a pseudonym for THIS server.
     *
     * The credential is a session token (`od-api`), which names an account --
     * but everything this object holds is keyed by psid, and it has no business
     * learning an account id. Converting once, here, is what keeps that true.
     *
     * Null for a missing, malformed, expired or wrong-audience token alike. A
     * JOIN TICKET lands in that bucket too: its audience is `od-relay:<sid>`,
     * so it cannot be used to write a turn or somebody else's orders.
     */
    private async callerPsid(request: Request): Promise<string | null> {
        const header = request.headers.get("authorization");
        if (!header) return null;
        const [scheme, ...rest] = header.split(" ");
        if (!scheme || scheme.toLowerCase() !== "bearer") return null;
        const token = rest.join(" ").trim();
        if (!token) return null;

        const claims = await verifySessionToken(this.env, token);
        const srv = this.descriptorClaims()?.srv;
        if (!claims || !srv) return null;
        return psidFor(this.env, claims.sub, srv);
    }

    // -------------------------------------------------------- long form ----

    /**
     * Publish or read a turn bundle.
     *
     * IMMUTABLE once written. "They cannot be edited by anyone but you" is what
     * TurnStore.cpp promises a host, and "making it un-editable is what stops
     * anyone rewriting history" is what TurnStore.h promises everyone else --
     * the second is the stronger claim, so a turn is refused even to the host
     * that wrote it.
     */
    private async handleTurn(request: Request, url: URL): Promise<Response> {
        if (!this.get("descriptor")) return this.noSession();
        const turn = turnNumber(url);
        if (turn === null) return blobFail(400, "bad_turn");
        const key = `turn:${turn}`;

        if (request.method === "GET") return this.blobRead(key, true);
        if (request.method !== "PUT") return blobFail(405, "bad_method");

        const caller = await this.callerPsid(request);
        if (!caller) return blobFail(401, "unauthorized");
        // The host slot belongs to the account that opened the session, which
        // is the same check the WebSocket handshake makes. Everyone invited
        // knows the join code, so "whoever asks" would be no protection.
        if (caller !== this.get("hostPsid")) return blobFail(403, "not_the_host");

        const body = await readBlobBody(request, turn);
        if (body === null) return blobFail(400, "bad_blob");

        if ([...this.sql.exec("SELECT k FROM blob WHERE k = ?", key)].length > 0) {
            return blobFail(409, "already_published");
        }
        return this.blobWrite(key, body);
    }

    /**
     * Submit or read one player's sealed orders.
     *
     * A player writes their OWN and nobody else's. This is the one guarantee a
     * public bucket cannot give: on jsonblob anyone holding a URL can overwrite
     * it, and the design accepts that because the seal turns tampering into a
     * lost turn rather than a forged one. Here we can simply refuse, so we do.
     */
    private async handleOrders(request: Request, url: URL): Promise<Response> {
        if (!this.get("descriptor")) return this.noSession();
        const turn = turnNumber(url);
        const psid = url.searchParams.get("psid");
        if (turn === null || !psid) return blobFail(400, "bad_request");
        const key = `orders:${turn}:${psid}`;

        if (request.method === "GET") return this.blobRead(key, false);
        if (request.method !== "PUT") return blobFail(405, "bad_method");

        const caller = await this.callerPsid(request);
        if (!caller) return blobFail(401, "unauthorized");
        if (caller !== psid) return blobFail(403, "not_your_orders");

        const body = await readBlobBody(request, turn);
        if (body === null) return blobFail(400, "bad_blob");

        // Overwritable, unlike a turn. Changing your orders before the turn
        // resolves is ordinary play, and the check above means only the player
        // whose orders these are can do it.
        return this.blobWrite(key, body);
    }

    private blobRead(key: string, immutable: boolean): Response {
        const rows = [...this.sql.exec<{ body: string }>(
            "SELECT body FROM blob WHERE k = ?", key,
        )];
        const body = rows[0]?.body;
        if (body === undefined) return blobFail(404, "no_blob");

        return new Response(body, {
            headers: {
                "content-type": "application/json; charset=utf-8",
                // A published turn never changes, so the edge can hold it and
                // spectators re-reading one cost no DO request at all. Orders
                // can still be revised, so they are not cacheable.
                "cache-control": immutable
                    ? "public, max-age=31536000, immutable"
                    : "no-store",
            },
        });
    }

    private blobWrite(key: string, body: string): Response {
        const counted = [...this.sql.exec<{ n: number }>("SELECT COUNT(*) AS n FROM blob")];
        const held = counted[0]?.n ?? 0;
        const replacing = [...this.sql.exec("SELECT k FROM blob WHERE k = ?", key)].length > 0;
        if (!replacing && held >= MAX_BLOBS) return blobFail(507, "session_full");

        this.sql.exec(
            "INSERT OR REPLACE INTO blob (k, body, at) VALUES (?, ?, ?)",
            key, body, Date.now(),
        );
        return new Response(JSON.stringify({ ok: true }), {
            headers: { "content-type": "application/json; charset=utf-8" },
        });
    }

    private relayFromClient(peer: Attachment, message: ArrayBuffer): void {
        if (message.byteLength > MAX_FROM_CLIENT) return;
        this.toHost(ToHost.Data, peer.peerId, new Uint8Array(message));
    }

    /**
     * The host is the only peer allowed to address others. Players cannot
     * reach each other through the relay at all -- there is no peer-to-peer
     * path, so there is no message a client can craft that another client will
     * see without the authoritative server having produced it.
     */
    private relayFromHost(ws: WebSocket, message: ArrayBuffer): void {
        if (message.byteLength > MAX_FROM_HOST) return;
        const view = new Uint8Array(message);
        if (view.length < 3) return;

        const kind = view[0]!;
        const target = view[1]! | (view[2]! << 8);
        const payload = view.subarray(3);

        switch (kind) {
            case FromHost.Broadcast:
                for (const other of this.ctx.getWebSockets()) {
                    const peer = this.attachment(other);
                    if (peer?.authed && peer.role !== "host") this.trySend(other, payload);
                }
                return;
            case FromHost.ToPeer: {
                const socket = this.socketFor(target);
                if (socket) this.trySend(socket, payload);
                return;
            }
            case FromHost.Kick:
            case FromHost.Ban: {
                const socket = this.socketFor(target);
                if (!socket) return;
                const peer = this.attachment(socket);
                if (kind === FromHost.Ban && peer?.psid) {
                    this.sql.exec("INSERT OR REPLACE INTO ban (psid) VALUES (?)", peer.psid);
                }
                socket.close(4403, new TextDecoder().decode(payload).slice(0, 100) || "removed");
                return;
            }
            default:
                // An unknown tag from the host is a version mismatch, not an
                // attack. Dropping it silently keeps an older host usable with
                // a newer relay.
                return;
        }
    }

    private socketFor(peerId: number): WebSocket | null {
        for (const ws of this.ctx.getWebSockets()) {
            const peer = this.attachment(ws);
            if (peer?.authed && peer.peerId === peerId) return ws;
        }
        return null;
    }

    private toHost(kind: ToHost, peerId: number, payload: Uint8Array): void {
        const host = this.ctx.getWebSockets()
            .find((ws) => this.attachment(ws)?.role === "host" && this.attachment(ws)?.authed);
        if (!host) return;
        const framed = new Uint8Array(3 + payload.length);
        framed[0] = kind;
        framed[1] = peerId & 0xff;
        framed[2] = (peerId >> 8) & 0xff;
        framed.set(payload, 3);
        this.trySend(host, framed);
    }

    private trySend(ws: WebSocket, data: Uint8Array): void {
        // A socket can die between the lookup and the send; that is normal
        // churn, not an error worth tearing anything down for.
        try { ws.send(data); } catch { /* peer went away */ }
    }

    private spendToken(ws: WebSocket, peer: Attachment): boolean {
        const now = Date.now();
        const refilled = Math.min(
            RATE_BURST, peer.tokens + ((now - peer.tokensAt) / 1000) * RATE_PER_SECOND,
        );
        if (refilled < 1) return false;
        ws.serializeAttachment({ ...peer, tokens: refilled - 1, tokensAt: now } satisfies Attachment);
        return true;
    }

    // ---------------------------------------------------------- lifecycle ----

    override async webSocketClose(ws: WebSocket): Promise<void> {
        const peer = this.attachment(ws);
        if (!peer) return;

        if (peer.authed && peer.role !== "host") {
            this.toHost(ToHost.PeerLeft, peer.peerId, new Uint8Array(0));
            return;
        }
        if (peer.role === "host") {
            // Do not tear down immediately: a host that dropped off wifi should
            // come back to a lobby that still has everyone in it.
            //
            // For a long-form game the host is SUPPOSED to be gone -- that is
            // what long-form means -- so the deadline is months, not seconds.
            const longForm = this.settings()?.longForm === true;
            await this.ctx.storage.setAlarm(
                Date.now() + (longForm ? LONGFORM_IDLE_MS : HOST_GRACE_MS));
        }
    }

    override async webSocketError(ws: WebSocket): Promise<void> {
        await this.webSocketClose(ws);
    }

    override async alarm(): Promise<void> {
        const sockets = this.ctx.getWebSockets();

        // Sweep anyone who connected and never authenticated.
        const cutoff = Date.now() - AUTH_GRACE_MS;
        for (const ws of sockets) {
            const peer = this.attachment(ws);
            if (peer && !peer.authed && peer.joinedAt < cutoff) ws.close(4401, "no hello");
        }

        const hostHere = sockets.some((ws) => {
            const peer = this.attachment(ws);
            return peer?.authed && peer.role === "host";
        });
        if (hostHere) return;

        // No host. Disconnect whoever is still waiting either way -- without an
        // authoritative server there is nothing to be connected TO.
        for (const ws of this.ctx.getWebSockets()) ws.close(4404, "host left");

        const longForm = this.settings()?.longForm === true;
        if (!longForm) {
            // Rapid: the game is over, so the session goes with it.
            await this.ctx.storage.deleteAll();
            // This instance keeps serving after the wipe, and `deleteAll` took
            // the tables with it. A straggler arriving now must get a clean
            // "no session", not a SQL error.
            this.ensureSchema();
            return;
        }

        // Long-form: KEEP EVERYTHING. The host coming back next week to process
        // a turn is the design, not an anomaly. Re-arm so a truly abandoned
        // session is still reclaimed eventually.
        await this.ctx.storage.setAlarm(Date.now() + LONGFORM_IDLE_MS);
    }
}
