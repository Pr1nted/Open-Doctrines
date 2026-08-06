// The Worker's bindings and secrets, in one place so nothing reaches for an
// environment variable by string literal.

export interface Env {
    OD_ACCOUNTS: KVNamespace;
    LOBBY: DurableObjectNamespace;

    // Per-IP request limiters. Two rather than one because the endpoint that
    // can instantiate a Durable Object is worth an order of magnitude more than
    // one that reads KV, and a single bucket would have to be sized for the
    // cheaper of the two. See wrangler.toml for the windows and why.
    RATE_LIMIT_SESSION: RateLimit;
    RATE_LIMIT_API: RateLimit;

    ISSUER: string;

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
}
