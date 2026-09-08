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
    authorizeUrl, exchangeCode, fetchIdentity, identityFromImplicitToken,
    type ResolvedIdentity,
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
import {
    checkQuota, forward, isDuplicate, isPrivateCategory, LIMITS as FEEDBACK_LIMITS,
    needsAccount, parseReport, recordAccepted, type Rejected,
} from "./feedback/report.js";
import { handleGithubHook } from "./feedback/github-hook.js";
import { announcePardon } from "./moderation/reports.js";
import {
    actOnAccount, announce, checkReporterQuota, decide, fileReport, findAccount,
    getReport, isModerator, listReports, parseReportInput, profileOf,
    type Rejected as ModRejected,
} from "./moderation/reports.js";
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
    if (post && path === "/auth/itch") return authItchToken(request, env);
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

    if (post && path === "/feedback") return feedbackSubmit(request, env);
    if (post && path === "/feedback/github") return feedbackGithubHook(request, env);
    if (post && path === "/moderation/report") return moderationReport(request, env);
    if (get  && path === "/moderation/reports") return moderationList(request, env);
    if (post && path === "/moderation/decide") return moderationDecide(request, env);
    if (get  && path === "/moderation/account") return moderationProfile(request, env, url);
    if (post && path === "/moderation/account") return moderationAct(request, env);
    if (get  && path === "/tournaments") return tournamentList(env);
    if (post && path === "/tournaments") return tournamentSet(request, env);

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

/**
 * The page that reads the fragment.
 *
 * Deliberately tiny and self-contained: it takes what is after the "#", posts
 * it to the service, and says what happened. It loads nothing, and the token
 * never leaves this page except in that one request.
 *
 * `history.replaceState` wipes the fragment from the address bar immediately,
 * so a token does not sit in the browser history or get shoulder-read off a
 * screen after the fact.
 */
function implicitCallbackPage(): Response {
    const body = `<!doctype html><meta charset="utf-8">
<title>Signing in</title>
<style>body{background:#0d0f14;color:#cdd3e2;font:14px system-ui,sans-serif;
display:grid;place-items:center;height:100vh;margin:0;text-align:center}
p{max-width:32rem;line-height:1.5}</style>
<p id="m">Finishing sign-in...</p>
<script>
(function () {
  var say = function (t) { document.getElementById("m").textContent = t; };
  var h = new URLSearchParams(location.hash.replace(/^#/, ""));
  var token = h.get("access_token"), state = h.get("state");
  // Off the address bar before anything else happens.
  history.replaceState(null, "", location.pathname);
  if (!token || !state) { say("That sign-in did not complete. Start again from the game."); return; }
  fetch("/auth/itch", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ state: state, access_token: token })
  }).then(function (r) { return r.json(); }).then(function (d) {
    say(d && d.ok ? "Signed in. You can close this tab and go back to the game."
                  : (d && d.message) || "That sign-in could not be completed.");
  }).catch(function () { say("Could not reach the service. Start again from the game."); });
})();
</script>`;
    return new Response(body, {
        status: 200,
        headers: {
            "content-type": "text/html; charset=utf-8",
            // Nothing here should be cached: it is a one-time page carrying a
            // credential, and a cached copy is a credential left on disk.
            "cache-control": "no-store",
        },
    });
}

/**
 * The other half of the implicit flow: the token, posted back by that page.
 *
 * Everything after the verification is the same code path the ordinary
 * providers take -- the same state check, the same link/login completion, the
 * same stored result for the game to poll.
 */
async function authItchToken(request: Request, env: Env): Promise<Response> {
    const limited = await rateLimit(env.RATE_LIMIT_API, request, env, 20);
    if (limited) return limited;

    const body = await readJson<{ state?: string; access_token?: string }>(request, 8 * 1024);
    if (!body?.state || !body.access_token) {
        return fail(400, "bad_request", "That sign-in could not be completed.");
    }
    const claims = await verifyAuthRequest(env, body.state);
    if (!claims || claims.provider !== "itch") {
        return fail(400, "bad_state", "That sign-in could not be completed.");
    }

    // Spent against itch.io, never trusted on its own.
    const identity = await identityFromImplicitToken("itch", body.access_token);
    if (!identity) {
        await storeResult(env, claims.rid, {
            kind: "error", code: "provider_failed",
            message: "itch.io did not complete the sign-in.",
        });
        return fail(502, "provider_failed", "itch.io did not complete the sign-in.");
    }

    const result = claims.purpose === "link"
        ? await completeLink(env, claims.link, "itch", identity.sub)
        : await completeLogin(env, "itch", identity);
    await storeResult(env, claims.rid, result);
    return json({ ok: result.kind !== "error", message: (result as { message?: string }).message });
}

