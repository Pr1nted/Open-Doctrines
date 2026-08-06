// The Worker: every HTTP route, and the only place that knows the URL shapes.
//
// Two rules hold throughout and are worth reading before the code:
//
//   NOTHING WRITES ON A READ PATH. Logging in, polling, fetching an account or
//   minting a join ticket all cost zero KV writes. The free plan allows 1,000
//   writes a day and they are reserved for things that genuinely change: a new
//   account, a nickname, a link, a badge.
//
//   NOTHING LOGS AN IDENTIFIER. No console.log of a token, a subject, a psid
//   or an IP anywhere in this project, and observability is off in
//   wrangler.toml. That is the retention promise in PRIVACY.md, and it is a
//   code rule rather than a habit -- test/privacy.test.ts checks it.

import type { Env } from "./env.js";
import {
    authenticate, fail, json, preflight, rateLimit, readJson, text, withCors,
} from "./http.js";
import {
    clientCredentials, isProviderId, PROVIDERS, type ProviderId,
} from "./auth/providers.js";
import {
    issueAuthRequest, issueSignupTicket, readResult, requestIdFor, storeResult,
    verifyAuthRequest, verifySignupTicket, type AuthPurpose, type AuthResult,
} from "./auth/device.js";
import {
    authorizeUrl, exchangeCode, fetchIdentity, type ResolvedIdentity,
} from "./auth/oauth.js";
import { issueSessionToken } from "./auth/token.js";
import { issueJoinTicket, psidFor } from "./auth/ticket.js";
import {
    accountForIdentity, asSubjectHash, changeNickname, createAccount, getAccount,
    banInForce, identSubHash, identityIsBanned, linkIdentity, publicAccount, setBanned,
    unlinkIdentity,
} from "./accounts/store.js";
import { checkAccountAge, tooNewMessage } from "./accounts/policy.js";
import { checkNickname } from "./accounts/nickname.js";
import { isAdmin, setBadge } from "./accounts/badges.js";
import {
    confirmDeletion, describeDeletion, exportAccount, issueDeleteConfirmation,
} from "./accounts/rights.js";
import {
    isSessionCode, issueServerCredential, issueSessionDescriptor, newSessionCode,
    verifyServerCredential, verifySessionDescriptor,
} from "./lobby/session.js";
import type { SessionSettings } from "./lobby/LobbyDO.js";
import { randomId } from "./util/crypto.js";
import PRIVACY_POLICY from "../PRIVACY.md";
import TERMS_OF_USE from "../TERMS.md";

export { LobbyDO } from "./lobby/LobbyDO.js";

/** Recommended poll interval, seconds. Mirrors RFC 8628's `interval`. */
const POLL_INTERVAL = 2;

export default {
    async fetch(request: Request, env: Env): Promise<Response> {
        if (request.method === "OPTIONS") return preflight();
        const url = new URL(request.url);
        const path = url.pathname.replace(/\/+$/, "") || "/";

        try {
            return await route(request, env, url, path);
        } catch {
            // Never echo the exception. A stack trace from a token parser is a
            // description of how to get past it.
            return fail(500, "internal", "Something went wrong.");
        }
    },
};

