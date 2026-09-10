// The provider round trip: build the consent URL, then turn the code that
// comes back into a stable user id.
//
// The `state` parameter is the signed auth-request token from device.ts, not a
// random string with a KV row behind it. It already carries the request id,
// provider and purpose, it is signed by us, and it expires -- which is
// everything `state` is supposed to guarantee, with nothing to store.

import type { Env } from "../env.js";
import { PROVIDERS, clientCredentials, type ProviderId } from "./providers.js";
import { pkceChallenge, pkceVerifier, type AuthRequestClaims } from "./device.js";

export function redirectUri(env: Env, provider: ProviderId): string {
    return `${env.ISSUER}/auth/callback/${provider}`;
}

/** What OpenID 2.0 calls "let the provider tell us who this is". */
const IDENTIFIER_SELECT = "http://specs.openid.net/auth/2.0/identifier_select";

/**
 * The OpenID 2.0 request, which shares almost nothing with the OAuth one.
 *
 * WHERE THE STATE GOES. OpenID 2.0 has no `state` parameter -- the spec simply
 * does not have one. It is carried in `return_to` instead, and that is safe for
 * the same reason a state parameter is: Steam signs `openid.return_to` as part
 * of the assertion, so a tampered state breaks the signature and
 * check_authentication fails. The callback checks the echoed return_to against
 * the one it would have built, which closes the loop.
 */
function openIdRequestUrl(env: Env, claims: AuthRequestClaims, state: string): string {
    const provider = PROVIDERS[claims.provider];
    const returnTo = new URL(redirectUri(env, claims.provider));
    returnTo.searchParams.set("state", state);

    const url = new URL(provider.authorizeUrl);
    url.searchParams.set("openid.ns", "http://specs.openid.net/auth/2.0");
    url.searchParams.set("openid.mode", "checkid_setup");
    url.searchParams.set("openid.return_to", returnTo.toString());
    // The realm is what Steam shows the player as the site asking. Our origin,
    // and it must cover return_to or Steam refuses the request outright.
    url.searchParams.set("openid.realm", new URL(env.ISSUER).origin);
    url.searchParams.set("openid.identity", IDENTIFIER_SELECT);
    url.searchParams.set("openid.claimed_id", IDENTIFIER_SELECT);
    return url.toString();
}

export async function authorizeUrl(
    env: Env, claims: AuthRequestClaims, state: string,
): Promise<string | null> {
    const provider = PROVIDERS[claims.provider];
    if (provider.flow === "openid2") return openIdRequestUrl(env, claims, state);

    const creds = clientCredentials(env as unknown as Record<string, unknown>, claims.provider);
    if (!creds) return null;

    const url = new URL(provider.authorizeUrl);
    url.searchParams.set("client_id", creds.id);
    url.searchParams.set("redirect_uri", redirectUri(env, claims.provider));
    // "token" for the implicit flow, which is all itch.io offers. It changes
    // where the credential arrives -- the fragment rather than the query -- and
    // therefore which callback can read it. See authCallback.
    url.searchParams.set("response_type", provider.flow === "implicit" ? "token" : "code");
    url.searchParams.set("state", state);
    if (provider.scope) url.searchParams.set("scope", provider.scope);
    if (provider.pkce) {
        url.searchParams.set("code_challenge", await pkceChallenge(await pkceVerifier(env, claims.rid)));
        url.searchParams.set("code_challenge_method", "S256");
    }
    // Google will not return an id token from a bare "openid" request without
    // being told the response is for a one-off sign-in; without this it may
    // skip the consent screen and reuse a stale grant.
    if (claims.provider === "google") url.searchParams.set("prompt", "select_account");
    return url.toString();
}

interface TokenResponse { access_token?: string; error?: string }

/**
 * Check an access token that arrived by the implicit flow, by asking the
 * provider who it belongs to.
 *
 * THIS IS WHAT MAKES THE WEAKER FLOW SAFE ENOUGH. We are handed a bearer token
 * by a browser and have no way to know where it came from -- so we do not
 * believe it. We spend it against the provider's own API and use the identity
 * the provider returns. A forged or altered token buys nothing, because itch.io
 * is the one deciding whose it is.
 */
export async function identityFromImplicitToken(
    provider: ProviderId, accessToken: string,
): Promise<ResolvedIdentity | null> {
    if (PROVIDERS[provider].flow !== "implicit") return null;
    if (!accessToken || accessToken.length > 4096) return null;
    return fetchIdentity(provider, accessToken);
}