async function authCallback(
    _request: Request, env: Env, url: URL, providerParam: string,
): Promise<Response> {
    if (!isProviderId(providerParam)) return htmlPage("Unknown provider", "That provider is not supported.");
    const provider: ProviderId = providerParam;

    // ── The implicit flow arrives with nothing in the query at all ──
    //
    // itch.io puts the access token in the URL FRAGMENT, which a browser never
    // sends to a server: this handler is reached with an empty query string and
    // the credential sitting in the address bar where only the page can see it.
    // So for that provider the callback is a page whose one job is to read the
    // fragment and hand it back over POST. Everything after that is identical.
    if (PROVIDERS[provider].flow === "implicit") return implicitCallbackPage();

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

/**
 * A bug report, a suggestion or a rating, from the game.
 *
 * DELIBERATELY UNAUTHENTICATED. Requiring a sign-in to report a crash means the
 * reports that arrive are from the players who already made an account, which
 * is not the population whose bugs are worth most -- the ones who cannot get
 * past the menu have nothing to sign in WITH. The cost is that this is an open
 * endpoint, and it is defended as one: the address limiter below, then a
 * per-install daily cap, then a duplicate window. See feedback/report.ts.
 *
 * The size cap is enforced twice on purpose. readJson refuses a body larger
 * than the ceiling before parsing it, and the fields are clipped again after,
 * so neither a lying content-length nor a single enormous field gets through.
 */
/**
 * GitHub's issues webhook.
 *
 * Deliberately NOT rate limited by address: the caller is GitHub, a burst of
 * deliveries after somebody closes ten issues is normal, and a 429 here means
 * GitHub retries and eventually disables the hook. The signature is what stands
 * between this and abuse, and an unsigned request is rejected before the body
 * is read as anything but bytes.
 */
/**
 * A player reporting a message to the account service.
 *
 * Needs an account, because an anonymous accusation cannot be weighed against
 * the person making it -- a queue nobody can attribute is a queue anybody can
 * flood, and the pattern of who reports whom is often the most informative
 * thing in it.
 */
/**
 * Official tournaments, announced and planned.
 *
 * Public and unauthenticated to READ -- it is a noticeboard, and requiring a
 * sign-in to find out when a tournament is would defeat the point of putting it
 * in front of people. Writing needs the developer badge, the same gate as
 * moderation: this is the maintainer's own announcement channel and nobody
 * else's.
 *
 * Stored as one small document rather than a row per tournament. There will be
 * a handful at a time, they change together when the maintainer edits them, and
 * one key is one read on a free tier.
 */
async function tournamentList(env: Env): Promise<Response> {
    const raw = await env.OD_ACCOUNTS.get("tournaments");
    let events: unknown = [];
    if (raw) { try { events = JSON.parse(raw); } catch { events = []; } }
    return json({ events });
}

async function tournamentSet(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!isModerator(account)) return fail(404, "not_found", "No such endpoint.");

    const body = await readJson<{ events?: unknown }>(request, 64 * 1024);
    if (!body || !Array.isArray(body.events)) {
        return fail(400, "bad_request", "Expected an events array.");
    }
    // Shaped and clipped here rather than trusted: this is served to every
    // client that opens the menu, so it is the one place a stray field would
    // reach everybody.
    const clean = (v: unknown, max: number) =>
        typeof v === "string"
            // eslint-disable-next-line no-control-regex
            ? v.replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "")
               .trim().slice(0, max)
            : "";
    const events = (body.events as Record<string, unknown>[]).slice(0, 25).map((e) => ({
        title: clean(e.title, 120),
        when: clean(e.when, 60),
        status: ["planned", "open", "running", "done"].includes(String(e.status))
            ? String(e.status) : "planned",
        detail: clean(e.detail, 1000),
        link: /^https:\/\//.test(String(e.link ?? "")) ? clean(e.link, 300) : "",
    })).filter((e) => e.title);

    await env.OD_ACCOUNTS.put("tournaments", JSON.stringify(events));
    return json({ ok: true, events });
}

async function moderationReport(request: Request, env: Env): Promise<Response> {
    const limited = await rateLimit(env.RATE_LIMIT_API, request, env, 20);
    if (limited) return limited;

    const reporter = await authenticate(request, env);
    if (!reporter) {
        return fail(401, "sign_in_required", "Please sign in to report someone.");
    }
    const raw = await readJson<unknown>(request, 32 * 1024);
    if (raw === null) return fail(413, "too_large", "That report is too large.");

    const parsed = parseReportInput(raw);
    if ("ok" in parsed && parsed.ok === false) {
        const r = parsed as ModRejected;
        return fail(r.status, r.code, r.message);
    }
    const over = await checkReporterQuota(env, reporter.id);
    if (over) return fail(over.status, over.code, over.message);

    const filed = await fileReport(env, reporter,
                                   parsed as Exclude<typeof parsed, ModRejected>);
    if ("ok" in filed && filed.ok === false) {
        const r = filed as ModRejected;
        return fail(r.status, r.code, r.message);
    }
    // The id comes back so the player can quote it if they follow up, but
    // nothing about the outcome does: whether somebody was banned is between
    // the service and that account.
    return json({ ok: true, id: (filed as { id: string }).id });
}