async function route(request: Request, env: Env, url: URL, path: string): Promise<Response> {
    const post = request.method === "POST";
    const get = request.method === "GET";

    // Before anything reads a binding. Every endpoint below is reachable
    // without a credential or is cheap to attempt without one, and until this
    // existed there was nothing anywhere -- in this file, in wrangler.toml or
    // at the edge -- that bounded how often any of them could be called.
    //
    // The relay path gets the tighter of the two buckets because it is the one
    // that can instantiate a Durable Object.
    const refusal = await rateLimit(
        path.startsWith("/session/") ? env.RATE_LIMIT_SESSION : env.RATE_LIMIT_API,
        request, env, 10,
    );
    if (refusal) return refusal;

    if (get && path === "/") {
        return json({
            service: "opendoctrines-net",
            issuer: env.ISSUER,
            // Only the ones that will actually work. A client that offers a
            // provider with no credentials sends the player to a dead end.
            providers: Object.values(PROVIDERS)
                .filter((p) => clientCredentials(env as unknown as Record<string, unknown>, p.id))
                // `canCreate` false means the provider can only be added to an
                // account that already exists, so a client can say so up front
                // rather than after a round trip through a consent screen.
                .map((p) => ({ id: p.id, label: p.label, canCreate: p.canCreateAccount })),
            privacy: `${env.ISSUER}/privacy`,
            terms: `${env.ISSUER}/terms`,
            keys: `${env.ISSUER}/.well-known/od-keys.json`,
        });
    }

    if (get && path === "/privacy") {
        return text(PRIVACY_POLICY, 200, "text/markdown; charset=utf-8");
    }

    // Served from the same repository file the client links to, so the text a
    // player agreed to and the text in the tree cannot drift apart.
    if (get && path === "/terms") {
        return text(TERMS_OF_USE, 200, "text/markdown; charset=utf-8");
    }

    // Every game server verifies tickets against this, offline and cached, so
    // a busy server never calls home and badges stay unforgeable without us
    // being in the join path.
    if (get && path === "/.well-known/od-keys.json") {
        return json(
            { keys: [JSON.parse(env.ED25519_PUBLIC_JWK)], issuer: env.ISSUER },
            200,
            { "cache-control": "public, max-age=3600" },
        );
    }

    // ------------------------------------------------------------- auth ----

    if (post && path === "/auth/device") return authDevice(request, env);
    if (get && path === "/auth/verify") return authVerify(request, env, url);
    if (get && path.startsWith("/auth/callback/")) {
        return authCallback(request, env, url, path.slice("/auth/callback/".length));
    }
    if (post && path === "/auth/poll") return authPoll(request, env);
    if (post && path === "/auth/refresh") return authRefresh(request, env);

    // ---------------------------------------------------------- account ----

    if (get && path === "/nickname/check") return nicknameCheck(env, url);
    if (post && path === "/account/create") return accountCreate(request, env);
    if (get && path === "/account/me") return accountMe(request, env);
    if (post && path === "/account/nickname") return accountNickname(request, env);
    if (post && path === "/account/unlink") return accountUnlink(request, env);
    if (get && path === "/account/export") return accountExport(request, env);
    if (post && path === "/account/delete") return accountDelete(request, env);

    if (post && path === "/admin/badge") return adminBadge(request, env);
    if (post && path === "/admin/ban") return adminBan(request, env);

    // ----------------------------------------------------------- lobby ----

    if (post && path === "/server/register") return serverRegister(request, env);
    if (post && path === "/session") return sessionCreate(request, env);
    if (post && path === "/ticket") return ticketMint(request, env);

    // `isSessionCode` rather than a shape spelled out here: a code we could not
    // have issued must be refused BEFORE `idFromName`, because reaching the
    // binding is what creates the Durable Object. See lobby/session.ts.
    const sessionMatch = /^\/session\/([^/]+)(\/ws)?$/.exec(path);
    if (sessionMatch && isSessionCode(sessionMatch[1]!)) {
        const code = sessionMatch[1]!;
        const stub = env.LOBBY.get(env.LOBBY.idFromName(code));
        if (sessionMatch[2]) return stub.fetch(new Request("https://lobby/ws", request));
        if (get) return withCors(await stub.fetch(new Request("https://lobby/info")));
    }

    // ------------------------------------------------------- long form ----
    //
    // Turn storage for TurnStoreKind::DurableObject (src/net/TurnStore.cpp).
    // The URL shapes are the game's, not ours to choose.
    //
    // READS ARE UNAUTHENTICATED, and that is the design rather than an
    // oversight. A published turn is public so that people can spectate a
    // tournament without joining it. Orders are sealed before they leave the
    // player's machine (src/net/TurnSeal.h), so a reader without the key holds
    // ciphertext -- confidentiality never comes from the store. What the store
    // must enforce is WRITES, and LobbyDO does.
    //
    // One honest difference from the jsonblob backend: there a blob sits at an
    // unguessable URL, whereas these are derivable from the join code. Nobody
    // gains readable orders by that, but an observer holding the code can tell
    // WHETHER a given player has submitted for a turn. In a game where the host
    // announces who is still to move, that is not a secret.
    const turnMatch = /^\/session\/([^/]+)\/turn\/(\d{1,9})$/.exec(path);
    if (turnMatch && isSessionCode(turnMatch[1]!)) {
        const stub = env.LOBBY.get(env.LOBBY.idFromName(turnMatch[1]!));
        return withCors(await stub.fetch(
            new Request(`https://lobby/turn?n=${turnMatch[2]}`, request),
        ));
    }

    // The psid is 22 base64url characters (see psidFor), so it never contains a
    // separator -- but it is matched rather than trusted, because it arrives in
    // a path and is about to become part of a storage key.
    const ordersMatch =
        /^\/session\/([^/]+)\/orders\/(\d{1,9})\/([A-Za-z0-9_-]{1,64})$/.exec(path);
    if (ordersMatch && isSessionCode(ordersMatch[1]!)) {
        const stub = env.LOBBY.get(env.LOBBY.idFromName(ordersMatch[1]!));
        return withCors(await stub.fetch(new Request(
            `https://lobby/orders?n=${ordersMatch[2]}&psid=${encodeURIComponent(ordersMatch[3]!)}`,
            request,
        )));
    }

    return fail(404, "not_found", "No such endpoint.");
}

