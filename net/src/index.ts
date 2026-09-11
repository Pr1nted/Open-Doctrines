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
    authenticate, fail, json, preflight, rateLimit, readJson, text, wantsHtml, withCors,
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
    identityFromOpenId, type ResolvedIdentity,
} from "./auth/oauth.js";
import { issueSessionToken } from "./auth/token.js";
import { issueJoinTicket, psidFor } from "./auth/ticket.js";
import {
    accountForIdentity, asSubjectHash, changeNickname, createAccount, getAccount,
    banInForce, identSubHash, identityIsBanned, linkIdentity, publicAccount, setBanned,
    unlinkIdentity, countAccounts,
} from "./accounts/store.js";
import { checkAccountAge, tooNewMessage } from "./accounts/policy.js";
import { checkNickname } from "./accounts/nickname.js";
import { isAdmin, setBadge } from "./accounts/badges.js";
import { applyEdit, forClients, readAll, type Announcement, type Edit } from "./announcements/store.js";
import { isLive } from "./live/lookup.js";
import { createLink, useLink } from "./live/viewerlink.js";
import { safeChannel } from "./live/platforms.js";
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

    // ── IS THE SERVICE UP ──
    //
    // Its own route rather than a reuse of "/", because the two answer
    // different questions and the callers are different. "/" describes the
    // service to a CLIENT -- issuer, providers, policy links -- and grows
    // whenever one of those changes. This answers a MONITOR, cheaply and in a
    // shape that will not move.
    //
    // SHALLOW ON PURPOSE. It reports that this worker is running and reachable,
    // and it checks nothing behind it: no KV read, no Durable Object woken. A
    // health check that touches its dependencies on every request is a way to
    // be billed for, and rate-limited by, whoever points a monitor at it -- and
    // an uptime monitor firing every thirty seconds would wake a DO each time.
    // So it says only what it actually knows, rather than implying it has
    // verified a stack it has not looked at.
    //
    // tests/net_live_check.cpp probes this URL. That check passes on ANY status
    // because what it tests is the TLS connect rather than the route, so it was
    // passing against a 404 -- the endpoint simply did not exist. It exists now,
    // which does not change that test and does give the URL it names a meaning.
    //
    // HEAD as well as GET, because that is what uptime monitors send: it is the
    // cheaper request and many default to it. Answering 404 to HEAD would
    // reproduce the very failure this route was added to end -- a monitor
    // reporting something other than the truth about the service -- and would
    // do it in the direction that cries wolf. The runtime drops the body.
    if ((get || request.method === "HEAD") && path === "/health") {
        return json({ ok: true, service: "opendoctrines-net" }, 200,
                    { "cache-control": "no-store" });
    }

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

    // ── ONE URL, TWO READERS ──
    //
    // These are markdown because that is what they are in the tree, and the
    // game links to them by URL. But a PERSON following that link gets a
    // browser showing the source, ## and ** on display -- which is what these
    // documents looked like to anyone who clicked "Privacy policy" in the
    // Account screen.
    //
    // A browser says so in its Accept header. When one asks, it is sent to the
    // rendered copy on the web build's own site, which is generated from these
    // very files at deploy time (packaging/web/policies). Anything that is not
    // a browser -- curl, a script, a client reading the text -- still gets the
    // markdown it asked for, byte for byte.
    //
    // WHY NOT JUST PUT THE HTML HERE: because then this Worker and the deploy
    // would each hold a rendering of the same document, and the second copy is
    // the one that goes stale. And Discord will not accept a workers.dev URL
    // for a policy link at all, so a rendered copy has to exist over there
    // regardless.
    if (get && (path === "/privacy" || path === "/terms")) {
        const doc = path === "/privacy" ? PRIVACY_POLICY : TERMS_OF_USE;
        if (env.DOCS_BASE && wantsHtml(request)) {
            return Response.redirect(`${env.DOCS_BASE}${path}`, 302);
        }
        return text(doc, 200, "text/markdown; charset=utf-8");
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

    // The main-menu board. Public and cacheable: it is the same handful of
    // sentences for everybody, and the client asks once per launch.
    if (get && path === "/announcements") return announcements(env);
    if (post && path === "/admin/announcement") return adminAnnouncement(request, env);

    if (post && path === "/admin/badge") return adminBadge(request, env);
    if (post && path === "/admin/ban") return adminBan(request, env);

    // ----------------------------------------------------------- lobby ----

    if (post && path === "/feedback") return feedbackSubmit(request, env);
    if (post && path === "/feedback/github") return feedbackGithubHook(request, env);
    if (post && path === "/moderation/report") return moderationReport(request, env);
    // The board's editing view, and the one write that changes it. Signed in
    // with the same session token the reports screen uses, and gated on the
    // same badge -- so posting an announcement from inside the game needs no
    // secret typed into a text field and stored on disk.
    // Who is live. Public, cached, and it answers "not live" for everything it
    // cannot check -- a live badge is decoration and must never be a reason a
    // lobby fails to open.
    if (post && path === "/live") return liveLookup(request, env);

    // A viewer link: minted by a host, clicked by a viewer. The session code
    // itself never travels through either of these in the clear on a stream --
    // see live/viewerlink.ts for why the code cannot do this job itself.
    if (post && path === "/viewer-link") return viewerLinkCreate(request, env);
    const joinMatch = /^\/j\/([a-z0-9]{10})$/.exec(path);
    if (joinMatch && get) return viewerLinkOpen(env, joinMatch[1]!);
    if (get  && path === "/moderation/announcements") return announcementList(request, env);
    if (post && path === "/moderation/announcement") return announcementEdit(request, env);
    if (get  && path === "/moderation/overview") return moderationOverview(request, env);
    if (get  && path === "/moderation/reports") return moderationList(request, env);
    if (post && path === "/moderation/decide") return moderationDecide(request, env);
    if (get  && path === "/moderation/account") return moderationProfile(request, env, url);
    if (post && path === "/moderation/account") return moderationAct(request, env);
    // ── HOW MUCH THE GAME IS ACTUALLY PLAYED ──
    //
    // Aggregated from the rows LobbyDO writes when a session ends: duration,
    // arrivals, and the most people in a lobby at once. Public because there is
    // nothing in it to protect -- no row identifies a person or a game, and the
    // individual rows are never returned, only counts and medians over a day.
    //
    // MEDIAN, NOT MEAN. One long-form session left running for a week would
    // drag an average into uselessness while saying nothing about how long a
    // game actually holds someone.
    if (get && path === "/stats") return sessionStats(env, url);

    // ── HOW LONG A PLAY LASTED, FROM PLAYERS WHO SAID YES ──
    //
    // OFF unless a player turned it on. Not "on unless they turned it off",
    // and not on for a first run before they have seen the setting: the game
    // promises no usage reporting, and the only honest way to have any is for
    // the player to have chosen it.
    //
    // The report is one coarse bucket and a surface. There is no id, no
    // cookie, no account, no address kept, and nothing that links two reports
    // from the same person -- which is exactly why "delete my data" has no
    // meaning here and why the policy says so instead of offering a button
    // that would do nothing. See usageReport().
    if (post && path === "/usage") return usageReport(request, env);

    // Deleting all of it, which is the only deletion this data admits of.
    // Developer badge only; see usageForget().
    if (post && path === "/usage/forget") return usageForget(request, env);

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
        // ── THE QUERY MUST SURVIVE THIS REWRITE ──
        //
        // The object reads `?role=` to decide whether a socket is the host, a
        // player or a spectator. Rewriting the URL without url.search dropped
        // it, so EVERY connection arrived with no role and defaulted to
        // "player" -- including the host's. The alarm then found no host,
        // closed everyone with 4404 and deleted the session, five seconds
        // after it opened. A host could never connect as one, so the relay had
        // never carried a game.
        if (sessionMatch[2]) {
            return stub.fetch(new Request("https://lobby/ws" + url.search, request));
        }
        if (get) return withCors(await stub.fetch(new Request("https://lobby/info")));
    }

    // ------------------------------------------------------- long form ----
    //
    // Turn storage for TurnStoreKind::DurableObject (src/net/TurnStore.cpp).
    // The URL shapes are the game's, not ours to choose.
    //
    // A PUBLISHED TURN is public, and unauthenticated reads of it are the
    // design rather than an oversight: that is what lets people spectate a
    // tournament without joining it, and it is immutable once written.
    //
    // ORDERS ARE NOT, AND USED TO BE. The reasoning was that orders are sealed
    // before they leave the player's machine (src/net/TurnSeal.h), so a reader
    // without the key holds ciphertext. True of an outsider; false of a rival,
    // who holds the same per-SESSION key every player is handed, and who can
    // derive the URL from the join code, the turn and a psid the roster
    // publishes. Reads of orders are authorised in LobbyDO now, and an
    // authenticated attempt at somebody else's is recorded for the host.
    //
    // One honest difference from the jsonblob backend remains: there a blob
    // sits at an unguessable URL, whereas these are derivable from the join
    // code, so an observer holding the code can tell WHETHER a given player has
    // submitted for a turn. In a game where the host announces who is still to
    // move, that is not a secret.
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
    // Who has been reaching for other people's orders. The host asks; LobbyDO
    // refuses everybody else. See handlePeeks.
    const peeksMatch = /^\/session\/([^/]+)\/peeks$/.exec(path);
    if (peeksMatch && isSessionCode(peeksMatch[1]!)) {
        const stub = env.LOBBY.get(env.LOBBY.idFromName(peeksMatch[1]!));
        return withCors(await stub.fetch(new Request("https://lobby/peeks", request)));
    }

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

    // The two remaining flows end identically and differ only in how the
    // identity is proved: OAuth exchanges a code for a token and asks the
    // provider who it belongs to; OpenID 2.0 gets a signed assertion up front
    // and asks Steam whether the signature is really theirs. Both arrive with
    // our own signed request token in `state` -- OpenID 2.0 has no such
    // parameter of its own, so it rides inside return_to, which Steam signs.
    const openid = PROVIDERS[provider].flow === "openid2";

    const state = url.searchParams.get("state");
    const claims = state ? await verifyAuthRequest(env, state) : null;
    if (!claims || claims.provider !== provider) {
        return htmlPage("Sign-in failed", "That sign-in could not be completed. Start again from the game.");
    }

    let identity: ResolvedIdentity | null;
    if (openid) {
        identity = await identityFromOpenId(env, provider, url);
    } else {
        const code = url.searchParams.get("code");
        if (!code) {
            return htmlPage("Sign-in failed", "That sign-in could not be completed. Start again from the game.");
        }
        const accessToken = await exchangeCode(env, provider, code, claims.rid);
        identity = accessToken ? await fetchIdentity(provider, accessToken) : null;
    }
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
async function viewerLinkCreate(request: Request, env: Env): Promise<Response> {
    // Held to an account, so a link cannot be minted by anybody who happens to
    // know a session code.
    const account = await authenticate(request, env);
    if (!account) return fail(401, "unauthorized", "Sign in first.");

    const body = await readJson<{ code?: string; uses?: number }>(request, 4 * 1024);
    const code = String(body?.code ?? "");
    if (!isSessionCode(code)) return fail(400, "bad_request", "Not a session code.");

    const link = await createLink(env, code, Math.floor(Date.now() / 1000),
                                  typeof body?.uses === "number" ? body.uses : undefined);
    // The URL is built here so the game never has to know the shape of it.
    return json({
        token: link.token,
        url: `${env.ISSUER}/j/${link.token}`,
        expires: link.expires,
        uses: link.uses,
    });
}

