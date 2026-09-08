// itch.io sign-in, which does a different dance from the other three.
//
// It is implicit-flow only: no token endpoint, no client secret, no PKCE, and
// the access token arrives in a URL fragment that browsers never send to a
// server. Two things therefore have to hold, and both are here:
//
//   1. The service NEVER believes a token it is handed. It spends it against
//      itch.io and uses the identity itch.io returns. That is the whole reason
//      a weaker flow is acceptable at all.
//   2. itch.io cannot CREATE an account, only link to one. Its profile carries
//      no creation date, so the one gate this service has -- refusing brand-new
//      throwaways at signup -- cannot be applied to it.

import { beforeEach, describe, expect, it, vi } from "vitest";
import {
    clientCredentials, PROVIDERS, PROVIDER_IDS, isProviderId,
} from "../src/auth/providers.js";
import { identityFromImplicitToken } from "../src/auth/oauth.js";

beforeEach(() => { vi.restoreAllMocks(); });

describe("how itch.io is configured", () => {
    it("is a provider the service knows", () => {
        expect(isProviderId("itch")).toBe(true);
        expect(PROVIDER_IDS).toContain("itch");
    });

    it("is implicit-flow, with no token endpoint and no PKCE", () => {
        const p = PROVIDERS.itch;
        expect(p.flow).toBe("implicit");
        expect(p.tokenUrl).toBe("");      // there is nothing to exchange a code at
        expect(p.pkce).toBe(false);       // itch.io does not offer it
    });

    it("CANNOT create an account, only link to one", () => {
        // The gate. itch.io tells us nothing about when the account was made,
        // so a throwaway registered this morning is indistinguishable from a
        // decade-old account -- and refusing new ones is the only gate there is.
        expect(PROVIDERS.itch.canCreateAccount).toBe(false);
        expect(PROVIDERS.itch.accountCreatedAt({ user: { id: 1 } })).toBeNull();
    });

    it("asks for the narrowest scope that yields an id", () => {
        // Not profile:games, not profile:owned. What somebody has bought is
        // none of this service's business.
        expect(PROVIDERS.itch.scope).toBe("profile:me");
        expect(PROVIDERS.itch.scope).not.toContain("owned");
        expect(PROVIDERS.itch.scope).not.toContain("games");
    });

    it("every other provider still does the ordinary code flow", () => {
        // A new provider must not inherit the weaker dance by accident.
        for (const id of ["google", "discord", "github"] as const) {
            expect(PROVIDERS[id].flow).toBe("code");
            expect(PROVIDERS[id].tokenUrl).not.toBe("");
        }
    });
});

describe("being configured at all", () => {
    it("needs only a client id, because there is no secret to have", () => {
        // The trap this guards: requiring a secret would mean itch could never
        // be offered however carefully it was set up, and the operator would
        // have no way to tell why nothing appeared.
        expect(clientCredentials({ ITCH_CLIENT_ID: "abc" }, "itch")).toEqual({
            id: "abc", secret: "",
        });
        expect(clientCredentials({}, "itch")).toBeNull();
        expect(clientCredentials({ ITCH_CLIENT_ID: "" }, "itch")).toBeNull();
    });

    it("but a code-flow provider still needs both", () => {
        expect(clientCredentials({ GITHUB_CLIENT_ID: "abc" }, "github")).toBeNull();
        expect(clientCredentials(
            { GITHUB_CLIENT_ID: "abc", GITHUB_CLIENT_SECRET: "s" }, "github",
        )).toEqual({ id: "abc", secret: "s" });
    });
});

describe("reading the identity itch.io returns", () => {
    const profile = {
        user: {
            id: 29789, username: "fasterthanlime", display_name: "Amos",
            gamer: true, developer: true,
        },
    };

    it("keys on the numeric id, not the username", () => {
        // A username can change hands; an id cannot. Keying on the name would
        // mean a renamed account becoming a different person -- or somebody
        // else claiming the freed name and inheriting the account.
        expect(PROVIDERS.itch.subjectOf(profile)).toBe("29789");
        expect(PROVIDERS.itch.subjectOf({ user: { username: "someone" } })).toBeNull();
        expect(PROVIDERS.itch.subjectOf({})).toBeNull();
        expect(PROVIDERS.itch.subjectOf({ user: null })).toBeNull();
    });

    it("suggests a display name, falling back to the username", () => {
        expect(PROVIDERS.itch.suggestedName(profile)).toBe("Amos");
        expect(PROVIDERS.itch.suggestedName({ user: { id: 1, username: "bob" } })).toBe("bob");
        expect(PROVIDERS.itch.suggestedName({ user: { id: 1 } })).toBeNull();
    });
});

describe("a token handed to us is spent, not believed", () => {
    it("asks itch.io who it belongs to and uses that answer", async () => {
        let sawAuth = "";
        let sawUrl = "";
        vi.stubGlobal("fetch", async (url: string, init: RequestInit) => {
            sawUrl = String(url);
            sawAuth = String((init.headers as Record<string, string>).authorization ?? "");
            return new Response(JSON.stringify({ user: { id: 29789, username: "amos" } }),
                                { status: 200, headers: { "content-type": "application/json" } });
        });

        const identity = await identityFromImplicitToken("itch", "a-token-from-the-fragment");
        expect(sawUrl).toBe("https://api.itch.io/profile");
        expect(sawAuth).toContain("a-token-from-the-fragment");
        expect(identity?.sub).toBe("29789");
    });

    it("gets nothing from a token itch.io rejects", async () => {
        // The property that makes the implicit flow safe enough: a forged or
        // stolen-then-revoked token buys nothing, because itch.io decides.
        vi.stubGlobal("fetch", async () => new Response("{}", { status: 401 }));
        expect(await identityFromImplicitToken("itch", "forged")).toBeNull();
    });

    it("refuses an empty or absurd token without calling out at all", async () => {
        const spy = vi.fn();
        vi.stubGlobal("fetch", spy);
        expect(await identityFromImplicitToken("itch", "")).toBeNull();
        expect(await identityFromImplicitToken("itch", "x".repeat(5000))).toBeNull();
        expect(spy).not.toHaveBeenCalled();
    });

    it("refuses to treat a code-flow provider as implicit", async () => {
        // Guards against a future provider being wired to the wrong path,
        // which would skip the code exchange and the client secret with it.
        const spy = vi.fn();
        vi.stubGlobal("fetch", spy);
        expect(await identityFromImplicitToken("github", "some-token")).toBeNull();
        expect(spy).not.toHaveBeenCalled();
    });
});