// ================================================================= auth ====

interface DeviceBody { provider?: string; purpose?: AuthPurpose }

/**
 * Begin a login.
 *
 * Both halves of the secret are generated HERE, never accepted from the
 * client. If a caller could name its own request id, it could name someone
 * else's, and the victim's game would collect a session belonging to the
 * attacker. Since we only ever sign request ids we made, that path is closed.
 */
async function authDevice(request: Request, env: Env): Promise<Response> {
    const body = await readJson<DeviceBody>(request);
    if (!body || !body.provider || !isProviderId(body.provider)) {
        return fail(400, "bad_provider", "Choose Google, Discord or GitHub.");
    }
    // Refuse here rather than at the consent screen. Without this, a provider
    // with no credentials still gets a device code and a verify URL, so the
    // game says "finish in your browser" and the browser says "not set up" --
    // the failure surfaces two steps away from its cause.
    if (!clientCredentials(env as unknown as Record<string, unknown>, body.provider)) {
        return fail(400, "provider_unavailable",
                    `${PROVIDERS[body.provider].label} sign-in is not set up on this server.`);
    }

    const purpose: AuthPurpose = body.purpose === "link" ? "link" : "login";

    let link: string | undefined;
    if (purpose === "link") {
        const account = await authenticate(request, env);
        if (!account) return fail(401, "unauthorized", "Sign in first to link another account.");
        link = account.id;
    }

    const pollSecret = randomId(43);
    const rid = await requestIdFor(pollSecret);
    const authRequest = await issueAuthRequest(env, rid, body.provider, purpose, link);

    return json({
        authRequest,
        pollSecret,
        verifyUrl: `${env.ISSUER}/auth/verify?r=${encodeURIComponent(authRequest)}`,
        interval: POLL_INTERVAL,
        expiresIn: 600,
    });
}

/** The URL the player's browser opens. Redirects straight to the provider. */
async function authVerify(_request: Request, env: Env, url: URL): Promise<Response> {
    const token = url.searchParams.get("r");
    const claims = token ? await verifyAuthRequest(env, token) : null;
    if (!claims) {
        return htmlPage("Link expired", "This sign-in link has expired. Start again from the game.");
    }
    const target = await authorizeUrl(env, claims, token!);
    if (!target) {
        return htmlPage("Not configured", `${PROVIDERS[claims.provider].label} sign-in is not set up on this server.`);
    }
    return Response.redirect(target, 302);
}