async function viewerLinkOpen(env: Env, token: string): Promise<Response> {
    const r = await useLink(env, token, Math.floor(Date.now() / 1000));
    // ── A PAGE, NOT A REDIRECT ──
    //
    // The viewer is arriving from a chat message on a phone or a browser that
    // has never heard of this game. A bare redirect to a custom scheme shows
    // them a browser error; a page can say what is about to happen, offer the
    // download to somebody who has not got the game, and put the code where
    // they can type it if the scheme handler is not registered.
    const escape = (v: string) =>
        v.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    const body = r.ok
        ? `<h1>Join the game</h1>
           <p><a class="go" href="opendoctrines://join/${escape(r.code)}">Open OpenDoctrines</a></p>
           <p class="small">Not working? Start the game, choose Play Multiplayer,
              and enter <code>${escape(r.code)}</code>.</p>`
        : `<h1>That link has ${r.reason === "expired" ? "expired"
                              : r.reason === "spent" ? "been used up" : "stopped working"}</h1>
           <p class="small">Ask the host for a new one.</p>`;
    const html = `<!doctype html><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenDoctrines</title>
<style>body{font-family:system-ui,sans-serif;background:#0c0d12;color:#dfe4f0;
display:flex;min-height:100vh;align-items:center;justify-content:center;margin:0}
main{max-width:32rem;padding:2rem}h1{font-size:1.4rem}
a.go{display:inline-block;padding:.7rem 1.1rem;border:1px solid #6ee7a0;
border-radius:.4rem;color:#6ee7a0;text-decoration:none}
.small{color:#98a0b8;font-size:.9rem}code{color:#fff}</style>
<main>${body}</main>`;
    return new Response(html, {
        status: r.ok ? 200 : 410,
        headers: { "content-type": "text/html; charset=utf-8",
                   // Never cached: it spends a use, and a cached copy would
                   // hand the same one out twice.
                   "cache-control": "no-store" },
    });
}

