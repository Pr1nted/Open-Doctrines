// The identity providers, and the scopes we ask them for.
//
// THE SCOPES ARE THE POINT. Each one is the narrowest that still yields a
// stable user id, and no more:
//
//   Google   "openid"    gives `sub`. NOT "email", NOT "profile".
//   Discord  "identify"  gives id + username. NOT "email".
//   GitHub   ""             no scope at all; /user still returns id + login.
//   itch.io  "profile:me"   gives the numeric user id. Nothing about games.
//
// ITCH.IO IS DIFFERENT IN KIND, NOT JUST IN DETAIL
//
// The other three do the authorization-code flow: the browser hands us a code,
// and we exchange it server-side using a client secret nobody else has. itch.io
// implements only the IMPLICIT flow -- no token endpoint, no client secret, no
// PKCE, and the access token comes back in the URL fragment, which browsers
// never send to a server. So it needs its own path through the callback; see
// authItchToken in index.ts.
//
// What makes it trustworthy anyway is that we never believe the token: the
// service calls api.itch.io/profile with it and uses what itch.io answers. A
// forged token gets nothing. What the weaker flow does cost is the assurance
// that the token reached us by a channel only we could read, which is why itch
// is LINK-ONLY below.
//
// We do not want an email address. We have nothing to send anyone, no password
// to reset, and no newsletter -- so collecting one would be data we hold, must
// secure, must disclose, and must delete on request, in exchange for nothing.
// The consent screen a player sees is correspondingly short, which is the
// visible half of the same decision.