/** Look somebody up by nickname or account id. Developer badge only. */
async function moderationProfile(request: Request, env: Env, url: URL): Promise<Response> {
    const account = await authenticate(request, env);
    if (!isModerator(account)) return fail(404, "not_found", "No such endpoint.");

    const query = (url.searchParams.get("q") ?? "").slice(0, 100);
    if (!query) return fail(400, "bad_request", "Give a nickname or an account id.");

    const found = await findAccount(env, query);
    if (!found) return fail(404, "no_account", "Nobody by that name or id.");
    return json({ profile: await profileOf(env, found) });
}

/**
 * Ban, time out or pardon somebody directly. Developer badge only.
 *
 * Separate from /moderation/decide, which closes a report. This acts on a
 * person with no report behind it, which is what a maintainer needs when they
 * saw the thing themselves -- or when a punishment needs lifting.
 */
async function moderationAct(request: Request, env: Env): Promise<Response> {
    const actor = await authenticate(request, env);
    if (!isModerator(actor)) return fail(404, "not_found", "No such endpoint.");

    const body = await readJson<{
        q?: string; action?: string; days?: number; reason?: string;
    }>(request, 8 * 1024);
    if (!body?.q) return fail(400, "bad_request", "Give a nickname or an account id.");
    if (body.action !== "ban" && body.action !== "timeout" && body.action !== "dismiss") {
        return fail(400, "bad_action", "Unknown action.");
    }

    const target = await findAccount(env, body.q);
    if (!target) return fail(404, "no_account", "Nobody by that name or id.");
    // A moderator cannot ban themselves out of the tool by accident.
    if (target.id === actor!.id) {
        return fail(400, "self", "You cannot act on your own account here.");
    }

    const outcome = await actOnAccount(env, target, {
        action: body.action,
        days: body.days,
        reason: String(body.reason ?? "").slice(0, 200),
    });

    // Announced through the same channel as a report's outcome, and with the
    // same restraint: who, and until when. A pardon is announced too -- it is
    // the correction of something that WAS announced, and a channel that only
    // ever reports punishments gives a false picture of the moderation.
    let announced = false;
    if (env.MODERATION_DISCORD_WEBHOOK) {
        announced = await announce(env, {
            id: "direct", at: Math.floor(Date.now() / 1000),
            reporterId: "", reporterNick: "",
            accusedId: target.id, accusedNick: target.nick,
            // "other", because the reason enum describes what a REPORTER
            // said was wrong, and there is no reporter on this path.
            reason: "other", note: "", message: "", context: [],
            server: "", status: outcome.banned ? "actioned" : "dismissed",
            outcome: outcome.outcome, until: outcome.until,
            decidedBy: actor!.nick, decidedAt: Math.floor(Date.now() / 1000),
        });
        if (!outcome.banned) announced = await announcePardon(env, target.nick, actor!.nick);
    }
    return json({ ok: true, ...outcome, profile: await profileOf(env, target), announced });
}

/** The queue, for an account carrying the developer badge. */
async function moderationList(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    // 404, not 403. A 403 confirms the route exists and that this account is
    // simply not privileged, which tells a prober exactly what to go looking
    // for; the /admin routes already answer this way.
    if (!isModerator(account)) return fail(404, "not_found", "No such endpoint.");
    return json({ reports: await listReports(env) });
}

/** Ban, time out, or dismiss. Developer badge only. */
async function moderationDecide(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!isModerator(account)) return fail(404, "not_found", "No such endpoint.");

    const body = await readJson<{
        id?: string; action?: string; days?: number; reason?: string;
    }>(request, 8 * 1024);
    if (!body?.id) return fail(400, "bad_request", "Missing report id.");
    if (body.action !== "ban" && body.action !== "timeout" && body.action !== "dismiss") {
        return fail(400, "bad_action", "Unknown action.");
    }
    const report = await getReport(env, body.id);
    if (!report) return fail(404, "no_report", "No such report.");

    const decided = await decide(env, report, {
        action: body.action,
        days: body.days,
        reason: String(body.reason ?? "").slice(0, 200),
    }, account!);
    if ("ok" in decided && decided.ok === false) {
        const r = decided as ModRejected;
        return fail(r.status, r.code, r.message);
    }
    // Announced after the ban is written, never before: a channel post about a
    // ban that then failed to apply is worse than no post.
    const announced = await announce(env, decided as Exclude<typeof decided, ModRejected>);
    return json({ ok: true, report: decided, announced });
}