async function liveLookup(request: Request, env: Env): Promise<Response> {
    const body = await readJson<{ channels?: { platform?: string; channel?: string }[] }>(
        request, 8 * 1024);
    const asked = Array.isArray(body?.channels) ? body!.channels! : [];
    // Bounded: a lobby is at most a few dozen people, and an unbounded list is
    // an unbounded fan-out of upstream requests from one cheap POST.
    const wanted = asked.slice(0, 32);

    const out: Record<string, { live: boolean; title?: string; viewers?: number }> = {};
    await Promise.all(wanted.map(async (row) => {
        const platform = row?.platform;
        if (platform !== "twitch" && platform !== "youtube" && platform !== "kick") return;
        const channel = safeChannel(String(row?.channel ?? ""));
        if (!channel) return;
        out[`${platform}:${channel}`] = await isLive(env, platform, channel);
    }));

    return json({ live: out }, 200, { "cache-control": "public, max-age=30" });
}

async function announcementList(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!isModerator(account)) return fail(404, "not_found", "No such endpoint.");
    // The WHOLE list, hidden entries included: putting something back requires
    // being able to see what was taken down.
    return json({ all: await readAll(env) });
}

async function announcementEdit(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!isModerator(account)) return fail(404, "not_found", "No such endpoint.");

    const body = await readJson<{ op?: string; id?: string; item?: Announcement }>(
        request, 16 * 1024);
    if (!body?.op) return fail(400, "bad_request", "Missing op.");

    let edit: Edit;
    if (body.op === "put") {
        if (!body.item) return fail(400, "bad_request", "put needs an item.");
        edit = { op: "put", item: body.item };
    } else if (body.op === "hide" || body.op === "show" || body.op === "purge") {
        if (!body.id) return fail(400, "bad_request", `${body.op} needs an id.`);
        edit = { op: body.op, id: body.id };
    } else {
        return fail(400, "bad_op", "op must be put, hide, show or purge.");
    }

    const result = await applyEdit(env, edit, Math.floor(Date.now() / 1000));
    // The reason is written for whoever is typing it, in the game, right now --
    // which is the whole point of validating on this side as well as in the
    // client. See announcements/store.ts.
    if (!result.ok) return fail(400, "bad_announcement", result.reason);
    return json({ all: result.all });
}

