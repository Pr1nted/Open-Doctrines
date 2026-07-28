// Anti-alt policy, and an honest account of what it can and cannot do.
//
// WHAT IS ACTUALLY ENFORCEABLE
//
// One provider identity maps to exactly one game account. That is real, and it
// is enforced in store.ts: an `ident:` key points at one account, and linking
// refuses to move an identity that already belongs to someone else. So a single
// GitHub account cannot become two OpenDoctrines accounts.
//
// WHAT IS NOT
//
// "One person, one account" is not enforceable and this file does not pretend
// otherwise. GitHub, Google and Discord accounts are free, instant and
// unlimited, and nothing ties a person's Google identity to their Discord one --
// least of all here, where we deliberately store no email and only a keyed hash
// of the provider's user id. Anyone determined to hold two accounts will.
//
// So the goal is COST, not prevention. A ban should mean going away and coming
// back in a month with a different long-lived account, rather than clicking
// "sign up" twice.
//
// THE SOFT SPOT, STATED PLAINLY
//
// Google exposes no account creation date under the `openid` scope, and getting
// one would mean requesting scopes PRIVACY.md says we do not request. So Google
// signups CANNOT be age-gated. If Google is enabled, an evader will use Google,
// and the age gate protects nothing. The two coherent positions are:
//
//   - leave Google disabled and gate the two providers that can be gated, or
//   - enable Google and treat the age gate as friction against the lazy only.
//
// Enabling Google while believing the age gate is a real defence is the one
// position that is wrong, which is why it is written down here.

/**
 * How old a provider account must be before it can create a game account.
 *
 * 30 days is chosen to be longer than a grudge lasts but shorter than any
 * legitimate player's account. A player whose provider account is genuinely new
 * is told exactly how long to wait, and can use a different provider.
 *
 * Set to 0 to disable the gate entirely.
 */
export const MIN_ACCOUNT_AGE_DAYS = 30;

export type AgeVerdict =
    | { ok: true }
    | { ok: false; reason: "too_new"; daysRemaining: number };

/**
 * `createdAt` is null when the provider does not say, and that is treated as a
 * PASS rather than a fail. Refusing everyone a provider cannot vouch for would
 * turn "Google tells us nothing" into "nobody may use Google", which is a
 * bigger change than an anti-alt measure should make on its own.
 */
export function checkAccountAge(
    createdAt: number | null,
    now = Date.now(),
    minDays = MIN_ACCOUNT_AGE_DAYS,
): AgeVerdict {
    if (minDays <= 0 || createdAt === null) return { ok: true };

    // A creation date in the future means the provider or our clock is wrong.
    // Treat it as unknown rather than as infinitely old, which would be the
    // one reading an attacker would want.
    if (createdAt > now) return { ok: true };

    const ageDays = (now - createdAt) / 86_400_000;
    if (ageDays >= minDays) return { ok: true };
    return { ok: false, reason: "too_new", daysRemaining: Math.ceil(minDays - ageDays) };
}

export function tooNewMessage(providerLabel: string, daysRemaining: number): string {
    const days = daysRemaining === 1 ? "1 more day" : `${daysRemaining} more days`;
    return `Your ${providerLabel} account is too new to create an OpenDoctrines ` +
           `account. Try again in ${days}, or sign in with a provider account ` +
           `you have had longer.`;
}
