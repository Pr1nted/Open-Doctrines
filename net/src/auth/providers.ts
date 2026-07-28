// The three identity providers, and the scopes we ask them for.
//
// THE SCOPES ARE THE POINT. Each one is the narrowest that still yields a
// stable user id, and no more:
//
//   Google   "openid"    gives `sub`. NOT "email", NOT "profile".
//   Discord  "identify"  gives id + username. NOT "email".
//   GitHub   ""          no scope at all; /user still returns id + login.
//
// We do not want an email address. We have nothing to send anyone, no password
// to reset, and no newsletter -- so collecting one would be data we hold, must
// secure, must disclose, and must delete on request, in exchange for nothing.
// The consent screen a player sees is correspondingly short, which is the
// visible half of the same decision.

export const PROVIDER_IDS = ["google", "discord", "github"] as const;
export type ProviderId = typeof PROVIDER_IDS[number];

export function isProviderId(v: string): v is ProviderId {
    return (PROVIDER_IDS as readonly string[]).includes(v);
}

export interface ProviderConfig {
    id: ProviderId;
    label: string;
    authorizeUrl: string;
    tokenUrl: string;
    userUrl: string;
    scope: string;
    /** Whether the provider supports PKCE (RFC 7636). All three do. */
    pkce: boolean;
    /** Pulls the stable, provider-scoped user id out of the userinfo response. */
    subjectOf(user: Record<string, unknown>): string | null;
    /** A name to OFFER as a starting nickname. Never stored on its own. */
    suggestedName(user: Record<string, unknown>): string | null;

    /**
     * When the account at this provider was created, in ms, or null when the
     * provider does not tell us.
     *
     * Used only to refuse brand-new throwaway accounts at signup. It is not
     * stored: it is read once, compared, and discarded.
     */
    accountCreatedAt(user: Record<string, unknown>): number | null;

    /**
     * Whether a brand-new account may be CREATED through this provider, as
     * opposed to merely linked to one that already exists.
     *
     * False is for providers that cannot be age-gated. An evader must then get
     * past a gateable provider to exist at all, and this one becomes a
     * convenience for people who already have an account rather than a way
     * around the gate. It costs no extra scopes and collects nothing new.
     */
    canCreateAccount: boolean;
}

export const PROVIDERS: Record<ProviderId, ProviderConfig> = {
    google: {
        id: "google",
        label: "Google",
        authorizeUrl: "https://accounts.google.com/o/oauth2/v2/auth",
        tokenUrl: "https://oauth2.googleapis.com/token",
        userUrl: "https://openidconnect.googleapis.com/v1/userinfo",
        scope: "openid",
        pkce: true,
        subjectOf: (u) => (typeof u.sub === "string" ? u.sub : null),
        // With "openid" alone there is no name to suggest, which is correct:
        // the player picks a nickname rather than being handed their real one.
        suggestedName: () => null,
        // Google exposes no creation date under `openid`, and getting one would
        // mean asking for scopes we deliberately do not ask for. Nor is there
        // any other usable signal: `sub` is documented as opaque, with no
        // ordering to infer age from, so a heuristic there would be a silent
        // breakage waiting to happen rather than a defence.
        accountCreatedAt: () => null,
        // So Google is LINK-ONLY. It cannot be used to create an account, which
        // is what stops it becoming the way around the age gate -- a new Google
        // account is free and instant, and there is nothing we could check.
        // Someone must first sign up with a provider that CAN be gated; after
        // that, Google is a perfectly good extra way in.
        canCreateAccount: false,
    },
    discord: {
        id: "discord",
        label: "Discord",
        authorizeUrl: "https://discord.com/oauth2/authorize",
        tokenUrl: "https://discord.com/api/oauth2/token",
        userUrl: "https://discord.com/api/users/@me",
        scope: "identify",
        pkce: true,
        subjectOf: (u) => (typeof u.id === "string" ? u.id : null),
        suggestedName: (u) => (typeof u.username === "string" ? u.username : null),
        // A Discord id is a snowflake: the top 42 bits are milliseconds since
        // 2015-01-01. So the creation date comes free with the id, needing no
        // extra request and no extra scope.
        accountCreatedAt: (u) => {
            if (typeof u.id !== "string" || !/^\d{1,20}$/.test(u.id)) return null;
            const ms = (BigInt(u.id) >> 22n) + 1420070400000n;
            const value = Number(ms);
            return Number.isFinite(value) && value > 0 ? value : null;
        },
        canCreateAccount: true,
    },
    github: {
        id: "github",
        label: "GitHub",
        authorizeUrl: "https://github.com/login/oauth/authorize",
        tokenUrl: "https://github.com/login/oauth/access_token",
        userUrl: "https://api.github.com/user",
        scope: "",
        pkce: true,
        // GitHub's id is numeric and, unlike the login, never changes hands.
        // Keying on `login` would mean a renamed account silently becoming a
        // different person -- or worse, someone else claiming the freed name.
        subjectOf: (u) => (typeof u.id === "number" ? String(u.id) : null),
        suggestedName: (u) => (typeof u.login === "string" ? u.login : null),
        accountCreatedAt: (u) => {
            if (typeof u.created_at !== "string") return null;
            const ms = Date.parse(u.created_at);
            return Number.isNaN(ms) ? null : ms;
        },
        canCreateAccount: true,
    },
};

export function clientCredentials(
    env: Record<string, unknown>, provider: ProviderId,
): { id: string; secret: string } | null {
    const id = env[`${provider.toUpperCase()}_CLIENT_ID`];
    const secret = env[`${provider.toUpperCase()}_CLIENT_SECRET`];
    if (typeof id !== "string" || typeof secret !== "string" || !id || !secret) return null;
    return { id, secret };
}