async function authCallback(
    _request: Request, env: Env, url: URL, providerParam: string,
): Promise<Response> {
    if (!isProviderId(providerParam)) return htmlPage("Unknown provider", "That provider is not supported.");
    const provider: ProviderId = providerParam;

    const state = url.searchParams.get("state");
    const code = url.searchParams.get("code");
    const claims = state ? await verifyAuthRequest(env, state) : null;
    if (!claims || !code || claims.provider !== provider) {
        return htmlPage("Sign-in failed", "That sign-in could not be completed. Start again from the game.");
    }

    const accessToken = await exchangeCode(env, provider, code, claims.rid);
    const identity = accessToken ? await fetchIdentity(provider, accessToken) : null;
    if (!identity) {
        await storeResult(env, claims.rid, {
            kind: "error", code: "provider_failed",
            message: `${PROVIDERS[provider].label} did not complete the sign-in.`,
        });
        return htmlPage("Sign-in failed", "That provider did not complete the sign-in. Try again from the game.");
    }

    const result = claims.purpose === "link"
        ? await completeLink(env, claims.link, provider, identity.sub)
        : await completeLogin(env, provider, identity);

    await storeResult(env, claims.rid, result);

    // The account name is shown deliberately. The device flow's residual risk
    // is that someone who could read the verification URL signs in as
    // THEMSELVES, leaving the player's game holding a session for a stranger's
    // account. Naming the account here, and again in the game, is what turns
    // that from silent into obvious.
    if (result.kind === "error") return htmlPage("Sign-in failed", result.message);
    const who = result.kind === "signup"
        ? "Now choose a nickname in the game."
        : `Signed in as ${describeAccount(result)}. You can close this tab and return to OpenDoctrines.`;
    return htmlPage("Signed in", who);
}

function describeAccount(result: AuthResult): string {
    if (result.kind === "session" || result.kind === "linked") {
        return String((result.account as { nickname?: string }).nickname ?? "your account");
    }
    return "your account";
}

async function completeLogin(
    env: Env, provider: ProviderId, identity: ResolvedIdentity,
): Promise<AuthResult> {
    // Hashed here, once, and the raw subject is not carried past this line.
    const subHash = await identSubHash(env, provider, identity.sub);
    const account = await accountForIdentity(env, provider, subHash);
    if (account) {
        return {
            kind: "session",
            token: await issueSessionToken(env, account.id),
            account: publicAccount(account),
        };
    }
    // From here on this is a NEW account, so the anti-alt checks apply. They
    // run before the signup ticket is issued rather than at /account/create,
    // so a refusal happens at the browser step where the player can read it.

    // A link-only provider is one that cannot be age-gated. Letting it create
    // accounts would make the gate pointless, since anyone refused elsewhere
    // would simply come back through this door.
    if (!PROVIDERS[provider].canCreateAccount) {
        const gateable = Object.values(PROVIDERS)
            .filter((p) => p.canCreateAccount).map((p) => p.label).join(" or ");
        return {
            kind: "error", code: "link_only",
            message: `${PROVIDERS[provider].label} cannot be used to create a new ` +
                     `OpenDoctrines account. Sign up with ${gateable} first, then ` +
                     `add ${PROVIDERS[provider].label} from your account screen.`,
        };
    }

    if (await identityIsBanned(env, provider, subHash)) {
        return {
            kind: "error", code: "banned",
            message: "That account cannot be used to create a new OpenDoctrines account.",
        };
    }

    const age = checkAccountAge(identity.createdAt);
    if (!age.ok) {
        return {
            kind: "error", code: "too_new",
            message: tooNewMessage(PROVIDERS[provider].label, age.daysRemaining),
        };
    }

    // Do NOT create the account here: the player has not chosen a nickname,
    // and writing a placeholder would spend three of the day's writes on
    // something they may abandon at the next screen.
    const suggested = identity.suggestedName;
    return {
        kind: "signup",
        ticket: await issueSignupTicket(env, provider, subHash, suggested),
        ...(suggested ? { suggested } : {}),
    };
}

async function completeLink(
    env: Env, accountId: string | undefined, provider: ProviderId, sub: string,
): Promise<AuthResult> {
    const account = accountId ? await getAccount(env, accountId) : null;
    if (!account) return { kind: "error", code: "no_account", message: "That account no longer exists." };

    const linked = await linkIdentity(env, account, provider, await identSubHash(env, provider, sub));
    if (!linked.ok) {
        return linked.reason === "already_linked_here"
            ? { kind: "linked", provider, account: publicAccount(account) }
            : {
                kind: "error", code: "linked_elsewhere",
                message: `That ${PROVIDERS[provider].label} account is already linked to a different OpenDoctrines account.`,
            };
    }
    return { kind: "linked", provider, account: publicAccount(linked.account) };
}

