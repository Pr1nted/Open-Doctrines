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

export async function authorizeUrl(
    env: Env, claims: AuthRequestClaims, state: string,
): Promise<string | null> {
    const provider = PROVIDERS[claims.provider];
    const creds = clientCredentials(env as unknown as Record<string, unknown>, claims.provider);
    if (!creds) return null;

    const url = new URL(provider.authorizeUrl);
    url.searchParams.set("client_id", creds.id);
    url.searchParams.set("redirect_uri", redirectUri(env, claims.provider));
    url.searchParams.set("response_type", "code");
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
