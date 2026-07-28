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
import { verify } from "../auth/token.js";
import { audRelay, type TicketClaims } from "../auth/token.js";
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

export class LobbyDO extends DurableObject<Env> {
    private sql: SqlStorage;

    constructor(ctx: DurableObjectState, env: Env) {
        super(ctx, env);
        this.sql = ctx.storage.sql;
        this.sql.exec(`
            CREATE TABLE IF NOT EXISTS meta  (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS nonce (n TEXT PRIMARY KEY, at INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS jti   (j TEXT PRIMARY KEY, exp INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS ban   (psid TEXT PRIMARY KEY);
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
            default: return new Response("not found", { status: 404 });
        }
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
    private handleInfo(): Response {
        const descriptor = this.get("descriptor");
        const settings = this.settings();
        if (!descriptor || !settings) {
            return new Response(JSON.stringify({ error: "no_session" }), { status: 404 });
        }
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

    private handleUpgrade(request: Request): Response {
        if (request.headers.get("upgrade") !== "websocket") {
            return new Response("expected websocket", { status: 426 });
        }
        if (!this.get("descriptor")) return new Response("no session", { status: 404 });

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

    private sessionId(): string | null {
        const descriptor = this.get("descriptor");
        if (!descriptor) return null;
        try {
            const parts = descriptor.split(".");
            const payload = JSON.parse(atob(
                parts[1]!.replace(/-/g, "+").replace(/_/g, "/"),
            )) as SessionDescriptorClaims;
            return payload.sid;
        } catch {
            return null;
        }
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
            return;
        }

        // Long-form: KEEP EVERYTHING. The host coming back next week to process
        // a turn is the design, not an anomaly. Re-arm so a truly abandoned
        // session is still reclaimed eventually.
        await this.ctx.storage.setAlarm(Date.now() + LONGFORM_IDLE_MS);
    }
}