async function authPoll(request: Request, env: Env): Promise<Response> {
    const body = await readJson<{ pollSecret?: string }>(request);
    if (!body?.pollSecret) return fail(400, "bad_request", "Missing pollSecret.");

    const result = await readResult(env, await requestIdFor(body.pollSecret));
    if (!result) return json({ status: "pending", interval: POLL_INTERVAL });
    if (result.kind === "error") return json({ status: "error", ...result });
    return json({ status: "ready", ...result });
}

/**
 * Trade a live session token for a fresh one.
 *
 * A sliding window that costs no storage: we re-sign rather than look anything
 * up. It exists so the token's lifetime can stay short without making players
 * re-authorise through a browser every day -- and so a token that stops being
 * used simply dies.
 */
async function authRefresh(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in again.");
    return json({
        token: await issueSessionToken(env, account.id),
        account: publicAccount(account),
    });
}

// ============================================================== account ====

/** Unauthenticated on purpose: it lets the client tell a player why a name is
 *  refused as they type, without a round trip through signup. */
async function nicknameCheck(env: Env, url: URL): Promise<Response> {
    const candidate = url.searchParams.get("n") ?? "";
    if (candidate.length > 200) return fail(400, "too_long", "That is not a nickname.");
    const check = await checkNickname(env, candidate);
    return json({
        ok: check.ok,
        canonical: check.canonical,
        ...(check.ok ? {} : { reason: check.reason, message: check.message }),
    });
}

async function accountCreate(request: Request, env: Env): Promise<Response> {
    const body = await readJson<{ signupTicket?: string; nickname?: string }>(request);
    if (!body?.signupTicket || !body.nickname) {
        return fail(400, "bad_request", "Missing signup ticket or nickname.");
    }
    const ticket = await verifySignupTicket(env, body.signupTicket);
    if (!ticket) return fail(401, "expired", "That signup expired. Sign in again.");

    const check = await checkNickname(env, body.nickname);
    if (!check.ok) return fail(400, check.reason!, check.message!);

    // The signup ticket carries the hashed subject, so this path never needs
    // the raw provider id again -- it was hashed at the callback and dropped.
    const created = await createAccount(
        env, ticket.provider, asSubjectHash(ticket.subHash), check.canonical,
    );
    if (!created.account) {
        if (created.nickError === "banned") {
            return fail(403, "banned",
                        "That account cannot be used to create a new OpenDoctrines account.");
        }
        return created.nickError === "tombstoned"
            ? fail(409, "tombstoned", "That nickname belonged to an account that was deleted recently. Try another.")
            : fail(409, "taken", "That nickname is taken.");
    }
    return json({
        token: await issueSessionToken(env, created.account.id),
        account: publicAccount(created.account),
    });
}

async function accountMe(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in again.");
    return json({ account: publicAccount(account) });
}

async function accountNickname(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in again.");

    const body = await readJson<{ nickname?: string }>(request);
    if (!body?.nickname) return fail(400, "bad_request", "Missing nickname.");

    const check = await checkNickname(env, body.nickname);
    if (!check.ok) return fail(400, check.reason!, check.message!);

    const result = await changeNickname(env, account, check.canonical);
    if (!result.ok) {
        if (result.reason === "cooldown") {
            const days = Math.ceil((result.retryAfter ?? 0) / 86400);
            return fail(429, "cooldown", `You can change your nickname again in ${days} day${days === 1 ? "" : "s"}.`);
        }
        return result.reason === "tombstoned"
            ? fail(409, "tombstoned", "That nickname belonged to a recently deleted account. Try another.")
            : fail(409, "taken", "That nickname is taken.");
    }
    return json({ account: publicAccount(result.account) });
}

