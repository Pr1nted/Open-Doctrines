// The Worker's bindings and secrets, in one place so nothing reaches for an
// environment variable by string literal.

export interface Env {
    OD_ACCOUNTS: KVNamespace;
    LOBBY: DurableObjectNamespace;

    /**
     * Download counters for the mod registry, one object per mod.
     *
     * A Durable Object rather than KV because counting is a read-modify-write
     * and the free plan's 1,000 KV writes a day are the budget that account
     * creation lives on. See mods/counts.ts.
     */
    MOD_COUNTS: DurableObjectNamespace;

    // Per-IP request limiters. Two rather than one because the endpoint that
    // can instantiate a Durable Object is worth an order of magnitude more than
    // one that reads KV, and a single bucket would have to be sized for the
    // cheaper of the two. See wrangler.toml for the windows and why.
    RATE_LIMIT_SESSION: RateLimit;
    RATE_LIMIT_API: RateLimit;

    ISSUER: string;

    /**
     * Where the human-readable copies of the policy documents live.
     *
     * Optional. Unset -- which is every fork that has not deployed a web build
     * -- means /privacy and /terms serve markdown to everybody, exactly as
     * before, rather than redirecting to a site that does not exist.
     */
    DOCS_BASE?: string;

    // Ed25519 keypair, as JWK JSON strings. Two separate secrets rather than
    // deriving the public half from the private one: `crypto.subtle` will not
    // export a public key from a private import, and we serve the public JWK
    // on a hot path (/.well-known/od-keys.json) where a derivation would be
    // pure overhead.
    ED25519_PRIVATE_KEY: string;
    ED25519_PUBLIC_JWK: string;

    // Keyed hashes. Both must be stable forever once accounts exist: rotating
    // IDENT_KEY orphans every linked identity (nobody can log in again), and
    // rotating PAIRWISE_KEY changes every player's per-server pseudonym, which
    // looks to every server like its entire playerbase was replaced. See
    // README.md, "Key rotation".
    IDENT_KEY: string;
    PAIRWISE_KEY: string;

    ADMIN_SECRET: string;

    GOOGLE_CLIENT_ID?: string;
    GOOGLE_CLIENT_SECRET?: string;
    DISCORD_CLIENT_ID?: string;
    DISCORD_CLIENT_SECRET?: string;
    GITHUB_CLIENT_ID?: string;
    GITHUB_CLIENT_SECRET?: string;

    // The streaming platforms. Optional like the rest: credentialsFor() returns
    // null when a provider has no id set, and a provider with no credentials is
    // simply not offered -- so a fork that has not registered an app with
    // Twitch does not show a Twitch button that cannot work.
    TWITCH_CLIENT_ID?: string;
    TWITCH_CLIENT_SECRET?: string;
    YOUTUBE_CLIENT_ID?: string;
    YOUTUBE_CLIENT_SECRET?: string;
    KICK_CLIENT_ID?: string;
    KICK_CLIENT_SECRET?: string;
    /// Public reads on the YouTube Data API take a key rather than OAuth.
    YOUTUBE_API_KEY?: string;

    // Where player reports go. All optional: with none of them set the
    // endpoint still accepts and validates a report and simply has nowhere to
    // put it, which is the right behaviour for a fork that has not configured
    // one -- the game should not show an error to a player for the
    // maintainer's missing secret.
    //
    // The security webhook is separate on purpose. A report saying "here is
    // how to get past the mod sandbox" must never become a public GitHub
    // issue, and routing it to its own channel is how that is enforced rather
    // than remembered. Falls back to the ordinary webhook when unset.
    FEEDBACK_DISCORD_WEBHOOK?: string;
    FEEDBACK_DISCORD_SUGGESTION_WEBHOOK?: string;
    FEEDBACK_DISCORD_SECURITY_WEBHOOK?: string;
    FEEDBACK_GITHUB_TOKEN?: string;
    FEEDBACK_GITHUB_REPO?: string;   // "owner/repo"
    /** Shared secret for the repository's issues webhook. See feedback/github-hook.ts. */
    FEEDBACK_GITHUB_WEBHOOK_SECRET?: string;
    /** Where bans and timeouts are announced. See moderation/reports.ts. */
    MODERATION_DISCORD_WEBHOOK?: string;

    /**
     * VirusTotal, for looking up a mod's declared SHA-256.
     *
     * Optional like every other integration here. Unset means listings are
     * shown as "unscanned", which is the truth, rather than the endpoint
     * failing or -- far worse -- defaulting to "clean". See mods/scan.ts.
     */
    VIRUSTOTAL_API_KEY?: string;
}