export const PROVIDER_IDS =
    ["google", "discord", "github", "itch", "twitch", "youtube", "kick", "steam"] as const;
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
    /** Whether the provider supports PKCE (RFC 7636). All but itch.io do. */
    pkce: boolean;

    /**
     * Which OAuth dance this provider does.
     *
     * "code" is the ordinary one: a code arrives on the callback and is
     * exchanged server-side for a token. "implicit" means the token itself
     * arrives in the URL fragment and the callback has to be a page that reads
     * it -- itch.io offers nothing else.
     *
     * "openid2" is not OAuth at all. Steam speaks OpenID 2.0: no client
     * registration, no secret, no scopes and no tokens. The identity arrives in
     * the callback query already signed, and the only thing that makes it
     * trustworthy is asking Steam to confirm the signature is theirs. See
     * identityFromOpenId.
     */
    flow: "code" | "implicit" | "openid2";
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
        flow: "code",
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
        flow: "code",
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
    // ── THE STREAMING PLATFORMS ──
    //
    // Linked for the same reason the others are -- a way in, and a verified
    // channel on a profile -- and additionally because a tournament wants to
    // know whose stream is whose. None of them is allowed to CREATE an account:
    // see canCreateAccount below.
    twitch: {
        id: "twitch",
        label: "Twitch",
        authorizeUrl: "https://id.twitch.tv/oauth2/authorize",
        tokenUrl: "https://id.twitch.tv/oauth2/token",
        userUrl: "https://id.twitch.tv/oauth2/userinfo",
        // OIDC, so `openid` alone is enough: the userinfo response carries the
        // stable id and the channel name, and nothing here wants an email.
        scope: "openid",
        // Twitch's OIDC documentation lists client_secret_post and does not
        // document code_challenge, so this asks for the plain code flow rather
        // than sending parameters the token endpoint may reject.
        pkce: false,
        flow: "code",
        subjectOf: (u) => (typeof u.sub === "string" ? u.sub : null),
        suggestedName: (u) =>
            typeof u.preferred_username === "string" ? u.preferred_username : null,
        // Twitch does not say when the account was made, so the young-account
        // check has nothing to work with here.
        accountCreatedAt: () => null,
        canCreateAccount: false,
    },
    youtube: {
        id: "youtube",
        label: "YouTube",
        // Google's OAuth, because that is what YouTube uses -- but the SUBJECT
        // is the channel id from the Data API rather than the Google `sub`.
        // Two reasons: a Google account may hold several channels, and linking
        // "youtube" must not silently be the same identity as linking "google".
        authorizeUrl: "https://accounts.google.com/o/oauth2/v2/auth",
        tokenUrl: "https://oauth2.googleapis.com/token",
        userUrl: "https://www.googleapis.com/youtube/v3/channels?part=id,snippet&mine=true",
        scope: "https://www.googleapis.com/auth/youtube.readonly",
        pkce: true,
        flow: "code",
        subjectOf: (u) => {
            const items = u.items;
            if (!Array.isArray(items) || items.length === 0) return null;
            const first = items[0] as Record<string, unknown>;
            return typeof first.id === "string" ? first.id : null;
        },
        suggestedName: (u) => {
            const items = u.items;
            if (!Array.isArray(items) || items.length === 0) return null;
            const snippet = (items[0] as Record<string, unknown>).snippet;
            if (!snippet || typeof snippet !== "object") return null;
            const title = (snippet as Record<string, unknown>).title;
            return typeof title === "string" ? title : null;
        },
        accountCreatedAt: (u) => {
            const items = u.items;
            if (!Array.isArray(items) || items.length === 0) return null;
            const snippet = (items[0] as Record<string, unknown>).snippet;
            if (!snippet || typeof snippet !== "object") return null;
            const at = (snippet as Record<string, unknown>).publishedAt;
            if (typeof at !== "string") return null;
            const ms = Date.parse(at);
            return Number.isFinite(ms) ? ms : null;
        },
        canCreateAccount: false,
    },
    kick: {
        id: "kick",
        label: "Kick",
        authorizeUrl: "https://id.kick.com/oauth/authorize",
        tokenUrl: "https://id.kick.com/oauth/token",
        // Called with no query parameters, this answers for the token's own
        // user.
        userUrl: "https://api.kick.com/public/v1/users",
        scope: "user:read",
        // Kick REQUIRES PKCE: the authorize endpoint mandates code_challenge
        // and code_challenge_method=S256.
        pkce: true,
        flow: "code",
        subjectOf: (u) => {
            // { data: [ { user_id: 123, name: "..." } ] }
            const data = u.data;
            const row = Array.isArray(data) && data.length > 0
                ? (data[0] as Record<string, unknown>) : null;
            const id = row ? row.user_id : u.user_id;
            // Numeric on the wire; stored as a string like every other subject.
            if (typeof id === "number" && Number.isFinite(id)) return String(id);
            return typeof id === "string" && id ? id : null;
        },
        suggestedName: (u) => {
            const data = u.data;
            const row = Array.isArray(data) && data.length > 0
                ? (data[0] as Record<string, unknown>) : null;
            const name = row ? row.name : u.name;
            return typeof name === "string" ? name : null;
        },
        accountCreatedAt: () => null,
        canCreateAccount: false,
    },
    github: {
        id: "github",
        label: "GitHub",
        authorizeUrl: "https://github.com/login/oauth/authorize",
        tokenUrl: "https://github.com/login/oauth/access_token",
        userUrl: "https://api.github.com/user",
        scope: "",
        pkce: true,
        flow: "code",
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
    itch: {
        id: "itch",
        label: "itch.io",
        authorizeUrl: "https://itch.io/user/oauth",
        // Empty on purpose: there is no token endpoint. The implicit flow hands
        // the token straight to the browser, so there is nothing to exchange.
        tokenUrl: "",
        userUrl: "https://api.itch.io/profile",
        // The narrowest scope itch.io offers that yields an id. NOT
        // profile:games, NOT profile:owned -- we have no business knowing what
        // somebody has bought.
        scope: "profile:me",
        pkce: false,
        flow: "implicit",
        // Nested under "user", and the numeric id rather than the username for
        // the same reason as GitHub: usernames change hands, ids do not.
        subjectOf: (u) => {
            const user = u.user as Record<string, unknown> | undefined;
            return user && typeof user.id === "number" ? String(user.id) : null;
        },
        suggestedName: (u) => {
            const user = u.user as Record<string, unknown> | undefined;
            if (!user) return null;
            if (typeof user.display_name === "string" && user.display_name) {
                return user.display_name;
            }
            return typeof user.username === "string" ? user.username : null;
        },
        // itch.io's profile carries no creation date, so there is nothing to
        // age-gate on. That is precisely why canCreateAccount is false.
        accountCreatedAt: () => null,
        /**
         * LINK ONLY, for two reasons that point the same way.
         *
         * There is no creation date, so a brand-new throwaway cannot be told
         * from a decade-old account -- and refusing new accounts at signup is
         * the only gate this service has. And the implicit flow is the weaker
         * of the two dances. Requiring an account to exist first means somebody
         * has already come through a gateable provider, and itch.io becomes a
         * convenience for people who are already here rather than a way round
         * the gate.
         */
        canCreateAccount: false,
    },

    /**
     * STEAM IS A DIFFERENT PROTOCOL, NOT A DIFFERENT DIALECT.
     *
     * itch.io is OAuth done weakly; Steam is not OAuth. It implements OpenID
     * 2.0, which has no client id, no client secret, no scope parameter and no
     * tokens of any kind. There is nothing to register and nothing to keep
     * safe. One endpoint serves as both the authorization endpoint and the
     * verification endpoint, told apart by `openid.mode`.
     *
     * What arrives on the callback is a signed assertion naming a SteamID64.
     * The signature is Steam's, and the ONLY way to check it is to hand every
     * parameter back to Steam with `openid.mode=check_authentication` and
     * believe the answer. Skip that step and this provider trusts a URL that
     * anybody can type. Same principle as itch.io: never believe the
     * credential, spend it against the provider and use what the provider says.
     */
    steam: {
        id: "steam",
        label: "Steam",
        authorizeUrl: "https://steamcommunity.com/openid/login",
        // Empty for a harder reason than itch.io's: OpenID 2.0 has no tokens,
        // so there is no endpoint an exchange could even be sent to.
        tokenUrl: "",
        // Also empty. The assertion carries the SteamID64 and nothing else. A
        // display name would mean a Steam Web API key in the Worker and a call
        // to ISteamUser/GetPlayerSummaries -- a new long-lived secret to hold
        // and rotate, bought with a nickname suggestion. Not a trade worth
        // making, and declining it makes Steam the narrowest provider here.
        userUrl: "",
        scope: "",
        pkce: false,
        flow: "openid2",
        /**
         * The SteamID64 out of the claimed identifier.
         *
         * Anchored deliberately. `claimed_id` is attacker-influenced until
         * check_authentication has passed, and a loose pattern would accept
         * something like `https://evil.example/steamcommunity.com/openid/id/1`.
         */
        subjectOf: (u) => {
            const claimed = u["openid.claimed_id"];
            if (typeof claimed !== "string") return null;
            const m = /^https:\/\/steamcommunity\.com\/openid\/id\/([0-9]{17})$/.exec(claimed);
            return m?.[1] ?? null;
        },
        // Nothing to suggest: the assertion has no name in it.
        suggestedName: () => null,
        /**
         * Steam does publish a creation date -- `timecreated` -- but only for
         * PUBLIC profiles, so it goes missing exactly when somebody has a
         * reason to hide it. A gate that any evader can switch off is not a
         * gate, which is why canCreateAccount is false below.
         */
        accountCreatedAt: () => null,
        /**
         * LINK ONLY, for the reason directly above.
         *
         * A player signing in with a linked Steam account lands straight in
         * their existing account; one who has never linked is told to sign up
         * through a gateable provider first and add Steam afterwards. Same
         * bargain as itch.io: a convenience for people who are already here,
         * not a way around the age gate.
         */
        canCreateAccount: false,
    },
};

/**
 * The credentials for a provider, or null when it is not configured.
 *
 * AN IMPLICIT-FLOW PROVIDER HAS NO SECRET, and requiring one would mean itch.io
 * could never be offered however carefully it was set up -- the operator would
 * set the client id, see nothing appear, and have no way to tell why. The
 * secret is required for exactly the providers that spend one.
 */
export function clientCredentials(
    env: Record<string, unknown>, provider: ProviderId,
): { id: string; secret: string } | null {
    // OpenID 2.0 has no client registration: nothing to send, nothing to keep
    // secret. Steam identifies the relying party by the realm in the request,
    // which is our own origin -- so there is no environment variable to miss,
    // and Steam sign-in works on a fresh deployment with no configuration.
    if (PROVIDERS[provider].flow === "openid2") return { id: "", secret: "" };

    const id = env[`${provider.toUpperCase()}_CLIENT_ID`];
    if (typeof id !== "string" || !id) return null;

    if (PROVIDERS[provider].flow === "implicit") return { id, secret: "" };

    const secret = env[`${provider.toUpperCase()}_CLIENT_SECRET`];
    if (typeof secret !== "string" || !secret) return null;
    return { id, secret };
}