async function accountUnlink(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in again.");

    const body = await readJson<{ provider?: string }>(request);
    if (!body?.provider || !isProviderId(body.provider)) {
        return fail(400, "bad_provider", "Unknown provider.");
    }
    const result = await unlinkIdentity(env, account, body.provider);
    if (!result.ok) {
        return result.reason === "last_identity"
            ? fail(409, "last_identity", "That is your only way to sign in. Link another first, or delete the account.")
            : fail(404, "not_linked", "That provider is not linked.");
    }
    return json({ account: publicAccount(result.account) });
}

async function accountExport(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in again.");
    return json(exportAccount(env, account), 200, {
        "content-disposition": 'attachment; filename="opendoctrines-account.json"',
    });
}

async function accountDelete(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in again.");

    const body = await readJson<{ confirm?: string }>(request);
    if (!body?.confirm) {
        // Step one: say exactly what goes, what lingers and why, and what we
        // cannot reach. Then hand back the token that spends it.
        return json({
            status: "confirm",
            confirmation: await issueDeleteConfirmation(env, account),
            ...describeDeletion(account),
        });
    }
    const result = await confirmDeletion(env, account, body.confirm);
    if (!result.ok) return fail(400, "bad_confirmation", "That confirmation expired. Start again.");
    return json({ status: "deleted" });
}

async function adminBadge(request: Request, env: Env): Promise<Response> {
    if (!isAdmin(request, env)) return fail(404, "not_found", "No such endpoint.");

    const body = await readJson<{ accountId?: string; badge?: string; on?: boolean }>(request);
    if (!body?.accountId || !body.badge) return fail(400, "bad_request", "Missing accountId or badge.");

    const result = await setBadge(env, body.accountId, body.badge, body.on !== false);
    if (!result.ok) {
        return result.reason === "bad_badge"
            ? fail(400, "bad_badge", "Unknown badge.")
            : fail(404, "no_account", "No such account.");
    }
    return json({ account: publicAccount(result.account) });
}

async function adminBan(request: Request, env: Env): Promise<Response> {
    if (!isAdmin(request, env)) return fail(404, "not_found", "No such endpoint.");

    const body = await readJson<{
        accountId?: string; on?: boolean; reason?: string; days?: number;
    }>(request);
    if (!body?.accountId) return fail(400, "bad_request", "Missing accountId.");

    const account = await getAccount(env, body.accountId);
    if (!account) return fail(404, "no_account", "No such account.");

    const on = body.on !== false;
    // No `days` means permanent, which is deliberate rather than a default:
    // a ban with no end has to be typed as such.
    const updated = await setBanned(
        env, account, on,
        String(body.reason ?? "This account cannot join games.").slice(0, 200),
        typeof body.days === "number" ? body.days : undefined,
    );
    return json({ account: publicAccount(updated) });
}

// ================================================================ lobby ====

async function serverRegister(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in to register a server.");
    return json({
        // Store this in the server config and keep it. It is what makes the
        // per-player pseudonyms on this server stable -- register again and
        // every returning player looks like a stranger.
        serverCredential: await issueServerCredential(env, account.id),
    });
}

interface SessionBody {
    serverCredential?: string;
    settings?: Partial<SessionSettings>;
}