async function feedbackGithubHook(request: Request, env: Env): Promise<Response> {
    // The RAW text. Re-serialising a parsed object does not reproduce the bytes
    // GitHub signed, so the signature must be checked against this exact string.
    const raw = await request.text();
    if (raw.length > 512 * 1024) return fail(413, "too_large", "Payload too large.");

    const outcome = await handleGithubHook(
        env, raw,
        request.headers.get("x-hub-signature-256"),
        request.headers.get("x-github-event"),
    );
    return json(outcome.body, outcome.status);
}

async function feedbackSubmit(request: Request, env: Env): Promise<Response> {
    const limited = await rateLimit(env.RATE_LIMIT_API, request, env, 30);
    if (limited) return limited;

    const maxBody = FEEDBACK_LIMITS.diagnostics + FEEDBACK_LIMITS.body + 4096;
    const raw = await readJson<unknown>(request, maxBody);
    if (raw === null) {
        return fail(413, "too_large", "That report is too large to send.");
    }

    const parsed = parseReport(raw);
    if ("ok" in parsed && parsed.ok === false) {
        const r = parsed as Rejected;
        return fail(r.status, r.code, r.message,
                    r.retryAfter ? { retryAfter: r.retryAfter } : {});
    }
    const report = parsed as Exclude<typeof parsed, Rejected>;

    // ── Who is sending this ──
    //
    // A bug or a suggestion needs an account. Both are published -- to a
    // channel, and for a bug to a public tracker -- and both are signed, so
    // there has to be something to sign them WITH that a stranger cannot make
    // up on the spot.
    //
    // The byline comes from the verified token and never from the body. A
    // client-supplied name is a name anyone can choose, and this one goes next
    // to a public accusation that something is broken.
    //
    // A rating is exempt: it is one tap, carries no prose and no identity, and
    // is published nowhere.
    if (needsAccount(report.kind)) {
        const account = await authenticate(request, env);
        if (!account) {
            return fail(401, "sign_in_required",
                        "Please sign in to send this. Reports are published, and " +
                        "signed with your nickname.");
        }
        if (account.banned) {
            // Told plainly rather than accepted and dropped: silently binning a
            // banned account's reports wastes their time and teaches nothing.
            return fail(403, "account_banned",
                        "This account cannot send reports.");
        }
        report.reporter = account.nick;
    }

    const overQuota = await checkQuota(env, report);
    if (overQuota) return fail(overQuota.status, overQuota.code, overQuota.message,
                               { retryAfter: overQuota.retryAfter });

    // A repeat is ACCEPTED, not refused. Sending the same report twice is what
    // a player does when they are not sure the first one worked, and answering
    // the second with an error teaches them it did not.
    if (await isDuplicate(env, report)) {
        return json({ ok: true, duplicate: true, private: isPrivateCategory(report) });
    }

    const notes: string[] = [];
    const destinations = await forward(env, report, notes);

    // A security report that reached nowhere is the one failure the player has
    // to be told about.
    //
    // Every other category is fine to accept quietly: an unconfigured fork has
    // no destinations, and telling a player "Discord returned 500" asks them to
    // solve a problem they cannot reach. A security report is different. It is
    // the one the reporter most needs to know actually arrived, it is the one
    // that must never be re-routed somewhere public to make delivery succeed,
    // and if it went nowhere the honest answer is to say so and let them reach
    // the maintainer another way. Not remembered either, so their retry is not
    // met with "already got that" for the next six hours.
    if (isPrivateCategory(report) && destinations.length === 0) {
        await recordAccepted(env, report, false);
        // The upstream reason goes in `detail`, NOT in the player's sentence.
        // They cannot act on "advisory HTTP 403" and it is not their problem;
        // the operator reading a curl output can act on nothing else.
        return fail(502, "not_delivered",
                    "Your security report could not be delivered, and has NOT been " +
                    "filed anywhere. Please contact the maintainer directly.",
                    notes.length > 0 ? { detail: notes.join("; ") } : {});
    }

    await recordAccepted(env, report);
    return json({
        ok: true,
        duplicate: false,
        delivered: destinations.length > 0,
        private: isPrivateCategory(report),
        // Present only when something went wrong on the way. The game does not
        // read it -- it is for whoever is holding a curl and a question.
        ...(notes.length > 0 ? { detail: notes.join("; ") } : {}),
    });
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