/**
 * The numbers the admin screen shows about the service as a whole.
 *
 * Moderator-gated like everything else here, so it is reachable from inside the
 * game with a session token and no secret. One field today; the shape is a
 * bag so a second number does not need a second round trip later.
 */
async function moderationOverview(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!isModerator(account)) return fail(404, "not_found", "No such endpoint.");
    const { accounts, exact } = await countAccounts(env);
    return json({ accounts, exact });
}

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

async function announcements(env: Env): Promise<Response> {
    const now = Math.floor(Date.now() / 1000);
    const items = forClients(await readAll(env), now);
    // Shaped exactly as src/net/Announcements.h reads it. A short cache because
    // a board that changes on the hour does not need to be fetched by every
    // launch in that hour, and the client asks only once per run anyway.
    return json({ items }, 200, { "cache-control": "public, max-age=300" });
}

async function adminAnnouncement(request: Request, env: Env): Promise<Response> {
    // 404 rather than 403, like the other admin routes: an endpoint that
    // answers "forbidden" is an endpoint somebody now knows exists.
    if (!isAdmin(request, env)) return fail(404, "not_found", "No such endpoint.");

    const body = await readJson<{
        op?: string; id?: string; item?: Announcement;
    }>(request);
    if (!body?.op) return fail(400, "bad_request", "Missing op.");

    let edit: Edit;
    if (body.op === "put") {
        if (!body.item) return fail(400, "bad_request", "put needs an item.");
        edit = { op: "put", item: body.item };
    } else if (body.op === "hide" || body.op === "show" || body.op === "purge") {
        if (!body.id) return fail(400, "bad_request", `${body.op} needs an id.`);
        edit = { op: body.op, id: body.id };
    } else {
        return fail(400, "bad_op", "op must be put, hide, show or purge.");
    }

    const result = await applyEdit(env, edit, Math.floor(Date.now() / 1000));
    if (!result.ok) return fail(400, "bad_announcement", result.reason);
    // The WHOLE list, hidden entries included -- this is the editing view, and
    // putting something back requires being able to see what was taken down.
    return json({ all: result.all });
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

interface StatRow { s: number; j: number; p: number; }

/**
 * Daily play statistics, aggregated on read.
 *
 * Aggregating here rather than keeping running totals is what makes this cost
 * one write per finished session instead of one per join -- see
 * recordSession() in LobbyDO. At the volumes this service sees, listing a
 * day's keys is cheap and a lost update is impossible by construction.
 */
async function sessionStats(env: Env, url: URL): Promise<Response> {
    const days = Math.min(90, Math.max(1, Number(url.searchParams.get("days") ?? "14")));
    const out: Array<Record<string, number | string | Record<string, number>>> = [];

    for (let i = 0; i < days; i++) {
        const day = new Date(Date.now() - i * 86_400_000).toISOString().slice(0, 10);
        const listed = await env.OD_ACCOUNTS.list({ prefix: `stat:${day}:`, limit: 1000 });
        const rows: StatRow[] = [];
        for (const k of listed.keys) {
            const raw = await env.OD_ACCOUNTS.get(k.name);
            if (!raw) continue;
            try { rows.push(JSON.parse(raw) as StatRow); } catch { /* skip a bad row */ }
        }
        // Opted-in play reports for the same day, counted by bucket.
        const usage: Record<string, number> = {};
        const uKeys = await env.OD_ACCOUNTS.list({ prefix: `usage:${day}:`, limit: 1000 });
        for (const k of uKeys.keys) {
            const raw = await env.OD_ACCOUNTS.get(k.name);
            if (!raw) continue;
            try {
                const u = JSON.parse(raw) as { b: string };
                usage[u.b] = (usage[u.b] ?? 0) + 1;
            } catch { /* skip a bad row */ }
        }

        if (rows.length === 0) { out.push({ day, sessions: 0, plays: usage }); continue; }

        const lengths = rows.map((r) => r.s).sort((a, b) => a - b);
        const median = lengths[Math.floor(lengths.length / 2)] ?? 0;
        out.push({
            day,
            sessions: rows.length,
            joins: rows.reduce((n, r) => n + (r.j || 0), 0),
            // A session nobody joined is a lobby that was opened and abandoned,
            // which is a different problem from one that filled and ended.
            sessionsWithAPlayer: rows.filter((r) => (r.j || 0) > 0).length,
            biggestLobby: rows.reduce((n, r) => Math.max(n, r.p || 0), 0),
            medianSeconds: median,
            longestSeconds: lengths[lengths.length - 1] ?? 0,
            // From players who opted in, and only ever as counts per bucket.
            plays: usage,
        });
    }
    return json({ days: out });
}

/** The only durations this service will record. A free-text field would be a
 *  place to put something identifying, so there is not one. */
const USAGE_BUCKETS = ["<1m", "1-5m", "5-15m", "15-60m", "60m+"] as const;
const USAGE_SURFACES = ["web", "desktop", "android"] as const;

/**
 * One anonymous report that a play session happened and roughly how long for.
 *
 * WHAT IS DELIBERATELY NOT STORED: anything at all about who sent it. No
 * account, no install id, no pseudonym, no IP, no user agent, no timestamp
 * finer than the day. Two reports from the same person are indistinguishable
 * from two reports by different people, on purpose -- it is what keeps this on
 * the right side of the promise in PRIVACY.md, and it is also the reason there
 * is no per-person deletion: there is no "per person" here to delete.
 *
 * The cost of that honesty: this cannot measure returning players. Retention
 * across sessions needs linkage, linkage needs an identifier, and an
 * identifier is the thing being refused.
 */
async function usageReport(request: Request, env: Env): Promise<Response> {
    const body = await readJson<{ bucket?: string; surface?: string }>(request);
    const bucket = body?.bucket ?? "";
    const surface = body?.surface ?? "";
    // An allowlist, not validation of free text: the set of things that can be
    // written here is fixed at deploy time and cannot be widened by a caller.
    if (!(USAGE_BUCKETS as readonly string[]).includes(bucket)) {
        return fail(400, "bad_request", "Not a bucket this service records.");
    }
    if (!(USAGE_SURFACES as readonly string[]).includes(surface)) {
        return fail(400, "bad_request", "Not a surface this service records.");
    }

    const day = new Date().toISOString().slice(0, 10);
    await env.OD_ACCOUNTS.put(
        `usage:${day}:${crypto.randomUUID()}`, JSON.stringify({ b: bucket, f: surface }),
        { expirationTtl: 90 * 24 * 60 * 60 },
    );
    return json({ ok: true });
}

/**
 * Erase every usage and session statistic this service holds.
 *
 * THE ONLY DELETION THAT MEANS ANYTHING HERE. Nothing recorded is tied to a
 * person, so "delete mine" cannot be honoured -- there is no way to tell which
 * rows would be yours, and a button that pretended otherwise would be worse
 * than not offering one. What CAN be promised is that the whole lot goes, on
 * request, immediately rather than waiting out the 90-day expiry.
 *
 * Gated on the developer badge, and it answers 404 to everyone else so the
 * route does not confirm it exists.
 */
async function usageForget(request: Request, env: Env): Promise<Response> {
    const account = await authenticate(request, env);
    if (!account || !account.badges?.includes("developer")) {
        return fail(404, "not_found", "No such endpoint.");
    }
    let removed = 0;
    for (const prefix of ["usage:", "stat:"]) {
        let cursor: string | undefined;
        do {
            const listed = await env.OD_ACCOUNTS.list({ prefix, cursor, limit: 1000 });
            for (const k of listed.keys) { await env.OD_ACCOUNTS.delete(k.name); removed++; }
            cursor = listed.list_complete ? undefined : listed.cursor;
        } while (cursor);
    }
    return json({ ok: true, removed });
}