/**
 * Turn a Steam OpenID callback into an identity, or refuse it.
 *
 * THE ENTIRE SECURITY OF THIS FLOW IS THE SECOND HALF. Everything in the query
 * was written by whoever loaded the URL; a `claimed_id` naming any SteamID64 in
 * the world costs an attacker nothing to type. What they cannot produce is
 * Steam's signature over it -- so the parameters go back to Steam with
 * `openid.mode=check_authentication`, and only Steam answering `is_valid:true`
 * makes any of it true. Nothing is read out of the query before that passes.
 */
export async function identityFromOpenId(
    env: Env, provider: ProviderId, url: URL,
): Promise<ResolvedIdentity | null> {
    const config = PROVIDERS[provider];
    if (config.flow !== "openid2") return null;
    // A cancelled sign-in comes back as mode=cancel, which is not a failure to
    // report to the player as one.
    if (url.searchParams.get("openid.mode") !== "id_res") return null;

    // The return_to Steam signed has to be the one we asked for. Checked before
    // the network call, so a forged callback aimed at another realm never costs
    // a round trip.
    const expected = new URL(redirectUri(env, provider));
    const state = url.searchParams.get("state");
    if (state) expected.searchParams.set("state", state);
    if (url.searchParams.get("openid.return_to") !== expected.toString()) return null;

    const body = new URLSearchParams();
    for (const [k, v] of url.searchParams) if (k.startsWith("openid.")) body.set(k, v);
    body.set("openid.mode", "check_authentication");

    const response = await fetch(config.authorizeUrl, {
        method: "POST",
        headers: {
            "content-type": "application/x-www-form-urlencoded",
            "user-agent": "OpenDoctrines-Net",
        },
        body,
    });
    if (!response.ok) return null;

    // Key-value form, not JSON: "ns:http://...\nis_valid:true\n". Matched on a
    // whole line so that `is_valid:false` can never satisfy a substring test.
    const text = await response.text();
    if (!text.split("\n").some((line) => line.trim() === "is_valid:true")) return null;

    const sub = config.subjectOf(Object.fromEntries(url.searchParams));
    if (!sub) return null;
    return { sub, suggestedName: null, createdAt: null };
}

export async function exchangeCode(
    env: Env, provider: ProviderId, code: string, rid: string,
): Promise<string | null> {
    const config = PROVIDERS[provider];
    const creds = clientCredentials(env as unknown as Record<string, unknown>, provider);
    if (!creds) return null;

    const body = new URLSearchParams({
        grant_type: "authorization_code",
        code,
        redirect_uri: redirectUri(env, provider),
        client_id: creds.id,
        client_secret: creds.secret,
    });
    if (config.pkce) body.set("code_verifier", await pkceVerifier(env, rid));

    const response = await fetch(config.tokenUrl, {
        method: "POST",
        headers: {
            "content-type": "application/x-www-form-urlencoded",
            // GitHub's token endpoint returns form-encoded unless asked
            // otherwise, and silently -- without this the JSON parse below
            // fails on a 200.
            accept: "application/json",
            "user-agent": "OpenDoctrines-Net",
        },
        body,
    });
    if (!response.ok) return null;

    const parsed = await response.json<TokenResponse>().catch(() => null);
    return parsed?.access_token ?? null;
}

export interface ResolvedIdentity {
    sub: string;
    suggestedName: string | null;
    /** When the provider account was made, ms, or null if it does not say. */
    createdAt: number | null;
}

export async function fetchIdentity(
    provider: ProviderId, accessToken: string,
): Promise<ResolvedIdentity | null> {
    const config = PROVIDERS[provider];
    const response = await fetch(config.userUrl, {
        headers: {
            authorization: `Bearer ${accessToken}`,
            accept: "application/json",
            // GitHub rejects requests without one.
            "user-agent": "OpenDoctrines-Net",
        },
    });
    if (!response.ok) return null;

    const user = await response.json<Record<string, unknown>>().catch(() => null);
    if (!user) return null;

    const sub = config.subjectOf(user);
    if (!sub) return null;
    return {
        sub,
        suggestedName: config.suggestedName(user),
        createdAt: config.accountCreatedAt(user),
    };
}
