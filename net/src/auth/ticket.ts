// Join tickets: what a game server is allowed to learn about a player.
//
// A server is run by a stranger. It needs to know enough to run a game --
// "this is the same person who was here yesterday", "call them this", "they
// have a developer badge" -- and it must not be able to learn who that person
// is, act as them anywhere, or compare notes with another server operator.
//
// THE PSEUDONYM
//
//     psid = HMAC(PAIRWISE_KEY, accountId + ":" + serverId)
//
// Same player, same server, forever the same psid -- so a host can keep stats
// and enforce its own bans. Same player, different server: an unrelated value,
// so two operators putting their logs side by side learn nothing. Nobody
// without PAIRWISE_KEY can invert it or link a pair, and PAIRWISE_KEY is a
// Worker secret. This is the OIDC pairwise-subject construction.
//
// WHAT THE TICKET DOES NOT CONTAIN
//
// No account id. No provider identity. No email -- we never collected one.
//
// WHY IT IS USELESS IF STOLEN
//
//   - audience is `od-relay:<sessionId>`, so it cannot call the account API,
//     which only accepts `od-api`;
//   - it is bound to ONE session, so it cannot be replayed at another;
//   - `nonce` is a challenge the HOST generated for one connection, so it
//     cannot be minted in advance or replayed onto another socket;
//   - it lives 120 seconds;
//   - its `jti` is burned on first use, so it cannot be replayed even at the
//     session it was minted for.
//
// THE TICKET NOW REACHES THE HOST, AND THAT IS FINE
//
// It used to be consumed by the relay, which forwarded a statement about the
// peer instead. Game connections no longer pass through us at all -- players
// connect straight to the host -- so the host receives the ticket and verifies
// it itself, offline, against the public key at
// `/.well-known/od-keys.json`. See src/net/JoinTicket.h.
//
// This costs the player nothing: every binding above still holds, so what the
// host ends up holding is a two-minute, single-use, single-session assertion
// that cannot act as the player anywhere. It gains the host the ability to run
// a game while we are down, and it keeps us out of the connection entirely.
//
// The audience still reads `od-relay:` for a design with no relay. It is an
// opaque string that the Worker and every game client must agree on byte for
// byte; renaming it would break all of them at once and buy nothing.
//
// THE LIMIT WE HAVE TO BE HONEST ABOUT
//
// A global nickname defeats all of this on its own -- two operators just
// compare display names, and a rare badge is as good as a name. The pseudonym
// only buys unlinkability if what is DISPLAYED is per-server too, which is why
// `alias` and `presentBadges` exist and why the client must say plainly that
// joining under your account nickname is a public, correlatable choice.

import type { Env } from "../env.js";
import type { Account } from "../accounts/store.js";
import { hmacId, randomId } from "../util/crypto.js";
import { audRelay, sign, TICKET_TTL, type TicketClaims } from "./token.js";

/** Stable, per-(account, server) pseudonym. */
export function psidFor(env: Env, accountId: string, serverId: string): Promise<string> {
    return hmacId(env.PAIRWISE_KEY, `${accountId}:${serverId}`, 22);
}

export interface TicketOptions {
    /** Display name for this server only. Falls back to the account nickname. */
    alias?: string;
    /**
     * Whether to show badges here.
     *
     * A CLIENT-side choice, not just a server-side one. A developer badge is
     * held by a handful of people, so presenting it on two servers links those
     * two appearances as surely as reusing a name would.
     */
    presentBadges: boolean;
    /** The relay's challenge, echoed so a ticket cannot be pre-minted. */
    nonce: string;
}

export async function issueJoinTicket(
    env: Env,
    account: Account,
    sessionId: string,
    serverId: string,
    options: TicketOptions,
    now = Math.floor(Date.now() / 1000),
): Promise<string> {
    const claims: TicketClaims = {
        iss: env.ISSUER,
        aud: audRelay(sessionId),
        psid: await psidFor(env, account.id, serverId),
        name: options.alias?.trim() || account.nick,
        badges: options.presentBadges ? account.badges : [],
        nonce: options.nonce,
        jti: randomId(22),
        iat: now,
        exp: now + TICKET_TTL,
    };
    return sign(env, claims);
}