async function sessionCreate(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in to host.");

    const body = await readJson<SessionBody>(request, 32 * 1024);
    if (!body?.serverCredential) return fail(400, "bad_request", "Missing server credential.");

    const credential = await verifyServerCredential(env, body.serverCredential);
    if (!credential) return fail(401, "bad_credential", "That server credential is not valid.");
    // The credential proves which server this is; the session token proves who
    // is opening it. Requiring both stops a leaked credential from being used
    // by someone else to impersonate a known server.
    if (credential.owner !== account.id) {
        return fail(403, "not_owner", "That server credential belongs to another account.");
    }

    const settings: SessionSettings = {
        name: String(body.settings?.name ?? "OpenDoctrines game").slice(0, 60),
        listed: body.settings?.listed === true,
        maxPlayers: Math.min(Math.max(Number(body.settings?.maxPlayers ?? 8), 1), 64),
        showBadges: body.settings?.showBadges !== false,
        longForm: body.settings?.longForm === true,
        requiredMods: (body.settings?.requiredMods ?? []).slice(0, 128).map(String),
        ...(body.settings?.authNotice ? { authNotice: String(body.settings.authNotice).slice(0, 400) } : {}),
    };

    const code = newSessionCode();
    const descriptor = await issueSessionDescriptor(env, code, credential.srv);

    const stub = env.LOBBY.get(env.LOBBY.idFromName(code));
    const init = await stub.fetch(new Request("https://lobby/init", {
        method: "POST",
        body: JSON.stringify({
            descriptor, settings,
            // The relay only ever sees pseudonyms, so this is how it will
            // recognise the host later: the same value that account's tickets
            // for this server will carry.
            hostPsid: await psidFor(env, account.id, credential.srv),
        }),
    }));
    if (!init.ok) return fail(500, "session_failed", "Could not open that session.");

    const wsBase = env.ISSUER.replace(/^http/, "ws");
    return json({
        code,
        descriptor,
        wsUrl: `${wsBase}/session/${code}/ws`,
        settings,
        // The host's own pseudonym on its own server. Only we can compute it,
        // and the host needs it to declare itself in WELCOME -- a server that
        // cannot say who runs it is refused by every client.
        hostPsid: await psidFor(env, account.id, credential.srv),
        issuer: env.ISSUER,
    });
}

interface TicketBody {
    descriptor?: string;
    nonce?: string;
    alias?: string;
    presentBadges?: boolean;
}

/**
 * Mint a join ticket.
 *
 * The descriptor is what makes this safe: it is signed by us and names the
 * server, so a host cannot claim to be a different server in order to see the
 * pseudonyms that server sees. The nonce is what stops tickets being minted in
 * advance -- it has to be a challenge the target lobby issued moments ago.
 */
async function ticketMint(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in to join a game.");

    const body = await readJson<TicketBody>(request);
    if (!body?.descriptor || !body.nonce) return fail(400, "bad_request", "Missing descriptor or nonce.");

    // Refused here, at the one chokepoint every join passes through. The
    // server is told nothing -- it simply never sees this player, so no host
    // learns that a ban exists or who it applied to.
    if (banInForce(account)) {
        return fail(403, "banned",
                    account.banned!.reason || "This account cannot join games.",
                    { until: account.banned!.until ?? null });
    }

    const descriptor = await verifySessionDescriptor(env, body.descriptor);
    if (!descriptor) return fail(400, "bad_session", "That game session is no longer valid.");

    // An alias is a display name for one server. It faces other players, so it
    // passes the same filter a nickname does -- but it is never stored here,
    // and never has to be unique.
    let alias: string | undefined;
    if (body.alias) {
        const check = await checkNickname(env, body.alias);
        if (!check.ok) return fail(400, check.reason!, check.message!);
        alias = check.canonical;
    }

    const ticket = await issueJoinTicket(env, account, descriptor.sid, descriptor.srv, {
        ...(alias ? { alias } : {}),
        presentBadges: body.presentBadges !== false,
        nonce: String(body.nonce).slice(0, 64),
    });
    return json({ ticket });
}

// ================================================================= misc ====

function htmlPage(title: string, message: string): Response {
    // Deliberately plain and self-contained: no fonts, no scripts, no
    // analytics. This page is shown mid-login and has no business talking to
    // anyone else.
    const escape = (s: string) => s.replace(/[<>&"]/g, (c) =>
        ({ "<": "&lt;", ">": "&gt;", "&": "&amp;", '"': "&quot;" }[c]!));
    return new Response(
        `<!doctype html><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>${escape(title)} — OpenDoctrines</title>
<style>
 body{font:16px/1.6 system-ui,sans-serif;max-width:34rem;margin:14vh auto;padding:0 1.5rem;
      background:#14161a;color:#e8e6e3}
 h1{font-size:1.3rem;margin:0 0 .6rem}
 p{margin:0;color:#b9b5ae}
 a{color:#ffd700}
</style>
<h1>${escape(title)}</h1>
<p>${escape(message)}</p>`,
        { status: 200, headers: { "content-type": "text/html; charset=utf-8" } },
    );
}
